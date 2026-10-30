/* Copyright © 2026 CCP ehf.
 *
 * Test of the Blue C API through blue_c/blue.h alone, in plain C11. Built with AddressSanitizer
 * and LeakSanitizer, so every value, error message and reference the test takes must be freed.
 *
 *   BlueCApiTest                 every case, ending with blue_shutdown
 *   BlueCApiTest init-failure    blue_initialize failing part-way (terminal)
 *   BlueCApiTest no-shutdown     exiting while blue still runs
 *
 * Blue starts once per process, hence one mode per process. Temporary directories go to
 * $TMPDIR (default /tmp).
 */

#define _POSIX_C_SOURCE 200809L

#include "blue_c/blue.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int s_failures = 0;

#define CHECK( cond ) \
	do \
	{ \
		if( !( cond ) ) \
		{ \
			fprintf( stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); \
			++s_failures; \
		} \
	} while( 0 )

/* expr is a call taking &e_ as its BlueCError*, expected to succeed */
#define CHECK_OK( expr ) \
	do \
	{ \
		BlueCError e_ = { BLUE_OK, NULL, 0 }; \
		int32_t s_ = ( expr ); \
		if( s_ != BLUE_OK ) \
		{ \
			fprintf( stderr, "FAIL %s:%d: %s -> %d (%s)\n", __FILE__, __LINE__, #expr, s_, e_.message ? e_.message : "" ); \
			++s_failures; \
		} \
		blue_error_clear( &e_ ); \
	} while( 0 )

/* expr is a call taking &err_, expected to fail with status and a message containing text */
#define CHECK_FAILS( expr, status, text ) \
	do \
	{ \
		BlueCError err_ = { BLUE_OK, NULL, 0 }; \
		int32_t s_ = ( expr ); \
		printf( "  %s -> %d (%s)\n", #expr, s_, err_.message ? err_.message : "" ); \
		if( s_ != ( status ) || err_.code != ( status ) || !err_.message || err_.message_len != strlen( err_.message ) || !strstr( err_.message, ( text ) ) ) \
		{ \
			fprintf( stderr, "FAIL %s:%d: %s: expected %d with \"%s\"\n", __FILE__, __LINE__, #expr, (int)( status ), ( text ) ); \
			++s_failures; \
		} \
		blue_error_clear( &err_ ); \
	} while( 0 )

#define NAME( literal ) literal, sizeof( literal ) - 1

static BlueCValue IntValue( int64_t v )
{
	BlueCValue value;
	memset( &value, 0, sizeof( value ) );
	value.kind = BLUE_VK_INT;
	value.u.i = v;
	return value;
}

static BlueCValue DoubleValue( double v )
{
	BlueCValue value;
	memset( &value, 0, sizeof( value ) );
	value.kind = BLUE_VK_DOUBLE;
	value.u.d = v;
	return value;
}

/* An integer attribute (INT or UINT, by its C++ type), or -12345 */
static int64_t GetInt( BlueObject* obj, const char* name )
{
	BlueCValue value;
	int64_t result = -12345;
	CHECK_OK( blue_get( obj, name, strlen( name ), &value, &e_ ) );
	if( value.kind == BLUE_VK_INT || value.kind == BLUE_VK_UINT )
	{
		result = value.u.i;
	}
	blue_value_release( &value );
	return result;
}

static void MakeTempDir( char* path, size_t size )
{
	const char* parent = getenv( "TMPDIR" );
	snprintf( path, size, "%s/blue-capi-test-XXXXXX", parent && *parent ? parent : "/tmp" );
	if( !mkdtemp( path ) )
	{
		perror( "mkdtemp" );
		exit( 2 );
	}
}

static BlueInitOptions Options( const char* appDir )
{
	BlueInitOptions options;
	memset( &options, 0, sizeof( options ) );
	options.struct_size = sizeof( options );
	options.app_dir = appDir;
	return options;
}

/*------------------------------------------------------------------------------------------*/

static void TestBeforeInitialize( void )
{
	printf( "before blue_initialize\n" );
	CHECK( blue_abi_version() == BLUE_CAPI_VERSION );
	CHECK( blue_abi_version() >> 16 == BLUE_CAPI_VERSION_MAJOR );
	CHECK( sizeof( BlueCValue ) == 40 );
	CHECK( blue_is_initialized() == 0 );

	CHECK_FAILS( blue_pump( &err_ ), BLUE_E_NOT_INITIALIZED, "blue_initialize" );
	CHECK( blue_pump( NULL ) == BLUE_E_NOT_INITIALIZED );
	CHECK( blue_class_count() == 0 );
	CHECK( blue_class_find( NAME( "MemStream" ) ) == NULL );
	CHECK( blue_module_object_count() == 0 );
	BlueObject* os = NULL;
	CHECK_FAILS( blue_module_object_get( NAME( "os" ), &os, &err_ ), BLUE_E_NOT_INITIALIZED, "not running" );
	CHECK( os == NULL );

	/* Bad options leave blue startable */
	char dir[] = "/nonexistent-blue-capi-test";
	BlueInitOptions options = Options( dir );
	CHECK_FAILS( blue_initialize( NULL, &err_ ), BLUE_E_INVALID_ARGUMENT, "options" );
	options.struct_size = 4;
	CHECK_FAILS( blue_initialize( &options, &err_ ), BLUE_E_INVALID_ARGUMENT, "struct_size" );
	/* Every 1.0 field is required */
	options.struct_size = (uint32_t)offsetof( BlueInitOptions, search_path_count );
	CHECK_FAILS( blue_initialize( &options, &err_ ), BLUE_E_INVALID_ARGUMENT, "struct_size" );
	options.struct_size = sizeof( options );
	CHECK_FAILS( blue_initialize( &options, &err_ ), BLUE_E_INVALID_ARGUMENT, "is not a directory" );
	options.app_dir = NULL;
	CHECK_FAILS( blue_initialize( &options, &err_ ), BLUE_E_INVALID_ARGUMENT, "app_dir" );
	options.app_dir = "/";
	options.arg_count = 1;
	CHECK_FAILS( blue_initialize( &options, &err_ ), BLUE_E_INVALID_ARGUMENT, "options.args" );
	options.arg_count = 0;
	const char* noKey[] = { "/somewhere" };
	options.search_paths = noKey;
	options.search_path_count = 1;
	CHECK_FAILS( blue_initialize( &options, &err_ ), BLUE_E_INVALID_ARGUMENT, "options.search_paths[0]" );
	CHECK( blue_is_initialized() == 0 );
}

static void* OffThread( void* arg )
{
	int32_t* results = (int32_t*)arg;
	BlueCError err = { BLUE_OK, NULL, 0 };
	results[0] = blue_pump( &err );
	results[1] = err.message && strstr( err.message, "main thread" ) ? 1 : 0;
	blue_error_clear( &err );
	results[2] = (int32_t)blue_class_count();
	results[3] = blue_is_initialized();
	return NULL;
}

static void TestInitialize( const char* appDir )
{
	printf( "blue_initialize\n" );
	const char* args[] = { "/resManThreadCount=2" };
	char searchPath[4200];
	snprintf( searchPath, sizeof( searchPath ), "res=%s", appDir );
	const char* searchPaths[] = { searchPath };
	/* As a host built against a later minor version passes it: a larger struct, whose fields
	 * this blue does not know */
	struct
	{
		BlueInitOptions options;
		uint64_t later;
	} newer;
	memset( &newer, 0, sizeof( newer ) );
	newer.options = Options( appDir );
	newer.options.struct_size = sizeof( newer );
	newer.later = 0xdeadbeef;
	BlueInitOptions* options = &newer.options;
	options->args = args;
	options->arg_count = 1;
	options->search_paths = searchPaths;
	options->search_path_count = 1;
	CHECK_OK( blue_initialize( options, &e_ ) );
	CHECK( blue_is_initialized() == 1 );
	CHECK_FAILS( blue_initialize( options, &err_ ), BLUE_E_FAILED, "already initialized" );

	/* Another thread is refused, except for the thread-free functions */
	pthread_t thread;
	int32_t results[4] = { 0, 0, -1, -1 };
	CHECK( pthread_create( &thread, NULL, OffThread, results ) == 0 );
	CHECK( pthread_join( thread, NULL ) == 0 );
	CHECK( results[0] == BLUE_E_WRONG_THREAD );
	CHECK( results[1] == 1 );
	CHECK( results[2] == 0 );
	CHECK( results[3] == 1 );
}

static void TestClasses( void )
{
	printf( "classes\n" );
	const size_t count = blue_class_count();
	printf( "  %zu classes\n", count );
	CHECK( count == BLUE_CAPI_TEST_CLASS_COUNT );
	CHECK( blue_class_at( count ) == NULL );

	size_t abstractCount = 0;
	for( size_t i = 0; i < count; ++i )
	{
		const BlueClass* cls = blue_class_at( i );
		size_t length = 0;
		const char* name = blue_class_name( cls, &length );
		CHECK( cls && name && length == strlen( name ) );
		if( !cls || !name )
		{
			continue;
		}
		/* Every listed class is found by its name, and no two share a handle */
		CHECK( blue_class_find( name, length ) == cls );
		for( size_t j = 0; j < i; ++j )
		{
			CHECK( blue_class_at( j ) != cls );
		}
		/* The parent chain ends */
		size_t depth = 0;
		for( const BlueClass* parent = blue_class_parent( cls ); parent && depth <= count; parent = blue_class_parent( parent ) )
		{
			++depth;
		}
		CHECK( depth <= count );

		/* Abstract classes and singletons are refused; everything else creates and releases */
		BlueObject* obj = NULL;
		BlueCError err = { BLUE_OK, NULL, 0 };
		const int32_t status = blue_create( cls, &obj, &err );
		if( blue_class_is_abstract( cls ) )
		{
			++abstractCount;
			CHECK( status == BLUE_E_UNSUPPORTED && obj == NULL && err.message && strstr( err.message, name ) );
		}
		else
		{
			if( status != BLUE_OK || !obj || blue_class_of( obj ) != cls )
			{
				fprintf( stderr, "FAIL: creating %s -> %d (%s)\n", name, status, err.message ? err.message : "" );
				++s_failures;
			}
			blue_release( obj );
		}
		blue_error_clear( &err );
	}
	printf( "  %zu cannot be created\n", abstractCount );
	CHECK( abstractCount > 0 && abstractCount < count );

	/* Names are counted, not NUL-terminated */
	const BlueClass* memStream = blue_class_find( "MemStreamXYZ", 9 );
	CHECK( memStream != NULL );
	size_t length = 0;
	CHECK( memStream && strcmp( blue_class_name( memStream, &length ), "MemStream" ) == 0 && length == 9 );
	CHECK( blue_class_find( NAME( "NoSuchClass" ) ) == NULL );
	CHECK( blue_class_find( NULL, 3 ) == NULL );
	CHECK( blue_class_is_abstract( NULL ) == 0 );
	CHECK( blue_class_name( NULL, &length ) == NULL );
	CHECK( blue_class_of( NULL ) == NULL );
	BlueObject* obj = NULL;
	CHECK_FAILS( blue_create( NULL, &obj, &err_ ), BLUE_E_INVALID_ARGUMENT, "blue_create" );
}

static void TestStreams( void )
{
	printf( "MemStream through IBlueStream's Read/Write/Seek\n" );
	const BlueClass* cls = blue_class_find( NAME( "MemStream" ) );
	BlueObject* stream = NULL;
	CHECK_OK( blue_create( cls, &stream, &e_ ) );
	CHECK( stream != NULL && blue_class_of( stream ) == cls );
	if( !stream )
	{
		return;
	}

	const BlueMethod* write = blue_method_find( cls, NAME( "Write" ) );
	const BlueMethod* read = blue_method_find( cls, NAME( "Read" ) );
	const BlueMethod* seek = blue_method_find( cls, NAME( "Seek" ) );
	CHECK( write && read && seek );
	if( !write || !read || !seek )
	{
		blue_release( stream );
		return;
	}
	/* The lowercase aliases exist too */
	CHECK( blue_method_find( cls, NAME( "write" ) ) && blue_method_find( cls, NAME( "read" ) ) && blue_method_find( cls, NAME( "seek" ) ) );
	CHECK( blue_method_find( cls, NAME( "NoSuchMethod" ) ) == NULL );
	size_t length = 0;
	CHECK( strcmp( blue_method_name( write, &length ), "Write" ) == 0 && length == 5 );

	CHECK( GetInt( stream, "size" ) == 0 );

	/* Binary data: an embedded NUL and a byte that is not UTF-8 */
	const char payload[] = { 'h', 'e', 0, 'l', 'l', (char)0xff, 'o' };
	BlueCValue data;
	CHECK_OK( blue_value_set_bytes( &data, payload, sizeof( payload ), &e_ ) );
	CHECK( data.kind == BLUE_VK_BYTES && data.u.str.len == sizeof( payload ) );
	CHECK_OK( blue_invoke( stream, write, &data, 1, NULL, &e_ ) );
	CHECK( GetInt( stream, "size" ) == 7 );
	CHECK( GetInt( stream, "pos" ) == 7 );

	/* Write rejects a size beyond the data, and text */
	BlueCValue tooLong[2] = { data, IntValue( 100 ) };
	CHECK_FAILS( blue_invoke( stream, write, tooLong, 2, NULL, &err_ ), BLUE_E_INVALID_ARGUMENT, "larger than" );
	BlueCValue text;
	CHECK_OK( blue_value_set_string( &text, NAME( "abc" ), &e_ ) );
	CHECK_FAILS( blue_invoke( stream, write, &text, 1, NULL, &err_ ), BLUE_E_TYPE, "expected bytes" );
	blue_value_release( &text );
	CHECK( text.kind == BLUE_VK_NONE );
	CHECK_FAILS( blue_invoke( stream, write, NULL, 0, NULL, &err_ ), BLUE_E_ARGUMENT_COUNT, "Write" );
	blue_value_release( &data );

	/* Reading at the end gives empty bytes */
	BlueCValue out;
	CHECK_OK( blue_invoke( stream, read, NULL, 0, &out, &e_ ) );
	CHECK( out.kind == BLUE_VK_BYTES && out.u.str.len == 0 );
	blue_value_release( &out );

	/* Seek to 2, read 3 */
	BlueCValue two = IntValue( 2 );
	CHECK_OK( blue_invoke( stream, seek, &two, 1, NULL, &e_ ) );
	CHECK( GetInt( stream, "pos" ) == 2 );
	BlueCValue three = IntValue( 3 );
	CHECK_OK( blue_invoke( stream, read, &three, 1, &out, &e_ ) );
	CHECK( out.kind == BLUE_VK_BYTES && out.u.str.len == 3 && memcmp( out.u.str.ptr, payload + 2, 3 ) == 0 );
	blue_value_release( &out );

	/* The default size, and any negative one, reads the rest */
	CHECK_OK( blue_invoke( stream, read, NULL, 0, &out, &e_ ) );
	CHECK( out.kind == BLUE_VK_BYTES && out.u.str.len == 2 && memcmp( out.u.str.ptr, payload + 5, 2 ) == 0 );
	blue_value_release( &out );
	BlueCValue begin[2] = { IntValue( 0 ), IntValue( 0 ) };
	CHECK_OK( blue_invoke( stream, seek, begin, 2, NULL, &e_ ) );
	BlueCValue minusOne = IntValue( -1 );
	CHECK_OK( blue_invoke( stream, read, &minusOne, 1, &out, &e_ ) );
	CHECK( out.kind == BLUE_VK_BYTES && out.u.str.len == 7 && memcmp( out.u.str.ptr, payload, 7 ) == 0 );
	blue_value_release( &out );

	/* A short read */
	CHECK_OK( blue_invoke( stream, seek, begin, 1, NULL, &e_ ) );
	BlueCValue many = IntValue( 1000 );
	CHECK_OK( blue_invoke( stream, read, &many, 1, &out, &e_ ) );
	CHECK( out.kind == BLUE_VK_BYTES && out.u.str.len == 7 && memcmp( out.u.str.ptr, payload, 7 ) == 0 );
	blue_value_release( &out );

	/* Seek origins as blue defines them: SO_CURRENT, and MemStream's SO_END is size - distance */
	BlueCValue back[2] = { IntValue( -6 ), IntValue( 1 ) };
	CHECK_OK( blue_invoke( stream, seek, back, 2, NULL, &e_ ) );
	CHECK( GetInt( stream, "pos" ) == 1 );
	BlueCValue fromEnd[2] = { IntValue( 3 ), IntValue( 2 ) };
	CHECK_OK( blue_invoke( stream, seek, fromEnd, 2, NULL, &e_ ) );
	CHECK( GetInt( stream, "pos" ) == 4 );
	BlueCValue badWhence[2] = { IntValue( 0 ), IntValue( 7 ) };
	CHECK_FAILS( blue_invoke( stream, seek, badWhence, 2, NULL, &err_ ), BLUE_E_INVALID_ARGUMENT, "whence" );

	/* The same methods through the IBlueStream interface pointer */
	BlueObject* asStream = NULL;
	CHECK_OK( blue_query_interface( stream, NAME( "IBlueStream" ), &asStream, &e_ ) );
	CHECK( asStream != NULL );
	CHECK_OK( blue_invoke( asStream, seek, begin, 1, NULL, &e_ ) );
	CHECK_OK( blue_invoke( asStream, read, &three, 1, &out, &e_ ) );
	CHECK( out.kind == BLUE_VK_BYTES && out.u.str.len == 3 && memcmp( out.u.str.ptr, payload, 3 ) == 0 );
	blue_value_release( &out );
	blue_release( asStream );

	/* A stream method on an object that is no stream */
	BlueObject* os = NULL;
	CHECK_OK( blue_module_object_get( NAME( "os" ), &os, &e_ ) );
	CHECK_FAILS( blue_invoke( os, read, NULL, 0, &out, &err_ ), BLUE_E_SELF, "IBlueStream" );
	blue_release( os );
	CHECK_FAILS( blue_invoke( NULL, read, NULL, 0, &out, &err_ ), BLUE_E_SELF, "" );

	blue_release( stream );
}

static void TestMembers( void )
{
	printf( "members and errors\n" );
	const BlueClass* cls = blue_class_find( NAME( "BlueTestHelperAttributes" ) );
	BlueObject* obj = NULL;
	CHECK_OK( blue_create( cls, &obj, &e_ ) );
	if( !obj )
	{
		return;
	}

	BlueCValue value = IntValue( 42 );
	CHECK_OK( blue_set( obj, NAME( "myInt" ), &value, &e_ ) );
	CHECK( GetInt( obj, "myInt" ) == 42 );

	/* Text round trip, with a non-ASCII character */
	const char utf8[] = "gr\xc3\xbc\xc3\x9f";
	CHECK_OK( blue_value_set_string( &value, NAME( utf8 ), &e_ ) );
	CHECK_OK( blue_set( obj, NAME( "myString" ), &value, &e_ ) );
	CHECK_OK( blue_set( obj, NAME( "myUnicode" ), &value, &e_ ) );
	blue_value_release( &value );
	CHECK_OK( blue_get( obj, NAME( "myString" ), &value, &e_ ) );
	CHECK( value.kind == BLUE_VK_STRING && value.u.str.len == strlen( utf8 ) && memcmp( value.u.str.ptr, utf8, value.u.str.len ) == 0 );
	blue_value_release( &value );
	CHECK_OK( blue_get( obj, NAME( "myUnicode" ), &value, &e_ ) );
	CHECK( value.kind == BLUE_VK_STRING && value.u.str.len == strlen( utf8 ) && memcmp( value.u.str.ptr, utf8, value.u.str.len ) == 0 );
	blue_value_release( &value );

	/* Wrong kind, out of range, unknown member */
	CHECK_OK( blue_value_set_string( &value, NAME( "x" ), &e_ ) );
	CHECK_FAILS( blue_set( obj, NAME( "myInt" ), &value, &err_ ), BLUE_E_TYPE, "myInt" );
	blue_value_release( &value );
	value = IntValue( (int64_t)1 << 40 );
	CHECK_FAILS( blue_set( obj, NAME( "myInt" ), &value, &err_ ), BLUE_E_OVERFLOW, "myInt" );
	value = DoubleValue( 1e300 );
	CHECK_FAILS( blue_set( obj, NAME( "myFloat" ), &value, &err_ ), BLUE_E_OVERFLOW, "myFloat" );
	CHECK( GetInt( obj, "myInt" ) == 42 );
	CHECK_FAILS( blue_get( obj, NAME( "nope" ), &value, &err_ ), BLUE_E_NOT_FOUND, "nope" );
	CHECK( value.kind == BLUE_VK_NONE );
	CHECK_FAILS( blue_get( NULL, NAME( "myInt" ), &value, &err_ ), BLUE_E_SELF, "" );
	blue_release( obj );

	/* A read-only attribute */
	BlueObject* stream = NULL;
	CHECK_OK( blue_create( blue_class_find( NAME( "MemStream" ) ), &stream, &e_ ) );
	value = IntValue( 3 );
	CHECK_FAILS( blue_set( stream, NAME( "size" ), &value, &err_ ), BLUE_E_READ_ONLY, "size" );

	/* Interfaces: every object answers to IRoot and its class name */
	BlueObject* other = NULL;
	CHECK_OK( blue_query_interface( stream, NAME( "IRoot" ), &other, &e_ ) );
	blue_release( other );
	other = NULL;
	CHECK_OK( blue_query_interface( stream, NAME( "MemStream" ), &other, &e_ ) );
	CHECK( other == stream );
	blue_release( other );
	other = NULL;
	CHECK_FAILS( blue_query_interface( stream, NAME( "IBluePaths" ), &other, &err_ ), BLUE_E_NOT_FOUND, "IBluePaths" );
	CHECK( other == NULL );

	/* Reference counting: an extra reference survives one release */
	blue_add_ref( stream );
	blue_release( stream );
	CHECK( GetInt( stream, "size" ) == 0 );
	blue_add_ref( NULL );
	blue_release( NULL );
	blue_release( stream );

	/* An error already in err is replaced (and its message freed), and kept on success */
	BlueCError err = { BLUE_OK, NULL, 0 };
	CHECK( blue_value_set_string( &value, "\xc3", 1, &err ) == BLUE_E_INVALID_ARGUMENT );
	CHECK( err.code == BLUE_E_INVALID_ARGUMENT && err.message && strstr( err.message, "UTF-8" ) );
	CHECK( blue_class_find( NAME( "NoSuchClass" ) ) == NULL );
	CHECK( blue_set_error_message( &err, BLUE_E_FAILED, NAME( "host failure" ) ) == BLUE_E_FAILED );
	CHECK( err.code == BLUE_E_FAILED && err.message && strcmp( err.message, "host failure" ) == 0 && err.message_len == 12 );
	BlueCValue bytes;
	CHECK( blue_value_set_bytes( &bytes, "", 0, &err ) == BLUE_OK );
	CHECK( err.code == BLUE_E_FAILED );
	blue_value_release( &bytes );
	blue_error_clear( &err );
	CHECK( err.code == BLUE_OK && err.message == NULL && err.message_len == 0 );
	blue_error_clear( NULL );
	blue_value_release( NULL );
}

static void TestModule( void )
{
	printf( "module objects, enums, functions\n" );
	const size_t objectCount = blue_module_object_count();
	printf( "  %zu module objects:", objectCount );
	int sawOs = 0, sawPaths = 0, sawResMan = 0;
	for( size_t i = 0; i < objectCount; ++i )
	{
		size_t length = 0;
		const char* name = blue_module_object_name( i, &length );
		CHECK( name && length == strlen( name ) );
		if( !name )
		{
			continue;
		}
		printf( " %s", name );
		sawOs |= strcmp( name, "os" ) == 0;
		sawPaths |= strcmp( name, "paths" ) == 0;
		sawResMan |= strcmp( name, "resMan" ) == 0;
		/* Every one is set once blue runs (paths only after blue_initialize) */
		BlueObject* obj = NULL;
		BlueCError err = { BLUE_OK, NULL, 0 };
		const int32_t status = blue_module_object_get( name, length, &obj, &err );
		if( status != BLUE_OK || !obj || !blue_class_of( obj ) )
		{
			fprintf( stderr, "\nFAIL: module object %s -> %d (%s)\n", name, status, err.message ? err.message : "" );
			++s_failures;
		}
		blue_error_clear( &err );
		blue_release( obj );
	}
	printf( "\n" );
	CHECK( sawOs && sawPaths && sawResMan );
	CHECK( blue_module_object_name( objectCount, NULL ) == NULL );
	BlueObject* obj = NULL;
	CHECK_FAILS( blue_module_object_get( NAME( "nope" ), &obj, &err_ ), BLUE_E_NOT_FOUND, "nope" );
	CHECK( obj == NULL );

	/* A method of a module object: os.Pump */
	BlueObject* os = NULL;
	CHECK_OK( blue_module_object_get( NAME( "os" ), &os, &e_ ) );
	CHECK( os && strcmp( blue_class_name( blue_class_of( os ), NULL ), "BlueOS" ) == 0 );
	const BlueMethod* pump = blue_method_find( blue_class_of( os ), NAME( "Pump" ) );
	CHECK( pump != NULL );
	CHECK_OK( blue_invoke( os, pump, NULL, 0, NULL, &e_ ) );
	blue_release( os );

	/* Enums */
	int sawLinux = 0, sawSuite = 0;
	for( size_t i = 0; i < blue_enum_count(); ++i )
	{
		const char* enumName = blue_enum_name( i, NULL );
		CHECK( enumName != NULL );
		printf( "  enum %s:", enumName ? enumName : "?" );
		const size_t valueCount = blue_enum_value_count( i );
		for( size_t j = 0; j < valueCount; ++j )
		{
			const char* name = NULL;
			size_t length = 0;
			int64_t value = -1;
			CHECK( blue_enum_value_at( i, j, &name, &length, &value ) == BLUE_OK );
			CHECK( name && length == strlen( name ) );
			printf( " %s=%lld", name ? name : "?", (long long)value );
			sawLinux |= enumName && name && strcmp( enumName, "OsPlatform" ) == 0 && strcmp( name, "LINUX" ) == 0 && value == 3;
		}
		printf( "\n" );
		sawSuite |= enumName && strcmp( enumName, "OsSuite" ) == 0 && valueCount == 3;
		CHECK( blue_enum_value_at( i, valueCount, NULL, NULL, NULL ) == BLUE_E_NOT_FOUND );
	}
	CHECK( sawLinux && sawSuite );
	CHECK( blue_enum_name( blue_enum_count(), NULL ) == NULL );
	CHECK( blue_enum_value_count( blue_enum_count() ) == 0 );

	/* Module functions */
	printf( "  functions:" );
	for( size_t i = 0; i < blue_function_count(); ++i )
	{
		const BlueFunction* fn = blue_function_at( i );
		const char* name = blue_function_name( fn, NULL );
		CHECK( name && blue_function_find( name, strlen( name ) ) == fn );
		printf( " %s", name ? name : "?" );
	}
	printf( "\n" );
	CHECK( blue_function_at( blue_function_count() ) == NULL );
	const BlueFunction* pdm = blue_function_find( NAME( "GetPDMByteData" ) );
	CHECK( pdm != NULL );
	CHECK_FAILS( blue_call( pdm, NULL, 3, NULL, &err_ ), BLUE_E_INVALID_ARGUMENT, "args is null" );
	CHECK( blue_function_find( NAME( "NoSuchFunction" ) ) == NULL );
}

static int s_callbackDestroyed = 0;

static int32_t HostCallback( void* userData, const BlueCValue* args, uint32_t argc, BlueCValue* out, BlueCError* err )
{
	(void)userData;
	(void)args;
	(void)argc;
	(void)out;
	(void)err;
	return BLUE_OK;
}

static void DestroyCallback( void* userData )
{
	++*(int*)userData;
}

static void TestValues( void )
{
	printf( "values and callbacks\n" );
	BlueCValue array;
	CHECK_OK( blue_value_set_array( &array, 2, &e_ ) );
	CHECK( array.kind == BLUE_VK_ARRAY && array.u.arr.count == 2 && array.u.arr.items[1].kind == BLUE_VK_NONE );
	if( array.kind == BLUE_VK_ARRAY )
	{
		CHECK_OK( blue_value_set_string( &array.u.arr.items[0], NAME( "item" ), &e_ ) );
		CHECK_OK( blue_value_set_map( &array.u.arr.items[1], 1, &e_ ) );
		CHECK_OK( blue_value_set_bytes( &array.u.arr.items[1].u.map.kv[0], NAME( "key" ), &e_ ) );
		array.u.arr.items[1].u.map.kv[1] = IntValue( 1 );
	}
	/* Frees the nested string, map and bytes */
	blue_value_release( &array );
	CHECK( array.kind == BLUE_VK_NONE );

	BlueCCallback callback = { HostCallback, &s_callbackDestroyed, DestroyCallback };
	BlueCallbackHandle* handle = blue_callback_create( &callback, NULL );
	CHECK( handle != NULL );
	BlueCValue value;
	memset( &value, 0, sizeof( value ) );
	value.kind = BLUE_VK_CALLBACK;
	value.u.callback = handle;
	CHECK( s_callbackDestroyed == 0 );
	/* Releasing the value drops the only reference */
	blue_value_release( &value );
	CHECK( s_callbackDestroyed == 1 );
	callback.fn = NULL;
	BlueCError err = { BLUE_OK, NULL, 0 };
	CHECK( blue_callback_create( &callback, &err ) == NULL && err.code != BLUE_OK );
	blue_error_clear( &err );
	blue_callback_release( NULL );
}

static void TestShutdown( const char* appDir )
{
	printf( "blue_pump and blue_shutdown\n" );
	for( int i = 0; i < 3; ++i )
	{
		CHECK_OK( blue_pump( &e_ ) );
	}
	/* An object held across shutdown may still be released */
	BlueObject* held = NULL;
	CHECK_OK( blue_create( blue_class_find( NAME( "MemStream" ) ), &held, &e_ ) );

	CHECK_OK( blue_shutdown( &e_ ) );
	CHECK( blue_is_initialized() == 0 );
	blue_release( held );
	CHECK_FAILS( blue_pump( &err_ ), BLUE_E_NOT_INITIALIZED, "blue_initialize" );
	CHECK_FAILS( blue_shutdown( &err_ ), BLUE_E_NOT_INITIALIZED, "" );
	CHECK( blue_class_count() == 0 );
	BlueInitOptions options = Options( appDir );
	CHECK_FAILS( blue_initialize( &options, &err_ ), BLUE_E_FAILED, "cannot be restarted" );
}

/*------------------------------------------------------------------------------------------*/

static int RunAll( void )
{
	char appDir[4096];
	MakeTempDir( appDir, sizeof( appDir ) );
	TestBeforeInitialize();
	TestInitialize( appDir );
	if( blue_is_initialized() )
	{
		TestClasses();
		TestStreams();
		TestMembers();
		TestModule();
		TestValues();
		TestShutdown( appDir );
	}
	rmdir( appDir );
	return s_failures;
}

/* A step of blue's startup fails after earlier steps ran: the call reports it, blue is torn
 * down again and stays down */
static int RunInitFailure( void )
{
	char appDir[4096];
	MakeTempDir( appDir, sizeof( appDir ) );
	printf( "blue_initialize failing part-way\n" );
	/* Passes the "<key>=<directory>" check, then blue refuses the circular reference */
	const char* searchPaths[] = { "loop=loop:x" };
	BlueInitOptions options = Options( appDir );
	options.search_paths = searchPaths;
	options.search_path_count = 1;
	CHECK_FAILS( blue_initialize( &options, &err_ ), BLUE_E_FAILED, "setting blue's search paths failed" );
	CHECK( blue_is_initialized() == 0 );
	CHECK( blue_class_count() == 0 );
	options.search_paths = NULL;
	options.search_path_count = 0;
	CHECK_FAILS( blue_initialize( &options, &err_ ), BLUE_E_FAILED, "cannot be restarted" );
	CHECK_FAILS( blue_pump( &err_ ), BLUE_E_NOT_INITIALIZED, "" );
	rmdir( appDir );
	return s_failures;
}

/* The host exits while blue still runs (worker threads, caches): no crash, no leak report */
static int RunNoShutdown( void )
{
	char appDir[4096];
	MakeTempDir( appDir, sizeof( appDir ) );
	printf( "exiting without blue_shutdown\n" );
	BlueInitOptions options = Options( appDir );
	CHECK_OK( blue_initialize( &options, &e_ ) );
	BlueObject* stream = NULL;
	CHECK_OK( blue_create( blue_class_find( NAME( "MemStream" ) ), &stream, &e_ ) );
	BlueCValue data;
	CHECK_OK( blue_value_set_bytes( &data, NAME( "data" ), &e_ ) );
	CHECK_OK( blue_invoke( stream, blue_method_find( blue_class_find( NAME( "MemStream" ) ), NAME( "Write" ) ), &data, 1, NULL, &e_ ) );
	blue_value_release( &data );
	CHECK_OK( blue_pump( &e_ ) );
	blue_release( stream );
	rmdir( appDir );
	return s_failures;
}

int main( int argc, char** argv )
{
	const char* mode = argc > 1 ? argv[1] : "all";
	int failures;
	if( strcmp( mode, "all" ) == 0 )
	{
		failures = RunAll();
	}
	else if( strcmp( mode, "init-failure" ) == 0 )
	{
		failures = RunInitFailure();
	}
	else if( strcmp( mode, "no-shutdown" ) == 0 )
	{
		failures = RunNoShutdown();
	}
	else
	{
		fprintf( stderr, "usage: %s [all|init-failure|no-shutdown]\n", argv[0] );
		return 2;
	}
	printf( failures ? "BlueCApiTest %s: %d FAILED\n" : "BlueCApiTest %s: OK\n", mode, failures );
	return failures ? 1 : 0;
}
