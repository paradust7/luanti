#ifdef _IRR_COMPILE_WITH_XR_DEVICE_

#include "OpenXRHeaders.h"
#include "IOpenXRInput.h"
#include "Common.h"
#include "OpenXRMath.h"
#include "os.h"

#define HAND_COUNT        2
#define HAND_LEFT_PREFIX  "/user/hand/left"
#define HAND_RIGHT_PREFIX "/user/hand/right"

using std::unique_ptr;

class COpenXRInput : public IOpenXRInput {
public:
	COpenXRInput(
		XrInstance instance,
		XrSession session)
		: Instance(instance), Session(session) {}

	virtual ~COpenXRInput()
	{
		for (int i = 0; i < HAND_COUNT; i++) {
			if (GripSpace[i] != XR_NULL_HANDLE)
				xrDestroySpace(GripSpace[i]);
			if (AimSpace[i] != XR_NULL_HANDLE)
				xrDestroySpace(AimSpace[i]);
		}
		if (MainActionSet != XR_NULL_HANDLE)
			xrDestroyActionSet(MainActionSet);
	}

	bool init() {
		State = core::XrInputState{};
		if (!setupActions()) return false;
		if (!setupBindings()) return false;
		if (!attachSet()) return false;
		return true;
	}
	bool setupActions();
	bool setupBindings();
	bool attachSet();

	struct BindingRecord {
		XrAction action;
		const char* suffix;
	};

	bool suggestBindings(
		const char* profile,
		std::initializer_list<BindingRecord> bindingsList);

	struct UpdateInfo {
		XrTime predictedDisplayTime;
		XrSpace baseSpace;
	};

	virtual bool updateState(XrSessionState sessionState, XrTime predictedDisplayTime, XrSpace baseSpace) override;
	virtual void getInputState(core::XrInputState* state) override;
	bool updateHand(const UpdateInfo& updateInfo, XrPath handPath, core::XrInputHand* handState, int i);
	bool updatePose(const UpdateInfo& updateInfo, XrPath handpath, core::XrInputPose* poseState, XrAction poseAction, XrSpace actionSpace);
	bool updateButton(
		const UpdateInfo& updateInfo,
		XrPath handPath,
		core::XrButton* buttonState,
		XrAction clickAction,
		XrAction touchAction,
		XrAction valueAction);

	XrInstance Instance;
	XrSession Session;

	XrActionSet MainActionSet = XR_NULL_HANDLE;
	XrPath HandPaths[HAND_COUNT] = {XR_NULL_PATH, XR_NULL_PATH};

	// Each XrAction is used for both hands
	XrAction GripPose = XR_NULL_HANDLE;
	XrAction AimPose = XR_NULL_HANDLE;
	XrAction AttackClick = XR_NULL_HANDLE;
	XrAction AttackTrigger = XR_NULL_HANDLE;
	XrAction UseClick = XR_NULL_HANDLE;
	XrAction MenuClick = XR_NULL_HANDLE;

	XrSpace GripSpace[HAND_COUNT] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	XrSpace AimSpace[HAND_COUNT] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

	// This is how we communicate with irrlicht / minetest
	core::XrInputState State;
private:
	bool check(XrResult result, const char* func)
	{
		return openxr_check(Instance, result, func);
	}
};


bool COpenXRInput::setupActions()
{
	XrActionSetCreateInfo setCreateInfo = {
		.type = XR_TYPE_ACTION_SET_CREATE_INFO,
		.priority = 0,
	};
	strcpy(setCreateInfo.actionSetName, "mainactions");
	strcpy(setCreateInfo.localizedActionSetName, "Main Actions");

	XR_CHECK(xrCreateActionSet, Instance, &setCreateInfo, &MainActionSet);
	XR_CHECK(xrStringToPath, Instance, HAND_LEFT_PREFIX, &HandPaths[0]);
	XR_CHECK(xrStringToPath, Instance, HAND_RIGHT_PREFIX, &HandPaths[1]);

	struct {
		XrAction* actionptr;
		const char* name;
		const char* localized;
		XrActionType actionType;
	} actionsToCreate[] = {
		{ &GripPose, "grippose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT },
		{ &AimPose, "aimpose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT },
		{ &AttackClick, "attack", "Attack", XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &AttackTrigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT },
		{ &UseClick, "use", "Use/Interact", XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &MenuClick, "menu", "Open Menu", XR_ACTION_TYPE_BOOLEAN_INPUT },
	};

	for (auto &rec : actionsToCreate) {
		XrActionCreateInfo createInfo = {
			.type = XR_TYPE_ACTION_CREATE_INFO,
			.next = NULL,
			.actionType = rec.actionType,
			.countSubactionPaths = HAND_COUNT,
			.subactionPaths = HandPaths,
		};
		snprintf_irr(createInfo.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", rec.name);
		snprintf_irr(createInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", rec.localized);
		XR_CHECK(xrCreateAction, MainActionSet, &createInfo, rec.actionptr);
	};

	// Create grip pose spaces
	for (int i = 0; i < HAND_COUNT; i++) {
		XrActionSpaceCreateInfo spaceCreateInfo = {
			.type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
			.action = GripPose,
			.subactionPath = HandPaths[i],
			.poseInActionSpace = IdentityPose,
		};
		XR_CHECK(xrCreateActionSpace, Session, &spaceCreateInfo, &GripSpace[i]);
	}

	// Create aim pose spaces
	for (int i = 0; i < HAND_COUNT; i++) {
		XrActionSpaceCreateInfo spaceCreateInfo = {
			.type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
			.action = AimPose,
			.subactionPath = HandPaths[i],
			.poseInActionSpace = IdentityPose,
		};
		XR_CHECK(xrCreateActionSpace, Session, &spaceCreateInfo, &AimSpace[i]);
	}
	return true;

}


bool COpenXRInput::setupBindings()
{
	// Give the runtime default binding recommendations
	// The runtime is free to alter the bindings, and it
	// may provide the user a way to modify and save changes
	// to the bindings.

	// Simple Controller Profile
	bool ok = suggestBindings(
		"/interaction_profiles/khr/simple_controller",
		{
			{ GripPose, "/input/grip/pose" },
			{ AimPose, "/input/aim/pose" },
			// This profile only has two buttons
			{ AttackClick, "/input/select/click" },
			{ UseClick, "/input/menu/click" },
			//{ MenuClick, "/input/menu/click" },
		});
	if (!ok)
		return false;

	// Valve Index
	ok = suggestBindings("/interaction_profiles/valve/index_controller",
		{
			{ GripPose, "/input/grip/pose" },
			{ AimPose, "/input/aim/pose" },
			{ AttackClick, "/input/trigger/click" },
			{ UseClick, "/input/a/click" },
			{ MenuClick, "/input/b/click" },
		});
	if (!ok)
		return false;

	return true;
}

bool COpenXRInput::suggestBindings(
	const char* profilePath,
	std::initializer_list<BindingRecord> bindingsList)
{
	XrPath interactionProfile;
	XR_CHECK(xrStringToPath, Instance, profilePath, &interactionProfile);

	std::vector<XrActionSuggestedBinding> bindings;
	for (const auto &r : bindingsList) {
		XrPath left;
		XrPath right;
		char buf[128];
		snprintf_irr(buf, sizeof(buf), "%s%s", HAND_LEFT_PREFIX, r.suffix);
		XR_CHECK(xrStringToPath, Instance, buf, &left);
		snprintf_irr(buf, sizeof(buf), "%s%s", HAND_RIGHT_PREFIX, r.suffix);
		XR_CHECK(xrStringToPath, Instance, buf, &right);
		bindings.push_back({
			.action = r.action,
			.binding = left});
		bindings.push_back({
			.action = r.action,
			.binding = right});
	}

	const XrInteractionProfileSuggestedBinding suggestedBindings = {
		.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
		.interactionProfile = interactionProfile,
		.countSuggestedBindings = (uint32_t)bindings.size(),
		.suggestedBindings = bindings.data(),
	};
	XR_CHECK(xrSuggestInteractionProfileBindings, Instance, &suggestedBindings);
	return true;
}

bool COpenXRInput::attachSet()
{
	const XrSessionActionSetsAttachInfo attachInfo = {
		.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
		.countActionSets = 1,
		.actionSets = &MainActionSet,
	};
	XR_CHECK(xrAttachSessionActionSets, Session, &attachInfo);
	return true;
}

bool COpenXRInput::updateState(XrSessionState sessionState, XrTime predictedDisplayTime, XrSpace baseSpace)
{
	const XrActiveActionSet activeActionSets[] = {
		{.actionSet = MainActionSet, .subactionPath = XR_NULL_PATH}
	};
	XrActionsSyncInfo syncInfo = {
		.type = XR_TYPE_ACTIONS_SYNC_INFO,
		.countActiveActionSets = sizeof(activeActionSets)/sizeof(*activeActionSets),
		.activeActionSets = activeActionSets,
	};
	XrResult result = xrSyncActions(Session, &syncInfo);
	if (result == XR_SESSION_NOT_FOCUSED) {
		// This can happen if there's a delay receiving the session state update event.
		State = core::XrInputState{};
		return true;
	}
	if (!check(result, "xrSyncActions"))
		return false;

	UpdateInfo updateInfo = {
		.predictedDisplayTime = predictedDisplayTime,
		.baseSpace = baseSpace,
	};
	for (int i = 0; i < HAND_COUNT; i++) {
		if (!updateHand(updateInfo, HandPaths[i], &State.Hand[i], i))
			return false;
	}
	return true;
}

bool COpenXRInput::updateHand(const UpdateInfo& updateInfo, XrPath handPath, core::XrInputHand* handState, int i)
{
	if (!updatePose(updateInfo, handPath, &handState->Aim, AimPose, AimSpace[i]))
		return false;

	if (!updatePose(updateInfo, handPath, &handState->Grip, GripPose, GripSpace[i]))
		return false;

	if (!updateButton(updateInfo, handPath, &handState->Attack, AttackClick, XR_NULL_HANDLE, AttackTrigger))
		return false;

	if (!updateButton(updateInfo, handPath, &handState->Use, UseClick, XR_NULL_HANDLE, XR_NULL_HANDLE))
		return false;

	if (!updateButton(updateInfo, handPath, &handState->Menu, MenuClick, XR_NULL_HANDLE, XR_NULL_HANDLE))
		return false;


	return true;
}

bool COpenXRInput::updatePose(const UpdateInfo& updateInfo, XrPath handPath, core::XrInputPose* poseState, XrAction poseAction, XrSpace actionSpace)
{
	// xrGetActionStatePose only tells us whether there is an active device
	XrActionStatePose actionPoseState = {
		.type = XR_TYPE_ACTION_STATE_POSE,
	};
	XrActionStateGetInfo getInfo = {
		.type = XR_TYPE_ACTION_STATE_GET_INFO,
		.action = poseAction,
		.subactionPath = handPath,
	};
	XR_CHECK(xrGetActionStatePose, Session, &getInfo, &actionPoseState);

	// TODO: Is there any reason to look at actionPoseState.isActive ?

	XrSpaceLocation location = {
		.type = XR_TYPE_SPACE_LOCATION,
	};
	XR_CHECK(xrLocateSpace, actionSpace, updateInfo.baseSpace, updateInfo.predictedDisplayTime, &location);
	bool valid =
		(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
		(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
	poseState->Valid = valid;
	if (valid) {
		poseState->Pose = xr_to_irrlicht(location.pose);
	} else {
		poseState->Pose = core::pose();
	}
	return true;
}

bool COpenXRInput::updateButton(
	const UpdateInfo& updateInfo,
	XrPath handPath,
	core::XrButton* buttonState,
	XrAction clickAction,
	XrAction touchAction,
	XrAction valueAction)
{
	if (clickAction != XR_NULL_HANDLE) {
		XrActionStateBoolean result = {
			.type = XR_TYPE_ACTION_STATE_BOOLEAN,
		};
		XrActionStateGetInfo getInfo = {
			.type = XR_TYPE_ACTION_STATE_GET_INFO,
			.action = clickAction,
			.subactionPath = handPath,
		};
		XR_CHECK(xrGetActionStateBoolean, Session, &getInfo, &result);
		buttonState->Pressed = result.currentState;
	} else {
		buttonState->Pressed = false;
	}

	if (touchAction != XR_NULL_HANDLE) {
		XrActionStateBoolean result = {
			.type = XR_TYPE_ACTION_STATE_BOOLEAN,
		};
		XrActionStateGetInfo getInfo = {
			.type = XR_TYPE_ACTION_STATE_GET_INFO,
			.action = touchAction,
			.subactionPath = handPath,
		};
		XR_CHECK(xrGetActionStateBoolean, Session, &getInfo, &result);
		buttonState->Touched = result.currentState;
	} else {
		buttonState->Touched = false;
	}

	if (valueAction != XR_NULL_HANDLE) {
		XrActionStateFloat result = {
			.type = XR_TYPE_ACTION_STATE_FLOAT,
		};
		XrActionStateGetInfo getInfo = {
			.type = XR_TYPE_ACTION_STATE_GET_INFO,
			.action = valueAction,
			.subactionPath = handPath,
		};
		XR_CHECK(xrGetActionStateFloat, Session, &getInfo, &result);
		buttonState->Value = result.currentState;
	} else {
		buttonState->Value = 0;
	}
	return true;
}


void COpenXRInput::getInputState(core::XrInputState* state)
{
	*state = State;
}

unique_ptr<IOpenXRInput> createOpenXRInput(
        XrInstance instance,
        XrSession session)
{
	unique_ptr<COpenXRInput> obj(new COpenXRInput(instance, session));
	if (!obj->init()) {
		os::Printer::log("[XR] createOpenXRInput failed", ELL_ERROR);
		return nullptr;
	}
	return obj;
}

#endif // _IRR_COMPILE_WITH_XR_DEVICE_

