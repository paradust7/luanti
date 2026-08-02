#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#if defined(WIN32)
#	define XR_USE_PLATFORM_WIN32
#endif

#ifdef _IRR_COMPILE_WITH_OPENGL_
#	define XR_USE_GRAPHICS_API_OPENGL
#endif

#if defined(_IRR_COMPILE_WITH_OGLES2_)
#	define XR_USE_GRAPHICS_API_OPENGL_ES
#	if !defined(_IRR_COMPILE_WITH_XR_EGL_)
#		error "XR driver needs EGL for GLES. Set ENABLE_OPENXR_EGL to TRUE."
#	endif
#endif

#if defined(__ANDROID__)
#	define XR_USE_PLATFORM_ANDROID
#endif

#if defined(_IRR_COMPILE_WITH_XR_EGL_)
#	define XR_USE_PLATFORM_EGL
#endif

#if defined(__APPLE__)
#	error "XR driver does not support MacOSX / iOS"
#endif

#if defined(_IRR_COMPILE_WITH_XR_X11_)
#	define XR_USE_PLATFORM_XLIB
#endif

// Headers required for openxr_platform.h

#ifdef XR_USE_PLATFORM_WIN32
#	define WIN32_LEAN_AND_MEAN
#	include <Unknwn.h>
#	include <windows.h>
#endif

#ifdef XR_USE_PLATFORM_XLIB
#	include <X11/Xlib.h>
#	include <GL/glx.h>
#endif

#ifdef XR_USE_PLATFORM_EGL
#	include "EGL/egl.h"
#endif

#ifdef XR_USE_GRAPHICS_API_OPENGL
#	include <vendor/gl.h>
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#endif // _IRR_COMPILE_WITH_XR_DEVICE_
