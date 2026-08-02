#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "os.h"
#include "IOpenXRConnector.h"
#include "IOpenXRSession.h"
#include "IOpenXRSwapchain.h"
#include "IOpenXRInput.h"
#include "Common.h"
#include "OpenXRMath.h"

#ifdef _IRR_USE_SDL3_
#include <SDL3/SDL_video.h>
#else
#include <SDL_video.h>
#endif

using std::unique_ptr;

uint64_t XrFrameCounter = 0;

class COpenXRSession : public IOpenXRSession {
public:
	COpenXRSession(
		XrInstance instance,
		video::IVideoDriver* driver,
		XrReferenceSpaceType playSpaceType)
		: Instance(instance), VideoDriver(driver), PlaySpaceType(playSpaceType)
	{
		VideoDriver->grab();
	}
	virtual ~COpenXRSession() {
		// Order is important!
		Input.reset();
		ViewLayers.clear();
		resetViewChains();
		resetHudChain();
		if (BasePlaySpace != XR_NULL_HANDLE)
			xrDestroySpace(BasePlaySpace);
		if (ViewSpace != XR_NULL_HANDLE)
			xrDestroySpace(ViewSpace);
		if (PlaySpace != XR_NULL_HANDLE)
			xrDestroySpace(PlaySpace);
		if (Session != XR_NULL_HANDLE)
			xrDestroySession(Session);
		VideoDriver->drop();
	}

	virtual bool setAppReady(bool ready) override;
	virtual void recenter() override;
	virtual void getInputState(core::XrInputState* state) override;
	virtual bool internalTryBeginFrame(bool *didBegin, const core::XrFrameConfig& config) override;
	virtual bool internalNextView(bool *gotView, core::XrViewInfo* info) override;
	virtual bool handleStateChange(XrEventDataSessionStateChanged *ev) override;
	bool init();
	bool endFrame();
protected:
	bool getSystem();
	bool getViewConfigs();
	bool setupViews();
	bool verifyGraphics();
	bool createSession();
	bool setupSpaces();
	bool setupViewChains();
	bool setupHudChain();
	bool setupCompositionLayers();
	bool setupInput();

	void resetViewChains();
	void resetHudChain();

	bool beginSession();
	bool waitFrame();
	bool endSession();

	bool recenterPlaySpace(XrTime ref);

	XrInstance Instance;
	video::IVideoDriver* VideoDriver;
	XrReferenceSpaceType PlaySpaceType;

	// System
	XrSystemId SystemId = XR_NULL_SYSTEM_ID;
	XrSystemProperties SystemProps;

	// Supported View Configurations (mono, stereo, etc)
	std::vector<XrViewConfigurationType> ViewConfigTypes;
	std::vector<XrViewConfigurationProperties> ViewConfigProperties;

	XrSession Session = XR_NULL_HANDLE;

	unique_ptr<IOpenXRInput> Input;

	// Parameters for the view config we're using
	// For stereo, this has left and right eyes
	XrViewConfigurationType ViewType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	std::vector<XrViewConfigurationView> ViewConfigs;

	// Set by setupSpaces()
	XrSpace BasePlaySpace = XR_NULL_HANDLE;
	XrSpace PlaySpace = XR_NULL_HANDLE;
	XrPosef PlaySpaceOffset = IdentityPose;
	float YawOffset = 0.0f;
	XrSpace ViewSpace = XR_NULL_HANDLE;
	bool DoRecenter = false;

	// Initialized by getSwapchainFormats
	// Ordered by optimal performance/quality (best first)
	std::vector<int64_t> SupportedFormats;
	int64_t ColorFormat;
	int64_t DepthFormat;
	float ZNear = 1.0f;
	float ZFar = 20000.f;

	struct ViewChainData {
		// Initialized by setupViewChains
                unique_ptr<IOpenXRSwapchain> Swapchain;
                unique_ptr<IOpenXRSwapchain> DepthSwapchain;

		// JANK ALERT
		// IRenderTarget groups together a framebuffer (FBO), texture, and depth/stencil texture.
		// But OpenXR acquires textures and depth textures independently. Their association is
		// not permanent.
		//
		// As a compromise, these render targets will always be bound to the same FBO and texture,
		// but their depth texture may be updated every frame.
		std::vector<video::IRenderTarget*> RenderTargets;

		// Initialized by setupCompositionLayers
		// `Layers` holds pointers to these structs
		XrCompositionLayerDepthInfoKHR DepthInfo;
	};
	std::vector<ViewChainData> ViewChains;

	// HUD Swapchain
	struct HudChainData {
		unique_ptr<IOpenXRSwapchain> Swapchain;
		unique_ptr<IOpenXRSwapchain> DepthSwapchain;
		std::vector<video::IRenderTarget*> RenderTargets;
	};
	HudChainData HudChain;
	uint32_t HudWidth = 1280;
	uint32_t HudHeight = 1024;

	// Initialized by setupCompositionLayers
	std::vector<XrCompositionLayerProjectionView> ViewLayers;

	XrSessionState SessionState = XR_SESSION_STATE_IDLE;
	bool Running = false;
	bool AppReady = false;
	bool DidWaitFrame = false;

	// ----------------------------------------------
	// These are only valid when InFrame is true
	bool InFrame = false;
	core::XrFrameConfig FrameConfig; // provided by the app
	bool RenderHud;
	uint32_t NextViewIndex = 0;
	XrFrameState FrameState;
	XrViewState ViewState;
	XrVector3f ViewCenter;
	std::vector<XrView> ViewInfo;
	// ----------------------------------------------

	bool check(XrResult result, const char* func)
	{
		return openxr_check(Instance, result, func);
	}
};

bool COpenXRSession::init()
{
	if (!getSystem()) return false;
	if (!getViewConfigs()) return false;
	if (!setupViews()) return false;
	if (!verifyGraphics()) return false;
	if (!createSession()) return false;
	// TODO: Initialize hand tracking
	if (!setupSpaces()) return false;
	if (!setupViewChains()) return false;
	if (!setupHudChain()) return false;
	if (!setupCompositionLayers()) return false;
	if (!setupInput()) return false;
	return true;
}

bool COpenXRSession::getSystem()
{
	XrFormFactor formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemGetInfo getInfo = {
		.type = XR_TYPE_SYSTEM_GET_INFO,
		.formFactor = formFactor,
	};
	XR_CHECK(xrGetSystem, Instance, &getInfo, &SystemId);

	SystemProps = XrSystemProperties{
		.type = XR_TYPE_SYSTEM_PROPERTIES,
	};
	XR_CHECK(xrGetSystemProperties, Instance, SystemId, &SystemProps);

	// Print out information about the system
	char buf[128 + XR_MAX_SYSTEM_NAME_SIZE];
	snprintf_irr(buf, sizeof(buf), "[XR] HMD: %s", SystemProps.systemName);
	os::Printer::log(buf, ELL_INFORMATION);

	snprintf_irr(buf, sizeof(buf), "[XR] Vendor id: %u", SystemProps.vendorId);
	os::Printer::log(buf, ELL_INFORMATION);

	snprintf_irr(buf, sizeof(buf), "[XR] Graphics: max swapchain %u x %u; %u composition layers",
		SystemProps.graphicsProperties.maxSwapchainImageWidth,
		SystemProps.graphicsProperties.maxSwapchainImageHeight,
		SystemProps.graphicsProperties.maxLayerCount);
	os::Printer::log(buf, ELL_INFORMATION);

	const char *tracking = "None";
	bool orientationTracking = SystemProps.trackingProperties.orientationTracking;
	bool positionTracking = SystemProps.trackingProperties.positionTracking;
	if (orientationTracking && positionTracking)
		tracking = "Orientation and Position";
	else if (orientationTracking)
		tracking = "Orientation only";
	else if (positionTracking)
		tracking = "Position only";
	snprintf_irr(buf, sizeof(buf), "[XR] Tracking: %s", tracking);
	os::Printer::log(buf, ELL_INFORMATION);

	return true;
}

bool COpenXRSession::getViewConfigs()
{
	uint32_t count = 0;
	XR_CHECK(xrEnumerateViewConfigurations, Instance, SystemId, 0, &count, NULL);

	ViewConfigTypes.clear();
	ViewConfigTypes.resize(count);
	XR_CHECK(xrEnumerateViewConfigurations, Instance, SystemId, count, &count, ViewConfigTypes.data());
	ViewConfigTypes.resize(count);

	// Fetch viewconfig properties
	ViewConfigProperties.clear();
	ViewConfigProperties.resize(count, {
		.type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES,
	});
	for (uint32_t i = 0; i < count; i++) {
		XR_CHECK(xrGetViewConfigurationProperties, Instance, SystemId, ViewConfigTypes[i], &ViewConfigProperties[i]);
	}

	// Print out some info
	for (const auto &prop : ViewConfigProperties) {
		char buf[128];
		const char *view = "other";
		switch (prop.viewConfigurationType) {
		case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO: view = "mono"; break;
		case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO: view = "stereo"; break;
		default: break;
		}
		snprintf_irr(buf, sizeof(buf), "[XR] Supported view: %s [type=%d, fovMutable=%s]",
			view, prop.viewConfigurationType, prop.fovMutable ? "yes" : "no");
		os::Printer::log(buf, ELL_INFORMATION);
	}
	return true;
}

bool COpenXRSession::setupViews()
{
	uint32_t count = 0;
	XR_CHECK(xrEnumerateViewConfigurationViews, Instance, SystemId, ViewType, 0, &count, NULL);

	ViewConfigs.clear();
	ViewConfigs.resize(count, { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW});
	XR_CHECK(xrEnumerateViewConfigurationViews, Instance, SystemId, ViewType, count, &count, ViewConfigs.data());
	ViewConfigs.resize(count);

	// Print out info
	os::Printer::log("[XR] Using stereo view", ELL_INFORMATION);
	for (uint32_t i = 0; i < ViewConfigs.size(); i++) {
		const auto &conf = ViewConfigs[i];
		char buf[256];
		snprintf_irr(buf, sizeof(buf),
			"[XR] View %d: Recommended/Max Resolution %dx%d/%dx%d, Swapchain samples %d/%d",
			i,
			conf.recommendedImageRectWidth,
			conf.recommendedImageRectHeight,
			conf.maxImageRectWidth,
			conf.maxImageRectHeight,
			conf.recommendedSwapchainSampleCount,
			conf.maxSwapchainSampleCount);
		os::Printer::log(buf, ELL_INFORMATION);
	}
	return true;
}

bool COpenXRSession::verifyGraphics()
{
	// OpenXR requires checking graphics compatibility before creating a session.
	// xrGetInstanceProcAddr must be used, since these methods might load in dynamically.
	XrVersion minApiVersionSupported = 0;
	XrVersion maxApiVersionSupported = 0;
	video::E_DRIVER_TYPE irrDriverType = VideoDriver->getDriverType();
	bool isGL = (
		irrDriverType == video::EDT_OPENGL ||
		irrDriverType == video::EDT_OPENGL3);
	bool isGLES = (irrDriverType == video::EDT_OGLES2);

#ifdef XR_USE_GRAPHICS_API_OPENGL
	if (isGL) {
		PFN_xrGetOpenGLGraphicsRequirementsKHR pfn_xrGetOpenGLGraphicsRequirementsKHR = nullptr;
		XR_CHECK(xrGetInstanceProcAddr, Instance, "xrGetOpenGLGraphicsRequirementsKHR",
			(PFN_xrVoidFunction*)&pfn_xrGetOpenGLGraphicsRequirementsKHR);

		XrGraphicsRequirementsOpenGLKHR reqs = {
			.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR,
		};
		XR_CHECK(pfn_xrGetOpenGLGraphicsRequirementsKHR, Instance, SystemId, &reqs);
		minApiVersionSupported = reqs.minApiVersionSupported;
		maxApiVersionSupported = reqs.maxApiVersionSupported;
	}
#endif

#ifdef XR_USE_GRAPHICS_API_OPENGL_ES
	if (isGLES) {
		PFN_xrGetOpenGLESGraphicsRequirementsKHR pfn_xrGetOpenGLESGraphicsRequirementsKHR = nullptr;
		XR_CHECK(xrGetInstanceProcAddr, Instance, "xrGetOpenGLESGraphicsRequirementsKHR",
			(PFN_xrVoidFunction*)&pfn_xrGetOpenGLESGraphicsRequirementsKHR);

		XrGraphicsRequirementsOpenGLESKHR reqs = {
			.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR,
		};
		XR_CHECK(pfn_xrGetOpenGLESGraphicsRequirementsKHR, Instance, SystemId, &reqs);
		minApiVersionSupported = reqs.minApiVersionSupported;
		maxApiVersionSupported = reqs.maxApiVersionSupported;
	}
#endif

	char buf[128];
	snprintf_irr(buf, sizeof(buf),
		"[XR] OpenXR supports OpenGL%s version range (%d.%d.%d, %d.%d.%d)",
		isGLES ? "ES" : "",
		XR_VERSION_MAJOR(minApiVersionSupported),
		XR_VERSION_MINOR(minApiVersionSupported),
		XR_VERSION_PATCH(minApiVersionSupported),
		XR_VERSION_MAJOR(maxApiVersionSupported),
		XR_VERSION_MINOR(maxApiVersionSupported),
		XR_VERSION_PATCH(maxApiVersionSupported));
	os::Printer::log(buf, ELL_INFORMATION);

	int glmajor = 0;
	int glminor = 0;
	int glmask = 0;
	SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &glmajor);
	SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &glminor);
	SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &glmask);
	XrVersion sdl_gl_version = XR_MAKE_VERSION(glmajor, glminor, 0);
	bool sdl_is_gles = glmask & SDL_GL_CONTEXT_PROFILE_ES;

	snprintf_irr(buf, sizeof(buf),
		"[XR] SDL is configured for OpenGL%s %d.%d.%d",
		sdl_is_gles ? "ES" : "",
		glmajor,
		glminor,
		glmask);
	os::Printer::log(buf, ELL_INFORMATION);

	if (sdl_is_gles != isGLES) {
		os::Printer::log("[XR] Unexpected ES profile mismatch (OpenGL <> OpenGLES)", ELL_ERROR);
		return false;
	}

	if (sdl_gl_version < minApiVersionSupported || sdl_gl_version > maxApiVersionSupported) {
		os::Printer::log("[XR] OpenGL initialized with incompatible version", ELL_ERROR);
		return false;
	}
	return true;
}

#ifdef XR_USE_PLATFORM_EGL

static bool FindCurrentEGLConfig(EGLConfig *found) {
	EGLDisplay display = eglGetCurrentDisplay();
	EGLContext context = eglGetCurrentContext();

	if (context == EGL_NO_CONTEXT) {
		os::Printer::log("Failed eglGetCurrentContext", ELL_ERROR);
		return false;
	}

	if (display == EGL_NO_DISPLAY) {
		os::Printer::log("Failed eglGetCurrentDisplay", ELL_ERROR);
		return false;
	}

	EGLint config_id = 0;
	if (!eglQueryContext(display, context, EGL_CONFIG_ID, &config_id)) {
		os::Printer::log("Failed eglQueryContext", ELL_ERROR);
		return false;
	}

	EGLint config_count = 0;
	if (!eglGetConfigs(display, NULL, 0, &config_count)) {
		os::Printer::log("Failed eglGetConfigs", ELL_ERROR);
		return false;
	}

	std::vector<EGLConfig> configs(config_count);
	if (!eglGetConfigs(display, configs.data(), config_count, &config_count)) {
		os::Printer::log("Failed eglGetConfigs", ELL_ERROR);
		return false;
	}

	for (EGLint i = 0; i < config_count; ++i) {
		EGLint candidate_id = 0;
		if (eglGetConfigAttrib(display, configs[i], EGL_CONFIG_ID, &candidate_id)) {
			if (candidate_id == config_id) {
				*found = configs[i];
				return true;
			}
		}
	}
	os::Printer::log("Failed to find EGL config", ELL_ERROR);
	return false;
}

#endif

// SDL and OpenXR don't know how to talk to each other
//
// For them to work together, it is necessary to pass
// the raw GL and display context(s) from SDL to OpenXR.
//
// What these values are/mean depends on the underlying
// windowing API.
//
// SDL2 doesn't expose these values in any way.
// SDL3 exposes them indirectly, but we still need to
// know the exact API that SDL used to do it correctly.
//
// These are the possibilities:
//
//     Windows + OpenGL         -> WGL, [1]EGL
//     Windows + OpenGLES       -> EGL, [1]WGL
//     X11 + OpenGL             -> GLX, [1]EGL
//     X11 + OpenGLES           -> EGL, [1]GLX
//     Wayland Native           -> EGL
//     Apple + OpenGL           -> [2]CGL
//
// [1] = Technically possible but not implemented
// [2] = Not supported yet
//
bool COpenXRSession::createSession()
{
	XrSessionCreateInfo session_create_info = {
		.type = XR_TYPE_SESSION_CREATE_INFO,
		.next = nullptr, // to be filled in
		.systemId = SystemId,
	};

	const char* raw_sdl_driver = SDL_GetCurrentVideoDriver();
	std::string sdl_driver = raw_sdl_driver ? raw_sdl_driver : "";
	video::E_DRIVER_TYPE irrDriverType = VideoDriver->getDriverType();
	bool isGL = (
		irrDriverType == video::EDT_OPENGL ||
		irrDriverType == video::EDT_OPENGL3);
	bool isGLES = (irrDriverType == video::EDT_OGLES2);

	// Suppress warning in case no platform uses it.
	(void)isGL;
	(void)isGLES;

/* TODO(paradust7): SDL3
	SDL_GLContext sdlContext = SDL_GL_GetCurrentContext();
	if (!sdlContext) {
		os::Printer::log("[XR] Unable to get SDL GL context", ELL_ERROR);
		return false;
	}

	SDL_Window *sdlWindow = SDL_GL_GetCurrentWindow();
	if (!sdlWindow) {
		os::Printer::log("[XR] Unable to find SDL Window", ELL_ERROR);
		return false;
	}

	SDL_PropertiesID propId = SDL_GetWindowProperties(sdlWindow);
	if (!propId) {
		os::Printer::log("[XR] Unable to get SDL window properties", ELL_ERROR);
		return false;
	}
*/

	{
		char buf[256];
		snprintf_irr(buf, sizeof(buf), "[XR] SDL driver is '%s'", sdl_driver.c_str());
		os::Printer::log(buf, ELL_INFORMATION);
	}

#ifdef XR_USE_PLATFORM_WIN32
	if (sdl_driver == "windows" && isGL) {
		XrGraphicsBindingOpenGLWin32KHR binding{
			.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR,
		};
		binding.hDC = wglGetCurrentDC();
		binding.hGLRC = wglGetCurrentContext();
		session_create_info.next = &binding;
		XR_CHECK(xrCreateSession, Instance, &session_create_info, &Session);
		return true;
	}
#endif

#ifdef XR_USE_PLATFORM_XLIB
	if (sdl_driver == "x11" && isGL) {
		XrGraphicsBindingOpenGLXlibKHR binding{
			.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR,
		};
		binding.xDisplay = XOpenDisplay(NULL);
		binding.glxContext = glXGetCurrentContext();
		binding.glxDrawable = glXGetCurrentDrawable();
		session_create_info.next = &binding;
		XR_CHECK(xrCreateSession, Instance, &session_create_info, &Session);
		return true;
	}
#endif

#ifdef XR_USE_PLATFORM_ANDROID
	if (sdl_driver == "android" && isGLES) {
		XrGraphicsBindingOpenGLESAndroidKHR binding{
			.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR,
		};
		binding.display = eglGetCurrentDisplay();
		XR_ASSERT(FindCurrentEGLConfig(&binding.config));
		binding.context = eglGetCurrentContext();
		session_create_info.next = &binding;
		XR_CHECK(xrCreateSession, Instance, &session_create_info, &Session);
		return true;
	}
#endif

#ifdef XR_USE_PLATFORM_EGL
	if (isGLES || (isGL && sdl_driver == "wayland")) {
		XrGraphicsBindingEGLMNDX binding{
			.type = XR_TYPE_GRAPHICS_BINDING_EGL_MNDX,
		};
		binding.getProcAddress = (PFN_xrEglGetProcAddressMNDX)eglGetProcAddress;
		binding.display = eglGetCurrentDisplay();
		XR_ASSERT(FindCurrentEGLConfig(&binding.config));
		binding.context = eglGetCurrentContext();
		session_create_info.next = &binding;
		XR_CHECK(xrCreateSession, Instance, &session_create_info, &Session);
		return true;
	}
#endif
	os::Printer::log("[XR] Couldn't bind graphics context", ELL_ERROR);
	return false;
}

bool COpenXRSession::setupSpaces()
{
	if (BasePlaySpace == XR_NULL_HANDLE) {
		XrReferenceSpaceCreateInfo createInfo = {
			.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
			.referenceSpaceType = PlaySpaceType,
			.poseInReferenceSpace = IdentityPose,
		};
		XR_CHECK(xrCreateReferenceSpace, Session, &createInfo, &BasePlaySpace);
	}

	XR_ASSERT(PlaySpace == XR_NULL_HANDLE);
	XrReferenceSpaceCreateInfo createInfo = {
		.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		.referenceSpaceType = PlaySpaceType,
		.poseInReferenceSpace = PlaySpaceOffset,
	};
	XR_CHECK(xrCreateReferenceSpace, Session, &createInfo, &PlaySpace);

	XR_ASSERT(ViewSpace == XR_NULL_HANDLE);
	XrReferenceSpaceCreateInfo viewCreateInfo = {
		.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW,
		.poseInReferenceSpace = IdentityPose,
	};
	XR_CHECK(xrCreateReferenceSpace, Session, &viewCreateInfo, &ViewSpace);
	return true;
}

bool COpenXRSession::recenterPlaySpace(XrTime ref)
{
	XrSpaceLocation location = {
		.type = XR_TYPE_SPACE_LOCATION,
	};
	XR_CHECK(xrLocateSpace, ViewSpace, BasePlaySpace, ref, &location);
	bool validPosition = location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT;
	bool validOrientation = location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

	// Quietly do nothing if there's incomplete data
	if (!validPosition || !validOrientation)
		return true;

	// For recentering, only the 'yaw' matters, because the runtime guarantees
	// that the XZ plane is parallel with the floor.
	XrVector3f forward = quatApply(location.pose.orientation, XrVector3f{0, 0, 1});
	float yaw = atan2f(forward.x, forward.z);
	PlaySpaceOffset.position = location.pose.position;
	PlaySpaceOffset.orientation = XrQuaternionf{0, sinf(yaw/2), 0, cosf(yaw/2)};
	xrDestroySpace(PlaySpace);
	PlaySpace = XR_NULL_HANDLE;
	xrDestroySpace(ViewSpace);
	ViewSpace = XR_NULL_HANDLE;
	if (!setupSpaces())
		return false;
	return true;
}

bool COpenXRSession::beginSession()
{
	XR_ASSERT(!Running);
	XrSessionBeginInfo session_begin_info = {
		.type = XR_TYPE_SESSION_BEGIN_INFO,
		.primaryViewConfigurationType = ViewType,
	};
	XR_CHECK(xrBeginSession, Session, &session_begin_info);
	Running = true;
	return true;
}

bool COpenXRSession::endSession()
{
	XR_ASSERT(Running);
	XR_ASSERT(!InFrame);
	XR_ASSERT(SessionState == XR_SESSION_STATE_STOPPING);
	XR_CHECK(xrEndSession, Session);
	Running = false;
	DidWaitFrame = false;
	return true;
}

bool COpenXRSession::waitFrame()
{
	XR_ASSERT(!DidWaitFrame);
	FrameState = XrFrameState{
		.type = XR_TYPE_FRAME_STATE,
	};
	XrFrameWaitInfo waitInfo = {
		.type = XR_TYPE_FRAME_WAIT_INFO,
	};
	XR_CHECK(xrWaitFrame, Session, &waitInfo, &FrameState);
	DidWaitFrame = true;

	if (!Input->updateState(SessionState, FrameState.predictedDisplayTime, PlaySpace))
		return false;

	return true;
}

void COpenXRSession::resetViewChains()
{
	// Clean up view swapchains
	for (auto& viewChain : ViewChains) {
		for (auto& target : viewChain.RenderTargets) {
			if (target) {
				VideoDriver->removeRenderTarget(target);
				target = nullptr;
			}
		}
		viewChain.Swapchain.reset();
		viewChain.DepthSwapchain.reset();
	}
}

bool COpenXRSession::setupViewChains()
{
	uint32_t count;
	XR_CHECK(xrEnumerateSwapchainFormats, Session, 0, &count, NULL);

	SupportedFormats.resize(count);
	XR_CHECK(xrEnumerateSwapchainFormats, Session, count, &count, SupportedFormats.data());
	SupportedFormats.resize(count);

	// Choose the color and depth formats
	// TODO: Determine the range of formats that need to be supported here.
	int64_t preferred_format = GL_SRGB8_ALPHA8;
	int64_t preferred_depth_format = GL_DEPTH_COMPONENT32F;
	ColorFormat = SupportedFormats[0];
	DepthFormat = -1;
	for (const auto& format : SupportedFormats) {
		if (format == preferred_format) {
			ColorFormat = format;
		}
		if (format == preferred_depth_format) {
			DepthFormat = format;
		}
	}
	char buf[128];
	snprintf_irr(buf, sizeof(buf),
		"[XR] ColorFormat %d (%s)", (int32_t)ColorFormat,
		(ColorFormat == GL_SRGB8_ALPHA8) ? "GL_SRGB8_ALPHA8" : "unknown");
	os::Printer::log(buf, ELL_INFORMATION);
	snprintf_irr(buf, sizeof(buf),
		"[XR] DepthFormat %d (%s)", (int32_t)DepthFormat,
		(ColorFormat == GL_DEPTH_COMPONENT32F) ? "GL_DEPTH_COMPONENT32F" : "unknown");
	os::Printer::log(buf, ELL_INFORMATION);
	if (ColorFormat != preferred_format) {
		os::Printer::log("[XR] Using non-preferred color format", ELL_WARNING);
	}
	if (DepthFormat == -1) {
		os::Printer::log("[XR] Couldn't find valid depth buffer format", ELL_ERROR);
		return false;
	}

	// Make swapchain and depth swapchain for each view
	size_t viewCount = ViewConfigs.size();
	ViewChains.resize(viewCount);
	for (size_t viewIndex = 0; viewIndex < viewCount; viewIndex++) {
		auto& viewChain = ViewChains[viewIndex];
		viewChain.Swapchain =
			createOpenXRSwapchain(
				VideoDriver,
				Instance,
				Session,
				XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
				ColorFormat,
				ViewConfigs[viewIndex].recommendedSwapchainSampleCount,
				ViewConfigs[viewIndex].recommendedImageRectWidth,
				ViewConfigs[viewIndex].recommendedImageRectHeight);
		if (!viewChain.Swapchain)
			return false;
		viewChain.DepthSwapchain =
			createOpenXRSwapchain(
				VideoDriver,
				Instance,
				Session,
				XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
				DepthFormat,
				ViewConfigs[viewIndex].recommendedSwapchainSampleCount,
				ViewConfigs[viewIndex].recommendedImageRectWidth,
				ViewConfigs[viewIndex].recommendedImageRectHeight);
		if (!viewChain.DepthSwapchain)
			return false;
		size_t swapchainLength = viewChain.Swapchain->getLength();
		// These are added as needed
		viewChain.RenderTargets.resize(swapchainLength, nullptr);
	}
	return true;
}

void COpenXRSession::resetHudChain()
{
	for (auto& target : HudChain.RenderTargets) {
		if (target) {
			VideoDriver->removeRenderTarget(target);
			target = nullptr;
		}
	}
	HudChain.Swapchain.reset();
	HudChain.DepthSwapchain.reset();
}

bool COpenXRSession::setupHudChain()
{
	// Setup HUD
	HudChain.Swapchain = createOpenXRSwapchain(
		VideoDriver,
		Instance,
		Session,
		XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
		ColorFormat,
		1,
		HudWidth,
		HudHeight);
	if (!HudChain.Swapchain)
		return false;
	HudChain.DepthSwapchain = createOpenXRSwapchain(
		VideoDriver,
		Instance,
		Session,
		XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		DepthFormat,
		1,
		HudWidth,
		HudHeight);
	if (!HudChain.DepthSwapchain)
		return false;
	size_t swapchainLength = HudChain.Swapchain->getLength();
	HudChain.RenderTargets.resize(swapchainLength, nullptr);
	return true;
}

bool COpenXRSession::setupCompositionLayers()
{
	size_t viewCount = ViewConfigs.size();
	for (size_t viewIndex = 0; viewIndex < viewCount; viewIndex++) {
		auto& depthInfo = ViewChains[viewIndex].DepthInfo;
		depthInfo = XrCompositionLayerDepthInfoKHR{
			.type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR,
			.next = NULL,
			.minDepth = 0.f,
			.maxDepth = 1.f,
			.nearZ = ZNear,
			.farZ = ZFar,
		};
		depthInfo.subImage.swapchain = ViewChains[viewIndex].DepthSwapchain->getHandle();
		depthInfo.subImage.imageArrayIndex = 0;
		depthInfo.subImage.imageRect.offset.x = 0;
		depthInfo.subImage.imageRect.offset.y = 0;
		depthInfo.subImage.imageRect.extent.width = ViewConfigs[viewIndex].recommendedImageRectWidth;
		depthInfo.subImage.imageRect.extent.height = ViewConfigs[viewIndex].recommendedImageRectHeight;
	}

	// Fill out projection views
	ViewLayers.resize(viewCount);
	for (size_t viewIndex = 0; viewIndex < viewCount; viewIndex++) {
		auto& layerInfo = ViewLayers[viewIndex];
		layerInfo = XrCompositionLayerProjectionView{
			.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
			.next = NULL, // &ViewChains[viewIndex].DepthInfo,
			// TODO(paradust): Determine why this breaks SteamVR
		};
		layerInfo.subImage.swapchain = ViewChains[viewIndex].Swapchain->getHandle();
		layerInfo.subImage.imageArrayIndex = 0;
		layerInfo.subImage.imageRect.offset.x = 0;
		layerInfo.subImage.imageRect.offset.y = 0;
		layerInfo.subImage.imageRect.extent.width = ViewConfigs[viewIndex].recommendedImageRectWidth;
		layerInfo.subImage.imageRect.extent.height = ViewConfigs[viewIndex].recommendedImageRectHeight;
		// pose and fov are filled in at the beginning of each frame
	}
	return true;
}

bool COpenXRSession::setupInput()
{
	Input = createOpenXRInput(Instance, Session);
	if (!Input)
		return false;
	return true;
}

bool COpenXRSession::setAppReady(bool ready)
{
	XR_ASSERT(!InFrame);
	AppReady = ready;
	if (!AppReady && Running) {
		// OpenXR 1.0 spec is very clear that xrEndSession can be called at any time:
		//
		// "Calling xrEndSession always transitions a session to the not running state,
		// regardless of any errors returned."
		//
		// Unfortunately, both Monado and OpenXR-CTS missed this detail. So the only
		// option is to destroy the session.
		return false;
	}
	return true;
}

void COpenXRSession::recenter()
{
	DoRecenter = true;
}

void COpenXRSession::getInputState(core::XrInputState* state)
{
	Input->getInputState(state);
}

bool COpenXRSession::internalTryBeginFrame(bool *didBegin, const core::XrFrameConfig& config)
{
	XR_ASSERT(!InFrame);
	// App should not send us frames except in between startXR() and stopXR()
	XR_ASSERT(AppReady);

	if (!Running) {
		if (SessionState != XR_SESSION_STATE_READY) {
			*didBegin = false;
			return true;
		}
		if (!beginSession())
			return false;

		if (!waitFrame())
			return false;
	}
	XR_ASSERT(Running);
	XR_ASSERT(DidWaitFrame);

	FrameConfig = config;
	RenderHud = FrameConfig.FloatingHud.Enable;

	// If the hud changed size, remake the swapchain
	if (RenderHud && (
		FrameConfig.HudSize.Width != HudWidth ||
		FrameConfig.HudSize.Height != HudHeight)) {
		resetHudChain();
		HudWidth = FrameConfig.HudSize.Width;
		HudHeight = FrameConfig.HudSize.Height;
		if (!setupHudChain())
			return false;
	}

	XrFrameBeginInfo beginInfo = {
		.type = XR_TYPE_FRAME_BEGIN_INFO,
	};
	XR_CHECK(xrBeginFrame, Session, &beginInfo);
	*didBegin = true;
	InFrame = true;
	NextViewIndex = 0;

	if (DoRecenter && FrameState.shouldRender) {
		DoRecenter = false;
		if (!recenterPlaySpace(FrameState.predictedDisplayTime)) {
			return false;
		}
	}

	// TODO: Do hand tracking calculations need to happen in between waiting and beginning the frame?
	// And xrLocateViews, xrSyncActions, xrGetActionStatePose, xrLocateSpace, xrGetActionStateFloat, xrApplyHapticFeedback, etc

	// Get view location info for this frame
	XrViewLocateInfo viewLocateInfo = {
		.type = XR_TYPE_VIEW_LOCATE_INFO,
		.viewConfigurationType = ViewType,
		.displayTime = FrameState.predictedDisplayTime,
		.space = PlaySpace,
	};
	uint32_t viewCount = ViewConfigs.size();
	ViewInfo.resize(viewCount);
	for (size_t i = 0; i < viewCount; i++) {
		ViewInfo[i].type = XR_TYPE_VIEW;
		ViewInfo[i].next = NULL;
	}
	ViewState = XrViewState{
		.type = XR_TYPE_VIEW_STATE,
		.next = NULL,
	};
	XR_CHECK(xrLocateViews, Session, &viewLocateInfo, &ViewState, viewCount, &viewCount, ViewInfo.data());
	XR_ASSERT(viewCount == ViewConfigs.size());

	bool validPositions = ViewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT;
	bool validOrientations = ViewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT;

	if (!validPositions || !validOrientations) {
		FrameState.shouldRender = false;
	}

	if (FrameState.shouldRender) {
		// Fill in pose/fov info
		for (uint32_t i = 0; i < viewCount; i++) {
			ViewLayers[i].pose = ViewInfo[i].pose;
			ViewLayers[i].fov = ViewInfo[i].fov;
		}
		// Compute eye center
		if (viewCount == 0) {
			ViewCenter = {0, 0, 0};
		} else if (viewCount == 1) {
			ViewCenter = ViewInfo[0].pose.position;
		} else {
			ViewCenter = vecScale(0.5, vecAdd(ViewInfo[0].pose.position, ViewInfo[1].pose.position));
		}
	}
	return true;
}

bool COpenXRSession::internalNextView(bool *gotView, core::XrViewInfo* info)
{
	XR_ASSERT(InFrame);
	if (FrameState.shouldRender == XR_TRUE) {
		// TODO(paradust): Unify and clean up
		if (NextViewIndex < ViewChains.size()) {
			uint32_t viewIndex = NextViewIndex++;
			auto& viewChain = ViewChains[viewIndex];
			auto& viewConfig = ViewConfigs[viewIndex];
			if (!viewChain.Swapchain->acquireAndWait())
				return false;
			if (!viewChain.DepthSwapchain->acquireAndWait())
				return false;
			auto& target = viewChain.RenderTargets[viewChain.Swapchain->getAcquiredIndex()];
			if (!target) {
				os::Printer::log("[XR] Adding render target", ELL_INFORMATION);
				target = VideoDriver->addRenderTarget();
			}
			target->setTexture(
				viewChain.Swapchain->getAcquiredTexture(),
				viewChain.DepthSwapchain->getAcquiredTexture());
			const auto& viewInfo = ViewInfo[viewIndex];
			const auto& fov = viewInfo.fov;
			const auto& position = viewInfo.pose.position;
			const auto& orientation = viewInfo.pose.orientation;
			info->Kind = (viewIndex == 0) ? core::XRVK_LEFT_EYE : core::XRVK_RIGHT_EYE;
			info->Target = target;
			info->Width = viewConfig.recommendedImageRectWidth;
			info->Height = viewConfig.recommendedImageRectHeight;
			info->PositionBase = xr_to_irrlicht(ViewCenter);
			// RH -> LH coordinates
			info->Position = xr_to_irrlicht(position);
			// RH -> LH coordinates + invert
			info->Orientation = core::quaternion(-orientation.x, -orientation.y, orientation.z, orientation.w);
			info->AngleLeft = fov.angleLeft;
			info->AngleRight = fov.angleRight;
			info->AngleUp = fov.angleUp;
			info->AngleDown = fov.angleDown;
			info->ZNear = ZNear;
			info->ZFar = ZFar;
			*gotView = true;
			return true;
		}

		// HUD
		if (RenderHud && NextViewIndex == ViewChains.size()) {
			++NextViewIndex;
			if (!HudChain.Swapchain->acquireAndWait())
				return false;
			if (!HudChain.DepthSwapchain->acquireAndWait())
				return false;
			auto& target = HudChain.RenderTargets[HudChain.Swapchain->getAcquiredIndex()];
			if (!target) {
				os::Printer::log("[XR] Adding render target", ELL_INFORMATION);
				target = VideoDriver->addRenderTarget();
			}
			target->setTexture(
				HudChain.Swapchain->getAcquiredTexture(),
				HudChain.DepthSwapchain->getAcquiredTexture());
			info->Kind = core::XRVK_HUD;
			info->Target = target;
			info->Width = HudWidth;
			info->Height = HudHeight;
			info->PositionBase = core::vector3df(0, 0, 0);
			info->Position = core::vector3df(0, 0, 0);
			info->Orientation = core::quaternion(0, 0, 0, 1);
			// These should really not be used
			info->AngleLeft = -45.0f;
			info->AngleRight = 45.0f;
			info->AngleUp = 45.0f;
			info->AngleDown = -45.0f;
			info->ZNear = 1.0f;
			info->ZFar = 10.0f;
			*gotView = true;
			return true;
		}

		// If we're here, we're about to end frame. So release all the swapchains.
		for (uint32_t viewIndex = 0; viewIndex < ViewChains.size(); ++viewIndex) {
			auto& viewChain = ViewChains[viewIndex];
			auto& target = viewChain.RenderTargets[viewChain.Swapchain->getAcquiredIndex()];
			XR_ASSERT(target->getReferenceCount() == 1);
			if (!viewChain.Swapchain->release())
				return false;
			if (!viewChain.DepthSwapchain->release())
				return false;
		}
		if (RenderHud) {
			if (!HudChain.Swapchain->release())
				return false;
			if (!HudChain.DepthSwapchain->release())
				return false;
		}

	}

	// End the frame and submit all layers for rendering
	if (!endFrame())
		return false;
	*gotView = false;
	NextViewIndex = 0;
	return true;
}

static const char* state_label(XrSessionState state)
{
	switch (state) {
	case XR_SESSION_STATE_IDLE: return "idle";
	case XR_SESSION_STATE_READY: return "ready";
	case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
	case XR_SESSION_STATE_VISIBLE: return "visible";
	case XR_SESSION_STATE_FOCUSED: return "focused";
	case XR_SESSION_STATE_STOPPING: return "stopping";
	case XR_SESSION_STATE_LOSS_PENDING: return "loss_pending";
	case XR_SESSION_STATE_EXITING: return "exiting";
	default: return "Unknown";
	}
}

bool COpenXRSession::handleStateChange(XrEventDataSessionStateChanged *ev)
{
	if (ev->session != Session) {
		// Stale message. Not sure if this can actually happen, but ignore just in case.
		os::Printer::log("[XR] Received stale session change message", ELL_INFORMATION);
		return true;
	}
	const char* label = state_label(ev->state);
	char buf[128];
	snprintf_irr(buf, sizeof(buf), "[XR] Session state changed to `%s`", label);
	os::Printer::log(buf, ELL_INFORMATION);
	SessionState = ev->state;
	if (SessionState == XR_SESSION_STATE_STOPPING) {
		if (!endSession())
			return false;
	}
	return true;
}

bool COpenXRSession::endFrame()
{
	XR_ASSERT(InFrame);
	uint32_t layerCount = 0;
	const XrCompositionLayerBaseHeader* layers[5];
	XrCompositionLayerProjection projectionLayer;
	XrCompositionLayerQuad hudLayer;
	if (FrameState.shouldRender) {
		projectionLayer = XrCompositionLayerProjection{
			.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
			.next = NULL,
			.layerFlags = 0,
			.space = PlaySpace,
			.viewCount = (uint32_t)ViewLayers.size(),
			.views = ViewLayers.data(),
		};
		layers[layerCount++] = (XrCompositionLayerBaseHeader*)&projectionLayer;
	}

	if (FrameState.shouldRender && RenderHud) {
		hudLayer = XrCompositionLayerQuad{
			.type = XR_TYPE_COMPOSITION_LAYER_QUAD,
			.layerFlags =
				XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
				XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,
			.space = PlaySpace,
			.eyeVisibility = XR_EYE_VISIBILITY_BOTH,
			.pose = {
				.orientation = irrlicht_to_xr(FrameConfig.FloatingHud.Orientation),
				.position = irrlicht_to_xr(FrameConfig.FloatingHud.Position),
			},
			.size = irrlicht_to_xr(FrameConfig.FloatingHud.Size),
		};
		hudLayer.subImage.swapchain = HudChain.Swapchain->getHandle();
		hudLayer.subImage.imageArrayIndex = 0;
		hudLayer.subImage.imageRect.offset.x = 0;
		hudLayer.subImage.imageRect.offset.y = 0;
		hudLayer.subImage.imageRect.extent.width = HudWidth;
		hudLayer.subImage.imageRect.extent.height = HudHeight;
		layers[layerCount++] = (XrCompositionLayerBaseHeader*)&hudLayer;
	}

	XrFrameEndInfo frameEndInfo = {
		.type = XR_TYPE_FRAME_END_INFO,
		.next = NULL,
		.displayTime = FrameState.predictedDisplayTime,
		.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		.layerCount = layerCount,
		.layers = layers,
	};
	XR_CHECK(xrEndFrame, Session, &frameEndInfo);
	InFrame = false;
	DidWaitFrame = false;
	++XrFrameCounter;

	// Schedule the next frame
	if (!waitFrame())
		return false;

	return true;
}

unique_ptr<IOpenXRSession> createOpenXRSession(
	XrInstance instance,
	video::IVideoDriver* driver,
	XrReferenceSpaceType playSpaceType)
{
	unique_ptr<COpenXRSession> obj(new COpenXRSession(instance, driver, playSpaceType));
	if (!obj->init()) {
		os::Printer::log("[XR] createOpenXRSession failed", ELL_ERROR);
		return nullptr;
	}
	return obj;
}

#endif // _IRR_COMPILE_WITH_XR_DEVICE_

