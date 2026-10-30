// Copyright © 2026 CCP ehf.
//
// Blue C API: module objects (BLUE_REGISTER_GLOBAL_AS_MODULE_OBJECT) and enums
// (BLUE_REGISTER_ENUM*), read from blue's registries the way Python's module init does.

#include "StdAfx.h"

#if BLUE_WITH_CABI

#include "BlueCApiInternal.h"

#include <iterator>

using namespace BlueCApi;

namespace
{

const EnumRegsMap::value_type* EnumAt( size_t index )
{
	const EnumRegsMap& enums = BlueRegistration::GetEnumRegs();
	if( index >= enums.size() )
	{
		return nullptr;
	}
	return &*std::next( enums.begin(), ptrdiff_t( index ) );
}

} // namespace


//--------------------------------------------------------------------
// Module objects
//--------------------------------------------------------------------

size_t blue_module_object_count( void )
{
	return IsCallerOk() ? BlueRegistration::GetObjectRegs().size() : 0;
}

const char* blue_module_object_name( size_t index, size_t* len )
{
	const char* name = nullptr;
	if( IsCallerOk() )
	{
		const ObjectRegsVector& objects = BlueRegistration::GetObjectRegs();
		if( index < objects.size() )
		{
			name = objects[index].first;
		}
	}
	return NameWithLength( name, len );
}

int32_t blue_module_object_get( const char* name, size_t name_len, BlueObject** out, BlueCError* err )
{
	if( out )
	{
		*out = nullptr;
	}
	return Call( err, [&]( BlueCError& error ) -> int32_t {
		if( !name || !out )
		{
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "blue_module_object_get needs a name and an out pointer" );
		}
		for( const auto& reg : BlueRegistration::GetObjectRegs() )
		{
			if( !reg.first || strlen( reg.first ) != name_len || memcmp( reg.first, name, name_len ) != 0 )
			{
				continue;
			}
			// The registry holds the address of the global, which may still be null ("paths"
			// before BlueInitializePaths)
			IRoot* obj = reg.second ? *reg.second : nullptr;
			if( !obj )
			{
				return BlueCSetError( &error, BLUE_E_NOT_FOUND, "module object '%s' is not set", reg.first );
			}
			obj->Lock();
			*out = reinterpret_cast<BlueObject*>( obj );
			return BLUE_OK;
		}
		return BlueCSetError( &error, BLUE_E_NOT_FOUND, "no module object '%.*s'", int( name_len ), name );
	} );
}


//--------------------------------------------------------------------
// Enums
//--------------------------------------------------------------------

size_t blue_enum_count( void )
{
	return IsCallerOk() ? BlueRegistration::GetEnumRegs().size() : 0;
}

const char* blue_enum_name( size_t index, size_t* len )
{
	const EnumRegsMap::value_type* entry = IsCallerOk() ? EnumAt( index ) : nullptr;
	return NameWithLength( entry ? entry->first.c_str() : nullptr, len );
}

size_t blue_enum_value_count( size_t index )
{
	const EnumRegsMap::value_type* entry = IsCallerOk() ? EnumAt( index ) : nullptr;
	return entry && entry->second.second ? entry->second.second().size() : 0;
}

int32_t blue_enum_value_at( size_t index, size_t value_index, const char** name, size_t* name_len, int64_t* value )
{
	if( name )
	{
		*name = nullptr;
	}
	if( name_len )
	{
		*name_len = 0;
	}
	if( value )
	{
		*value = 0;
	}
	return Call( nullptr, [&]( BlueCError& error ) -> int32_t {
		const EnumRegsMap::value_type* entry = EnumAt( index );
		if( !entry || !entry->second.second )
		{
			return BlueCSetError( &error, BLUE_E_NOT_FOUND, "no enum %zu", index );
		}
		const EnumValues& values = entry->second.second();
		if( value_index >= values.size() )
		{
			return BlueCSetError( &error, BLUE_E_NOT_FOUND, "enum %s has no value %zu", entry->first.c_str(), value_index );
		}
		const Be::VarChooser& chooser = values[value_index];
		NameWithLength( chooser.mKey, name_len );
		if( name )
		{
			*name = chooser.mKey;
		}
		if( value )
		{
			*value = chooser.mValue.mLong;
		}
		return BLUE_OK;
	} );
}

const char* blue_enum_value_doc( size_t index, size_t value_index, size_t* len )
{
	const char* doc = nullptr;
	const EnumRegsMap::value_type* entry = IsCallerOk() ? EnumAt( index ) : nullptr;
	if( entry && entry->second.second )
	{
		const EnumValues& values = entry->second.second();
		if( value_index < values.size() )
		{
			doc = values[value_index].mDescription ? values[value_index].mDescription : "";
		}
	}
	return NameWithLength( doc, len );
}

#endif
