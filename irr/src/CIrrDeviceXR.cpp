// TODO: License

#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "CIrrDeviceXR.h"
#include "OpenXR/IOpenXRConnector.h"

//! constructor
CIrrDeviceXR::CIrrDeviceXR(const SIrrlichtCreationParameters& param)
	: CIrrDeviceSDL(param), Connector(nullptr), DeviceMotionActive(false)

{
	if (!VideoDriver)
		// SDL was unable to initialize
		return;

	Connector = createOpenXRConnector(VideoDriver, 0);
	if (!Connector) {
		// Signal failure to createDeviceEx
		VideoDriver->drop();
		VideoDriver = 0;
		return;
	}
}


//! destructor
CIrrDeviceXR::~CIrrDeviceXR()
{
}

//! Activate device motion.
bool CIrrDeviceXR::activateDeviceMotion(float updateInterval)
{
	return true;
}

//! Deactivate device motion.
bool CIrrDeviceXR::deactivateDeviceMotion()
{
	return true;
}

//! Is device motion active.
bool CIrrDeviceXR::isDeviceMotionActive()
{
	return true;
}

//! Is device motion available.
bool CIrrDeviceXR::isDeviceMotionAvailable()
{
	return true;
}

bool CIrrDeviceXR::hasXR() const
{
	return true;
}

void CIrrDeviceXR::recenterXR()
{
	Connector->recenter();
}

void CIrrDeviceXR::xrGetInputState(core::XrInputState* state)
{
	Connector->getInputState(state);
}

void CIrrDeviceXR::startXR()
{
	Connector->startXR();
}

bool CIrrDeviceXR::beginFrame(const core::XrFrameConfig& config)
{
	Connector->handleEvents();
	if (!Connector->tryBeginFrame(config)) {
		return false;
	}
	return true;

}

bool CIrrDeviceXR::nextView(core::XrViewInfo* info)
{
	return Connector->nextView(info);
}

void CIrrDeviceXR::stopXR()
{
	Connector->stopXR();
}

#endif // _IRR_COMPILE_WITH_XR_DEVICE_
