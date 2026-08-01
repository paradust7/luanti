#pragma once
#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "IOpenXRConnector.h"
#include "OpenXRHeaders.h"

class IOpenXRInput {
public:
	virtual ~IOpenXRInput() {}

	virtual bool updateState(XrSessionState sessionState, XrTime predictedDisplayTime, XrSpace baseSpace) = 0;
	virtual void getInputState(core::XrInputState* state) = 0;
};

std::unique_ptr<IOpenXRInput> createOpenXRInput(
        XrInstance instance,
	XrSession session);

#endif // _IRR_COMPILE_WITH_XR_DEVICE_
