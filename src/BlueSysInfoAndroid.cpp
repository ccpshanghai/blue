// Copyright © 2026 CCP ehf.

// Android implementation of the eleven platform-specific BlueSysInfo methods and the three
// BlueSysInfo* constructors that sit alongside them. blue's convention is one file per
// platform -- BlueSysInfo.cpp holds the Windows bodies inside a large #ifdef _WIN32,
// BlueSysInfo.mm the Apple ones -- so Android gets a third file rather than a third branch.
// Everything cross-platform (GetProcessTimes, GetMemory, GetNetworkAdapters, GetPDMData,
// GetCpuInfo, GetOsInfo) stays in BlueSysInfo.cpp and is already built here.
//
// Only nine of the eleven methods were undefined at the link. GetSystemFontsDirectory and
// GetProcessStartTime are written anyway: implementing exactly today's undefined list is how
// the PDM backend ended up needing a second pass.
//
// Two things about Android shape most of the answers below. A native library has no
// Context, so every framework-managed path has to be derived from /proc rather than asked
// for; and the app sandbox means "shared" and "system" locations mostly do not exist.

#include "StdAfx.h"

#if defined( __ANDROID__ )

#include "BlueSysInfo.h"
#include <BlueExposure.h>
#include "pdm.h"

#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/system_properties.h>

static CcpLogChannel_t s_androidCh = CCP_LOG_DEFINE_CHANNEL( "BlueSysInfoAndroid" );

namespace
{
	const int64_t s_startTime = TimeNow();

	// The property API writes at most PROP_VALUE_MAX bytes including the terminator.
	std::string GetSystemProperty( const char* name )
	{
		char buffer[PROP_VALUE_MAX] = { 0 };
		int length = __system_property_get( name, buffer );
		return length > 0 ? std::string( buffer, static_cast<size_t>( length ) ) : std::string();
	}

	// The app's private data directory, derived rather than asked for: for the main process
	// /proc/self/cmdline is exactly the package name, and a `:name` suffix marks a secondary
	// process of the same package. /data/data/<package> is the user-0 symlink to it and is
	// what Context.getDataDir() returns.
	//
	// Returns empty rather than a guess when the directory is not there -- a plausible wrong
	// path is worse than no path, because the caller will create files under it.
	std::string GetAppDataDirectory()
	{
		FILE* file = fopen( "/proc/self/cmdline", "r" );
		if( !file )
		{
			return {};
		}

		char buffer[256] = { 0 };
		size_t read = fread( buffer, 1, sizeof( buffer ) - 1, file );
		fclose( file );
		if( read == 0 )
		{
			return {};
		}

		// cmdline is NUL-separated; argv[0] is all we want. Trim a secondary-process suffix.
		std::string package( buffer );
		auto colon = package.find( ':' );
		if( colon != std::string::npos )
		{
			package.erase( colon );
		}
		if( package.empty() )
		{
			return {};
		}

		std::string path = "/data/data/" + package;
		return access( path.c_str(), F_OK ) == 0 ? path : std::string();
	}

	std::wstring Widen( const std::string& value )
	{
		return value.empty() ? std::wstring() : UTF8ToWide( value );
	}
}


// The internal files directory, Context.getFilesDir(). The framework creates it with the app,
// so unlike the Documents path below it is always present.
std::wstring BlueSysInfo::GetUserApplicationDataDirectory() const
{
	auto base = GetAppDataDirectory();
	return base.empty() ? std::wstring() : Widen( base + "/files" );
}

// Android has no location shared between apps that is writable without a permission the user
// must grant, so this is the same directory as the per-user one. That is the same compromise
// the macOS body documents when it falls back from /Library/Application Support to the
// per-user copy -- here there is no other option at all rather than a privilege problem.
std::wstring BlueSysInfo::GetSharedApplicationDataDirectory() const
{
	return GetUserApplicationDataDirectory();
}

// App-scoped, not the user-visible /sdcard/Documents: that one is shared storage, needs
// READ_EXTERNAL_STORAGE below API 30 and MANAGE_EXTERNAL_STORAGE above it, and Play restricts
// the latter -- returning it would hand the caller a path it cannot open. Nothing on a modern
// Android is both app-private and user-visible (Android 11 closed Android/data to the Files
// app and to SAF), and app-private is the half that iOS's sandbox Documents keeps.
//
// This creates the directory, which is the one place these getters have a side effect. It is
// deliberate. Measured on an iOS 17 simulator: NSDocumentDirectory resolves to
// <container>/Documents, which exists and is writable because the OS creates it with the app
// container. Android's framework creates files/ but stops there, so without this the caller
// gets a path it must mkdir first -- and blue's own test_sysinfo.py asserts the directory
// exists and is writable. Android's own documents API has the same semantics:
// Context.getExternalFilesDir( DIRECTORY_DOCUMENTS ) creates what it returns.
std::wstring BlueSysInfo::GetUserDocumentsDirectory() const
{
	auto base = GetAppDataDirectory();
	if( base.empty() )
	{
		return {};
	}

	auto path = base + "/files/Documents";

	// 0700 because this is app-private storage; the app is the only uid that should reach it.
	// EEXIST is the normal case from the second call onwards. Anything else is worth a line in
	// the log but is not worth withholding the path: where documents belong is still the right
	// answer even when creating the directory failed, and the caller will find out when it
	// tries to write.
	if( mkdir( path.c_str(), 0700 ) != 0 && errno != EEXIST )
	{
		CCP_LOGERR_CH( s_androidCh, "could not create %s (errno %d)", path.c_str(), errno );
	}

	return Widen( path );
}

// Android keeps one font directory and it is read-only. macOS distinguishes /Library/Fonts
// from /System/Library/Fonts; there is no such split here, so both getters answer the same
// path and neither is writable.
std::wstring BlueSysInfo::GetSharedFontsDirectory() const
{
	return L"/system/fonts";
}

std::wstring BlueSysInfo::GetSystemFontsDirectory() const
{
	return L"/system/fonts";
}

uint32_t BlueSysInfo::GetProcessBitCount() const
{
#if __LP64__
	return 64;
#else
	return 32;
#endif
}

// Read rather than hardcoded, because a 32-bit process on a 64-bit device is a supported
// Android configuration and GetProcessBitCount above would then disagree with this. The
// property lists the 64-bit ABIs the device supports and is empty on 32-bit-only hardware.
uint32_t BlueSysInfo::GetSystemBitCount() const
{
	return GetSystemProperty( "ro.product.cpu.abilist64" ).empty() ? 32 : 64;
}

uint64_t BlueSysInfo::GetProcessStartTime() const
{
	return s_startTime;
}

// There is no device identifier available to native code, and this returns empty rather than
// substituting something weaker. ro.serialno became privileged in Android 10, ANDROID_ID
// needs a JNI round trip through Settings.Secure and is per-app-signing-key anyway, and
// neither is a hardware id in the sense macOS kIOPlatformUUIDKey is. The iOS branch of this
// same method does have a sanctioned substitute in identifierForVendor; Android has none.
// Matches what the pdm fork's Android backend reports for GetMachineUuidString.
std::string BlueSysInfo::GetMachineUuid() const
{
	return {};
}

// gethostname is what the Apple body uses, but on Android it answers "localhost" on
// essentially every device, which is useless as a machine name. ro.product.device is the
// device's codename and is the closest thing to one; the hostname stays as a fallback so this
// still says something on an emulator or an unusual image. Same order as the pdm backend.
std::wstring BlueSysInfo::GetMachineName() const
{
	auto device = GetSystemProperty( "ro.product.device" );
	if( !device.empty() )
	{
		return Widen( device );
	}

	char buffer[256] = { 0 };
	if( gethostname( buffer, sizeof( buffer ) - 1 ) != 0 )
	{
		return {};
	}
	if( auto dot = strchr( buffer, '.' ) )
	{
		*dot = 0;
	}
	return Widen( std::string( buffer ) );
}

// Android devices are not domain-joined and nothing in the platform models a domain, so there
// is no answer to give. The Apple body splits a DNS suffix out of the hostname; that hostname
// is "localhost" here, which would make this a constant lie rather than a constant blank.
std::wstring BlueSysInfo::GetDomainName() const
{
	return {};
}


// The Apple body reads machdep.cpu.* sysctls directly. bionic has no sysctlbyname and no
// equivalent nodes, but PDM's own CPUID layer already answers all of this from /proc/cpuinfo
// and the aarch64 ID registers -- so this asks PDM rather than parsing the same files again.
BlueSysInfoCpu::BlueSysInfoCpu() :
	m_extensions( PDM::GetCPUInfo().extensions )
{
	auto pdmCpu = PDM::GetCPUInfo();

	m_mHz = pdmCpu.frequency;
	m_brand = pdmCpu.brand;
	m_logicalCpuCount = pdmCpu.logicalCoreCount;
	m_bitCount = pdmCpu.bitness == PDM::Bitness::BITNESS_64 ? 64 : 32;
	m_revision = uint32_t( ( pdmCpu.model << 8 ) | pdmCpu.stepping );
	// ARM has no CPUID family register and PDM does not model one, so there is nothing to
	// report here. The Apple body reads machdep.cpu.family, which is an x86 concept that
	// survives on Apple silicon only as a synthetic value.
	m_family = 0;

	const char* platform;
#if __aarch64__
	m_architecture = "ARM64";
	platform = "ARM64";
#elif __arm__
	m_architecture = "ARM";
	platform = "ARM";
#elif __x86_64__
	m_architecture = "AMD64";
	platform = "AMD64";
#elif __i386__
	m_architecture = "x86";
	platform = "x86";
#else
#error "Unsupported architecture"
#endif

	char buffer[512] = { 0 };
	snprintf( buffer, sizeof( buffer ), "%s Family %i Model %i Stepping %i, %s",
	          platform, int( m_family ), int( pdmCpu.model ), int( pdmCpu.stepping ),
	          pdmCpu.vendor.c_str() );
	m_identifier = buffer;
}


BlueSysInfoOs::BlueSysInfoOs()
{
	m_platform = ANDROID_OS;

	// ro.build.version.release is the marketing version -- "14", "13.0", occasionally "4.4.4".
	// The SDK level stands in for a build number: it is the number every Android compatibility
	// decision is actually made against, and unlike ro.build.id it is an integer.
	auto release = GetSystemProperty( "ro.build.version.release" );
	auto dot = release.find( '.' );

	m_majorVersion = atoi( release.substr( 0, dot ).c_str() );
	m_minorVersion = dot == std::string::npos ? 0 : atoi( release.substr( dot + 1 ).c_str() );
	m_buildNumber = atoi( GetSystemProperty( "ro.build.version.sdk" ).c_str() );

	// Suite has no Android meaning; DESKTOP is what the Apple body reports for the same reason.
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
		// The Apple body assigns unconditionally and would publish whatever the struct came in
		// with. Zeroing is honest: it says the figure is unavailable rather than fabricating it.
		m_workingSet = 0;
		m_pageFile = 0;
	}

	// Better than the Apple body, which halves the total because it "could not find a way to
	// get it": Linux reports free pages directly, so m_availablePhysical is a real measurement
	// here rather than an estimate.
	long pageSize = sysconf( _SC_PAGE_SIZE );
	long totalPages = sysconf( _SC_PHYS_PAGES );
	long freePages = sysconf( _SC_AVPHYS_PAGES );

	m_totalPhysical = pageSize > 0 && totalPages > 0
		? uint64_t( totalPages ) * uint64_t( pageSize ) : 0;
	m_availablePhysical = pageSize > 0 && freePages > 0
		? uint64_t( freePages ) * uint64_t( pageSize ) : 0;
}

#endif
