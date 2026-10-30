// Copyright © 2026 CCP ehf.
//
// Blue C API: the value, callback and error helpers of the C-ABI backend (BlueCAbiValue.h),
// re-exported for hosts. They work on any thread, before and after blue runs.

#include "StdAfx.h"

#if BLUE_WITH_CABI

#include "BlueCApiInternal.h"

using namespace BlueCApi;

void blue_value_release( BlueCValue* value )
{
	if( value )
	{
		BlueCValueRelease( value );
	}
}

int32_t blue_value_set_string( BlueCValue* out, const char* utf8, size_t len, BlueCError* err )
{
	return Call( err, [&]( BlueCError& error ) { return BlueCValueSetString( out, utf8, len, &error ); }, false );
}

int32_t blue_value_set_bytes( BlueCValue* out, const void* data, size_t len, BlueCError* err )
{
	return Call( err, [&]( BlueCError& error ) { return BlueCValueSetBytes( out, data, len, &error ); }, false );
}

int32_t blue_value_set_array( BlueCValue* out, size_t count, BlueCError* err )
{
	return Call( err, [&]( BlueCError& error ) { return BlueCValueSetArray( out, count, &error ); }, false );
}

int32_t blue_value_set_map( BlueCValue* out, size_t pair_count, BlueCError* err )
{
	return Call( err, [&]( BlueCError& error ) { return BlueCValueSetMap( out, pair_count, &error ); }, false );
}

BlueCallbackHandle* blue_callback_create( const BlueCCallback* callback, BlueCError* err )
{
	BlueCallbackHandle* handle = nullptr;
	Call( err, [&]( BlueCError& error ) -> int32_t {
		handle = BlueCCallbackCreate( callback, &error );
		return handle ? BLUE_OK : ( error.code != BLUE_OK ? error.code : BLUE_E_FAILED );
	}, false );
	return handle;
}

void blue_callback_release( BlueCallbackHandle* handle )
{
	if( handle )
	{
		BlueCCallbackRelease( handle );
	}
}

void blue_error_clear( BlueCError* err )
{
	if( err )
	{
		BlueCErrorClear( err );
	}
}

int32_t blue_set_error_message( BlueCError* err, int32_t code, const char* message, size_t len )
{
	return BlueCSetErrorMessage( err, code, message, len );
}

#endif
