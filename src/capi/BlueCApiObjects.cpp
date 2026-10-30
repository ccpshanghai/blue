// Copyright © 2026 CCP ehf.
//
// Blue C API: classes, objects, members, methods and module functions. Handles are blueexposure's
// own structures: a BlueClass is a Be::ClassInfo, a BlueMethod and a BlueFunction a
// BlueCMethodDef, a BlueObject an IRoot.

#include "StdAfx.h"

#if BLUE_WITH_CABI

#include "BlueCApiInternal.h"

using namespace BlueCApi;

namespace
{

const Be::ClassInfo* ToClassInfo( const BlueClass* cls )
{
	return reinterpret_cast<const Be::ClassInfo*>( cls );
}

const BlueClass* ToHandle( const Be::ClassInfo* type )
{
	return reinterpret_cast<const BlueClass*>( type );
}

IRoot* ToRoot( BlueObject* obj )
{
	return reinterpret_cast<IRoot*>( obj );
}

const char* ClassName( const Be::ClassInfo* type )
{
	return type && type->mClassId ? type->mClassId->GetName() : nullptr;
}

bool NameEquals( const char* candidate, const char* name, size_t nameLength )
{
	return candidate && name && strlen( candidate ) == nameLength && memcmp( candidate, name, nameLength ) == 0;
}

// The registration of type in blue's class registry, or null
const Be::ClassRegistration* FindRegistration( const Be::ClassInfo* type )
{
	if( !type )
	{
		return nullptr;
	}
	for( const Be::ClassRegistration& reg : BlueRegistration::GetClassRegs() )
	{
		if( reg.mType == type )
		{
			return &reg;
		}
	}
	return nullptr;
}

bool IsCreatable( const Be::ClassRegistration& reg )
{
	// Abstract classes have the null factory; the rest of the flagged ones are singletons
	// whose factory hands out the global instance, which Python does not construct either
	return reg.mCreateFn && reg.mCreateFn != &ClassRegistrarNullFactory && ( reg.mFlags & Be::ClassRegistration::DISABLE_PYTHON_CONSTRUCTION ) == 0;
}

const BlueCMethodDef* ToMethodDef( const BlueMethod* method )
{
	return reinterpret_cast<const BlueCMethodDef*>( method );
}

const BlueCMethodDef* ToFunctionDef( const BlueFunction* fn )
{
	return reinterpret_cast<const BlueCMethodDef*>( fn );
}

int32_t CheckArguments( const BlueCValue* args, uint32_t argc, BlueCError& err )
{
	if( argc > 0 && !args )
	{
		return BlueCSetError( &err, BLUE_E_INVALID_ARGUMENT, "args is null but argc is %u", argc );
	}
	return BLUE_OK;
}

} // namespace


//--------------------------------------------------------------------
// Classes
//--------------------------------------------------------------------

size_t blue_class_count( void )
{
	return IsCallerOk() ? BlueRegistration::GetClassRegs().size() : 0;
}

const BlueClass* blue_class_at( size_t index )
{
	if( !IsCallerOk() )
	{
		return nullptr;
	}
	const ClassRegsVector& regs = BlueRegistration::GetClassRegs();
	return index < regs.size() ? ToHandle( regs[index].mType ) : nullptr;
}

const BlueClass* blue_class_find( const char* name, size_t name_len )
{
	if( !IsCallerOk() )
	{
		return nullptr;
	}
	for( const Be::ClassRegistration& reg : BlueRegistration::GetClassRegs() )
	{
		if( NameEquals( ClassName( reg.mType ), name, name_len ) )
		{
			return ToHandle( reg.mType );
		}
	}
	return nullptr;
}

const char* blue_class_name( const BlueClass* cls, size_t* len )
{
	return NameWithLength( IsCallerOk() ? ClassName( ToClassInfo( cls ) ) : nullptr, len );
}

const BlueClass* blue_class_parent( const BlueClass* cls )
{
	if( !IsCallerOk() || !cls )
	{
		return nullptr;
	}
	return ToHandle( ToClassInfo( cls )->mParentClassInfo );
}

int32_t blue_class_is_abstract( const BlueClass* cls )
{
	if( !IsCallerOk() )
	{
		return 0;
	}
	const Be::ClassRegistration* reg = FindRegistration( ToClassInfo( cls ) );
	return reg && !IsCreatable( *reg ) ? 1 : 0;
}

const BlueClass* blue_class_of( BlueObject* obj )
{
	if( !IsCallerOk() || !obj )
	{
		return nullptr;
	}
	return ToHandle( ToRoot( obj )->ClassType() );
}


//--------------------------------------------------------------------
// Objects
//--------------------------------------------------------------------

int32_t blue_create( const BlueClass* cls, BlueObject** out, BlueCError* err )
{
	if( out )
	{
		*out = nullptr;
	}
	return Call( err, [&]( BlueCError& error ) -> int32_t {
		if( !cls || !out )
		{
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "blue_create needs a class and an out pointer" );
		}
		const Be::ClassInfo* type = ToClassInfo( cls );
		const char* name = ClassName( type );
		const Be::ClassRegistration* reg = FindRegistration( type );
		if( !reg )
		{
			return BlueCSetError( &error, BLUE_E_NOT_FOUND, "class %s is not registered", name ? name : "<unknown>" );
		}
		if( !IsCreatable( *reg ) )
		{
			return BlueCSetError( &error, BLUE_E_UNSUPPORTED, "class %s cannot be created (abstract, or a singleton reached as a module object)", name );
		}

		ErrorCapture capture;
		void* instance = nullptr;
		if( !reg->mCreateFn( GetIRootIID(), &instance ) || !instance )
		{
			return capture.Fail( error, BLUE_E_FAILED, "creating the object failed" );
		}
		// The factory hands over one reference
		*out = static_cast<BlueObject*>( instance );
		return BLUE_OK;
	} );
}

int32_t blue_query_interface( BlueObject* obj, const char* iid, size_t iid_len, BlueObject** out, BlueCError* err )
{
	if( out )
	{
		*out = nullptr;
	}
	return Call( err, [&]( BlueCError& error ) -> int32_t {
		if( !obj )
		{
			return BlueCSetError( &error, BLUE_E_SELF, "blue_query_interface without an object" );
		}
		if( !iid || iid_len == 0 || !out )
		{
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "blue_query_interface needs an interface name and an out pointer" );
		}
		// Be::IID wants a NUL-terminated name (and keeps a copy of every name it sees)
		const std::string name( iid, iid_len );
		if( name.find( '\0' ) != std::string::npos )
		{
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "interface name contains a NUL" );
		}

		IRoot* root = ToRoot( obj );
		void* result = nullptr;
		if( !root->QueryInterface( Be::IID( name.c_str() ), &result, BEQI_SILENT ) || !result )
		{
			return BlueCSetError( &error, BLUE_E_NOT_FOUND, "%s does not implement %s", BlueCClassNameOf( root ), name.c_str() );
		}
		// Every interface derives from IRoot first, so the interface pointer is an IRoot too
		*out = static_cast<BlueObject*>( result );
		return BLUE_OK;
	} );
}

void blue_add_ref( BlueObject* obj )
{
	if( obj )
	{
		ToRoot( obj )->Lock();
	}
}

void blue_release( BlueObject* obj )
{
	if( obj )
	{
		ToRoot( obj )->Unlock();
	}
}


//--------------------------------------------------------------------
// Members
//--------------------------------------------------------------------

int32_t blue_get( BlueObject* obj, const char* name, size_t name_len, BlueCValue* out, BlueCError* err )
{
	if( out )
	{
		BlueCValueSetNone( *out );
	}
	return Call( err, [&]( BlueCError& error ) -> int32_t {
		IRoot* root = ToRoot( obj );
		const int32_t status = BlueCGetMember( root, name, name_len, out, &error );
		// Lookup failures already name the member
		if( status != BLUE_OK && status != BLUE_E_NOT_FOUND && status != BLUE_E_SELF )
		{
			PrefixMessage( error, BlueCClassNameOf( root ), name, name_len );
		}
		return status;
	} );
}

int32_t blue_set( BlueObject* obj, const char* name, size_t name_len, const BlueCValue* value, BlueCError* err )
{
	return Call( err, [&]( BlueCError& error ) -> int32_t {
		IRoot* root = ToRoot( obj );
		const int32_t status = BlueCSetMember( root, name, name_len, value, &error );
		if( status != BLUE_OK && status != BLUE_E_NOT_FOUND && status != BLUE_E_SELF )
		{
			PrefixMessage( error, BlueCClassNameOf( root ), name, name_len );
		}
		return status;
	} );
}


//--------------------------------------------------------------------
// Methods
//--------------------------------------------------------------------

const BlueMethod* blue_method_find( const BlueClass* cls, const char* name, size_t name_len )
{
	if( !IsCallerOk() || !cls )
	{
		return nullptr;
	}
	return reinterpret_cast<const BlueMethod*>( BlueCFindMethod( ToClassInfo( cls ), name, name_len ) );
}

const char* blue_method_name( const BlueMethod* method, size_t* len )
{
	return NameWithLength( IsCallerOk() && method ? ToMethodDef( method )->name : nullptr, len );
}

int32_t blue_invoke( BlueObject* obj, const BlueMethod* method, const BlueCValue* args, uint32_t argc, BlueCValue* out, BlueCError* err )
{
	if( out )
	{
		BlueCValueSetNone( *out );
	}
	return Call( err, [&]( BlueCError& error ) -> int32_t {
		const BlueCMethodDef* def = ToMethodDef( method );
		if( !def || !def->fn )
		{
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "blue_invoke without a method" );
		}
		int32_t status = CheckArguments( args, argc, error );
		if( status == BLUE_OK )
		{
			// The thunk checks obj (BLUE_E_SELF for a null or foreign object)
			status = def->fn( obj, args, argc, out, &error );
		}
		if( status != BLUE_OK )
		{
			PrefixMessage( error, BlueCClassNameOf( ToRoot( obj ) ), def->name, strlen( def->name ) );
		}
		return status;
	} );
}


//--------------------------------------------------------------------
// Module functions
//--------------------------------------------------------------------

size_t blue_function_count( void )
{
	return IsCallerOk() ? BlueRegistration::GetCFuncRegs().size() : 0;
}

const BlueFunction* blue_function_at( size_t index )
{
	if( !IsCallerOk() )
	{
		return nullptr;
	}
	const CFuncRegsVector& functions = BlueRegistration::GetCFuncRegs();
	return index < functions.size() ? reinterpret_cast<const BlueFunction*>( &functions[index] ) : nullptr;
}

const BlueFunction* blue_function_find( const char* name, size_t name_len )
{
	if( !IsCallerOk() )
	{
		return nullptr;
	}
	return reinterpret_cast<const BlueFunction*>( BlueCFindFunction( BlueRegistration::GetCFuncRegs(), name, name_len ) );
}

const char* blue_function_name( const BlueFunction* fn, size_t* len )
{
	return NameWithLength( IsCallerOk() && fn ? ToFunctionDef( fn )->name : nullptr, len );
}

int32_t blue_call( const BlueFunction* fn, const BlueCValue* args, uint32_t argc, BlueCValue* out, BlueCError* err )
{
	if( out )
	{
		BlueCValueSetNone( *out );
	}
	return Call( err, [&]( BlueCError& error ) -> int32_t {
		const BlueCMethodDef* def = ToFunctionDef( fn );
		if( !def || !def->fn )
		{
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "blue_call without a function" );
		}
		int32_t status = CheckArguments( args, argc, error );
		if( status == BLUE_OK )
		{
			status = def->fn( nullptr, args, argc, out, &error );
		}
		if( status != BLUE_OK )
		{
			PrefixMessage( error, nullptr, def->name, strlen( def->name ) );
		}
		return status;
	} );
}

#endif
