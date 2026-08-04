// TODO: License

#pragma once

#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "IrrlichtDevice.h"

#ifndef _IRR_COMPILE_WITH_SDL_DEVICE_
#error SDL required for XR device
#endif
#include "CIrrDeviceSDL.h"

#include "OpenXR/IOpenXRConnector.h"

namespace video
{
	class CXRManager;
}

class CIrrDeviceXR : public CIrrDeviceSDL
{
public:

	//! constructor
	CIrrDeviceXR(const SIrrlichtCreationParameters& param);

	//! post-constructor initializer
	void init() override;

	//! destructor
	virtual ~CIrrDeviceXR();

	//! Get the device type
	E_DEVICE_TYPE getType() const override
	{
		return EIDT_XR;
	}

	virtual void createContextManager() override;

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
	void setFallbackRenderer(std::function<void()> cb) override;

protected:
	friend class video::CXRManager;
	void beforeSwap();

	std::unique_ptr<IOpenXRConnector> Connector;
	bool DeviceMotionActive;

	std::function<void()> FallbackRenderer;

	// The connector's startXR has been called
	bool InXR;

	// When Luanti is rendering 2D (loading screen, main menu, etc)
	// but not in game, we create a basic environment to display the menu.
	bool In2D;

	// Game is feeding 3D
	bool In3D;
};

#endif // _IRR_COMPILE_WITH_XR_DEVICE_
