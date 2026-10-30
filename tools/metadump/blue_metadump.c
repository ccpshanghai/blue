/* Copyright © 2026 CCP ehf.
 *
 * blue-metadump: writes metadata.json, a language-neutral description of everything the Blue
 * C API exposes (classes, their members and methods, module functions, module objects, enums),
 * read through the C API's introspection functions (blue_c/blue.h alone, plain C11).
 *
 *   blue-metadump --out <file> [--app-dir <dir>] [--blue-commit <s>] [--blueexposure-commit <s>]
 *
 * Without --app-dir blue starts in a fresh directory under $TMPDIR (default /tmp), removed
 * afterwards. The commits are recorded as given (null when absent).
 *
 * The output is deterministic: every list is sorted by name in byte order, except enum values
 * (declaration order) and signature arguments; two-space indentation, one key per line, "\n"
 * line ends and a trailing newline. It holds no addresses, offsets or timestamps.
 *
 * Exit status: 0 on success, 1 when blue or the C API fails (the message goes to stderr and
 * no file is written), 2 for bad usage.
 */

#define _XOPEN_SOURCE 700

#include "blue_c/blue.h"

#include <ftw.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The JSON shape; bump it when a key is added, removed or changes meaning */
#define METADATA_SCHEMA_VERSION 1

static const char* s_program = "blue-metadump";

static int Fail( const char* format, ... )
{
	va_list args;
	va_start( args, format );
	fprintf( stderr, "%s: ", s_program );
	vfprintf( stderr, format, args );
	fputc( '\n', stderr );
	va_end( args );
	return 0;
}

/*--------------------------------------------------------------------------------------------
 * JSON writer: pretty-printed with two-space indentation, one value or key per line, empty
 * containers as [] / {}. Strings must be UTF-8; '"', '\' and control characters are escaped.
 *------------------------------------------------------------------------------------------*/

#define JSON_MAX_DEPTH 16

typedef struct JsonWriter
{
	FILE* file;
	int depth;
	size_t count[JSON_MAX_DEPTH];	/* values written at each open level */
	int ok;
} JsonWriter;

static int IsUtf8( const char* text, size_t len )
{
	const unsigned char* s = (const unsigned char*)text;
	size_t i = 0;
	while( i < len )
	{
		unsigned char c = s[i];
		size_t extra;
		uint32_t cp;
		if( c < 0x80 )
		{
			++i;
			continue;
		}
		if( c >= 0xC2 && c <= 0xDF )
		{
			extra = 1;
			cp = c & 0x1F;
		}
		else if( c >= 0xE0 && c <= 0xEF )
		{
			extra = 2;
			cp = c & 0x0F;
		}
		else if( c >= 0xF0 && c <= 0xF4 )
		{
			extra = 3;
			cp = c & 0x07;
		}
		else
		{
			return 0;
		}
		if( len - i <= extra )
		{
			return 0;
		}
		for( size_t k = 1; k <= extra; ++k )
		{
			if( ( s[i + k] & 0xC0 ) != 0x80 )
			{
				return 0;
			}
			cp = ( cp << 6 ) | ( s[i + k] & 0x3F );
		}
		/* overlong forms, surrogates, beyond U+10FFFF */
		if( ( extra == 2 && cp < 0x800 ) || ( extra == 3 && ( cp < 0x10000 || cp > 0x10FFFF ) ) || ( cp >= 0xD800 && cp <= 0xDFFF ) )
		{
			return 0;
		}
		i += extra + 1;
	}
	return 1;
}

static void JsonIndent( JsonWriter* w, int depth )
{
	for( int i = 0; i < depth; ++i )
	{
		fputs( "  ", w->file );
	}
}

static void JsonRawString( JsonWriter* w, const char* text, size_t len )
{
	if( !IsUtf8( text, len ) )
	{
		w->ok = Fail( "a string from blue is not UTF-8: \"%.*s\"", (int)len, text );
		return;
	}
	fputc( '"', w->file );
	for( size_t i = 0; i < len; ++i )
	{
		unsigned char c = (unsigned char)text[i];
		switch( c )
		{
		case '"':
			fputs( "\\\"", w->file );
			break;
		case '\\':
			fputs( "\\\\", w->file );
			break;
		case '\b':
			fputs( "\\b", w->file );
			break;
		case '\f':
			fputs( "\\f", w->file );
			break;
		case '\n':
			fputs( "\\n", w->file );
			break;
		case '\r':
			fputs( "\\r", w->file );
			break;
		case '\t':
			fputs( "\\t", w->file );
			break;
		default:
			if( c < 0x20 )
			{
				fprintf( w->file, "\\u%04x", (unsigned)c );
			}
			else
			{
				fputc( c, w->file );
			}
			break;
		}
	}
	fputc( '"', w->file );
}

/* Start a value inside the current container: separator, new line, indentation, "key": */
static void JsonBeginValue( JsonWriter* w, const char* key )
{
	if( w->depth > 0 )
	{
		fputs( w->count[w->depth - 1]++ ? ",\n" : "\n", w->file );
		JsonIndent( w, w->depth );
	}
	if( key )
	{
		JsonRawString( w, key, strlen( key ) );
		fputs( ": ", w->file );
	}
}

static void JsonOpen( JsonWriter* w, const char* key, char bracket )
{
	JsonBeginValue( w, key );
	fputc( bracket, w->file );
	if( w->depth >= JSON_MAX_DEPTH )
	{
		w->ok = Fail( "JSON nesting too deep" );
		return;
	}
	w->count[w->depth++] = 0;
}

static void JsonClose( JsonWriter* w, char bracket )
{
	--w->depth;
	if( w->count[w->depth] )
	{
		fputc( '\n', w->file );
		JsonIndent( w, w->depth );
	}
	fputc( bracket, w->file );
}

static void JsonBeginObject( JsonWriter* w, const char* key )
{
	JsonOpen( w, key, '{' );
}

static void JsonEndObject( JsonWriter* w )
{
	JsonClose( w, '}' );
}

static void JsonBeginArray( JsonWriter* w, const char* key )
{
	JsonOpen( w, key, '[' );
}

static void JsonEndArray( JsonWriter* w )
{
	JsonClose( w, ']' );
}

/* A string value; NULL writes null */
static void JsonStringN( JsonWriter* w, const char* key, const char* text, size_t len )
{
	JsonBeginValue( w, key );
	if( text )
	{
		JsonRawString( w, text, len );
	}
	else
	{
		fputs( "null", w->file );
	}
}

static void JsonString( JsonWriter* w, const char* key, const char* text )
{
	JsonStringN( w, key, text, text ? strlen( text ) : 0 );
}

static void JsonInt( JsonWriter* w, const char* key, int64_t value )
{
	JsonBeginValue( w, key );
	fprintf( w->file, "%" PRId64, value );
}

static void JsonBool( JsonWriter* w, const char* key, int value )
{
	JsonBeginValue( w, key );
	fputs( value ? "true" : "false", w->file );
}

/*--------------------------------------------------------------------------------------------
 * Sorting: by name in byte order, ties (none expected) by original index so the order never
 * depends on qsort
 *------------------------------------------------------------------------------------------*/

typedef struct Named
{
	const char* name;
	size_t len;
	size_t index;	/* index in blue's own enumeration */
	const void* handle;
} Named;

static int CompareNamed( const void* a, const void* b )
{
	const Named* x = (const Named*)a;
	const Named* y = (const Named*)b;
	size_t shorter = x->len < y->len ? x->len : y->len;
	int order = memcmp( x->name, y->name, shorter );
	if( order != 0 )
	{
		return order;
	}
	if( x->len != y->len )
	{
		return x->len < y->len ? -1 : 1;
	}
	return x->index < y->index ? -1 : ( x->index > y->index ? 1 : 0 );
}

static Named* NewNamedArray( size_t count )
{
	Named* items = (Named*)calloc( count ? count : 1, sizeof( Named ) );
	if( !items )
	{
		Fail( "out of memory" );
	}
	return items;
}

static void SortNamed( Named* items, size_t count )
{
	if( count > 1 )
	{
		qsort( items, count, sizeof( Named ), CompareNamed );
	}
}

/*--------------------------------------------------------------------------------------------
 * Walk
 *------------------------------------------------------------------------------------------*/

static int IsUnknownType( const char* type )
{
	return strncmp( type, "unknown:", 8 ) == 0;
}

/* "returns", "args", "optional" and "callable" of a method or function */
static int WriteSignature( JsonWriter* w, const BlueSignatureInfo* signature, const char* owner )
{
	if( !signature->returns || ( signature->arg_count && !signature->args ) )
	{
		return Fail( "%s: incomplete signature", owner );
	}
	int callable = !IsUnknownType( signature->returns );
	JsonString( w, "returns", signature->returns );
	JsonBeginArray( w, "args" );
	for( uint32_t i = 0; i < signature->arg_count; ++i )
	{
		if( !signature->args[i] )
		{
			return Fail( "%s: argument %u has no type", owner, (unsigned)i );
		}
		callable = callable && !IsUnknownType( signature->args[i] );
		JsonString( w, NULL, signature->args[i] );
	}
	JsonEndArray( w );
	JsonInt( w, "optional", signature->optional_count );
	JsonBool( w, "callable", callable );
	return 1;
}

static const char* KindName( int32_t kind )
{
	switch( kind )
	{
	case BLUE_CLASS_CREATABLE:
		return "creatable";
	case BLUE_CLASS_ABSTRACT:
		return "abstract";
	case BLUE_CLASS_SINGLETON:
		return "singleton";
	default:
		return NULL;
	}
}

static int WriteInterfaces( JsonWriter* w, const BlueClass* cls, const char* className )
{
	size_t count = blue_class_interface_count( cls );
	Named* items = NewNamedArray( count );
	if( !items )
	{
		return 0;
	}
	int ok = 1;
	for( size_t i = 0; i < count && ok; ++i )
	{
		items[i].index = i;
		items[i].name = blue_class_interface_at( cls, i, &items[i].len );
		if( !items[i].name )
		{
			ok = Fail( "%s: blue_class_interface_at( %zu ) failed", className, i );
		}
	}
	if( ok )
	{
		SortNamed( items, count );
		JsonBeginArray( w, "interfaces" );
		for( size_t i = 0; i < count; ++i )
		{
			JsonStringN( w, NULL, items[i].name, items[i].len );
		}
		JsonEndArray( w );
	}
	free( items );
	return ok;
}

static int WriteMembers( JsonWriter* w, const BlueClass* cls, const char* className )
{
	size_t count = blue_class_member_count( cls );
	BlueMemberInfo* infos = (BlueMemberInfo*)calloc( count ? count : 1, sizeof( BlueMemberInfo ) );
	Named* items = NewNamedArray( count );
	int ok = infos && items;
	if( !infos )
	{
		Fail( "out of memory" );
	}
	for( size_t i = 0; i < count && ok; ++i )
	{
		infos[i].struct_size = sizeof( BlueMemberInfo );
		int32_t status = blue_class_member_at( cls, i, &infos[i] );
		if( status != BLUE_OK || !infos[i].name || !infos[i].type || !infos[i].doc )
		{
			ok = Fail( "%s: blue_class_member_at( %zu ) -> %d", className, i, (int)status );
			break;
		}
		items[i].index = i;
		items[i].name = infos[i].name;
		items[i].len = strlen( infos[i].name );
		items[i].handle = &infos[i];
	}
	if( ok )
	{
		SortNamed( items, count );
		JsonBeginArray( w, "members" );
		for( size_t i = 0; i < count && ok; ++i )
		{
			const BlueMemberInfo* info = (const BlueMemberInfo*)items[i].handle;
			const char* access;
			if( ( info->flags & ( BLUE_MEMBER_READABLE | BLUE_MEMBER_WRITABLE ) ) == ( BLUE_MEMBER_READABLE | BLUE_MEMBER_WRITABLE ) )
			{
				access = "readwrite";
			}
			else if( info->flags & BLUE_MEMBER_READABLE )
			{
				access = "read";
			}
			else
			{
				/* schema 1 knows readable members only */
				ok = Fail( "%s.%s: member flags 0x%x have no access in schema %d", className, info->name, (unsigned)info->flags, METADATA_SCHEMA_VERSION );
				break;
			}
			JsonBeginObject( w, NULL );
			JsonString( w, "name", info->name );
			JsonString( w, "type", info->type );
			JsonString( w, "access", access );
			JsonBool( w, "property", ( info->flags & BLUE_MEMBER_PROPERTY ) != 0 );
			JsonString( w, "doc", info->doc );
			JsonEndObject( w );
		}
		JsonEndArray( w );
	}
	free( items );
	free( infos );
	return ok;
}

static int WriteMethods( JsonWriter* w, const BlueClass* cls, const char* className )
{
	size_t count = blue_class_method_count( cls );
	Named* items = NewNamedArray( count );
	if( !items )
	{
		return 0;
	}
	int ok = 1;
	for( size_t i = 0; i < count && ok; ++i )
	{
		const BlueMethod* method = blue_class_method_at( cls, i );
		items[i].index = i;
		items[i].handle = method;
		items[i].name = method ? blue_method_name( method, &items[i].len ) : NULL;
		if( !items[i].name )
		{
			ok = Fail( "%s: blue_class_method_at( %zu ) failed", className, i );
		}
	}
	if( ok )
	{
		SortNamed( items, count );
		JsonBeginArray( w, "methods" );
		for( size_t i = 0; i < count && ok; ++i )
		{
			const BlueMethod* method = (const BlueMethod*)items[i].handle;
			char owner[512];
			snprintf( owner, sizeof( owner ), "%s.%.*s", className, (int)items[i].len, items[i].name );
			size_t docLen = 0;
			const char* doc = blue_method_doc( method, &docLen );
			BlueSignatureInfo signature;
			memset( &signature, 0, sizeof( signature ) );
			signature.struct_size = sizeof( signature );
			int32_t status = blue_method_signature( method, &signature );
			if( !doc || status != BLUE_OK )
			{
				ok = Fail( "%s: blue_method_doc / blue_method_signature -> %d", owner, (int)status );
				break;
			}
			JsonBeginObject( w, NULL );
			JsonStringN( w, "name", items[i].name, items[i].len );
			JsonStringN( w, "doc", doc, docLen );
			ok = WriteSignature( w, &signature, owner );
			JsonEndObject( w );
		}
		JsonEndArray( w );
	}
	free( items );
	return ok;
}

static int WriteClass( JsonWriter* w, const BlueClass* cls, const char* name, size_t nameLen )
{
	char className[256];
	snprintf( className, sizeof( className ), "%.*s", (int)nameLen, name );

	size_t iidLen = 0, docLen = 0, parentLen = 0;
	const char* iid = blue_class_iid( cls, &iidLen );
	const char* doc = blue_class_doc( cls, &docLen );
	const char* kind = KindName( blue_class_kind( cls ) );
	const BlueClass* parent = blue_class_parent( cls );
	const char* parentName = parent ? blue_class_name( parent, &parentLen ) : NULL;
	if( !iid || !doc || !kind || ( parent && !parentName ) )
	{
		return Fail( "%s: blue_class_iid / blue_class_doc / blue_class_kind / blue_class_parent failed", className );
	}

	JsonBeginObject( w, NULL );
	JsonStringN( w, "name", name, nameLen );
	JsonStringN( w, "iid", iid, iidLen );
	JsonString( w, "kind", kind );
	JsonStringN( w, "parent", parentName, parentLen );
	JsonStringN( w, "doc", doc, docLen );
	int ok = WriteInterfaces( w, cls, className ) && WriteMembers( w, cls, className ) && WriteMethods( w, cls, className );
	JsonEndObject( w );
	return ok;
}

static int WriteClasses( JsonWriter* w )
{
	size_t count = blue_class_count();
	if( count == 0 )
	{
		return Fail( "blue_class_count() is 0" );
	}
	Named* items = NewNamedArray( count );
	if( !items )
	{
		return 0;
	}
	int ok = 1;
	for( size_t i = 0; i < count && ok; ++i )
	{
		const BlueClass* cls = blue_class_at( i );
		items[i].index = i;
		items[i].handle = cls;
		items[i].name = cls ? blue_class_name( cls, &items[i].len ) : NULL;
		if( !items[i].name )
		{
			ok = Fail( "blue_class_at( %zu ) failed", i );
		}
	}
	if( ok )
	{
		SortNamed( items, count );
		JsonBeginArray( w, "classes" );
		for( size_t i = 0; i < count && ok; ++i )
		{
			ok = WriteClass( w, (const BlueClass*)items[i].handle, items[i].name, items[i].len );
		}
		JsonEndArray( w );
	}
	free( items );
	return ok;
}

static int WriteFunctions( JsonWriter* w )
{
	size_t count = blue_function_count();
	Named* items = NewNamedArray( count );
	if( !items )
	{
		return 0;
	}
	int ok = 1;
	for( size_t i = 0; i < count && ok; ++i )
	{
		const BlueFunction* fn = blue_function_at( i );
		items[i].index = i;
		items[i].handle = fn;
		items[i].name = fn ? blue_function_name( fn, &items[i].len ) : NULL;
		if( !items[i].name )
		{
			ok = Fail( "blue_function_at( %zu ) failed", i );
		}
	}
	if( ok )
	{
		SortNamed( items, count );
		JsonBeginArray( w, "functions" );
		for( size_t i = 0; i < count && ok; ++i )
		{
			const BlueFunction* fn = (const BlueFunction*)items[i].handle;
			char owner[256];
			snprintf( owner, sizeof( owner ), "%.*s", (int)items[i].len, items[i].name );
			size_t docLen = 0;
			const char* doc = blue_function_doc( fn, &docLen );
			BlueSignatureInfo signature;
			memset( &signature, 0, sizeof( signature ) );
			signature.struct_size = sizeof( signature );
			int32_t status = blue_function_signature( fn, &signature );
			if( !doc || status != BLUE_OK )
			{
				ok = Fail( "%s: blue_function_doc / blue_function_signature -> %d", owner, (int)status );
				break;
			}
			JsonBeginObject( w, NULL );
			JsonStringN( w, "name", items[i].name, items[i].len );
			JsonStringN( w, "doc", doc, docLen );
			ok = WriteSignature( w, &signature, owner );
			JsonEndObject( w );
		}
		JsonEndArray( w );
	}
	free( items );
	return ok;
}

/* Only each object's class is read, never a member value (some getters block, such as
 * BlueNetworkStream.size) */
static int WriteModuleObjects( JsonWriter* w )
{
	size_t count = blue_module_object_count();
	Named* items = NewNamedArray( count );
	if( !items )
	{
		return 0;
	}
	int ok = 1;
	for( size_t i = 0; i < count && ok; ++i )
	{
		items[i].index = i;
		items[i].name = blue_module_object_name( i, &items[i].len );
		if( !items[i].name )
		{
			ok = Fail( "blue_module_object_name( %zu ) failed", i );
		}
	}
	if( ok )
	{
		SortNamed( items, count );
		JsonBeginArray( w, "moduleObjects" );
		for( size_t i = 0; i < count && ok; ++i )
		{
			BlueObject* obj = NULL;
			BlueCError err = { BLUE_OK, NULL, 0 };
			int32_t status = blue_module_object_get( items[i].name, items[i].len, &obj, &err );
			const BlueClass* cls = status == BLUE_OK ? blue_class_of( obj ) : NULL;
			size_t classLen = 0;
			const char* className = cls ? blue_class_name( cls, &classLen ) : NULL;
			if( !className )
			{
				ok = Fail( "module object %.*s: blue_module_object_get -> %d (%s), or it has no class", (int)items[i].len, items[i].name, (int)status, err.message ? err.message : "" );
			}
			else
			{
				JsonBeginObject( w, NULL );
				JsonStringN( w, "name", items[i].name, items[i].len );
				JsonStringN( w, "class", className, classLen );
				JsonEndObject( w );
			}
			blue_error_clear( &err );
			blue_release( obj );
		}
		JsonEndArray( w );
	}
	free( items );
	return ok;
}

static int WriteEnums( JsonWriter* w )
{
	size_t count = blue_enum_count();
	Named* items = NewNamedArray( count );
	if( !items )
	{
		return 0;
	}
	int ok = 1;
	for( size_t i = 0; i < count && ok; ++i )
	{
		items[i].index = i;
		items[i].name = blue_enum_name( i, &items[i].len );
		if( !items[i].name )
		{
			ok = Fail( "blue_enum_name( %zu ) failed", i );
		}
	}
	if( ok )
	{
		SortNamed( items, count );
		JsonBeginArray( w, "enums" );
		for( size_t i = 0; i < count && ok; ++i )
		{
			size_t index = items[i].index;
			JsonBeginObject( w, NULL );
			JsonStringN( w, "name", items[i].name, items[i].len );
			/* declaration order: it is the enum's own */
			JsonBeginArray( w, "values" );
			size_t valueCount = blue_enum_value_count( index );
			for( size_t j = 0; j < valueCount && ok; ++j )
			{
				const char* name = NULL;
				size_t nameLen = 0, docLen = 0;
				int64_t value = 0;
				int32_t status = blue_enum_value_at( index, j, &name, &nameLen, &value );
				const char* doc = blue_enum_value_doc( index, j, &docLen );
				if( status != BLUE_OK || !name || !doc )
				{
					ok = Fail( "enum %.*s: value %zu -> %d", (int)items[i].len, items[i].name, j, (int)status );
					break;
				}
				JsonBeginObject( w, NULL );
				JsonStringN( w, "name", name, nameLen );
				JsonInt( w, "value", value );
				JsonStringN( w, "doc", doc, docLen );
				JsonEndObject( w );
			}
			JsonEndArray( w );
			JsonEndObject( w );
		}
		JsonEndArray( w );
	}
	free( items );
	return ok;
}

static int WriteMetadata( FILE* file, const char* blueCommit, const char* blueexposureCommit )
{
	uint32_t version = blue_abi_version();
	char abiVersion[32];
	snprintf( abiVersion, sizeof( abiVersion ), "%u.%u", (unsigned)( version >> 16 ), (unsigned)( version & 0xFFFF ) );

	JsonWriter w;
	memset( &w, 0, sizeof( w ) );
	w.file = file;
	w.ok = 1;
	JsonBeginObject( &w, NULL );
	JsonInt( &w, "schemaVersion", METADATA_SCHEMA_VERSION );
	JsonString( &w, "abiVersion", abiVersion );
	JsonString( &w, "blueCommit", blueCommit );
	JsonString( &w, "blueexposureCommit", blueexposureCommit );
	int ok = WriteClasses( &w ) && WriteFunctions( &w ) && WriteModuleObjects( &w ) && WriteEnums( &w );
	JsonEndObject( &w );
	fputc( '\n', file );
	return ok && w.ok;
}

/*--------------------------------------------------------------------------------------------
 * Main
 *------------------------------------------------------------------------------------------*/

static int RemoveEntry( const char* path, const struct stat* sb, int type, struct FTW* ftw )
{
	(void)sb;
	(void)type;
	(void)ftw;
	if( remove( path ) != 0 )
	{
		perror( path );
	}
	return 0;
}

static int Usage( void )
{
	fprintf( stderr, "usage: %s --out <file> [--app-dir <dir>] [--blue-commit <s>] [--blueexposure-commit <s>]\n", s_program );
	return 2;
}

int main( int argc, char** argv )
{
	const char* out = NULL;
	const char* appDir = NULL;
	const char* blueCommit = NULL;
	const char* blueexposureCommit = NULL;
	for( int i = 1; i < argc; ++i )
	{
		const char** target = NULL;
		if( strcmp( argv[i], "--out" ) == 0 )
		{
			target = &out;
		}
		else if( strcmp( argv[i], "--app-dir" ) == 0 )
		{
			target = &appDir;
		}
		else if( strcmp( argv[i], "--blue-commit" ) == 0 )
		{
			target = &blueCommit;
		}
		else if( strcmp( argv[i], "--blueexposure-commit" ) == 0 )
		{
			target = &blueexposureCommit;
		}
		if( !target || i + 1 >= argc )
		{
			fprintf( stderr, "%s: bad argument: %s\n", s_program, argv[i] );
			return Usage();
		}
		*target = argv[++i];
	}
	if( !out || !*out )
	{
		return Usage();
	}

	uint32_t version = blue_abi_version();
	if( ( version >> 16 ) != BLUE_CAPI_VERSION_MAJOR || ( version & 0xFFFF ) < BLUE_CAPI_VERSION_MINOR )
	{
		Fail( "blue's C API is %u.%u; this tool needs %d.%d or a later minor version", (unsigned)( version >> 16 ), (unsigned)( version & 0xFFFF ), BLUE_CAPI_VERSION_MAJOR, BLUE_CAPI_VERSION_MINOR );
		return 1;
	}

	char tempDir[4096] = "";
	if( !appDir )
	{
		const char* parent = getenv( "TMPDIR" );
		snprintf( tempDir, sizeof( tempDir ), "%s/blue-metadump-XXXXXX", parent && *parent ? parent : "/tmp" );
		if( !mkdtemp( tempDir ) )
		{
			perror( "mkdtemp" );
			return 1;
		}
		appDir = tempDir;
	}

	/* Written next to out and renamed over it only when everything succeeded */
	char partial[4096];
	snprintf( partial, sizeof( partial ), "%s.partial", out );

	int ok = 1;
	BlueInitOptions options;
	memset( &options, 0, sizeof( options ) );
	options.struct_size = sizeof( options );
	options.app_dir = appDir;
	BlueCError err = { BLUE_OK, NULL, 0 };
	int32_t status = blue_initialize( &options, &err );
	if( status != BLUE_OK )
	{
		ok = Fail( "blue_initialize -> %d (%s)", (int)status, err.message ? err.message : "" );
		blue_error_clear( &err );
	}
	else
	{
		FILE* file = fopen( partial, "wb" );
		if( !file )
		{
			perror( partial );
			ok = 0;
		}
		else
		{
			ok = WriteMetadata( file, blueCommit, blueexposureCommit );
			if( ferror( file ) )
			{
				ok = Fail( "writing %s failed", partial );
			}
			if( fclose( file ) != 0 )
			{
				perror( partial );
				ok = 0;
			}
		}
		status = blue_shutdown( &err );
		if( status != BLUE_OK )
		{
			ok = Fail( "blue_shutdown -> %d (%s)", (int)status, err.message ? err.message : "" );
			blue_error_clear( &err );
		}
	}

	if( ok && rename( partial, out ) != 0 )
	{
		perror( out );
		ok = 0;
	}
	if( !ok )
	{
		remove( partial );
	}
	if( tempDir[0] )
	{
		nftw( tempDir, RemoveEntry, 16, FTW_DEPTH | FTW_PHYS );
	}
	return ok ? 0 : 1;
}
