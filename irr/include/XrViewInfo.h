#pragma once

#include "quaternion.h"
#include "vector3d.h"
#include "pose.h"
#include "dimension2d.h"
#include "IRenderTarget.h"

namespace core
{

enum XR_INPUT_HAND {
	XRIH_LEFT = 0,
	XRIH_RIGHT = 1,
};

struct XrButton {
	bool Pressed; // button is pressed

	// If the controller has capacitive sensors, this will indicate
	// that the player's finger is on or very near the button
	bool Touched;

	// On a button with continuous trigger, this can be
	// any value from 0.0 to 1.0 Otherwise, it will jump 0 to 1.
	// Generally only the primary button has a trigger.
	float Value;
};

struct XrInputPose {
	// Position and Orientation are only meaningful if Valid is set
	bool Valid;
	pose Pose;
};

struct XrInputHand {
	// Aim position and direction
	XrInputPose Aim;

	// Where objects are gripped
	XrInputPose Grip;

	// Buttons
	XrButton Attack;
	XrButton Use;
	XrButton Menu;

	// Joystick
	// TBD

	// Touchpad
	// TBD
};

struct XrInputState {
	XrInputHand Hand[2];
};

// A flat quad placed in the world and composited by the XR runtime.
struct XrQuadConfig {
	bool Enable;
	// Size of the surface rendered into the quad, in pixels
	dimension2du Resolution;
	// Size of the quad in the world, in meters
	dimension2df Size;
	// Coordinates of the center (in the XR fixed frame)
	vector3df Position;
	quaternion Orientation;
};

struct XrFrameConfig {
	// Composited on top of everything else.
	XrQuadConfig FloatingHud;

	// Composited underneath the eye views, so that 3D content can be drawn
	// over it. This only works if the eye views are cleared to transparent,
	// since they are blended over the underlay rather than replacing it.
	XrQuadConfig FloatingUnderlay;
};

enum XR_VIEW_KIND {
	XRVK_INVALID,
	XRVK_LEFT_EYE,
	XRVK_RIGHT_EYE,
	XRVK_HUD,
	XRVK_UNDERLAY,
	XRVK_GENERIC,
};

struct XrViewInfo {
	XR_VIEW_KIND Kind;
	video::IRenderTarget* Target;

	// Viewport
	u32 Width;
	u32 Height;

	// HMD translation/orientation of eye relative to playspace origin
	core::vector3df Position;
	core::quaternion Orientation;

	// If this is an eye (left/right), this is the center point between
	// the two eyes. Used for IPD adjustment.
	core::vector3df PositionBase;

	// FoV angles (in radians)
	// For symmetric FoV, left/down will be negative
	// Total angles are (angleRight - angleLeft) and (angleUp - angleDown)
	f32 AngleLeft;
	f32 AngleRight;
	f32 AngleUp;
	f32 AngleDown;

	f32 ZNear;
	f32 ZFar;
};

} // end namespace core
