// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "COSOperator.h"

#ifdef _IRR_WINDOWS_API_
#include <windows.h>
#else
#include <cstring>
#include <unistd.h>
#ifndef _IRR_ANDROID_PLATFORM_
#include <sys/types.h>
#ifdef _IRR_OSX_PLATFORM_
#include <sys/sysctl.h>
#endif
#endif
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <mutex>
#include <string>
#endif

// "SDL_version.h" for SDL_VERSION_ATLEAST
#ifdef _IRR_USE_SDL3_
	#include <SDL3/SDL_clipboard.h>
	#include <SDL3/SDL_version.h>
#else
	#include <SDL_clipboard.h>
	#include <SDL_version.h>
#endif

#ifdef __EMSCRIPTEN__

namespace
{
// The clipboard, as far as the game is concerned: whatever was copied last,
// either in the game or elsewhere on the player's computer.
//
// SDL's clipboard is no use in the browser. Emscripten's video driver has no
// clipboard hooks, so SDL_SetClipboardText()/SDL_GetClipboardText() only ever
// see an internal buffer that nothing outside the tab can reach.
std::mutex clipboard_mutex;
std::string clipboard_text;
}

extern "C" {
	// Called by the page, from the browser thread, when the browser hands it
	// the system clipboard in a 'paste' event. The page follows it with
	// irrlicht_paste() (see CIrrDeviceSDL.cpp) to deliver the keypress.
	EMSCRIPTEN_KEEPALIVE
	void irrlicht_set_clipboard(const char *text);
}

void irrlicht_set_clipboard(const char *text)
{
	std::lock_guard<std::mutex> lock(clipboard_mutex);
	clipboard_text = text ? text : "";
}

#endif

// constructor
COSOperator::COSOperator()
{}

COSOperator::~COSOperator()
{
#ifdef _IRR_COMPILE_WITH_SDL_DEVICE_
	SDL_free(ClipboardSelectionText);
	SDL_free(PrimarySelectionText);
#endif
}

//! copies text to the clipboard
void COSOperator::copyToClipboard(const c8 *text) const
{
	if (strlen(text) == 0)
		return;

#if defined(__EMSCRIPTEN__)
	irrlicht_set_clipboard(text);

	// Hand it to the browser as well, so that it can be pasted outside the
	// tab. This needs a secure context, and a recent user gesture in some
	// browsers; if it is refused the text can still be pasted in the game.
	MAIN_THREAD_EM_ASM({
		if (navigator.clipboard && navigator.clipboard.writeText)
			navigator.clipboard.writeText(UTF8ToString($0)).catch(() => {});
	}, text);
#elif defined(_IRR_COMPILE_WITH_SDL_DEVICE_)
	SDL_SetClipboardText(text);
#endif
}

//! copies text to the primary selection
void COSOperator::copyToPrimarySelection(const c8 *text) const
{
	if (strlen(text) == 0)
		return;

#if defined(_IRR_COMPILE_WITH_SDL_DEVICE_)
#if SDL_VERSION_ATLEAST(2, 25, 0)
	SDL_SetPrimarySelectionText(text);
#endif
#endif
}

//! gets text from the clipboard
const c8 *COSOperator::getTextFromClipboard() const
{
#if defined(__EMSCRIPTEN__)
	// Copied out, so that the pointer stays good once the lock is dropped.
	std::lock_guard<std::mutex> lock(clipboard_mutex);
	ClipboardBuf = clipboard_text;
	return ClipboardBuf.c_str();
#elif defined(_IRR_COMPILE_WITH_SDL_DEVICE_)
	SDL_free(ClipboardSelectionText);
	ClipboardSelectionText = SDL_GetClipboardText();
	return ClipboardSelectionText;
#else

	return 0;
#endif
}

//! gets text from the primary selection
const c8 *COSOperator::getTextFromPrimarySelection() const
{
#if defined(_IRR_COMPILE_WITH_SDL_DEVICE_)
#if SDL_VERSION_ATLEAST(2, 25, 0)
	SDL_free(PrimarySelectionText);
	PrimarySelectionText = SDL_GetPrimarySelectionText();
	return PrimarySelectionText;
#endif
	return 0;

#else

	return 0;
#endif
}
