// Copyright (C) 2022 sfan5
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in Irrlicht.h

#include "CXRManager.h"

#if defined(_IRR_COMPILE_WITH_XR_DEVICE_)

#include "CIrrDeviceXR.h"
#include "os.h"

namespace video
{

CXRManager::CXRManager(CIrrDeviceXR *device) :
		CSDLManager(device), XRDevice(device)
{}

bool CXRManager::swapBuffers()
{
	XRDevice->beforeSwap();
	return CSDLManager::swapBuffers();
}

}

#endif
