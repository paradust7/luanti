// Copyright (C) 2022 sfan5
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in Irrlicht.h

#pragma once

#if defined(_IRR_COMPILE_WITH_XR_DEVICE_)

#include "IContextManager.h"
#include "CSDLManager.h"

class CIrrDeviceXR;

namespace video
{

// Manager for SDL with OpenGL
class CXRManager : public CSDLManager
{
public:
	CXRManager(CIrrDeviceXR *device);

	virtual ~CXRManager() {}

	bool swapBuffers() override;

private:
	CIrrDeviceXR *XRDevice;
};
}

#endif
