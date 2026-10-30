// Copyright © 2026 CCP ehf.
//
// Blue C API: classes, objects, members, methods and module functions, and their introspection
// (1.1). Handles are blueexposure's own structures: a BlueClass is a Be::ClassInfo, a BlueMethod
// and a BlueFunction a BlueCMethodDef, a BlueObject an IRoot.

#include "StdAfx.h"

#if BLUE_WITH_CABI

#include "BlueCApiInternal.h"

#include <unordered_map>
#include <vector>

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

BlueClassKind ClassKind( const Be::ClassRegistration& reg )
{
	// Abstract classes have the null factory; the rest of the flagged ones are singletons
	// whose factory hands out the global instance, which Python does not construct either
	if( !reg.mCreateFn || reg.mCreateFn == &ClassRegistrarNullFactory )
	{
		return BLUE_CLASS_ABSTRACT;
	}
	if( reg.mFlags & Be::ClassRegistration::DISABLE_PYTHON_CONSTRUCTION )
	{
		return BLUE_CLASS_SINGLETON;
	}
	return BLUE_CLASS_CREATABLE;
}

bool IsCreatable( const Be::ClassRegistration& reg )
{
	return ClassKind( reg ) == BLUE_CLASS_CREATABLE;
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

const char* DocOrEmpty( const char* doc )
{
	return doc ? doc : "";
}


//--------------------------------------------------------------------
// Introspection state (1.1). Built on first use and kept for the process; only the main thread
// touches it, because every entry point checks the caller first.
//--------------------------------------------------------------------

// The end of the last 1.1 field: the smallest struct_size accepted
constexpr size_t kMemberInfoSize_1_1 = offsetof( BlueMemberInfo, doc ) + sizeof( const char* );
constexpr size_t kSignatureInfoSize_1_1 = offsetof( BlueSignatureInfo, args ) + sizeof( const char* const* );

// What the introspection functions list for one class
struct ClassListing
{
	std::vector<const char*> interfaces;			// own, deduplicated, without IRoot and self
	std::vector<const Be::VarEntry*> members;		// own, without hidden ones
	std::vector<const BlueCMethodDef*> methods;		// BlueCForEachMethod order
};

std::unordered_map<const Be::ClassInfo*, ClassListing>& Listings()
{
	static std::unordered_map<const Be::ClassInfo*, ClassListing> s_listings;
	return s_listings;
}

// A BlueMethod maps to one signature, whether it is in a class's table or an interface's
// registration; BlueCMethodDef does not hold it, so it is looked up here
typedef std::unordered_map<const BlueCMethodDef*, const BlueCSignature*> SignatureIndex;

SignatureIndex& Signatures()
{
	static SignatureIndex s_signatures;
	return s_signatures;
}

bool IndexMethod( const BlueCMethodDef* method, const BlueCSignature* signature, void* context )
{
	static_cast<SignatureIndex*>( context )->emplace( method, signature );
	return true;
}

// The signature index, filled on first use from every registered class (own, parents' and
// interface methods) and every interface registration. Classes listed later through
// blue_class_method_at add theirs too. May throw std::bad_alloc.
SignatureIndex& GetSignatureIndex()
{
	static bool s_built = false;
	SignatureIndex& index = Signatures();
	if( !s_built )
	{
		for( const Be::ClassRegistration& reg : BlueRegistration::GetClassRegs() )
		{
			BlueCForEachMethod( reg.mType, &IndexMethod, &index );
		}
		for( const BlueCThunkerRegistration& reg : BlueRegistration::GetGlobalCThunkerRegs() )
		{
			for( size_t i = 0; reg.mMethods && reg.mMethods[i].name; ++i )
			{
				index.emplace( &reg.mMethods[i], reg.mSignatures ? &reg.mSignatures[i] : nullptr );
			}
		}
		s_built = true;
	}
	return index;
}

bool ListMethod( const BlueCMethodDef* method, const BlueCSignature* signature, void* context )
{
	static_cast<ClassListing*>( context )->methods.push_back( method );
	Signatures().emplace( method, signature );
	return true;
}

// The listing of type, built on first use; null for a null type or when out of memory (the
// accessors that use it return NULL or 0, and must not throw)
const ClassListing* GetListing( const Be::ClassInfo* type ) noexcept
{
	if( !type )
	{
		return nullptr;
	}
	try
	{
		std::unordered_map<const Be::ClassInfo*, ClassListing>& listings = Listings();
		auto found = listings.find( type );
		if( found != listings.end() )
		{
			return &found->second;
		}

		ClassListing listing;
		const Be::IID& rootIID = GetIRootIID();
		std::vector<const Be::IID*> seen;
		for( const Be::InterfaceEntry* entry = type->mInterfaceTable; entry && entry->mIID; ++entry )
		{
			const Be::IID& iid = *entry->mIID;
			if( iid.IsEqual( rootIID ) || ( type->mIID && iid.IsEqual( *type->mIID ) ) )
			{
				continue;
			}
			bool duplicate = false;
			for( const Be::IID* other : seen )
			{
				duplicate = duplicate || other->IsEqual( iid );
			}
			if( !duplicate && iid.GetName() )
			{
				seen.push_back( &iid );
				listing.interfaces.push_back( iid.GetName() );
			}
		}
		// As BlueCFindMember sees them
		for( const Be::VarEntry* entry = type->mMemberTable; entry && entry->mName; ++entry )
		{
			if( ( entry->mEditFlags & Be::HIDDEN ) == 0 )
			{
				listing.members.push_back( entry );
			}
		}
		BlueCForEachMethod( type, &ListMethod, &listing );

		return &listings.emplace( type, std::move( listing ) ).first->second;
	}
	catch( ... )
	{
		return nullptr;
	}
}

// BLUE_MEMBER_* of entry, as BlueCGetMember and BlueCSetMember treat it
uint32_t MemberFlags( const Be::VarEntry& entry )
{
	if( entry.mCGet || entry.mGetProperty )
	{
		uint32_t flags = BLUE_MEMBER_PROPERTY;
		if( entry.mCGet && ( entry.mEditFlags & Be::READ ) )
		{
			flags |= BLUE_MEMBER_READABLE;
		}
		if( entry.mCSet )
		{
			flags |= BLUE_MEMBER_WRITABLE;
		}
		return flags;
	}
	// Attributes are read whatever their flags; an embedded object is never assigned
	uint32_t flags = BLUE_MEMBER_READABLE;
	if( ( entry.mEditFlags & Be::WRITE ) && entry.mType != Be::IROOT )
	{
		flags |= BLUE_MEMBER_WRITABLE;
	}
	return flags;
}

// Check out of a 1.1 info struct: BLUE_OK, or BLUE_E_INVALID_ARGUMENT (out is not touched)
template< typename Info >
int32_t CheckInfo( const Info* out, size_t minimumSize, const char* function, BlueCError& error )
{
	if( !out )
	{
		return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "%s needs an out struct", function );
	}
	if( out->struct_size < minimumSize )
	{
		return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "%s: struct_size is %u, expected at least %zu", function, out->struct_size, minimumSize );
	}
	return BLUE_OK;
}

// Fill the 1.1 fields of out with signature (null: no arguments, null returns)
void FillSignature( const BlueCSignature* signature, BlueSignatureInfo& out )
{
	out.arg_count = signature ? signature->argCount : 0;
	out.optional_count = signature ? signature->optionalCount : 0;
	out.returns = signature ? signature->returns : nullptr;
	out.args = signature && signature->argCount ? signature->args : nullptr;
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

int32_t blue_class_kind( const BlueClass* cls )
{
	if( !IsCallerOk() )
	{
		return BLUE_CLASS_INVALID;
	}
	const Be::ClassRegistration* reg = FindRegistration( ToClassInfo( cls ) );
	return reg ? ClassKind( *reg ) : BLUE_CLASS_INVALID;
}

const char* blue_class_doc( const BlueClass* cls, size_t* len )
{
	return NameWithLength( IsCallerOk() && cls ? DocOrEmpty( ToClassInfo( cls )->mDescription ) : nullptr, len );
}

const char* blue_class_iid( const BlueClass* cls, size_t* len )
{
	const Be::ClassInfo* type = IsCallerOk() ? ToClassInfo( cls ) : nullptr;
	return NameWithLength( type && type->mIID ? type->mIID->GetName() : nullptr, len );
}

size_t blue_class_interface_count( const BlueClass* cls )
{
	const ClassListing* listing = IsCallerOk() ? GetListing( ToClassInfo( cls ) ) : nullptr;
	return listing ? listing->interfaces.size() : 0;
}

const char* blue_class_interface_at( const BlueClass* cls, size_t index, size_t* len )
{
	const ClassListing* listing = IsCallerOk() ? GetListing( ToClassInfo( cls ) ) : nullptr;
	return NameWithLength( listing && index < listing->interfaces.size() ? listing->interfaces[index] : nullptr, len );
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

size_t blue_class_member_count( const BlueClass* cls )
{
	const ClassListing* listing = IsCallerOk() ? GetListing( ToClassInfo( cls ) ) : nullptr;
	return listing ? listing->members.size() : 0;
}

int32_t blue_class_member_at( const BlueClass* cls, size_t index, BlueMemberInfo* out )
{
	return Call( nullptr, [&]( BlueCError& error ) -> int32_t {
		const int32_t status = CheckInfo( out, kMemberInfoSize_1_1, "blue_class_member_at", error );
		if( status != BLUE_OK )
		{
			return status;
		}
		out->flags = 0;
		out->name = nullptr;
		out->type = nullptr;
		out->doc = nullptr;

		const ClassListing* listing = GetListing( ToClassInfo( cls ) );
		if( !listing || index >= listing->members.size() )
		{
			return BlueCSetError( &error, BLUE_E_NOT_FOUND, "no member %zu", index );
		}
		const Be::VarEntry& entry = *listing->members[index];
		const char* type = BlueCMemberTypeName( entry );
		out->flags = MemberFlags( entry );
		out->name = entry.mName;
		out->type = type;
		out->doc = DocOrEmpty( entry.mDescription );
		return BLUE_OK;
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

size_t blue_class_method_count( const BlueClass* cls )
{
	const ClassListing* listing = IsCallerOk() ? GetListing( ToClassInfo( cls ) ) : nullptr;
	return listing ? listing->methods.size() : 0;
}

const BlueMethod* blue_class_method_at( const BlueClass* cls, size_t index )
{
	const ClassListing* listing = IsCallerOk() ? GetListing( ToClassInfo( cls ) ) : nullptr;
	return listing && index < listing->methods.size() ? reinterpret_cast<const BlueMethod*>( listing->methods[index] ) : nullptr;
}

const char* blue_method_doc( const BlueMethod* method, size_t* len )
{
	return NameWithLength( IsCallerOk() && method ? DocOrEmpty( ToMethodDef( method )->doc ) : nullptr, len );
}

int32_t blue_method_signature( const BlueMethod* method, BlueSignatureInfo* out )
{
	return Call( nullptr, [&]( BlueCError& error ) -> int32_t {
		const int32_t status = CheckInfo( out, kSignatureInfoSize_1_1, "blue_method_signature", error );
		if( status != BLUE_OK )
		{
			return status;
		}
		FillSignature( nullptr, *out );
		if( !method )
		{
			return BlueCSetError( &error, BLUE_E_NOT_FOUND, "blue_method_signature without a method" );
		}

		const SignatureIndex& index = GetSignatureIndex();
		auto found = index.find( ToMethodDef( method ) );
		if( found == index.end() || !found->second )
		{
			return BlueCSetError( &error, BLUE_E_NOT_FOUND, "method %s has no signature", ToMethodDef( method )->name );
		}
		FillSignature( found->second, *out );
		return BLUE_OK;
	} );
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

const char* blue_function_doc( const BlueFunction* fn, size_t* len )
{
	return NameWithLength( IsCallerOk() && fn ? DocOrEmpty( ToFunctionDef( fn )->doc ) : nullptr, len );
}

int32_t blue_function_signature( const BlueFunction* fn, BlueSignatureInfo* out )
{
	return Call( nullptr, [&]( BlueCError& error ) -> int32_t {
		const int32_t status = CheckInfo( out, kSignatureInfoSize_1_1, "blue_function_signature", error );
		if( status != BLUE_OK )
		{
			return status;
		}
		FillSignature( nullptr, *out );
		const BlueCMethodDef* def = ToFunctionDef( fn );
		if( !def || !def->name )
		{
			return BlueCSetError( &error, BLUE_E_NOT_FOUND, "blue_function_signature without a function" );
		}

		// Recorded by name, next to GetCFuncRegs (this module's copy)
		const CFuncSignatures& signatures = BlueRegistration::GetCFuncSignatures();
		auto found = signatures.find( def->name );
		if( found == signatures.end() )
		{
			return BlueCSetError( &error, BLUE_E_NOT_FOUND, "function %s has no signature", def->name );
		}
		FillSignature( &found->second, *out );
		return BLUE_OK;
	} );
}

#endif
