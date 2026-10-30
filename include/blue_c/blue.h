/* Copyright © 2026 CCP ehf.
 *
 * The Blue C API: a versioned, language-neutral way to start blue, create and call its
 * objects, and read and write their streams, without CPython. Plain C (C99 or later).
 *
 * Values and errors are the C-ABI backend's types from BlueCAbi.h (installed next to this
 * header); its ownership rules apply to every function here:
 *   - Inputs (names, args, values passed in) are borrowed for the duration of the call.
 *   - Outputs (out values, err->message) belong to the caller and are freed with
 *     blue_value_release / blue_error_clear. An object handed out carries one reference (+1),
 *     dropped with blue_release. out is overwritten without being released.
 *   - err must be { BLUE_OK, NULL, 0 } or hold an earlier error (filling it frees the old
 *     message). It may be NULL. It is left untouched on success, so test the returned status,
 *     not err->code.
 *   - Names are UTF-8 with an explicit length and need not be NUL-terminated.
 *   - No C++ exception crosses this API; one caught inside is BLUE_E_EXCEPTION.
 *
 * Threads: blue runs on one thread, the one that called blue_initialize (the main thread).
 * Every function returns BLUE_E_WRONG_THREAD from any other thread, and BLUE_E_NOT_INITIALIZED
 * before blue_initialize or after blue_shutdown; functions that return a handle, a name or a
 * count return NULL or 0 instead. The exceptions, callable from any thread at any time, are
 * blue_abi_version, blue_is_initialized, blue_add_ref, blue_release, the blue_value_* and
 * blue_callback_* helpers, blue_error_clear and blue_set_error_message.
 *
 * Logging: blue's log goes nowhere unless the environment variable BLUE_CAPI_LOG is 1, which
 * echoes it to stderr. Errors logged during a failing call become its error message.
 */

#pragma once
#ifndef BLUE_C_BLUE_H
#define BLUE_C_BLUE_H

#include "BlueCAbi.h"

#include <stddef.h>
#include <stdint.h>

#if defined( _WIN32 )
#if defined( BLUE_CAPI_BUILD )
#define BLUE_CAPI __declspec( dllexport )
#else
#define BLUE_CAPI __declspec( dllimport )
#endif
#define BLUE_CALL __cdecl
#else
#define BLUE_CAPI __attribute__( ( visibility( "default" ) ) )
#define BLUE_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------------------------
 * Version. Adding functions bumps the minor version; changing a signature, a struct or a rule
 * bumps the major version. A host built against major M must refuse a blue with another major.
 *------------------------------------------------------------------------------------------*/
#define BLUE_CAPI_VERSION_MAJOR 1
#define BLUE_CAPI_VERSION_MINOR 2
#define BLUE_CAPI_VERSION ( ( BLUE_CAPI_VERSION_MAJOR << 16 ) | BLUE_CAPI_VERSION_MINOR )

/* (major << 16) | minor of the blue that is loaded */
BLUE_CAPI uint32_t BLUE_CALL blue_abi_version( void );

/*--------------------------------------------------------------------------------------------
 * Handles. Classes, methods and functions live as long as the process; objects (BlueObject,
 * from BlueCAbi.h) are reference counted.
 *------------------------------------------------------------------------------------------*/
typedef struct BlueClass BlueClass;			/* an exposed class */
typedef struct BlueMethod BlueMethod;		/* a method of a class or of an interface it maps */
typedef struct BlueFunction BlueFunction;	/* a module function */

/*--------------------------------------------------------------------------------------------
 * Lifecycle. Blue starts once per process and cannot be restarted.
 *
 * BlueInitOptions grows only at its end, in minor versions; blue reads only the fields that
 * struct_size covers (later ones count as zero), so a host built against an older minor
 * version keeps working. Every field below is part of 1.0.
 *------------------------------------------------------------------------------------------*/
typedef struct BlueInitOptions
{
	uint32_t struct_size;				/* sizeof( BlueInitOptions ) as the host compiled it */
	const char* app_dir;				/* UTF-8, NUL-terminated; must be an existing directory */
	const char* const* args;			/* startup arguments such as "/resManThreadCount=2"; may be NULL */
	uint32_t arg_count;
	const char* const* search_paths;	/* "<key>=<directory>", such as "res=/data/res"; may be NULL */
	uint32_t search_path_count;
} BlueInitOptions;

/* Start blue on the calling thread, which becomes the main thread. Strings in options are
 * UTF-8 and NUL-terminated. Fails with BLUE_E_FAILED when blue is already running, or has run
 * before in this process; with BLUE_E_INVALID_ARGUMENT for bad options (blue is then not
 * started and the call may be repeated). */
BLUE_CAPI int32_t BLUE_CALL blue_initialize( const BlueInitOptions* options, BlueCError* err );

/* Stop blue without exiting the process: ends the resource waits still open (their fn runs,
 * with BLUE_RESOURCE_SHUTDOWN unless the load had finished), joins its worker threads and frees
 * its caches. Objects the host still holds must not be used afterwards, except to blue_release
 * them. */
BLUE_CAPI int32_t BLUE_CALL blue_shutdown( BlueCError* err );

/* 1 while blue is running (between blue_initialize and blue_shutdown), else 0 */
BLUE_CAPI int32_t BLUE_CALL blue_is_initialized( void );

/* One frame of blue's main-thread work: the resource manager's main queue (finishing
 * asynchronous loads), cache housekeeping, deferred deletes and statistics; then the resource
 * waits that have been decided (see "Resource waits"). Never sleeps. Call it once per frame. */
BLUE_CAPI int32_t BLUE_CALL blue_pump( BlueCError* err );

/*--------------------------------------------------------------------------------------------
 * Type names (1.1): what members, arguments and return values hold, as the introspection
 * functions below report it. NUL-terminated, living as long as the process, and ASCII without
 * spaces except for the raw part of "unknown:" (a C++ type name):
 *
 *   type      := scalar | "string" | "bytes" | vector | "callback" | object | container
 *              | "unknown:" raw
 *   scalar    := "void" | "bool" | "int8" | "int16" | "int32" | "int64"
 *              | "uint8" | "uint16" | "uint32" | "uint64" | "float" | "double"
 *   vector    := "vector2" | "vector3" | "vector4" | "vector2d" | "vector3d" | "vector4d"
 *              | "vector3i" | "color" | "quaternion" | "matrix"
 *   object    := "object:" iid          an interface or class name; always nullable
 *   container := "list<" type ">" | "map<" type "," type ">" | "tuple<" type ( "," type )* ">"
 *              | "optional<" type ">"
 *
 * "void" is a return type only. Strings of every width are "string" (BLUE_VK_STRING, UTF-8);
 * "bytes" is BLUE_VK_BYTES. Enums are their underlying integer type. "unknown:" marks a type
 * the C ABI cannot convert: a call that takes or returns one fails with BLUE_E_UNSUPPORTED.
 * New names may be added in minor versions; a host treats a name it does not know as
 * "unknown:".
 *------------------------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------------------------
 * Classes
 *------------------------------------------------------------------------------------------*/

/* The registered classes, by index */
BLUE_CAPI size_t BLUE_CALL blue_class_count( void );
BLUE_CAPI const BlueClass* BLUE_CALL blue_class_at( size_t index );

/* The registered class named name (without a module prefix, such as "MemStream"), or NULL */
BLUE_CAPI const BlueClass* BLUE_CALL blue_class_find( const char* name, size_t name_len );

/* The class's name, NUL-terminated; len (optional) receives its length */
BLUE_CAPI const char* BLUE_CALL blue_class_name( const BlueClass* cls, size_t* len );

/* The class's parent class, or NULL */
BLUE_CAPI const BlueClass* BLUE_CALL blue_class_parent( const BlueClass* cls );

/* 1 if the class cannot be created with blue_create (abstract, or a singleton reached through
 * blue_module_object_get), 0 if it can, 0 for NULL or an unregistered class */
BLUE_CAPI int32_t BLUE_CALL blue_class_is_abstract( const BlueClass* cls );

/* The class of an object, or NULL */
BLUE_CAPI const BlueClass* BLUE_CALL blue_class_of( BlueObject* obj );

/* What blue_class_kind reports (1.1); splits what blue_class_is_abstract merges */
typedef enum BlueClassKind
{
	BLUE_CLASS_INVALID = 0,		/* NULL, or a class that is not registered */
	BLUE_CLASS_CREATABLE = 1,	/* blue_create makes instances */
	BLUE_CLASS_ABSTRACT = 2,	/* no factory: instances only come from blue */
	BLUE_CLASS_SINGLETON = 3	/* one global instance, reached through blue_module_object_get */
} BlueClassKind;

/* The class's BlueClassKind (1.1) */
BLUE_CAPI int32_t BLUE_CALL blue_class_kind( const BlueClass* cls );

/* The class's description, NUL-terminated, "" when it has none; NULL for NULL. len (optional)
 * receives its length. (1.1) */
BLUE_CAPI const char* BLUE_CALL blue_class_doc( const BlueClass* cls, size_t* len );

/* The name of the class's own interface ID, NUL-terminated, or NULL. It is the class name except
 * for a few template classes (List, Dict). len (optional) receives its length. (1.1) */
BLUE_CAPI const char* BLUE_CALL blue_class_iid( const BlueClass* cls, size_t* len );

/* The interfaces the class itself maps (blue_query_interface answers to them), in declaration
 * order, each once, without IRoot and the class's own IID. Parents map their own: walk
 * blue_class_parent for the rest. A bad index gives NULL; len (optional) receives the length.
 * (1.1) */
BLUE_CAPI size_t BLUE_CALL blue_class_interface_count( const BlueClass* cls );
BLUE_CAPI const char* BLUE_CALL blue_class_interface_at( const BlueClass* cls, size_t index, size_t* len );

/*--------------------------------------------------------------------------------------------
 * Objects
 *------------------------------------------------------------------------------------------*/

/* A new instance of cls in *out (+1). BLUE_E_UNSUPPORTED for a class blue_class_is_abstract
 * reports. */
BLUE_CAPI int32_t BLUE_CALL blue_create( const BlueClass* cls, BlueObject** out, BlueCError* err );

/* obj as the interface named iid (a C++ interface name such as "IBlueStream", or a class name)
 * in *out (+1); BLUE_E_NOT_FOUND if obj does not implement it */
BLUE_CAPI int32_t BLUE_CALL blue_query_interface( BlueObject* obj, const char* iid, size_t iid_len, BlueObject** out, BlueCError* err );

/* Add or drop one reference; NULL is ignored. The last release destroys the object on the
 * calling thread, so release on the main thread. */
BLUE_CAPI void BLUE_CALL blue_add_ref( BlueObject* obj );
BLUE_CAPI void BLUE_CALL blue_release( BlueObject* obj );

/*--------------------------------------------------------------------------------------------
 * Members: attributes and properties, looked up in the object's class and then its parents
 *------------------------------------------------------------------------------------------*/
BLUE_CAPI int32_t BLUE_CALL blue_get( BlueObject* obj, const char* name, size_t name_len, BlueCValue* out, BlueCError* err );
BLUE_CAPI int32_t BLUE_CALL blue_set( BlueObject* obj, const char* name, size_t name_len, const BlueCValue* value, BlueCError* err );

/* BlueMemberInfo.flags (1.1) */
#define BLUE_MEMBER_READABLE 0x1u	/* blue_get reads it */
#define BLUE_MEMBER_WRITABLE 0x2u	/* blue_set assigns it */
#define BLUE_MEMBER_PROPERTY 0x4u	/* backed by accessor functions, not a field */

/* A member as blue_class_member_at describes it (1.1). Like BlueInitOptions it grows only at its
 * end: the host sets struct_size, and blue fills only the fields struct_size covers. */
typedef struct BlueMemberInfo
{
	uint32_t struct_size;	/* in: sizeof( BlueMemberInfo ) as the host compiled it */
	uint32_t flags;			/* BLUE_MEMBER_* */
	const char* name;
	const char* type;		/* a type name, see "Type names" below */
	const char* doc;		/* "" when none */
} BlueMemberInfo;

/* The class's own members (parents have their own: walk blue_class_parent), in declaration
 * order, without hidden ones: what blue_get and blue_set reach. (1.1) */
BLUE_CAPI size_t BLUE_CALL blue_class_member_count( const BlueClass* cls );

/* Member index of cls in *out. Strings in it are NUL-terminated and live as long as the process.
 * BLUE_E_INVALID_ARGUMENT for a NULL out or a struct_size smaller than the 1.1 fields (out is
 * then left as it is); BLUE_E_NOT_FOUND for a NULL class or a bad index (out's strings are then
 * NULL and its flags 0). (1.1) */
BLUE_CAPI int32_t BLUE_CALL blue_class_member_at( const BlueClass* cls, size_t index, BlueMemberInfo* out );

/*--------------------------------------------------------------------------------------------
 * Methods. A class's own methods come first, then its parents', then those of the interfaces
 * it maps (such as IBlueStream's Read, Write and Seek on every stream).
 *------------------------------------------------------------------------------------------*/

/* The method named name of cls, or NULL */
BLUE_CAPI const BlueMethod* BLUE_CALL blue_method_find( const BlueClass* cls, const char* name, size_t name_len );

/* The method's name, NUL-terminated; len (optional) receives its length */
BLUE_CAPI const char* BLUE_CALL blue_method_name( const BlueMethod* method, size_t* len );

/* Every method blue_method_find resolves for cls, each name once, in its lookup order (own,
 * parents', interfaces'). A bad index gives NULL. (1.1) */
BLUE_CAPI size_t BLUE_CALL blue_class_method_count( const BlueClass* cls );
BLUE_CAPI const BlueMethod* BLUE_CALL blue_class_method_at( const BlueClass* cls, size_t index );

/* The method's description, NUL-terminated, "" when it has none; NULL for NULL. len (optional)
 * receives its length. (1.1) */
BLUE_CAPI const char* BLUE_CALL blue_method_doc( const BlueMethod* method, size_t* len );

/* What a method or module function takes and returns (1.1). Grows only at its end, like
 * BlueMemberInfo: the host sets struct_size, blue fills the fields it covers. */
typedef struct BlueSignatureInfo
{
	uint32_t struct_size;		/* in: sizeof( BlueSignatureInfo ) as the host compiled it */
	uint32_t arg_count;			/* arguments a caller passes at most */
	uint32_t optional_count;	/* how many trailing ones it may omit */
	const char* returns;		/* a type name; "void" when the call returns BLUE_VK_NONE */
	const char* const* args;	/* arg_count type names; may be NULL when arg_count is 0 */
} BlueSignatureInfo;

/* The method's signature in *out; its strings and args array live as long as the process.
 * BLUE_E_INVALID_ARGUMENT for a NULL out or a struct_size smaller than the 1.1 fields (out is
 * then left as it is); BLUE_E_NOT_FOUND for a NULL method or one without a recorded signature
 * (out then has no arguments and a NULL returns). (1.1) */
BLUE_CAPI int32_t BLUE_CALL blue_method_signature( const BlueMethod* method, BlueSignatureInfo* out );

/* Call method on obj (an instance of the class it was found on, or of a subclass; otherwise
 * BLUE_E_SELF). out may be NULL to discard the result. */
BLUE_CAPI int32_t BLUE_CALL blue_invoke( BlueObject* obj, const BlueMethod* method, const BlueCValue* args, uint32_t argc, BlueCValue* out, BlueCError* err );

/*--------------------------------------------------------------------------------------------
 * Module functions
 *------------------------------------------------------------------------------------------*/
BLUE_CAPI size_t BLUE_CALL blue_function_count( void );
BLUE_CAPI const BlueFunction* BLUE_CALL blue_function_at( size_t index );
BLUE_CAPI const BlueFunction* BLUE_CALL blue_function_find( const char* name, size_t name_len );
BLUE_CAPI const char* BLUE_CALL blue_function_name( const BlueFunction* fn, size_t* len );
BLUE_CAPI int32_t BLUE_CALL blue_call( const BlueFunction* fn, const BlueCValue* args, uint32_t argc, BlueCValue* out, BlueCError* err );

/* The function's description, as blue_method_doc (1.1) */
BLUE_CAPI const char* BLUE_CALL blue_function_doc( const BlueFunction* fn, size_t* len );

/* The function's signature, as blue_method_signature (1.1) */
BLUE_CAPI int32_t BLUE_CALL blue_function_signature( const BlueFunction* fn, BlueSignatureInfo* out );

/*--------------------------------------------------------------------------------------------
 * Module objects: blue's singletons ("os", "paths", "resMan", ...)
 *------------------------------------------------------------------------------------------*/
BLUE_CAPI size_t BLUE_CALL blue_module_object_count( void );

/* The name of module object index, NUL-terminated, or NULL; len (optional) receives its length */
BLUE_CAPI const char* BLUE_CALL blue_module_object_name( size_t index, size_t* len );

/* The module object named name in *out (+1); BLUE_E_NOT_FOUND for an unknown name or one that
 * is not set */
BLUE_CAPI int32_t BLUE_CALL blue_module_object_get( const char* name, size_t name_len, BlueObject** out, BlueCError* err );

/*--------------------------------------------------------------------------------------------
 * Enums: named sets of integer values. Several names may share a value.
 *------------------------------------------------------------------------------------------*/
BLUE_CAPI size_t BLUE_CALL blue_enum_count( void );

/* The name of enum index, NUL-terminated, or NULL; len (optional) receives its length */
BLUE_CAPI const char* BLUE_CALL blue_enum_name( size_t index, size_t* len );

/* The number of values of enum index (0 for a bad index) */
BLUE_CAPI size_t BLUE_CALL blue_enum_value_count( size_t index );

/* Value value_index of enum index: its name (NUL-terminated, living as long as the process),
 * the name's length and its value; every output pointer is optional. BLUE_E_NOT_FOUND for a
 * bad index. */
BLUE_CAPI int32_t BLUE_CALL blue_enum_value_at( size_t index, size_t value_index, const char** name, size_t* name_len, int64_t* value );

/* The description of value value_index of enum index, NUL-terminated, "" when it has none; NULL
 * for a bad index. len (optional) receives its length. (1.1) */
BLUE_CAPI const char* BLUE_CALL blue_enum_value_doc( size_t index, size_t value_index, size_t* len );

/*--------------------------------------------------------------------------------------------
 * Resource waits (1.2): a one-shot notification when an asynchronously loading resource (a
 * BlueAsyncRes, such as what the resource manager's GetResource returns) has finished.
 *
 * A wait's fn runs exactly once, on the main thread, and never inside the call that registered
 * it: from blue_pump, after blue's own work of that frame, or from blue_shutdown. Blue does not
 * cancel or restart the load for a wait: resources are shared, so another holder may be waiting
 * for the same one.
 *------------------------------------------------------------------------------------------*/

/* What a resource wait's fn receives (1.2) */
typedef enum BlueResourceState
{
	BLUE_RESOURCE_READY = 1,		/* loaded and prepared: the resource's isGood is 1 */
	BLUE_RESOURCE_FAILED = 2,		/* finished, but not good: isPrepared is 1, isGood 0 */
	BLUE_RESOURCE_CANCELLED = 3,	/* blue_resource_wait_cancel ended the wait first */
	BLUE_RESOURCE_SHUTDOWN = 4		/* blue_shutdown ended the wait first */
} BlueResourceState;

/* Receives the user_data given to blue_resource_wait and a BlueResourceState */
typedef void ( BLUE_CALL *BlueResourceWaitFn )( void* user_data, int32_t state );

/* Wait for resource (a BlueAsyncRes) to finish loading. fn( user_data, state ) runs exactly
 * once, on the main thread: from blue_pump once the load has finished (READY or FAILED), from
 * blue_pump after blue_resource_wait_cancel (CANCELLED), or from blue_shutdown (SHUTDOWN). It
 * never runs inside this call, also when the resource has already finished; then the next
 * blue_pump delivers it. fn should only record the state: it may call the functions that are
 * callable from any thread, nothing else (a blue_pump inside it delivers nothing). A reload
 * while waiting is waited for too.
 *
 * The wait holds a reference to resource until fn has run. *out_id receives a non-zero id,
 * unique in the process, for blue_resource_wait_cancel; it is 0 when the call fails.
 * BLUE_E_INVALID_ARGUMENT for a NULL resource, fn or out_id, or for a resource that is neither
 * loading nor finished (one that was never initialised, such as a resource a dynamic:/
 * constructor created); BLUE_E_TYPE for an object that is not a BlueAsyncRes. (1.2) */
BLUE_CAPI int32_t BLUE_CALL blue_resource_wait( BlueObject* resource, BlueResourceWaitFn fn, void* user_data, uint64_t* out_id, BlueCError* err );

/* End wait id early: its fn runs with BLUE_RESOURCE_CANCELLED from the next blue_pump, unless the
 * load has already finished, in which case it runs with READY or FAILED. The load itself goes
 * on. BLUE_E_NOT_FOUND for an id that is unknown or whose fn has already run. (1.2) */
BLUE_CAPI int32_t BLUE_CALL blue_resource_wait_cancel( uint64_t id, BlueCError* err );

/*--------------------------------------------------------------------------------------------
 * Values and callbacks. Values a host callback returns to blue must be built with these
 * (blue frees them); see BlueCAbi.h.
 *------------------------------------------------------------------------------------------*/

/* Free everything a value blue produced owns and reset it to BLUE_VK_NONE; NULL is ignored.
 * Objects in it are released as blue_release does, so release values that hold objects on the
 * main thread. */
BLUE_CAPI void BLUE_CALL blue_value_release( BlueCValue* value );

/* BLUE_VK_STRING holding a copy of utf8[0, len) (BLUE_E_INVALID_ARGUMENT unless valid UTF-8) */
BLUE_CAPI int32_t BLUE_CALL blue_value_set_string( BlueCValue* out, const char* utf8, size_t len, BlueCError* err );

/* BLUE_VK_BYTES holding a copy of data[0, len) */
BLUE_CAPI int32_t BLUE_CALL blue_value_set_bytes( BlueCValue* out, const void* data, size_t len, BlueCError* err );

/* BLUE_VK_ARRAY of count BLUE_VK_NONE items, filled in place */
BLUE_CAPI int32_t BLUE_CALL blue_value_set_array( BlueCValue* out, size_t count, BlueCError* err );

/* BLUE_VK_MAP of pair_count (NONE, NONE) pairs, filled in place */
BLUE_CAPI int32_t BLUE_CALL blue_value_set_map( BlueCValue* out, size_t pair_count, BlueCError* err );

/* A handle holding one reference to a copy of callback, for BLUE_VK_CALLBACK values; NULL
 * (err filled) when callback or its fn is NULL. destroy runs once, after the last release. */
BLUE_CAPI BlueCallbackHandle* BLUE_CALL blue_callback_create( const BlueCCallback* callback, BlueCError* err );
BLUE_CAPI void BLUE_CALL blue_callback_release( BlueCallbackHandle* handle );

/*--------------------------------------------------------------------------------------------
 * Errors
 *------------------------------------------------------------------------------------------*/

/* Free err's message and reset it to BLUE_OK; NULL is ignored */
BLUE_CAPI void BLUE_CALL blue_error_clear( BlueCError* err );

/* Set err to code with a copy of message[0, len) allocated by blue, for a host callback's
 * error. Returns code. */
BLUE_CAPI int32_t BLUE_CALL blue_set_error_message( BlueCError* err, int32_t code, const char* message, size_t len );

#ifdef __cplusplus
}
#endif

#endif /* BLUE_C_BLUE_H */
