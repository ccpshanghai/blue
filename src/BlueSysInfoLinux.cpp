// Copyright © 2026 CCP ehf.

// Desktop Linux implementation of the eleven platform-specific BlueSysInfo methods and the
// three BlueSysInfo* constructors beside them. Same one-file-per-platform convention as
// BlueSysInfoAndroid.cpp, and guarded so that Android -- which also defines __linux__ -- keeps
// its own file.
//
// Directories follow the XDG base directory spec where it has an answer and the conventional
// FHS location where it does not. Nothing here creates a directory: unlike an app sandbox, a
// desktop home already has these, and a getter that mkdirs in someone's home is a surprise.

#include "StdAfx.h"

#if defined( __linux__ ) && !defined( __ANDROID__ )

#include "BlueSysInfo.h"
#include <BlueExposure.h>
#include "pdm.h"

#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>

namespace
{
	const int64_t s_startTime = TimeNow();

	std::wstring Widen( const std::string& value )
	{
		return value.empty() ? std::wstring() : UTF8ToWide( value );
	}

	bool IsDirectory( const std::string& path )
	{
		struct stat info;
		return !path.empty() && stat( path.c_str(), &info ) == 0 && S_ISDIR( info.st_mode );
	}

	std::string Home()
	{
		const char* home = getenv( "HOME" );
		return home ? std::string( home ) : std::string();
	}

	// $XDG_DATA_HOME, or ~/.local/share when it is unset or empty, as the spec says.
	std::string DataHome()
	{
		const char* xdg = getenv( "XDG_DATA_HOME" );
		if( xdg && *xdg )
		{
			return xdg;
		}
		auto home = Home();
		return home.empty() ? std::string() : home + "/.local/share";
	}

	std::string Hostname()
	{
		char buffer[256] = { 0 };
		if( gethostname( buffer, sizeof( buffer ) - 1 ) != 0 )
		{
			return {};
		}
		return buffer;
	}

	std::string ReadFirstLine( const char* path )
	{
		std::ifstream file( path );
		std::string line;
		std::getline( file, line );
		return line;
	}

	// PDM models model and stepping but not the x86 family, and on x86 the family is a real
	// register value rather than the synthetic one the Apple body reads on Apple silicon.
	int ReadCpuFamily()
	{
		std::ifstream file( "/proc/cpuinfo" );
		std::string line;
		while( std::getline( file, line ) )
		{
			if( line.compare( 0, 10, "cpu family" ) == 0 )
			{
				auto colon = line.find( ':' );
				return colon == std::string::npos ? 0 : atoi( line.c_str() + colon + 1 );
			}
		}
		return 0;
	}
}


// The macOS body answers ~/Library/Application Support without an app name appended; the XDG
// data home is the same thing on Linux.
std::wstring BlueSysInfo::GetUserApplicationDataDirectory() const
{
	return Widen( DataHome() );
}

// /usr/local/share is the shared location, and like /Library/Application Support on macOS it
// is not writable by an ordinary user. Falls back to the per-user directory for the same
// reason the macOS body does.
std::wstring BlueSysInfo::GetSharedApplicationDataDirectory() const
{
	if( access( "/usr/local/share", W_OK ) == 0 )
	{
		return L"/usr/local/share";
	}
	return GetUserApplicationDataDirectory();
}

// xdg-user-dirs localizes this folder, but ~/Documents is what it creates on an English
// desktop and what every desktop recognizes. Home is the answer when it is missing rather than
// a path the caller would have to create.
std::wstring BlueSysInfo::GetUserDocumentsDirectory() const
{
	auto home = Home();
	auto documents = home + "/Documents";
	return Widen( IsDirectory( documents ) ? documents : home );
}

std::wstring BlueSysInfo::GetSharedFontsDirectory() const
{
	return L"/usr/local/share/fonts";
}

std::wstring BlueSysInfo::GetSystemFontsDirectory() const
{
	return L"/usr/share/fonts";
}

uint32_t BlueSysInfo::GetProcessBitCount() const
{
#if __LP64__
	return 64;
#else
	return 32;
#endif
}

// The kernel's machine name, so a 32-bit process on a 64-bit kernel still reports 64.
uint32_t BlueSysInfo::GetSystemBitCount() const
{
	struct utsname name;
	if( uname( &name ) != 0 )
	{
		return GetProcessBitCount();
	}
	return strstr( name.machine, "64" ) ? 64 : 32;
}

uint64_t BlueSysInfo::GetProcessStartTime() const
{
	return s_startTime;
}

// /etc/machine-id is the stable per-installation id systemd and dbus both use, and it is what
// the pdm fork's Linux backend reports for GetMachineUuidString. The two must agree.
std::string BlueSysInfo::GetMachineUuid() const
{
	return ReadFirstLine( "/etc/machine-id" );
}

std::wstring BlueSysInfo::GetMachineName() const
{
	auto host = Hostname();
	auto dot = host.find( '.' );
	if( dot != std::string::npos )
	{
		host.erase( dot );
	}
	return Widen( host );
}

// The label after the host, as the Apple body splits it. A bare hostname has no domain, and
// this says so rather than repeating the hostname the way the Apple body would.
std::wstring BlueSysInfo::GetDomainName() const
{
	auto host = Hostname();
	auto first = host.find( '.' );
	if( first == std::string::npos )
	{
		return {};
	}
	auto domain = host.substr( first + 1 );
	auto second = domain.find( '.' );
	if( second != std::string::npos )
	{
		domain.erase( second );
	}
	return Widen( domain );
}


BlueSysInfoCpu::BlueSysInfoCpu() :
	m_extensions( PDM::GetCPUInfo().extensions )
{
	auto pdmCpu = PDM::GetCPUInfo();

	m_mHz = pdmCpu.frequency;
	m_brand = pdmCpu.brand;
	m_logicalCpuCount = pdmCpu.logicalCoreCount;
	m_bitCount = pdmCpu.bitness == PDM::Bitness::BITNESS_64 ? 64 : 32;
	m_revision = uint32_t( ( pdmCpu.model << 8 ) | pdmCpu.stepping );

	const char* platform;
#if __x86_64__
	m_architecture = "AMD64";
	platform = "AMD64";
	m_family = ReadCpuFamily();
#elif __i386__
	m_architecture = "x86";
	platform = "x86";
	m_family = ReadCpuFamily();
#elif __aarch64__
	// No CPUID family on ARM; see the Android body.
	m_architecture = "ARM64";
	platform = "ARM64";
	m_family = 0;
#else
#error "Unsupported architecture"
#endif

	char buffer[512] = { 0 };
	snprintf( buffer, sizeof( buffer ), "%s Family %i Model %i Stepping %i, %s",
	          platform, int( m_family ), int( pdmCpu.model ), int( pdmCpu.stepping ),
	          pdmCpu.vendor.c_str() );
	m_identifier = buffer;
}


// The kernel release, "7.0.0-30-generic": major, minor, and the patch level as the build
// number. The distribution version is a different question and lives in /etc/os-release.
BlueSysInfoOs::BlueSysInfoOs()
{
	m_platform = LINUX_OS;
	m_majorVersion = 0;
	m_minorVersion = 0;
	m_buildNumber = 0;

	struct utsname name;
	if( uname( &name ) == 0 )
	{
		int major = 0, minor = 0, patch = 0;
		sscanf( name.release, "%d.%d.%d", &major, &minor, &patch );
		m_majorVersion = major;
		m_minorVersion = minor;
		m_buildNumber = patch;
	}

	m_suite = DESKTOP;
}


BlueSysInfoMemory::BlueSysInfoMemory()
{
	CcpProcessMemoryInfo memInfo;
	if( CcpGetProcessMemoryInfo( memInfo ) )
	{
		m_workingSet = uint64_t( memInfo.workingSetSize );
		m_pageFile = uint64_t( memInfo.pageFileUsage );
	}
	else
	{
		m_workingSet = 0;
		m_pageFile = 0;
	}

	long pageSize = sysconf( _SC_PAGE_SIZE );
	long totalPages = sysconf( _SC_PHYS_PAGES );
	long freePages = sysconf( _SC_AVPHYS_PAGES );

	m_totalPhysical = pageSize > 0 && totalPages > 0
		? uint64_t( totalPages ) * uint64_t( pageSize ) : 0;
	m_availablePhysical = pageSize > 0 && freePages > 0
		? uint64_t( freePages ) * uint64_t( pageSize ) : 0;
}

#endif
