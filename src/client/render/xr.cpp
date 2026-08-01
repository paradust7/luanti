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
		config.HudSize = driver->getScreenSize();

		// This reference is only valid for immediate use.
		const auto& hud_mode = g_settings->get("xr_hud");
		f32 aspect_ratio = (f32)config.HudSize.Width / config.HudSize.Height;
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

class DrawMouse : public RenderStep {
public:
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
	video::IImage* cursorImage = nullptr;
	video::ITexture* cursorTexture = nullptr;

	virtual void setRenderSource(RenderSource *) override {}
	virtual void setRenderTarget(RenderTarget *target) override {}

	virtual void reset(PipelineContext &context) override {}
	virtual void run(PipelineContext &context) override {
		auto device = context.device;
		auto driver = device->getVideoDriver();
		auto control = device->getCursorControl();
		if (!cursorTexture) {
			initTexture(context);
		}

		if (isMenuActive() && control) {
			core::position2d<s32> mousePos = control->getPosition();
			driver->draw2DImage(cursorTexture, mousePos, true);
		}
	}
private:
	void initTexture(PipelineContext &context) {
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
		auto driver = context.device->getVideoDriver();
		cursorImage = driver->createImageFromData(
			video::ECF_A8R8G8B8,
			core::dimension2d<u32>(cursorWidth, cursorHeight),
			rawImageData, true, true);
		cursorTexture = driver->addTexture("xr_mouse_cursor", cursorImage);
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
