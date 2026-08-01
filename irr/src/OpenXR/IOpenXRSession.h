#pragma once
#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "IOpenXRConnector.h"
#include "OpenXRHeaders.h"

#include <memory>

class IOpenXRSession {
public:
	virtual ~IOpenXRSession() {}

	virtual bool setAppReady(bool ready) = 0;
	virtual void recenter() = 0;
	virtual void getInputState(core::XrInputState* state) = 0;
	virtual bool internalTryBeginFrame(bool *didBegin, const core::XrFrameConfig& config) = 0;
	virtual bool internalNextView(bool *gotView, core::XrViewInfo* info) = 0;
	virtual bool handleStateChange(XrEventDataSessionStateChanged *ev) = 0;
};

std::unique_ptr<IOpenXRSession> createOpenXRSession(
	XrInstance instance,
	video::IVideoDriver* driver,
	XrReferenceSpaceType playSpaceType);

#endif // _IRR_COMPILE_WITH_XR_DEVICE_
