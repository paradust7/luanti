#pragma once

#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "OpenXRHeaders.h"
#include "ITexture.h"
#include "IVideoDriver.h"

#include <memory>

class IOpenXRSwapchain {
public:
	virtual ~IOpenXRSwapchain() {}

	virtual XrSwapchain getHandle() = 0;

	// Length of the swapchain
	// Acquired indices run from 0 to getLength() - 1
	virtual size_t getLength() = 0;

	// Acquire an image in the swapchain and wait for it to become ready.
	// Must be called after frame has begun.
	//
	// Returns true on success.
	//
	// Returns false on fatal error
	// (session and instance should be destroyed)
	virtual bool acquireAndWait() = 0;

	// These can only be called when an image is acquired.
	virtual size_t getAcquiredIndex() = 0;
	virtual video::ITexture* getAcquiredTexture() = 0;

	// Release the swapchain.
	virtual bool release() = 0;
};

std::unique_ptr<IOpenXRSwapchain> createOpenXRSwapchain(
	video::IVideoDriver* driver,
	XrInstance instance,
	XrSession session,
	XrSwapchainUsageFlags usageFlags,
	int64_t format,
	uint32_t sampleCount,
	uint32_t width,
	uint32_t height);

#endif
