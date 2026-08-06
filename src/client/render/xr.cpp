/*
Minetest
Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>
Copyright (C) 2017 numzero, Lobachevskiy Vitaliy <numzer0@yandex.ru>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "gui/mainmenumanager.h" // for isMenuActive

#include "client/client.h"
#include "client/camera.h"
#include "pipeline.h"
#include "plain.h"
#include "XrViewInfo.h"
#include "settings.h"
#include "SColor.h"

#include <ICameraSceneNode.h>
#include <IEventReceiver.h>
#include <S3DVertex.h>
#include <SMaterial.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <iostream>

extern uint64_t XrFrameCounter;

using std::unique_ptr;

struct ViewState : public RenderPipelineObject
{
	core::XrViewInfo info;
};

struct CameraState : public RenderPipelineObject
{
	core::vector3df cameraPos;
	core::vector3df cameraRot;
	f32 fovUp, fovDown, fovRight, fovLeft;
	f32 znear, zfar;
	// These are computed values
	core::matrix4 baseTransform;
	core::quaternion baseRotation;
};

class XrTarget : public RenderTarget
{
public:
	XrTarget() = delete;
	XrTarget(ViewState* viewState) : view(viewState) {}
	virtual void activate(PipelineContext &context) override
	{
		auto driver = context.device->getVideoDriver();
		driver->setRenderTargetEx(view->info.Target, video::ECBF_ALL, context.clear_color);
		driver->OnResize(core::dimension2du(view->info.Width, view->info.Height));
	}
private:
	ViewState* view;
};

class SaveCameraState : public RenderStep
{
public:
	SaveCameraState() = delete;
	SaveCameraState(CameraState* camState) : state(camState) {}

	virtual void setRenderSource(RenderSource *source) override {}
	virtual void setRenderTarget(RenderTarget *target) override {}

	virtual void reset(PipelineContext &) override {}

	virtual void run(PipelineContext &context) override
	{
		scene::ICameraSceneNode* cameraNode = context.client->getCamera()->getCameraNode();
		state->cameraPos = cameraNode->getPosition();
		state->cameraRot = cameraNode->getRotation();
		cameraNode->getFOV(&state->fovUp, &state->fovDown, &state->fovRight, &state->fovLeft);
		state->znear = cameraNode->getNearValue();
		state->zfar = cameraNode->getFarValue();
		state->baseTransform = cameraNode->getRelativeTransformation();
		state->baseRotation = core::quaternion(cameraNode->getRotation() * core::DEGTORAD);
	};

private:
	CameraState* state;
};

class RestoreCameraState : public RenderStep
{
public:
	RestoreCameraState() = delete;
	RestoreCameraState(CameraState* camState) : state(camState) {}

	virtual void setRenderSource(RenderSource *source) override {}
	virtual void setRenderTarget(RenderTarget *target) override {}

	virtual void reset(PipelineContext &) override {}

	virtual void run(PipelineContext &context) override {
		scene::ICameraSceneNode* cameraNode = context.client->getCamera()->getCameraNode();
		cameraNode->setPosition(state->cameraPos);
		cameraNode->setRotation(state->cameraRot);
		cameraNode->setNearValue(state->znear);
		cameraNode->setFarValue(state->zfar);
		cameraNode->setFOV(state->fovUp, state->fovDown, state->fovRight, state->fovLeft);
	}
private:
	CameraState* state;
};

//! Adjustable attachment pose
class AttachmentPose {
public:
	AttachmentPose() {}

	bool isLocked() const
	{
		return IsLocked;
	}

	void lock()
	{
		LockPose = getCurrent();
		IsLocked = true;
	}

	void unlock()
	{
		if (!IsLocked)
			return;
		BaseParent = CurrentParent;
		BaseAttach = LockPose;
		IsLocked = false;
	}

	void setParent(const core::pose& parent)
	{
		CurrentParent = parent;
	}

	core::pose getCurrent() const
	{
		if (IsLocked)
			return LockPose;
		// WHY IS THIS SO COMPLICATED
		// There's probably a simpler formula, but it eludes me.
		core::vector3df dHP = CurrentParent.Position - BaseParent.Position;
		core::quaternion dHO = BaseParent.Rotation * CurrentParent.Rotation.inverse();
			return core::pose(
				dHP + BaseParent.Position + dHO * (BaseAttach.Position - BaseParent.Position),
				dHO.inverse() * BaseAttach.Rotation);
	}

private:
	bool IsLocked = false;
	core::pose BaseParent;
	core::pose BaseAttach;
	core::pose CurrentParent;
	core::pose LockPose;
};


class XrForEachView : public RenderStep {
public:
	XrForEachView(ViewState *viewState, CameraState *camState, RenderStep* renderView, RenderStep* renderHud)
		: view(viewState), cam(camState), render_view(renderView), render_hud(renderHud) {}

        virtual void setRenderSource(RenderSource *) override {}
        virtual void setRenderTarget(RenderTarget *target) override {}

        virtual void reset(PipelineContext &context) override {}
        virtual void run(PipelineContext &context) override {
		auto device = context.device;
		auto driver = device->getVideoDriver();

		core::XrFrameConfig config = {};

		// DrawHUD is very sensitive to changes in the size of the "screen".
		// If it gets rendered to targets of different sizes, the UI breaks.
		// So always make the xr HUD target the same resolution as the screen.
		config.FloatingHud.Resolution = driver->getScreenSize();

		// This reference is only valid for immediate use.
		const auto& hud_mode = g_settings->get("xr_hud");
		f32 aspect_ratio = (f32)config.FloatingHud.Resolution.Width /
				config.FloatingHud.Resolution.Height;
		config.FloatingHud.Size = core::dimension2df(2.0f * aspect_ratio, 2.0f);
		config.FloatingHud.Position = core::vector3df(0, 0, 1.25f);
		config.FloatingHud.Orientation = core::quaternion();
		if (hud_mode != "off" || isMenuActive()) {
			config.FloatingHud.Enable = true;
			config.FloatingHud.Position = core::vector3df(0, 0, 2.0f);
			config.FloatingHud.Orientation = core::quaternion();
		}

		if (!device->beginFrame(config))
			return;

		core::XrInputState inputState;
		device->xrGetInputState(&inputState);

		// Enable wielded tool
		static AttachmentPose attachment[2];
		Camera* camera = context.client->getCamera();
		for (int i = 0; i < 2; i++) {
			if (!inputState.Hand[i].Grip.Valid)
				continue;
			std::cout << "GOT HAND DATA " << i << std::endl;
			core::pose handPos = inputState.Hand[i].Grip.Pose;
			attachment[i].setParent(handPos);
			core::pose toolPose = attachment[i].getCurrent();

			if (!attachment[i].isLocked() && inputState.Hand[i].Attack.Pressed) {
				attachment[i].lock();
			} else if (attachment[i].isLocked() && !inputState.Hand[i].Attack.Pressed) {
				attachment[i].unlock();
			}

			core::vector3df toolPosition = toolPose.Position * BS;
			cam->baseTransform.transformVect(toolPosition);
			core::quaternion toolRotation = toolPose.Rotation.inverse(); //inputState.Hand[i].Grip.Orientation;
			toolRotation = toolRotation * cam->baseRotation;
			camera->enableSceneHand((i == 0) ? true : false, toolPosition, toolRotation);
		}

		auto oldScreenSize = driver->getScreenSize();
		auto oldViewPort = driver->getViewPort();
		v2u32 old_target_size = context.target_size;
		while (device->nextView(&view->info)) {
			context.target_size = v2u32(view->info.Width, view->info.Height);
			if (view->info.Kind == core::XRVK_HUD) {
				video::SColor oldClearColor = context.clear_color;
				context.clear_color = video::SColor(0, 0, 0, 0); // transparent
				render_hud->reset(context);
				render_hud->run(context);
				context.clear_color = oldClearColor;
			} else {
				render_view->reset(context);
				render_view->run(context);
			}
		}
		context.target_size = old_target_size;

		// Reset driver
		driver->setRenderTarget(nullptr, video::ECBF_NONE);
		driver->OnResize(oldScreenSize);
		driver->setViewPort(oldViewPort);
		camera->disableSceneHands();
	}
private:
	ViewState* view;
	CameraState* cam;
	RenderStep* render_view;
	RenderStep* render_hud;
};

//! Setup the camera for rendering to an XR view target
class XrSetupCamera : public RenderStep
{
public:
	XrSetupCamera(const CameraState* camState, const ViewState* viewState)
		: cam(camState), view(viewState) {}

	virtual void setRenderSource(RenderSource *source) override {}
	virtual void setRenderTarget(RenderTarget *target) override {}

	virtual void reset(PipelineContext &) override {}

	virtual void run(PipelineContext &context) override
	{
		scene::ICameraSceneNode* cameraNode = context.client->getCamera()->getCameraNode();
		const auto& info = view->info;

		// Apply virtual IPD adjustment
		float vipd = g_settings->getFloat("xr_vipd");
		core::vector3df adjPos = (info.PositionBase + (info.Position - info.PositionBase) * vipd) * BS;
		cam->baseTransform.transformVect(adjPos);

		core::quaternion adjRot = info.Orientation * cam->baseRotation;

		// Scale device coordinates by BS
		cameraNode->setPosition(adjPos);
		cameraNode->updateAbsolutePosition();

		core::vector3df euler;
		adjRot.toEulerDeg(euler);
		cameraNode->setRotation(euler);
		cameraNode->setUpVector(euler.rotationToDirection(core::vector3df(0, 1, 0)));
		cameraNode->setNearValue(info.ZNear);
		cameraNode->setFarValue(info.ZFar);
		cameraNode->setFOV(info.AngleUp, info.AngleDown, info.AngleRight, info.AngleLeft);
		cameraNode->updateMatrices();
	}
private:
	const CameraState* cam;
	const ViewState* view;
};

static constexpr const char* cursorArt[] = {
	".                ",
	"..               ",
	"._.              ",
	".__.             ",
	".___.            ",
	".____.           ",
	"._____.          ",
	".______.         ",
	"._______.        ",
	".________.       ",
	"._________.      ",
	".__________.     ",
	".___.__......    ",
	".__..__.         ",
	"._.  .__.        ",
	"..   .__.        ",
	".     .__.       ",
	"      .__.       ",
	"       .__.      ",
	"       .__.      ",
	"        .__.     ",
	"        .__.     ",
	"         ....    ",
};

//! Mouse cursor bitmap, shared by the HUD pipeline and the fallback renderer.
//! The hotspot is the top left pixel, so it can be drawn at the pointer
//! position directly.
static video::ITexture* getCursorTexture(video::IVideoDriver* driver)
{
	static const char* cursorName = "xr_mouse_cursor";
	if (video::ITexture* existing = driver->findTexture(cursorName))
		return existing;

	size_t cursorWidth = strlen(cursorArt[0]);
	size_t cursorHeight = sizeof(cursorArt) / sizeof(cursorArt[0]);
	// The image takes ownership of this
	u8* rawImageData = new u8[4 * cursorWidth * cursorHeight];
	u32* imageData = reinterpret_cast<u32*>(rawImageData);
	for (size_t y = 0; y < cursorHeight; ++y) {
		for (size_t x = 0; x < cursorWidth; ++x) {
			video::SColor color;
			switch (cursorArt[y][x]) {
			case '.':
				color = video::SColor(255, 0, 0, 0); // black
				break;
			case '_':
				color = video::SColor(255, 255, 255, 255); // white
				break;
			default:
				color = video::SColor(0, 0, 0, 0); // transparent
				break;
			}
			imageData[y * cursorWidth + x] = color.color;
		}
	}
	video::IImage* cursorImage = driver->createImageFromData(
		video::ECF_A8R8G8B8,
		core::dimension2d<u32>(cursorWidth, cursorHeight),
		rawImageData, true, true);
	video::ITexture* cursorTexture = driver->addTexture(cursorName, cursorImage);
	cursorImage->drop();
	return cursorTexture;
}

class DrawMouse : public RenderStep {
public:
	virtual void setRenderSource(RenderSource *) override {}
	virtual void setRenderTarget(RenderTarget *target) override {}

	virtual void reset(PipelineContext &context) override {}
	virtual void run(PipelineContext &context) override {
		auto device = context.device;
		auto control = device->getCursorControl();
		if (!isMenuActive() || !control)
			return;

		auto driver = device->getVideoDriver();
		if (video::ITexture* cursor = getCursorTexture(driver))
			driver->draw2DImage(cursor, control->getPosition(), true);
	}
};

/*
TODO(paradust): Add as a debug feature
		gui::IGUIFont *font = device->getGUIEnvironment()->getBuiltInFont();
		if (!font) {
			std::cout << "font is NULL" << std::endl;
		} else {
			wchar_t buf[256];
			swprintf(buf, 256, L"F%u", (unsigned int)XrFrameCounter);
			font->draw(buf,
				core::rect<s32>(info.Width/2, info.Height/2, info.Width/2 + 100, info.Height/2 + 100),
				video::SColor(255, 255, 255, 255));
		}
*/

static unique_ptr<RenderStep> createRenderViewPipeline(Client *client, CameraState *camState, ViewState *viewState)
{
	unique_ptr<RenderPipeline> inner(new RenderPipeline());
	RenderStep* draw3d = inner->own(create3DStage(client, v2f(1.0f, 1.0f)));
	RenderTarget* target = inner->createOwned<XrTarget>(viewState);
	inner->addStep<XrSetupCamera>(camState, viewState);
	inner->addStep(draw3d);
	draw3d->setRenderTarget(target);
	return inner;
}

static unique_ptr<RenderStep> createRenderHudPipeline(Client *client, CameraState *camState, ViewState *viewState)
{
	unique_ptr<RenderPipeline> inner(new RenderPipeline());
	RenderTarget* target = inner->createOwned<XrTarget>(viewState);
	inner->addStep<XrSetupCamera>(camState, viewState);
	inner->addStep<DrawHUD>()->setRenderTarget(target);
	inner->addStep<DrawMouse>();
	return inner;
}

void populateXrPipeline(RenderPipeline *pipeline, Client *client)
{
	CameraState* camState = pipeline->createOwned<CameraState>();
	ViewState* viewState = pipeline->createOwned<ViewState>();

	// First render to screen normally
	populatePlainPipeline(pipeline, client);

	RenderStep* renderView = pipeline->own(createRenderViewPipeline(client, camState, viewState));
	RenderStep* renderHud = pipeline->own(createRenderHudPipeline(client, camState, viewState));
	pipeline->addStep<SaveCameraState>(camState);
	pipeline->addStep<XrForEachView>(viewState, camState, renderView, renderHud);
	pipeline->addStep<RestoreCameraState>(camState);
}

//! Convert a rotation as it is handed to/from the XR backend into a regular
//! Irrlicht rotation.
//! The backend mirrors the Z axis to get from OpenXR's right-handed space to
//! Irrlicht's left-handed one, but does so by conjugating the quaternion. The
//! upshot is that the rotation stored in an XrInputPose (or in XrFrameConfig's
//! quad orientation) maps playspace into local space, i.e. it is the inverse of
//! what the rest of Irrlicht calls a rotation. XrViewInfo::Orientation is the
//! exception: it is inverted a second time by the backend, and is already a
//! plain Irrlicht rotation.
//! TODO(paradust7): Fix this mismatch
static inline core::quaternion xrRotation(const core::quaternion &q)
{
	return q.inverse();
}

//! A controller ray traced against a floating quad.
struct QuadPointer
{
	//! Controller is being tracked
	bool Valid = false;
	//! Ray crosses the plane of the quad in front of the controller
	bool OnPlane = false;
	//! ... and does so within the bounds of the quad
	bool OnScreen = false;
	//! Ray endpoints, in playspace meters
	core::vector3df Origin;
	core::vector3df End;
	//! Point under the ray in normalized quad coordinates: (0,0) is the top
	//! left corner and (1,1) the bottom right. Clamped to the quad, and only
	//! meaningful if OnPlane.
	core::vector2df Uv;
};

//! Resolve a normalized quad coordinate against a pixel grid.
//! The screen and the quad's swapchain don't have to be the same size - the
//! screen is scaled into the quad by the blit - so the two spaces are kept
//! apart: mouse events are posted in screen pixels, while anything drawn onto
//! the quad is placed in quad pixels.
static core::position2d<s32> quadPixel(const core::vector2df &uv, const core::dimension2du &size)
{
	return core::position2d<s32>(
		core::clamp((s32)(uv.X * size.Width), 0, (s32)size.Width - 1),
		core::clamp((s32)(uv.Y * size.Height), 0, (s32)size.Height - 1));
}

//! Length of a laser that doesn't hit anything, in meters
static constexpr f32 POINTER_LENGTH = 6.0f;
//! Half the thickness of the laser, in meters
static constexpr f32 POINTER_RADIUS = 0.004f;

//! Trace the aim ray of one controller against a floating quad.
static QuadPointer traceQuadPointer(const core::XrQuadConfig &quad, const core::XrInputPose &aim)
{
	QuadPointer pointer;
	if (!aim.Valid)
		return pointer;

	pointer.Valid = true;
	pointer.Origin = aim.Pose.Position;

	// The aim pose points along -Z in OpenXR, which is +Z once mirrored.
	core::vector3df dir = xrRotation(aim.Pose.Rotation) * core::vector3df(0.0f, 0.0f, 1.0f);
	pointer.End = pointer.Origin + dir * POINTER_LENGTH;

	if (!quad.Enable)
		return pointer;

	// Move into the frame of the quad, where it lies flat in the z=0 plane
	// spanning [-Size/2, Size/2], with +X right and +Y up as seen by a viewer
	// in front of it.
	core::quaternion intoQuad = xrRotation(quad.Orientation).inverse();
	core::vector3df origin = intoQuad * (pointer.Origin - quad.Position);
	core::vector3df ray = intoQuad * dir;
	if (fabsf(ray.Z) < 1e-6f)
		return pointer; // parallel to the quad

	f32 distance = -origin.Z / ray.Z;
	if (distance <= 0.0f)
		return pointer; // quad is behind the controller

	core::vector3df hit = origin + ray * distance;
	f32 u = hit.X / quad.Size.Width + 0.5f;
	f32 v = 0.5f - hit.Y / quad.Size.Height;

	pointer.OnPlane = true;
	pointer.OnScreen = (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f);
	if (pointer.OnScreen)
		pointer.End = pointer.Origin + dir * distance;
	pointer.Uv = core::vector2df(core::clamp(u, 0.0f, 1.0f), core::clamp(v, 0.0f, 1.0f));
	return pointer;
}

//! Turns XR controller aiming into mouse input on the 2D screen that is shown
//! on a floating quad.
class XrMenuPointer
{
public:
	//! Trace both controllers against the quad and pick the one driving the
	//! cursor. Call once per frame, before drawing.
	void trace(const core::XrQuadConfig &quad, const core::XrInputState &input)
	{
		for (int i = 0; i < 2; i++) {
			Pointers[i] = traceQuadPointer(quad, input.Hand[i].Aim);
			Pressed[i] = input.Hand[i].Attack.Pressed;
		}

		int hand = -1;
		if (ButtonDown && ActiveHand >= 0 && Pointers[ActiveHand].OnPlane) {
			// Hold on to the hand that started the click, so that the other
			// controller can't steal the cursor in the middle of a drag.
			hand = ActiveHand;
		} else {
			// Prefer a hand that is aiming at the quad and pulling the trigger,
			// then the hand that already had the cursor, then either one.
			for (int i = 0; i < 2 && hand < 0; i++) {
				if (Pointers[i].OnScreen && Pressed[i])
					hand = i;
			}
			if (hand < 0 && ActiveHand >= 0 && Pointers[ActiveHand].OnScreen)
				hand = ActiveHand;
			for (int i = 0; i < 2 && hand < 0; i++) {
				if (Pointers[i].OnScreen)
					hand = i;
			}
		}
		ActiveHand = hand;
	}

	//! Feed the traced state into the event queue as mouse input, in the pixel
	//! coordinates the GUI works in.
	//! This runs GUI handlers synchronously, so call it once the XR frame has
	//! been submitted rather than in the middle of rendering it.
	void sendEvents(IrrlichtDevice *device, const core::dimension2du &screenSize)
	{
		if (ActiveHand < 0) {
			// Don't leave the button stuck down if the controller was lost or
			// aimed away while it was held.
			if (ButtonDown) {
				ButtonDown = false;
				postMouseEvent(device, EMIE_LMOUSE_LEFT_UP, LastPixel);
			}
			return;
		}

		core::position2d<s32> pixel = quadPixel(Pointers[ActiveHand].Uv, screenSize);
		if (pixel != LastPixel) {
			LastPixel = pixel;
			postMouseEvent(device, EMIE_MOUSE_MOVED, pixel);
		}

		if (Pressed[ActiveHand] != ButtonDown) {
			ButtonDown = Pressed[ActiveHand];
			postMouseEvent(device,
				ButtonDown ? EMIE_LMOUSE_PRESSED_DOWN : EMIE_LMOUSE_LEFT_UP, pixel);
		}
	}

	//! Ray of hand `hand`, valid until the next trace().
	const QuadPointer &getPointer(int hand) const { return Pointers[hand]; }

	//! Hand currently driving the cursor, or -1 if none is aiming at the quad.
	int getActiveHand() const { return ActiveHand; }

	//! Cursor position in normalized quad coordinates, valid if getActiveHand()
	//! is not -1.
	core::vector2df getCursorUv() const { return Pointers[ActiveHand].Uv; }

private:
	void postMouseEvent(IrrlichtDevice *device, EMOUSE_INPUT_EVENT type,
			const core::position2d<s32> &pixel)
	{
		SEvent event;
		event.EventType = EET_MOUSE_INPUT_EVENT;
		event.MouseInput.Event = type;
		event.MouseInput.X = pixel.X;
		event.MouseInput.Y = pixel.Y;
		event.MouseInput.Shift = false;
		event.MouseInput.Control = false;
		// Not "simulated": this is an absolute pointer and should behave
		// exactly like a mouse, hover included. GUIModalMenu ignores the
		// position of simulated events.
		event.MouseInput.Simulated = false;
		event.MouseInput.ButtonStates = ButtonDown ? SDL_BUTTON_MASK(SDL_BUTTON_LEFT) : 0;
		device->postEventFromUser(event);
	}

	QuadPointer Pointers[2];
	bool Pressed[2] = {false, false};
	int ActiveHand = -1;
	bool ButtonDown = false;
	core::position2d<s32> LastPixel = core::position2d<s32>(-1, -1);
};

//! Draw a laser between two playspace points as a thin quad turned to face the
//! eye, so that it keeps its width from any angle. Coordinates are in meters.
static void drawPointer(video::IVideoDriver *driver, const core::vector3df &eye,
		const core::vector3df &from, const core::vector3df &to, video::SColor color)
{
	core::vector3df axis = to - from;
	f32 length = axis.getLength();
	if (length < 0.001f)
		return;
	axis /= length;

	core::vector3df side = axis.crossProduct(eye - from);
	if (side.getLengthSQ() < 1e-12f)
		return; // looking straight down the laser
	side.normalize();
	side *= POINTER_RADIUS;

	core::vector3df normal = side.crossProduct(axis);
	video::S3DVertex vertices[4] = {
		video::S3DVertex((from - side) * BS, normal, color, core::vector2df(0.0f, 0.0f)),
		video::S3DVertex((from + side) * BS, normal, color, core::vector2df(1.0f, 0.0f)),
		video::S3DVertex((to + side) * BS, normal, color, core::vector2df(1.0f, 1.0f)),
		video::S3DVertex((to - side) * BS, normal, color, core::vector2df(0.0f, 1.0f)),
	};
	static const u16 indices[6] = {0, 1, 2, 0, 2, 3};
	driver->drawIndexedTriangleList(vertices, 4, indices, 2);
}

//! Point the driver at one XR eye view, so that playspace coordinates scaled by
//! BS can be drawn directly.
static void setupEyeTransforms(video::IVideoDriver *driver, const core::XrViewInfo &info)
{
	// Unlike controller poses, XrViewInfo::Orientation is already a plain
	// Irrlicht rotation. See xrRotation().
	core::vector3df position = info.Position * BS;
	core::vector3df forward = info.Orientation * core::vector3df(0.0f, 0.0f, 1.0f);
	core::vector3df up = info.Orientation * core::vector3df(0.0f, 1.0f, 0.0f);

	core::matrix4 projection;
	projection.buildProjectionMatrixPerspectiveFovLH(
		info.AngleUp, info.AngleDown, info.AngleRight, info.AngleLeft,
		info.ZNear, info.ZFar, false);

	core::matrix4 view;
	view.buildCameraLookAtMatrixLH(position, position + forward, up);

	driver->setTransform(video::ETS_PROJECTION, projection);
	driver->setTransform(video::ETS_VIEW, view);
	driver->setTransform(video::ETS_WORLD, core::matrix4());
}

void renderFallback(IrrlichtDevice *device)
{
	video::IVideoDriver *driver = device->getVideoDriver();
	core::XrFrameConfig config = {};

	// The screen goes on an underlay rather than the HUD quad, so that the
	// laser drawn into the eye views is composited on top of it instead of
	// disappearing behind it.
	core::dimension2du screenSize = driver->getScreenSize();
	f32 aspect_ratio = (f32)screenSize.Width / screenSize.Height;
	config.FloatingUnderlay.Enable = true;
	config.FloatingUnderlay.Resolution = screenSize;
	config.FloatingUnderlay.Size = core::dimension2df(2.0f * aspect_ratio, 2.0f);
	config.FloatingUnderlay.Position = core::vector3df(0, 0, 2.0f);
	config.FloatingUnderlay.Orientation = core::quaternion();

	if (!device->beginFrame(config))
		return;

	core::XrInputState inputState;
	device->xrGetInputState(&inputState);

	// Aim the controllers at the floating quad. The resulting mouse events are
	// only posted once the frame has been submitted, because handling them can
	// tear down or replace the menu we are in the middle of showing.
	static XrMenuPointer pointer;
	pointer.trace(config.FloatingUnderlay, inputState);

	// Laser material: plain opaque vertex colours, drawn from either side
	// since the quad is turned to face the eye. Depth test and write are left
	// at their defaults, so that the two lasers intersect correctly where they
	// cross, and so the depth buffer is right if it is ever handed to the
	// compositor for reprojection.
	video::SMaterial pointerMaterial;
	pointerMaterial.MaterialType = video::EMT_SOLID;
	pointerMaterial.BackfaceCulling = false;

	auto oldViewPort = driver->getViewPort();

	core::XrViewInfo info;
	while (device->nextView(&info)) {
		if (info.Kind == core::XRVK_UNDERLAY) {
			// Without this, the blit goes nowhere because
			// IRenderTarget::setTexture() doesn't bind the
			// frame buffer to the textures until update().
			driver->setRenderTargetEx(info.Target, video::ECBF_NONE);

			// Copy the menu/loading screen into the floating quad. This scales
			// the screen up or down to the size of the quad's swapchain.
			driver->blitRenderTarget(nullptr, info.Target);

			// Draw the cursor onto the quad rather than in 3D, so that it can't
			// be lost against the busy menu behind it. 2D drawing is positioned
			// against the render target, so this is in quad pixels, not screen
			// pixels.
			core::dimension2du quadSize(info.Width, info.Height);
			video::ITexture* cursor = getCursorTexture(driver);
			if (!cursor) {
				// nothing to draw
			} else if (pointer.getActiveHand() >= 0) {
				driver->draw2DImage(cursor, quadPixel(pointer.getCursorUv(), quadSize), true);
			} else if (isMenuActive()) {
				// No controller is aiming at the menu, so show the real mouse.
				// It lives outside the framebuffer we just blitted.
				if (auto control = device->getCursorControl()) {
					core::position2d<s32> mouse = control->getPosition();
					core::vector2df uv(
						(f32)mouse.X / screenSize.Width,
						(f32)mouse.Y / screenSize.Height);
					driver->draw2DImage(cursor, quadPixel(uv, quadSize), true);
				}
			}
		} else {
			// Transparent, so that the underlay shows through everywhere the
			// laser isn't. setRenderTargetEx sets the viewport to match, so
			// don't call OnResize here: blitRenderTarget() reads the screen
			// size to find the source rectangle for the underlay view.
			driver->setRenderTargetEx(info.Target, video::ECBF_ALL,
					video::SColor(0, 0, 0, 0));

			setupEyeTransforms(driver, info);
			driver->setMaterial(pointerMaterial);
			for (int i = 0; i < 2; i++) {
				const QuadPointer &ray = pointer.getPointer(i);
				if (!ray.Valid)
					continue;
				// Highlight the hand holding the cursor, and light it up while
				// the trigger is down.
				video::SColor color;
				if (i != pointer.getActiveHand())
					color = video::SColor(255, 60, 90, 110);
				else if (inputState.Hand[i].Attack.Pressed)
					color = video::SColor(255, 255, 220, 90);
				else
					color = video::SColor(255, 110, 200, 255);
				drawPointer(driver, info.Position, ray.Origin, ray.End, color);
			}
		}
	}

	// Restore driver state
	driver->setRenderTarget(nullptr, video::ECBF_NONE);
	driver->OnResize(screenSize);
	driver->setViewPort(oldViewPort);

	// The XR frame has been submitted, so it's safe to let the GUI react.
	pointer.sendEvents(device, screenSize);
}
