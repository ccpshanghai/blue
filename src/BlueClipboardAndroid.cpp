// Copyright © 2026 CCP ehf.

// Android has no clipboard reachable from native code, and this is the one platform in blue
// where that is a hard platform limit rather than a gap to be filled later.
//
// ClipboardManager lives in the Java framework and has no NDK counterpart -- there is no
// AIDL surface, no /dev node, nothing to bind to. Worse, since Android 10 a background app
// cannot read the clipboard at all: only the app that currently has input focus may, and that
// check happens inside the framework. So even a JNI implementation would be a bridge the
// caller has to supply an Activity for, not something this library can reach on its own.
//
// Every method therefore reports CLIPBOARD_FAILURE, which is an existing value in the enum
// and already means "the platform refused". The alternative -- an in-process buffer that
// looks like a clipboard but is invisible to every other app -- would make copy and paste
// appear to work while silently doing nothing, which is worse than a clean failure.
//
// BlueClipboard.cpp is #if _WIN32 and BlueClipboard.mm is Apple-only and set HEADER_FILE_ONLY
// off Apple, so without this file the three referenced methods are simply absent from the
// link. SetData( std::string ) is not referenced today; it is written anyway.

#include "StdAfx.h"

#if defined( __ANDROID__ )

#include "BlueClipboard.h"

BlueClipboard::OperationResult BlueClipboard::GetData( std::string& data ) const
{
	data.clear();
	return CLIPBOARD_FAILURE;
}

BlueClipboard::OperationResult BlueClipboard::GetData( std::wstring& data ) const
{
	data.clear();
	return CLIPBOARD_FAILURE;
}

BlueClipboard::OperationResult BlueClipboard::SetData( const std::string& data )
{
	( void )data;
	return CLIPBOARD_FAILURE;
}

BlueClipboard::OperationResult BlueClipboard::SetData( const std::wstring& data )
{
	( void )data;
	return CLIPBOARD_FAILURE;
}

#endif
