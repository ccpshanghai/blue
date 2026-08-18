// Copyright © 2021 CCP ehf.

#include "StdAfx.h"

#if __APPLE__

#include "BlueClipboard.h"
#include <TargetConditionals.h>

#if TARGET_OS_IPHONE

// iOS has no AppKit and no NSPasteboard. UIPasteboard is the equivalent, but its API is
// narrower: there is a single `string` property rather than typed pasteboard items, so the
// availableTypeFromArray / declareTypes dance below has no counterpart -- a nil `string`
// is the only "no text on the clipboard" signal, and the setter reports no success value.
#import <UIKit/UIKit.h>

BlueClipboard::OperationResult BlueClipboard::GetData( std::string& data ) const
{
    @autoreleasepool
    {
        NSString* string = [UIPasteboard generalPasteboard].string;
        if( !string )
        {
            return CLIPBOARD_INCOMPATIBLE_FORMAT;
        }
        data.resize( [string lengthOfBytesUsingEncoding:NSUTF8StringEncoding] );
        memcpy( &data[0], [string UTF8String], data.length() );
        return CLIPBOARD_OK;
    }
}

BlueClipboard::OperationResult BlueClipboard::GetData( std::wstring& result ) const
{
    @autoreleasepool
    {
        NSString* string = [UIPasteboard generalPasteboard].string;
        if( !string )
        {
            return CLIPBOARD_INCOMPATIBLE_FORMAT;
        }
        NSData* data = [string dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
        int32_t length = int32_t( [data length] ) / sizeof( wchar_t );
        auto characters = reinterpret_cast<const wchar_t*>( [data bytes] );
        result = std::wstring( characters, characters + length );
        return CLIPBOARD_OK;
    }
}

BlueClipboard::OperationResult BlueClipboard::SetData( const std::string& data )
{
    @autoreleasepool
    {
        NSString* string = [[NSString alloc] initWithBytesNoCopy:(void*)data.c_str()
                                                          length:data.length()
                                                        encoding:NSUTF8StringEncoding
                                                    freeWhenDone:NO];
        [UIPasteboard generalPasteboard].string = string;
        return CLIPBOARD_OK;
    }
}

BlueClipboard::OperationResult BlueClipboard::SetData( const std::wstring& data )
{
    @autoreleasepool
    {
        NSString* string = [[NSString alloc] initWithBytesNoCopy:(void*)data.c_str()
                                                          length:data.length() * sizeof( wchar_t )
                                                        encoding:NSUTF32LittleEndianStringEncoding
                                                    freeWhenDone:NO];
        [UIPasteboard generalPasteboard].string = string;
        return CLIPBOARD_OK;
    }
}

#else // macOS, unchanged below

#import <AppKit/AppKit.h>


BlueClipboard::OperationResult BlueClipboard::GetData( std::string& data ) const
{
    @autoreleasepool
    {
        auto pasteboard = [NSPasteboard generalPasteboard];

        auto supported = [pasteboard availableTypeFromArray:[NSArray arrayWithObject:NSPasteboardTypeString]];
        if( !supported )
        {
            return CLIPBOARD_INCOMPATIBLE_FORMAT;
        }
        NSString* string = [pasteboard stringForType:NSPasteboardTypeString];
        data.resize( [string lengthOfBytesUsingEncoding:NSUTF8StringEncoding] );
        memcpy( &data[0], [string UTF8String], data.length() );
        return CLIPBOARD_OK;
    }
}

BlueClipboard::OperationResult BlueClipboard::GetData( std::wstring& result ) const
{
    @autoreleasepool
    {
        auto pasteboard = [NSPasteboard generalPasteboard];

        auto supported = [pasteboard availableTypeFromArray:[NSArray arrayWithObject:NSPasteboardTypeString]];
        if( !supported )
        {
            return CLIPBOARD_INCOMPATIBLE_FORMAT;
        }
        NSString* string = [pasteboard stringForType:NSPasteboardTypeString];
        
        NSData* data = [string dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
        int32_t length = int32_t( [data length] ) / sizeof( wchar_t );
        auto characters = reinterpret_cast<const wchar_t*>( [data bytes] );
        result = std::wstring( characters, characters + length );
        return CLIPBOARD_OK;
    }
}

BlueClipboard::OperationResult BlueClipboard::SetData( const std::string& data )
{
    @autoreleasepool
    {
        auto pasteboard = [NSPasteboard generalPasteboard];
        NSString* string = [[NSString alloc] initWithBytesNoCopy:(void*)data.c_str()
                                                          length:data.length()
                                                        encoding:NSUTF8StringEncoding
                                                    freeWhenDone:NO];
        [pasteboard declareTypes:[NSArray arrayWithObject:NSPasteboardTypeString] owner:nil];
        auto success = [pasteboard setString:string forType:NSPasteboardTypeString];
        return success ? CLIPBOARD_OK : CLIPBOARD_FAILURE;
    }
}

BlueClipboard::OperationResult BlueClipboard::SetData( const std::wstring& data )
{
    @autoreleasepool
    {
        auto pasteboard = [NSPasteboard generalPasteboard];
        NSString* string = [[NSString alloc] initWithBytesNoCopy:(void*)data.c_str()
                                                          length:data.length() * sizeof( wchar_t )
                                                        encoding:NSUTF32LittleEndianStringEncoding
                                                    freeWhenDone:NO];
        [pasteboard declareTypes:[NSArray arrayWithObject:NSPasteboardTypeString] owner:nil];
        auto success = [pasteboard setString:string forType:NSPasteboardTypeString];
        return success ? CLIPBOARD_OK : CLIPBOARD_FAILURE;
    }
}

#endif // TARGET_OS_IPHONE

#endif