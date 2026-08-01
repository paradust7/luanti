#pragma once

#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "IVideoDriver.h"
#include "IRenderTarget.h"
#include "XrViewInfo.h"

#include <vector>
#include <unordered_set>
#include <memory>

enum XR_MODE_FLAGS {
	XRMF_ROOM_SCALE = 0x1,
};

/*
 * Important:
 *
 * After createOpenXRConnector() succeeds, there are no failures at this layer.
 *
 * Methods of COpenXRConnector will continue to pretend to work, even if the OpenXR instance is
 * dead and failing to recreate.
 *
 * This is because the OpenXR specification allows clients to reconnect after the runtime comes
 * back online, even after crash/shutdown/update. Clients with another UI are able to keep
 * running.
 *
 * TODO: Pause the game in the event the system shuts off.
 *
 */
class IOpenXRConnector {
public:
	virtual ~IOpenXRConnector() {}

	// Register that the app is ready to start delivering frames
	virtual void startXR() = 0;

	// Register that the app is no longer delivering frames
	virtual void stopXR() = 0;

	// Handles all pending events. Returns when the event queue is empty.
	// This needs to be called at least once between frames (not during a frame).
	// If the event queue overflows, events are lost.
	virtual void handleEvents() = 0;

	// Schedule a recenter before the next frame.
	virtual void recenter() = 0;

	virtual void getInputState(core::XrInputState* state) = 0;

	// tryBeginFrame
	//
	// Try to begin the next frame. This method blocks to achieve VSync with the
	// HMD display, so it should only be called when everything else has been processed.
	//
	// If it returns TRUE:
	//   The next frame has begun and `predicted_time_delta` is set to the
	//   predicted future display time of the frame. (nanoseconds from now)
	//   EndFrame() must be called after drawing is finished.
	//
	// If it returns FALSE:
	//   OpenXR rendering should be skipped for this frame. The render loop must be
	//   throttled using another method (e.g. sleep)
	//
	// If the system becomes idle (HMD is turned off), or the session is closed,
	// then TryBeginFrame() could return `false` for an extended period.
	//
	// HandleEvents() should continue to be called every frame. If the system
	// comes back online, it will re-initialize, and TryBeginFrame() will return
	// true again.
	virtual bool tryBeginFrame(const core::XrFrameConfig& config) = 0;

	// Once a frame has begun, call NextView repeatedly until it returns false.
	//
	// For each view, render the appropriate image.
	//
	// Don't assume every view will appear. If the openxr service crashes during
	// rendering, it may stop short.
	//
	// After NextView returns false, the frame is considered ended.
	virtual bool nextView(core::XrViewInfo* info) = 0;
};


std::unique_ptr<IOpenXRConnector> createOpenXRConnector(video::IVideoDriver* driver, uint32_t mode_flags);

#endif // _IRR_COMPILE_WITH_XR_DEVICE_
