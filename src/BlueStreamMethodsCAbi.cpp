// Copyright © 2026 CCP ehf.
//
// IBlueStream's Read/read, Write/write and Seek/seek for the C-ABI backend: the twin of the
// Python thunker in BlueStreamMethods.cpp. Registered on the interface, so every object whose
// class maps IBlueStream answers to them, after its class's own methods.

#include "StdAfx.h"

#if BLUE_WITH_CABI

#include "IBluePersist.h"

namespace
{

using BlueCAbiDetail::CheckArgumentCount;
using BlueCAbiDetail::GetSelf;
using BlueCAbiDetail::Guard;

//--------------------------------------------------------------------
// Read( size = -1 ) -> bytes
//--------------------------------------------------------------------
int32_t StreamRead( BlueObject* self, const BlueCValue* args, uint32_t argc, BlueCValue* out, BlueCError* err )
{
	return Guard( out, err, [&]( BlueCValue& result, BlueCError& error ) -> int32_t {
		if( !CheckArgumentCount( args, argc, 1, 1, &error ) )
		{
			return error.code;
		}
		IBlueStream* stream = GetSelf<IBlueStream>( self, error );
		if( !stream )
		{
			return error.code;
		}
		ptrdiff_t size = -1;
		if( argc > 0 && !BlueCConvert<ptrdiff_t>::Extract( args[0], size, 0, &error ) )
		{
			return error.code;
		}

		// Missing or negative: the rest of the stream
		if( size < 0 )
		{
			size = stream->GetSize() - stream->GetPosition();
			if( size < 0 )
			{
				size = 0;
			}
		}

		// Read straight into the result; a short read shortens it
		char* buffer = BlueCAllocateBytes( size_t( size ), result, &error );
		if( !buffer )
		{
			return error.code;
		}
		if( size > 0 )
		{
			const ptrdiff_t read = stream->Read( buffer, size );
			if( read < 0 )
			{
				return BlueCSetError( &error, BLUE_E_FAILED, "reading %td bytes failed", size );
			}
			if( read < size )
			{
				result.u.str.len = size_t( read );
			}
		}
		return BLUE_OK;
	} );
}

//--------------------------------------------------------------------
// Write( data, size = -1 )
//--------------------------------------------------------------------
int32_t StreamWrite( BlueObject* self, const BlueCValue* args, uint32_t argc, BlueCValue* out, BlueCError* err )
{
	return Guard( out, err, [&]( BlueCValue&, BlueCError& error ) -> int32_t {
		if( !CheckArgumentCount( args, argc, 2, 1, &error ) )
		{
			return error.code;
		}
		IBlueStream* stream = GetSelf<IBlueStream>( self, error );
		if( !stream )
		{
			return error.code;
		}
		const void* data = nullptr;
		size_t length = 0;
		if( !BlueCExtractBytes( args[0], data, length, 0, &error ) )
		{
			return error.code;
		}
		ptrdiff_t size = -1;
		if( argc > 1 && !BlueCConvert<ptrdiff_t>::Extract( args[1], size, 1, &error ) )
		{
			return error.code;
		}

		// As in Python, the whole of data is written and size only matters when it is larger.
		// Python then reads past the end of the buffer; here that is an error.
		if( size > ptrdiff_t( length ) )
		{
			return BlueCSetArgumentError( &error, BLUE_E_INVALID_ARGUMENT, 1, "size %td is larger than the %zu bytes of data", size, length );
		}
		if( length > 0 )
		{
			const ptrdiff_t written = stream->Write( data, length );
			if( written != ptrdiff_t( length ) )
			{
				return BlueCSetError( &error, BLUE_E_FAILED, "writing %zu bytes failed (wrote %td)", length, written );
			}
		}
		return BLUE_OK;
	} );
}

//--------------------------------------------------------------------
// Seek( pos, whence = 0 )
//--------------------------------------------------------------------
int32_t StreamSeek( BlueObject* self, const BlueCValue* args, uint32_t argc, BlueCValue* out, BlueCError* err )
{
	return Guard( out, err, [&]( BlueCValue&, BlueCError& error ) -> int32_t {
		if( !CheckArgumentCount( args, argc, 2, 1, &error ) )
		{
			return error.code;
		}
		IBlueStream* stream = GetSelf<IBlueStream>( self, error );
		if( !stream )
		{
			return error.code;
		}
		ptrdiff_t position = 0;
		if( !BlueCConvert<ptrdiff_t>::Extract( args[0], position, 0, &error ) )
		{
			return error.code;
		}
		int32_t whence = IBlueStream::SO_BEGIN;
		if( argc > 1 && !BlueCConvert<int32_t>::Extract( args[1], whence, 1, &error ) )
		{
			return error.code;
		}
		if( whence != IBlueStream::SO_BEGIN && whence != IBlueStream::SO_CURRENT && whence != IBlueStream::SO_END )
		{
			return BlueCSetArgumentError( &error, BLUE_E_INVALID_ARGUMENT, 1, "whence %d is not 0 (begin), 1 (current) or 2 (end)", whence );
		}

		if( stream->Seek( position, IBlueStream::SeekOrigin( whence ) ) == -1 )
		{
			return BlueCSetError( &error, BLUE_E_FAILED, "seeking to %td (whence %d) failed", position, whence );
		}
		return BLUE_OK;
	} );
}

const char* const s_readDoc =
	"Reads bytes from the current position.\n"
	":param size: number of bytes to read; missing or negative reads the rest of the stream\n"
	":returns: the bytes read, fewer than size at the end of the stream";

const char* const s_writeDoc =
	"Writes all of data at the current position.\n"
	":param data: the bytes to write\n"
	":param size: ignored unless larger than data, which is an error";

const char* const s_seekDoc =
	"Moves the current position.\n"
	":param pos: the new position, relative to whence\n"
	":param whence: 0 from the beginning, 1 from the current position, 2 from the end";

const BlueCMethodDef s_streamMethods[] = {
	{ "Read", &StreamRead, s_readDoc, 1 },
	{ "read", &StreamRead, s_readDoc, 1 },
	{ "Write", &StreamWrite, s_writeDoc, 1 },
	{ "write", &StreamWrite, s_writeDoc, 1 },
	{ "Seek", &StreamSeek, s_seekDoc, 1 },
	{ "seek", &StreamSeek, s_seekDoc, 1 },
	{ nullptr, nullptr, nullptr, 0 },
};

// C signatures, in the type names of BlueCAbiTypeName.h, at the same index as s_streamMethods
const char* const s_readArgs[] = { "int64" };
const char* const s_writeArgs[] = { "bytes", "int64" };
const char* const s_seekArgs[] = { "int64", "int32" };

const BlueCSignature s_streamSignatures[] = {
	{ "bytes", s_readArgs, 1, 1 },
	{ "bytes", s_readArgs, 1, 1 },
	{ "void", s_writeArgs, 2, 1 },
	{ "void", s_writeArgs, 2, 1 },
	{ "void", s_seekArgs, 2, 1 },
	{ "void", s_seekArgs, 2, 1 },
};

} // namespace

BLUE_REGISTER_CTHUNKER_WITH_SIGNATURES( s_streamMethods, s_streamSignatures, GetIBlueStreamIID() );

#endif
