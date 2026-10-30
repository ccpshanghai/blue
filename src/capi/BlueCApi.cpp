// Copyright © 2026 CCP ehf.
//
// Blue C API: version, lifecycle, main-thread check, errors and the per-frame pump.

#include "StdAfx.h"

#if BLUE_WITH_CABI

#include "BlueCApiInternal.h"
#include "Blue.h"
#include "IBlueOS.h"
#include "IBluePaths.h"
#include "ResourceLoading.h"

#include <CcpThread.h>

#include <atomic>
#include <vector>

#include <sys/stat.h>

static CcpLogChannel_t s_ch = CCP_LOG_DEFINE_CHANNEL( "CApi" );

namespace BlueCApi
{

namespace
{

enum StateValue
{
	STATE_UNINITIALIZED,
	STATE_STARTING,		// inside blue_initialize; callers see "not initialized"
	STATE_RUNNING,
	STATE_SHUT_DOWN,	// after blue_shutdown, or a blue_initialize that failed part-way
};

std::atomic<int> s_state{ STATE_UNINITIALIZED };
CcpThreadId_t s_mainThread{};

bool s_logToStderr = false;
thread_local std::string* t_capturedError = nullptr;

const char* LogTypeName( CCP::LogType type )
{
	switch( type )
	{
	case CCP::LOGTYPE_INFO:
		return "INFO";
	case CCP::LOGTYPE_NOTICE:
		return "NOTICE";
	case CCP::LOGTYPE_WARN:
		return "WARN";
	case CCP::LOGTYPE_ERR:
		return "ERR";
	default:
		return "?";
	}
}

void LogEcho( CcpLogChannel_t& channel, CCP::LogType type, unsigned long, const char* message )
{
	if( !message )
	{
		return;
	}
	if( type >= CCP::LOGTYPE_ERR && t_capturedError )
	{
		size_t length = strlen( message );
		while( length > 0 && ( message[length - 1] == '\n' || message[length - 1] == '\r' ) )
		{
			--length;
		}
		t_capturedError->assign( message, length );
	}
	if( s_logToStderr )
	{
		fprintf( stderr, "%s/%s %s: %s\n", channel.facility ? channel.facility : "", channel.object ? channel.object : "", LogTypeName( type ), message );
	}
}

long CountBeOsErrors()
{
	long count = 0;
	while( BeOS->GetError( count ) )
	{
		++count;
	}
	return count;
}

const char* StatusMessage( int32_t code )
{
	switch( code )
	{
	case BLUE_E_FAILED:
		return "failed";
	case BLUE_E_ARGUMENT_COUNT:
		return "wrong number of arguments";
	case BLUE_E_TYPE:
		return "value of the wrong kind";
	case BLUE_E_OVERFLOW:
		return "number out of range";
	case BLUE_E_INVALID_ARGUMENT:
		return "invalid argument";
	case BLUE_E_SELF:
		return "missing or wrong object";
	case BLUE_E_UNSUPPORTED:
		return "unsupported";
	case BLUE_E_READ_ONLY:
		return "read-only";
	case BLUE_E_NOT_FOUND:
		return "not found";
	case BLUE_E_EXCEPTION:
		return "C++ exception";
	case BLUE_E_OUT_OF_MEMORY:
		return "out of memory";
	case BLUE_E_WRONG_THREAD:
		return "called off blue's main thread (the thread that called blue_initialize)";
	case BLUE_E_NOT_INITIALIZED:
		return "blue is not running (call blue_initialize first; it cannot be used after blue_shutdown)";
	default:
		return "unknown error";
	}
}

bool Utf8ToWide( const char* text, std::wstring& result )
{
	return text && BlueCUtf8ToWide( text, strlen( text ), result );
}

bool IsDirectory( const char* utf8Path, const std::wstring& widePath )
{
#ifdef _WIN32
	(void)utf8Path;
	struct _stat64 info;
	return _wstat64( widePath.c_str(), &info ) == 0 && ( info.st_mode & _S_IFDIR ) != 0;
#else
	(void)widePath;
	struct stat info;
	return stat( utf8Path, &info ) == 0 && S_ISDIR( info.st_mode );
#endif
}

// Converts a counted array of NUL-terminated UTF-8 strings; what names them in messages
int32_t ConvertStrings( const char* const* strings, uint32_t count, const char* what, std::vector<std::wstring>& result, BlueCError& err )
{
	if( count > 0 && !strings )
	{
		return BlueCSetError( &err, BLUE_E_INVALID_ARGUMENT, "%s is null but its count is %u", what, count );
	}
	for( uint32_t i = 0; i < count; ++i )
	{
		std::wstring value;
		if( !Utf8ToWide( strings[i], value ) )
		{
			return BlueCSetError( &err, BLUE_E_INVALID_ARGUMENT, "%s[%u] is null or not UTF-8", what, i );
		}
		result.push_back( std::move( value ) );
	}
	return BLUE_OK;
}

// Undo what blue_initialize started; safe on a partial start
void TearDown()
{
	BlueShutdownResourceLoading();
	BeClasses->ProcessAllPendingDeletes();
	BeOS->SetError( BECLEAR );
	UnregisterLogEcho();
}

} // namespace


int32_t CheckCaller()
{
	if( s_state.load() != STATE_RUNNING )
	{
		return BLUE_E_NOT_INITIALIZED;
	}
	if( CcpGetCurrentThreadId() != s_mainThread )
	{
		return BLUE_E_WRONG_THREAD;
	}
	return BLUE_OK;
}

bool IsCallerOk()
{
	return CheckCaller() == BLUE_OK;
}

void EnsureMessage( BlueCError& err, int32_t code )
{
	// A body that failed with code has set err to it; anything else is stale or missing
	if( err.code != code || !err.message )
	{
		BlueCSetError( &err, code, "%s", StatusMessage( code ) );
	}
}

void PrefixMessage( BlueCError& err, const char* owner, const char* name, size_t nameLength )
{
	const std::string message = err.message ? std::string( err.message, err.message_len ) : std::string( StatusMessage( err.code ) );
	if( owner )
	{
		BlueCSetError( &err, err.code, "%s.%.*s: %s", owner, int( nameLength ), name ? name : "", message.c_str() );
	}
	else
	{
		BlueCSetError( &err, err.code, "%.*s: %s", int( nameLength ), name ? name : "", message.c_str() );
	}
}

ErrorCapture::ErrorCapture()
	: m_previous( t_capturedError )
	, m_beOsErrorCount( CountBeOsErrors() )
{
	t_capturedError = &m_logged;
}

ErrorCapture::~ErrorCapture()
{
	t_capturedError = m_previous;
}

int32_t ErrorCapture::Fail( BlueCError& err, int32_t code, const char* fallback ) const
{
	std::string detail = m_logged;
	if( detail.empty() )
	{
		const long count = CountBeOsErrors();
		if( count > m_beOsErrorCount )
		{
			const IBlueOS::Error* newest = BeOS->GetError( count - 1 );
			if( newest && newest->mDescription )
			{
				detail = newest->mDescription;
			}
		}
	}
	if( detail.empty() )
	{
		return BlueCSetError( &err, code, "%s", fallback );
	}
	return BlueCSetError( &err, code, "%s: %s", fallback, detail.c_str() );
}

void RegisterLogEcho()
{
	const char* setting = getenv( "BLUE_CAPI_LOG" );
	s_logToStderr = setting && strcmp( setting, "1" ) == 0;
	// Thread safe: errors logged on blue's worker threads are dropped by the capture, which
	// only listens on threads inside a C API call
	CCP::RegisterLogEcho( &LogEcho, s_logToStderr ? CCP::LOGTYPE_INFO : CCP::LOGTYPE_ERR, true );
}

void UnregisterLogEcho()
{
	CCP::UnregisterLogEcho( &LogEcho );
}

} // namespace BlueCApi


using namespace BlueCApi;

uint32_t blue_abi_version( void )
{
	return BLUE_CAPI_VERSION;
}

int32_t blue_initialize( const BlueInitOptions* options, BlueCError* err )
{
	return Call( err, [&]( BlueCError& error ) -> int32_t {
		switch( s_state.load() )
		{
		case STATE_UNINITIALIZED:
			break;
		case STATE_SHUT_DOWN:
			return BlueCSetError( &error, BLUE_E_FAILED, "blue cannot be restarted in one process" );
		default:
			return BlueCSetError( &error, BLUE_E_FAILED, "blue is already initialized" );
		}

		// Check every option before starting anything, so that a bad call can be repeated
		if( !options )
		{
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "no options" );
		}
		if( options->struct_size < sizeof( BlueInitOptions ) )
		{
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "options.struct_size is %u, expected at least %zu", options->struct_size, sizeof( BlueInitOptions ) );
		}

		std::wstring appDir;
		if( !Utf8ToWide( options->app_dir, appDir ) )
		{
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "options.app_dir is null or not UTF-8" );
		}
		// blue would fall back to the working directory without a word
		if( !IsDirectory( options->app_dir, appDir ) )
		{
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "options.app_dir '%s' is not a directory", options->app_dir );
		}

		// Element 0 stands for the program name, which blue skips
		std::vector<std::wstring> args{ L"blue" };
		int32_t status = ConvertStrings( options->args, options->arg_count, "options.args", args, error );
		if( status != BLUE_OK )
		{
			return status;
		}

		std::vector<std::wstring> searchPaths;
		status = ConvertStrings( options->search_paths, options->search_path_count, "options.search_paths", searchPaths, error );
		if( status != BLUE_OK )
		{
			return status;
		}
		for( uint32_t i = 0; i < options->search_path_count; ++i )
		{
			// blue only warns about a path without a key, and skips it
			if( searchPaths[i].find( L'=' ) == std::wstring::npos || searchPaths[i][0] == L'=' )
			{
				return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "options.search_paths[%u] is not '<key>=<directory>'", i );
			}
		}

		int expected = STATE_UNINITIALIZED;
		if( !s_state.compare_exchange_strong( expected, STATE_STARTING ) )
		{
			return BlueCSetError( &error, BLUE_E_FAILED, "blue is already initialized" );
		}

		// From here on blue's own steps run; they cannot be repeated, so a failure is final
		struct StartGuard
		{
			bool committed = false;
			~StartGuard()
			{
				if( !committed )
				{
					TearDown();
					s_state = STATE_SHUT_DOWN;
				}
			}
		} guard;

		RegisterLogEcho();
		ErrorCapture capture;

		BlueSetStartupArgs( args );
		// Registers the classes, which BlueInitializePaths creates its file system from
		BlueModuleStartup();
		if( !BlueInitializePaths( appDir ) )
		{
			return capture.Fail( error, BLUE_E_FAILED, "initializing blue's paths failed" );
		}
		if( !BlueInitializeResourceLoading() )
		{
			return capture.Fail( error, BLUE_E_FAILED, "starting blue's resource loading failed" );
		}
		if( !BeOS->Startup( 0 ) )
		{
			return capture.Fail( error, BLUE_E_FAILED, "starting blue's OS layer failed" );
		}
		// Only the Stackless startup sets standard search paths; these are the host's
		if( !searchPaths.empty() && !BlueSetSearchPaths( searchPaths ) )
		{
			return capture.Fail( error, BLUE_E_FAILED, "setting blue's search paths failed" );
		}

		s_mainThread = CcpGetCurrentThreadId();
		guard.committed = true;
		s_state = STATE_RUNNING;
		CCP_LOG_CH( s_ch, "Blue C API %u.%u started", unsigned( BLUE_CAPI_VERSION_MAJOR ), unsigned( BLUE_CAPI_VERSION_MINOR ) );
		return BLUE_OK;
	}, false );
}

int32_t blue_shutdown( BlueCError* err )
{
	return Call( err, []( BlueCError& ) -> int32_t {
		// Terminal even if a step throws
		s_state = STATE_SHUT_DOWN;
		CCP_LOG_CH( s_ch, "Blue C API shutting down" );
		// Not BlueTerminate: that one exits the process
		TearDown();
		return BLUE_OK;
	} );
}

int32_t blue_is_initialized( void )
{
	return s_state.load() == STATE_RUNNING ? 1 : 0;
}

int32_t blue_pump( BlueCError* err )
{
	return Call( err, []( BlueCError& ) -> int32_t {
		BeOS->PumpOS();
		return BLUE_OK;
	} );
}

#endif
