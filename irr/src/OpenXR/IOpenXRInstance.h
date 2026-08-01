#pragma once

#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "IVideoDriver.h"
#include "OpenXRHeaders.h"
#include "IOpenXRConnector.h"

#include <cstdint>
#include <memory>

class IOpenXRInstance {
public:
	virtual ~IOpenXRInstance() {}
	virtual void setAppReady(bool ready) = 0;
	virtual bool handleEvents() = 0;
	virtual void recenter() = 0;
	virtual void getInputState(core::XrInputState* state) = 0;
	virtual bool internalTryBeginFrame(bool *didBegin, const core::XrFrameConfig& config) = 0;
	virtual bool internalNextView(bool *gotView, core::XrViewInfo* info) = 0;
};

std::unique_ptr<IOpenXRInstance> createOpenXRInstance(
	video::IVideoDriver* driver,
	XrReferenceSpaceType playSpaceType);

#endif // _IRR_COMPILE_WITH_XR_DEVICE_
