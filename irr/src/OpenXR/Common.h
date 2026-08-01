#pragma once

#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include <exception>
#include <string>

#include "OpenXRHeaders.h"

class OpenXRException : public std::exception
{
public:
	explicit OpenXRException(const char* message) throw() : Msg(message) {}
	explicit OpenXRException(const std::string& message) throw() : Msg(message) {}
	virtual ~OpenXRException() throw() {}

	virtual const char * what() const throw() override
	{
		return Msg.c_str();
	}
protected:
	std::string Msg;
};

extern bool openxr_check(XrInstance instance, XrResult result, const char* func);

#define XR_CHECK(method, ...) do { \
	if (!check(method(__VA_ARGS__), #method)) return false; \
} while (0)

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

// This assert is used for assertions that should always happen, even in release.
#define XR_ASSERT(expr) do { \
	if (!(expr)) { \
		os::Printer::log("[XR] Assertion failed: " #expr, ELL_ERROR); \
		os::Printer::log("[XR] File: " __FILE__, ELL_ERROR); \
		os::Printer::log("[XR] Line: " STRINGIFY(__LINE__), ELL_ERROR); \
		abort(); \
	} \
} while (0)

#endif
