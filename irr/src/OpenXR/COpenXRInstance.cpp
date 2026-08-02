#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "os.h"
#include "IOpenXRInstance.h"
#include "IOpenXRSession.h"
#include "Common.h"

#include <cassert>
#include <memory>

#ifdef _IRR_USE_SDL3_
#include <SDL3/SDL_video.h>
#else
#include <SDL_video.h>
#endif

#ifndef XR_API_VERSION_1_0
#define XR_API_VERSION_1_0 XR_MAKE_VERSION(1, 0, XR_VERSION_PATCH(XR_CURRENT_API_VERSION))
#endif

using std::unique_ptr;

class COpenXRInstance : public IOpenXRInstance {
public:
	COpenXRInstance(video::IVideoDriver* driver, XrReferenceSpaceType playSpaceType)
		: VideoDriver(driver),
		  PlaySpaceType(playSpaceType)
	{
		VideoDriver->grab();
	}
	bool init();
	virtual ~COpenXRInstance() override;
	virtual void setAppReady(bool ready) override;
	virtual bool handleEvents() override;
	virtual void recenter() override;
	virtual void getInputState(core::XrInputState* state) override;
	virtual bool internalTryBeginFrame(bool *didBegin, const core::XrFrameConfig& config) override;
	virtual bool internalNextView(bool *gotView, core::XrViewInfo* info) override;
protected:
	bool loadExtensions();
	bool createInstance();
	bool tryCreateSession();
	bool check(XrResult result, const char* func)
	{
		return openxr_check(Instance, result, func);
	}
	void invalidateSession();

	video::IVideoDriver* VideoDriver;
	XrReferenceSpaceType PlaySpaceType;

	// Supported extensions
	std::vector<XrExtensionProperties> Extensions;
	std::unordered_set<std::string> ExtensionNames;

	XrInstance Instance = XR_NULL_HANDLE;
	XrInstanceProperties InstanceProperties;

	unique_ptr<IOpenXRSession> Session;
	u32 SessionRetryInterval = 5 * 1000;
	u32 SessionRetryTime = 0;
	bool AppReady = false;
};


bool COpenXRInstance::init()
{
	if (!loadExtensions()) return false;
	if (!createInstance()) return false;
	XR_ASSERT(Instance != XR_NULL_HANDLE);
	if (!tryCreateSession()) return false;
	return true;
}

COpenXRInstance::~COpenXRInstance()
{
	Session = nullptr;
	if (Instance != XR_NULL_HANDLE)
		xrDestroyInstance(Instance);
	VideoDriver->drop();
}

void COpenXRInstance::invalidateSession()
{
	os::Printer::log("[XR] Session lost", ELL_ERROR);
	Session = nullptr;
	SessionRetryTime = os::Timer::getTime() + SessionRetryInterval;
}

bool COpenXRInstance::loadExtensions()
{

	os::Printer::log("[XR] Initializing OpenXR", ELL_INFORMATION);

	XrResult result;
	uint32_t extCount = 0;
	result = xrEnumerateInstanceExtensionProperties(NULL, 0, &extCount, NULL);
	check(result, "xrEnumerateInstanceExtensionProperties");

	Extensions.resize(extCount, { XR_TYPE_EXTENSION_PROPERTIES, nullptr });
	result = xrEnumerateInstanceExtensionProperties(NULL, extCount, &extCount, Extensions.data());
	check(result, "xrEnumerateInstanceExtensionProperties");

	os::Printer::log("[XR] Supported extensions:", ELL_INFORMATION);
	for (const auto& extension : Extensions) {
		char buf[32 + XR_MAX_EXTENSION_NAME_SIZE];
		snprintf_irr(buf, sizeof(buf), "[XR]   %s", extension.extensionName);
		os::Printer::log(buf, ELL_INFORMATION);
		ExtensionNames.emplace(extension.extensionName);
	}
	return true;
}

bool COpenXRInstance::createInstance()
{
	std::vector<const char*> extensionsToEnable;
	if (!ExtensionNames.count(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME)) {
		os::Printer::log("OpenXR runtime does not support depth composition layer");
		return false;
	}
	extensionsToEnable.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);


	const char* raw_sdl_driver = SDL_GetCurrentVideoDriver();
	std::string sdl_driver = raw_sdl_driver ? raw_sdl_driver : "";
	(void)sdl_driver;

	video::E_DRIVER_TYPE driverType = VideoDriver->getDriverType();
	(void)driverType;

#ifdef XR_USE_GRAPHICS_API_OPENGL
	if (driverType == video::EDT_OPENGL || driverType == video::EDT_OPENGL3) {
		if (!ExtensionNames.count(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) {
			os::Printer::log("OpenXR runtime does not support OpenGL", ELL_ERROR);
			return false;
		}
		extensionsToEnable.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
	}
#endif

#ifdef XR_USE_GRAPHICS_API_OPENGL_ES
	if (driverType == video::EDT_OGLES2) {
		if (!ExtensionNames.count(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
			os::Printer::log("OpenXR runtime does not support OpenGL ES", ELL_ERROR);
			return false;
		}
		extensionsToEnable.push_back(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
	}
#endif

#ifdef XR_USE_PLATFORM_EGL
	if (driverType == video::EDT_OGLES2 || sdl_driver == "wayland") {
		if (!ExtensionNames.count(XR_MNDX_EGL_ENABLE_EXTENSION_NAME)) {
			os::Printer::log("OpenXR runtime does not support EGL", ELL_ERROR);
			return false;
		}
		extensionsToEnable.push_back(XR_MNDX_EGL_ENABLE_EXTENSION_NAME);
	}
#endif

	XrInstanceCreateInfo info = {
		XR_TYPE_INSTANCE_CREATE_INFO,
		nullptr,
		0,
		{
			"Minetest", 1, "", 0, XR_API_VERSION_1_0,
		},
		0,
		NULL,
		(uint32_t)extensionsToEnable.size(),
		extensionsToEnable.data()
	};
	XR_CHECK(xrCreateInstance, &info, &Instance);

	XrInstanceProperties instanceProperties = {
		.type = XR_TYPE_INSTANCE_PROPERTIES,
	};
	XR_CHECK(xrGetInstanceProperties, Instance, &instanceProperties);

	// Print out some info
	char buf[128 + XR_MAX_RUNTIME_NAME_SIZE];
	snprintf_irr(buf, sizeof(buf), "[XR] OpenXR Runtime: %s", instanceProperties.runtimeName);
	os::Printer::log(buf, ELL_INFORMATION);
	snprintf_irr(buf, sizeof(buf), "[XR] OpenXR Version: %d.%d.%d",
		XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
		XR_VERSION_MINOR(instanceProperties.runtimeVersion),
		XR_VERSION_PATCH(instanceProperties.runtimeVersion));
	os::Printer::log(buf, ELL_INFORMATION);
	return true;
}

void COpenXRInstance::setAppReady(bool ready)
{
	AppReady = ready;
	if (Session) {
		if (!Session->setAppReady(ready)) {
			// Session has to destroy itself to do fast termination.
			invalidateSession();
			SessionRetryTime = os::Timer::getTime();
		}
	}
}

bool COpenXRInstance::handleEvents()
{
	// Try reviving the session
	if (!Session) {
                u32 now = os::Timer::getTime();
                if (now > SessionRetryTime) {
			tryCreateSession();
			SessionRetryTime = now + SessionRetryInterval;
			if (Session && AppReady) {
				Session->setAppReady(AppReady);
			}
		}
	}

	for (;;) {
		XrEventDataBuffer event = {
			.type = XR_TYPE_EVENT_DATA_BUFFER,
		};
		XrResult result = xrPollEvent(Instance, &event);
		if (result == XR_EVENT_UNAVAILABLE) {
			// No more events
			break;
		} else if (result != XR_SUCCESS) {
			// Called to log the error
			check(result, "xrPollEvent");
			return false;
		}
		switch (event.type) {
		case XR_TYPE_EVENT_DATA_EVENTS_LOST:
			os::Printer::log("[XR] OpenXR event queue overflowed, lost events", ELL_ERROR);
			break;
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			os::Printer::log("[XR] Disconnected (lost instance)", ELL_ERROR);
			return false;
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			XrEventDataSessionStateChanged* e = (XrEventDataSessionStateChanged*)&event;
			if (Session) {
				if (!Session->handleStateChange(e)) {
					invalidateSession();
				}
			}
			if (e->state == XR_SESSION_STATE_EXITING) {
				// Force instance destroy
				return false;
			}
			break;
		}
		default:
			break;
		}
	}
	return true;
}

void COpenXRInstance::recenter()
{
	if (Session)
		Session->recenter();
}

void COpenXRInstance::getInputState(core::XrInputState* state)
{
	if (Session)
		Session->getInputState(state);
	else
		*state = core::XrInputState{};
}

bool COpenXRInstance::tryCreateSession()
{
	XR_ASSERT(!Session);
	Session = createOpenXRSession(Instance, VideoDriver, PlaySpaceType);
	if (!Session)
		return false;
	return true;
}

bool COpenXRInstance::internalTryBeginFrame(bool *didBegin, const core::XrFrameConfig& config)
{
	if (!Session) {
		*didBegin = false;
		return true;
	}
	if (!Session->internalTryBeginFrame(didBegin, config)) {
		invalidateSession();
		*didBegin = false;
		return true;
	}
	return true;
}

bool COpenXRInstance::internalNextView(bool *gotView, core::XrViewInfo* info)
{
	if (!Session) {
		*gotView = false;
		return true;
	}
	if (!Session->internalNextView(gotView, info)) {
		invalidateSession();
		*gotView = false;
		return true;
	}
	return true;
}

unique_ptr<IOpenXRInstance> createOpenXRInstance(
	video::IVideoDriver* driver,
	XrReferenceSpaceType playSpaceType)
{
	unique_ptr<COpenXRInstance> obj(new COpenXRInstance(driver, playSpaceType));
	if (!obj->init())
		return nullptr;
	return obj;
}

#endif // _IRR_COMPILE_WITH_XR_DEVICE_
