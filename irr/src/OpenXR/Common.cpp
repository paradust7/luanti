#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "os.h"
#include "OpenXRHeaders.h"

bool openxr_check(XrInstance instance, XrResult result, const char* func)
{
	if (result == XR_SUCCESS)
		return true;

	if (!instance && result == XR_ERROR_RUNTIME_FAILURE)
	{
		os::Printer::log(
			"Failed to connect to OpenXR runtime!\n"
			"Ensure that your XR provider (e.g. SteamVR)\n"
			"is running and has OpenXR enabled.",
			ELL_ERROR);
		return false;
	}

	char buf[XR_MAX_RESULT_STRING_SIZE];
	if (instance && xrResultToString(instance, result, buf) == XR_SUCCESS) {
		// buf was written
	} else {
		snprintf_irr(buf, sizeof(buf), "XR_ERROR(%d)", (int)result);
	}

	std::string text = func;
	text += " error: ";
	text += buf;
	os::Printer::log(text.c_str(), ELL_ERROR);
	return false;
}

#endif // _IRR_COMPILE_WITH_XR_DEVICE_

