#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include <cassert>
#include <unordered_set>

#include "Common.h"
#include "mt_opengl.h"
#include "os.h"
#include "IOpenXRConnector.h"
#include "IOpenXRInstance.h"
#include "OpenXRHeaders.h"

using std::unique_ptr;

class COpenXRConnector : public IOpenXRConnector {
	public:
		COpenXRConnector(video::IVideoDriver* driver, uint32_t mode_flags);
		bool init();
		virtual ~COpenXRConnector();
		virtual void startXR() override;
		virtual void stopXR() override;
		virtual void handleEvents() override;
		virtual void recenter() override;
		virtual void getInputState(core::XrInputState* state) override;
		virtual bool tryBeginFrame(const core::XrFrameConfig& config) override;
		virtual bool nextView(core::XrViewInfo* info) override;
	protected:
		video::IVideoDriver* VideoDriver;
		uint32_t ModeFlags;
		XrReferenceSpaceType PlaySpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		unique_ptr<IOpenXRInstance> Instance;
		// Retry every 10 seconds
		u32 InstanceRetryInterval = 10 * 1000;
		u32 InstanceRetryTime = 0;
		bool AppReady = false;
		void invalidateInstance();

		// Used to prevent automatic instance recreation
		// after the runtime forces exit.
		bool InstanceExited = false;
};

COpenXRConnector::COpenXRConnector(video::IVideoDriver* driver, uint32_t mode_flags)
	: VideoDriver(driver), ModeFlags(mode_flags)
{
	if (mode_flags & XRMF_ROOM_SCALE)
		PlaySpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	VideoDriver->grab();
}

bool COpenXRConnector::init() {
	Instance = createOpenXRInstance(VideoDriver, PlaySpaceType);
	if (!Instance)
		return false;
	return true;
}

COpenXRConnector::~COpenXRConnector()
{
	Instance = nullptr;
	VideoDriver->drop();
}

void COpenXRConnector::startXR()
{
	XR_ASSERT(!AppReady);
	AppReady = true;
	InstanceExited = false;
	if (Instance)
		Instance->setAppReady(true);
}

void COpenXRConnector::stopXR()
{
	XR_ASSERT(AppReady);
	AppReady = false;
	if (Instance)
		Instance->setAppReady(false);
}

void COpenXRConnector::invalidateInstance()
{
	os::Printer::log("[XR] Instance lost", ELL_ERROR);
	Instance = nullptr;
	InstanceRetryTime = os::Timer::getTime() + InstanceRetryInterval;
}

void COpenXRConnector::handleEvents()
{
	if (!Instance) {
		if (InstanceExited)
			return;
		u32 now = os::Timer::getTime();
		if (now > InstanceRetryTime) {
			Instance = createOpenXRInstance(VideoDriver, PlaySpaceType);
			InstanceRetryTime = now + InstanceRetryInterval;
			if (Instance && AppReady) {
				Instance->setAppReady(true);
			}
		}
	}
	if (!Instance)
		return;
	if (!Instance->handleEvents()) {
		invalidateInstance();
		InstanceExited = true;
	}
}

void COpenXRConnector::recenter()
{
	if (Instance)
		Instance->recenter();
}

void COpenXRConnector::getInputState(core::XrInputState* state)
{
	if (Instance)
		Instance->getInputState(state);
	else
		*state = core::XrInputState{};
}

bool COpenXRConnector::tryBeginFrame(const core::XrFrameConfig& config)
{
	if (!Instance)
		return false;
	bool didBegin = false;
	if (!Instance->internalTryBeginFrame(&didBegin, config)) {
		invalidateInstance();
		return false;
	}
	return didBegin;
}

bool COpenXRConnector::nextView(core::XrViewInfo* info)
{
	if (!Instance)
		return false;
	bool gotView = false;
	if (!Instance->internalNextView(&gotView, info)) {
		invalidateInstance();
		return false;
	}
	return gotView;
}

unique_ptr<IOpenXRConnector> createOpenXRConnector(video::IVideoDriver* driver, uint32_t mode_flags)
{
	unique_ptr<COpenXRConnector> conn(new COpenXRConnector(driver, mode_flags));
	if (!conn->init())
		return nullptr;
	return conn;
}

#endif // _IRR_COMPILE_WITH_XR_DEVICE_
