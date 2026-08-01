// TODO: License

#pragma once

#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "IrrlichtDevice.h"

#ifndef _IRR_COMPILE_WITH_SDL_DEVICE_
#error SDL required for XR device
#endif
#include "CIrrDeviceSDL.h"

#include "OpenXR/IOpenXRConnector.h"

class CIrrDeviceXR : public CIrrDeviceSDL
{
public:

	//! constructor
	CIrrDeviceXR(const SIrrlichtCreationParameters& param);

	//! destructor
	virtual ~CIrrDeviceXR();

	//! Get the device type
	E_DEVICE_TYPE getType() const override
	{
		return EIDT_XR;
	}

	//! Activate device motion.
	bool activateDeviceMotion(float updateInterval = 0.016666f) override;

	//! Deactivate device motion.
	bool deactivateDeviceMotion() override;

	//! Is device motion active.
	bool isDeviceMotionActive() override;

	//! Is device motion available.
	bool isDeviceMotionAvailable() override;

	bool hasXR() const override;
	void recenterXR() override;
	void startXR() override;
	void xrGetInputState(core::XrInputState* state) override;
	bool beginFrame(const core::XrFrameConfig& config) override;
	bool nextView(core::XrViewInfo* info) override;
	void stopXR() override;

protected:
	std::unique_ptr<IOpenXRConnector> Connector;
	bool DeviceMotionActive;
};

#endif // _IRR_COMPILE_WITH_XR_DEVICE_
