// Copyright © 2026 CCP ehf.
//
// Shared by the Blue C API sources (src/capi): the lifecycle state, the main-thread check,
// the guard every status-returning entry point runs in, and the capture of blue's error log
// that gives failures outside thunks a message.

#pragma once

#define BLUE_CAPI_BUILD
#include "blue_c/blue.h"

#include <cstring>
#include <exception>
#include <string>

namespace BlueCApi
{

// BLUE_OK if blue is running and this is its main thread, else the status to return
int32_t CheckCaller();

// True if CheckCaller passes; for entry points that return a handle, a name or a count
bool IsCallerOk();

// Fill err with a message for code, unless the body that failed already gave one
void EnsureMessage( BlueCError& err, int32_t code );

// Run body( error ) after CheckCaller, with error never null (a local one stands in when the
// caller passes null) and C++ exceptions turned into BLUE_E_EXCEPTION. A failure always leaves
// a message in err.
template< typename Body >
int32_t Call( BlueCError* err, Body&& body, bool checkCaller = true )
{
	BlueCError localError = { BLUE_OK, nullptr, 0 };
	BlueCError& error = err ? *err : localError;

	int32_t status = checkCaller ? CheckCaller() : BLUE_OK;
	if( status != BLUE_OK )
	{
		EnsureMessage( error, status );
	}
	else
	{
		try
		{
			status = body( error );
		}
		catch( const std::exception& e )
		{
			status = BlueCSetError( &error, BLUE_E_EXCEPTION, "C++ exception: %s", e.what() );
		}
		catch( ... )
		{
			status = BlueCSetError( &error, BLUE_E_EXCEPTION, "unknown C++ exception" );
		}
		if( status > 0 )
		{
			status = BLUE_E_FAILED;
		}
		if( status != BLUE_OK )
		{
			EnsureMessage( error, status );
		}
	}

	BlueCErrorClear( &localError );
	return status;
}

// name, with its length in *len (len may be null); for the name accessors
inline const char* NameWithLength( const char* name, size_t* len )
{
	if( len )
	{
		*len = name ? strlen( name ) : 0;
	}
	return name;
}

// Prefix err's message with "<owner>.<name>: ", so conversion errors from a thunk say which
// member or method they came from (owner may be null)
void PrefixMessage( BlueCError& err, const char* owner, const char* name, size_t nameLength );

// While alive, remembers the newest error blue logs on this thread (the C API's log echo
// feeds it) and the newest entry added to BeOS's error log, for failures that blue reports
// only that way (lifecycle steps, object creation)
class ErrorCapture
{
public:
	ErrorCapture();
	~ErrorCapture();
	ErrorCapture( const ErrorCapture& ) = delete;
	ErrorCapture& operator=( const ErrorCapture& ) = delete;

	// Set err to code with the captured message, or fallback when blue logged nothing
	int32_t Fail( BlueCError& err, int32_t code, const char* fallback ) const;

private:
	std::string* m_previous;
	std::string m_logged;
	long m_beOsErrorCount;
};

// The log echo: registered by blue_initialize, removed by blue_shutdown
void RegisterLogEcho();
void UnregisterLogEcho();

// Resource waits (BlueCApiResources.cpp). blue_pump delivers the decided ones after blue's own
// work, outside any notify loop (nested calls deliver nothing); blue_shutdown delivers every
// one left, the pending ones as SHUTDOWN, before blue tears down.
void DeliverResourceWaits();
void ShutDownResourceWaits();

} // namespace BlueCApi
