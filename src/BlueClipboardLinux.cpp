// Copyright © 2026 CCP ehf.

// Desktop Linux has a clipboard, but blue has no route to it. Wayland only hands clipboard
// data to the client that owns a focused surface, over that client's own wl_display -- the
// shell's SDL connection, which blue never sees. Text input in the shell goes through SDL and
// ImGui, not through this class.
//
// So, as on Android, every method reports CLIPBOARD_FAILURE. Unlike Android this is a gap
// rather than a platform limit: a shell-supplied bridge to SDL_GetClipboardText would close it.

#include "StdAfx.h"

#if defined( __linux__ ) && !defined( __ANDROID__ )

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
