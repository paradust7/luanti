// TODO: License

#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "CIrrDeviceXR.h"
#include "CXRManager.h"

#include "OpenXR/IOpenXRConnector.h"

#include "mt_opengl.h"


//! constructor
CIrrDeviceXR::CIrrDeviceXR(const SIrrlichtCreationParameters& param)
	: CIrrDeviceSDL(param), Connector(nullptr), DeviceMotionActive(false), InXR(false), In2D(true), In3D(false)
{
}

void CIrrDeviceXR::init()
{
	CIrrDeviceSDL::init();

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

void CIrrDeviceXR::createContextManager()
{
        ContextManager = new video::CXRManager(this);
        ContextManager->initialize(CreationParams, {});
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
	assert(!In3D);
	In3D = true;
	In2D = false;
	if (!InXR) {
		InXR = true;
		Connector->startXR();
	}
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
	assert(In3D && InXR);
	In3D = false;
	In2D = true;
	InXR = false;
	Connector->stopXR();
}

void CIrrDeviceXR::setFallbackRenderer(std::function<void()> cb)
{
	FallbackRenderer = std::move(cb);
}

void CIrrDeviceXR::beforeSwap()
{
	if (!In2D)
		return;

	if (!FallbackRenderer)
		return;

	// StartXR on first rendered frame
	if (!InXR) {
		InXR = true;
		Connector->startXR();
	}

	FallbackRenderer();
	GL.Flush();
}


#endif // _IRR_COMPILE_WITH_XR_DEVICE_
