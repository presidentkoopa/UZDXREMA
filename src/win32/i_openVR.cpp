#include <Windows.h>
#include "c_cvars.h"
#include "d_event.h"
#include "i_input.h"
#include "openvr_include.h"
#include "menu.h"

using namespace openvr;

EXTERN_CVAR(Bool, vr_secondary_button_mappings);

namespace s3d
{
	bool OpenVR_OnHandIsRight();
	VRControllerState_t& OpenVR_GetState(int hand);
	int OpenVR_GetTouchPadAxis();
	int OpenVR_GetJoystickAxis();
}

const float DEFAULT_DEADZONE = 0.25f;

enum Hand
{
	ON, OFF
};

enum Source
{
	STICK, PAD
};

enum Axis
{
	X, Y
};


enum AxisID
{
	OFF_HAND_PAD_X,
	OFF_HAND_STICK_X,
	OFF_HAND_PAD_Y,
	OFF_HAND_STICK_Y,

	ON_HAND_PAD_X,
	ON_HAND_STICK_X,
	ON_HAND_PAD_Y,
	ON_HAND_STICK_Y,

	NUM_AXES
};

static const Hand Hands[NUM_AXES] = { OFF, OFF, OFF, OFF, ON, ON, ON, ON };
static const Source Sources[NUM_AXES] = { PAD, STICK, PAD, STICK, PAD, STICK, PAD, STICK };
static const Axis AxisSources[NUM_AXES] = { X, X, Y, Y, X, X, Y, Y };

//===========================================================================
//
// FORK NOTE -- former "DefaultMap[]" axis-to-game-function table
//
// UZDoom 5.0.0-rc.2 removed the per-device axis mapping API from
// IJoystickConfig: enum EJoyAxis, GetAxisMap(), SetAxisMap() and
// IsAxisMapDefault() no longer exist. Axis-to-game-function binding now lives
// in the bindings system, addressed by axis codes (see NUM_AXIS_CODES), and is
// no longer something a device driver declares for itself.
//
// The mapping this fork used to ship as its default -- and which the user was
// free to override from the joystick menu -- was exactly:
//
//     OFF_HAND_PAD_X    -> JOYAXIS_Side      (off-hand pad, horizontal)
//     OFF_HAND_STICK_X  -> JOYAXIS_Side      (off-hand thumbstick, horizontal)
//     OFF_HAND_PAD_Y    -> JOYAXIS_Forward   (off-hand pad, vertical)
//     OFF_HAND_STICK_Y  -> JOYAXIS_Forward   (off-hand thumbstick, vertical)
//     ON_HAND_PAD_X     -> JOYAXIS_Yaw       (on-hand/dominant pad, horizontal)
//     ON_HAND_STICK_X   -> JOYAXIS_Yaw       (on-hand/dominant stick, horizontal)
//     ON_HAND_PAD_Y     -> JOYAXIS_Up        (on-hand/dominant pad, vertical)
//     ON_HAND_STICK_Y   -> JOYAXIS_Up        (on-hand/dominant stick, vertical)
//
// Note that nothing was ever mapped to JOYAXIS_Pitch by default, which is why
// GetPitch() below yields 0 unless the table is changed.
//
// TODO: this default has to be re-expressed as default axis-code BINDINGS in
// the new bindings system. Until that is done, the *configurable* half of the
// old behaviour (the user remapping a VR axis to a different game function from
// the menu) is gone -- there is nowhere in the new IJoystickConfig to put it.
//
// The table itself is kept below in fork-local form, because it is NOT only
// configuration: GetYaw()/GetPitch()/GetDirectionalMove() are VR-specific
// accumulators read once per *render* frame by gl_openvr.cpp (via
// I_OpenVRGetYaw/Pitch/DirectionalMove) and they need to know which physical VR
// axis drives which motion. Deleting the table outright would silently kill VR
// smooth turning and stick locomotion. It is now a private enum rather than
// EJoyAxis so it carries no implication of being engine-visible, and it is
// const because it is no longer user-editable.
//
//===========================================================================

enum VRAxisFunction
{
	VRFUNC_None,
	VRFUNC_Yaw,
	VRFUNC_Pitch,
	VRFUNC_Forward,
	VRFUNC_Side,
	VRFUNC_Up,
};

static const VRAxisFunction AxisFunctions[NUM_AXES] =
{
	VRFUNC_Side,		// OFF_HAND_PAD_X
	VRFUNC_Side,		// OFF_HAND_STICK_X
	VRFUNC_Forward,		// OFF_HAND_PAD_Y
	VRFUNC_Forward,		// OFF_HAND_STICK_Y
	VRFUNC_Yaw,			// ON_HAND_PAD_X
	VRFUNC_Yaw,			// ON_HAND_STICK_X
	VRFUNC_Up,			// ON_HAND_PAD_Y
	VRFUNC_Up,			// ON_HAND_STICK_Y
};


class FOpenVRJoystick : public IJoystickConfig
{
public:
	FOpenVRJoystick()
	{
		SetDefaultConfig();
		Multiplier = 1;
		M_LoadJoystickConfig(this);
	}
	
	~FOpenVRJoystick()
	{
		M_SaveJoystickConfig(this);
	}


	void ProcessInput()
	{

	}

	float GetAxisValue(int i, VRControllerState_t& offState, VRControllerState_t& onState)
	{
		//joysticks should be disabled while menu is shown, otherwise player moves while scrolling menu
		if (CurrentMenu == nullptr)
		{
			constexpr uint64_t kOpenVRGripButtonMask = (1ULL << 2);
			const bool dominantGripModifier = vr_secondary_button_mappings &&
				((onState.ulButtonPressed & kOpenVRGripButtonMask) != 0);
			if (dominantGripModifier && Hands[i] == OFF && Sources[i] == STICK)
			{
				return 0.0f;
			}

			VRControllerState_t& state = Hands[i] == ON ? onState : offState;
			int axis = Sources[i] == PAD ? s3d::OpenVR_GetTouchPadAxis() : s3d::OpenVR_GetJoystickAxis();
			if (axis != -1)
			{
				float value = AxisSources[i] == X ? -state.rAxis[axis].x : state.rAxis[axis].y;
				if (fabsf(value) > Axes[i].DeadZone)
				{
					return value * Axes[i].Multiplier * Multiplier;
				}
			}
		}
		return 0.0f;
	}

	void AddAxes(float axes[NUM_AXIS_CODES])
	{
		// OpenVR gameplay should be driven by the VR backend's explicit movement, turn, and
		// stick-to-button paths rather than the engine's generic joystick gameplay axes. Leaving
		// this enabled causes backend-specific hidden actions to leak through and bypass binds
		(void)axes;
	}
	
	float GetYaw()
	{
		VRControllerState_t& onState = s3d::OpenVR_GetState(1);
		VRControllerState_t& offState = s3d::OpenVR_GetState(0);

		float yaw = 0.0f;

		for (int i = 0; i < NUM_AXES; i++)
		{
			//yaw needs special handling - must accumulate per render frame, not logical frame
			if (AxisFunctions[i] == VRFUNC_Yaw)
			{
				yaw += GetAxisValue(i, offState, onState);
			}
		}

		return yaw;
	}

	float GetPitch()
	{
		VRControllerState_t& onState = s3d::OpenVR_GetState(1);
		VRControllerState_t& offState = s3d::OpenVR_GetState(0);

		float pitch = 0.0f;

		for (int i = 0; i < NUM_AXES; i++)
		{
			// nothing is assigned to pitch by default; see the fork note above
			if (AxisFunctions[i] == VRFUNC_Pitch)
			{
				pitch += GetAxisValue(i, offState, onState);
			}
		}

		return pitch;
	}
	
	float GetDirectionalMove()
	{
		VRControllerState_t& onState = s3d::OpenVR_GetState(1);
		VRControllerState_t& offState = s3d::OpenVR_GetState(0);

		float yaw = 0.0f;

		for (int i = 0; i < NUM_AXES; i++)
		{
			//must accumulate per render frame, not logical frame
			if (AxisFunctions[i] == VRFUNC_Forward)
			{
				yaw += GetAxisValue(i, offState, onState);
			}
		}

		return yaw;
	}

	FString GetName()
	{
		return "OpenVR";
	}

	float GetSensitivity()
	{
		return Multiplier;
	}

	void SetSensitivity(float scale)
	{
		Multiplier = scale;
	}

	// The OpenVR backend drives haptics itself through its own action set, so it
	// does not expose an engine-side rumble strength. Same stance as FDInputJoystick.
	bool HasHaptics()
	{
		return false;
	}

	float GetHapticsStrength()
	{
		return JOYHAPSTRENGTH_DEFAULT;
	}

	void SetHapticsStrength(float strength)
	{
		// no engine-side haptics on this device
	}

	bool IsHapticsStrengthDefault()
	{
		return true;
	}

	int GetNumAxes()
	{
		return NUM_AXES;
	}

	float GetAxisDeadZone(int axis)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			return Axes[axis].DeadZone;
		}
		return 0;
	}

	float GetAxisDigitalThreshold(int axis)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			return Axes[axis].DigitalThreshold;
		}
		return JOYTHRESH_DEFAULT;
	}

	EJoyCurve GetAxisResponseCurve(int axis)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			return Axes[axis].ResponseCurvePreset;
		}
		return JOYCURVE_DEFAULT;
	}

	float GetAxisResponseCurvePoint(int axis, int point)
	{
		if (unsigned(axis) < NUM_AXES && unsigned(point) < 4)
		{
			return Axes[axis].ResponseCurve.pts[point];
		}
		return 0;
	}

	const char* GetAxisName(int axis)
	{
		FString& name = Axes[axis].Name;
		
		name = "";
		name += s3d::OpenVR_OnHandIsRight() ? (Hands[axis] == ON ? "Right " : "Left ") : (Hands[axis] == ON ? "Left " : "Right ");
		name += Sources[axis] == PAD ? "Pad " : "Joystick ";
		name += AxisSources[axis] == X ? "Horizontal" : "Vertical";

		return name.GetChars();
	}

	float GetAxisScale(int axis)
	{
		return Axes[axis].Multiplier;
	}

	void SetAxisDeadZone(int axis, float v)
	{
		Axes[axis].DeadZone = v;
	}

	void SetAxisScale(int axis, float v)
	{
		Axes[axis].Multiplier = v;
	}

	void SetAxisDigitalThreshold(int axis, float threshold)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			Axes[axis].DigitalThreshold = threshold;
		}
	}

	void SetAxisResponseCurve(int axis, EJoyCurve preset)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			if (preset >= NUM_JOYCURVE || preset < JOYCURVE_CUSTOM) return;
			Axes[axis].ResponseCurvePreset = preset;
			if (preset == JOYCURVE_CUSTOM) return;
			Axes[axis].ResponseCurve = JOYCURVE[preset];
		}
	}

	void SetAxisResponseCurvePoint(int axis, int point, float value)
	{
		if (unsigned(axis) < NUM_AXES && unsigned(point) < 4)
		{
			Axes[axis].ResponseCurvePreset = JOYCURVE_CUSTOM;
			Axes[axis].ResponseCurve.pts[point] = value;
		}
	}

	bool IsSensitivityDefault()
	{
		return Multiplier == JOYSENSITIVITY_DEFAULT;
	}

	bool IsAxisDeadZoneDefault(int axis)
	{
		return Axes[axis].DeadZone == DEFAULT_DEADZONE;
	}

	bool IsAxisScaleDefault(int axis)
	{
		return Axes[axis].Multiplier == 1;
	}

	bool IsAxisDigitalThresholdDefault(int axis)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			return Axes[axis].DigitalThreshold == Axes[axis].DefaultDigitalThreshold;
		}
		return true;
	}

	bool IsAxisResponseCurveDefault(int axis)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			return Axes[axis].ResponseCurvePreset == Axes[axis].DefaultResponseCurvePreset;
		}
		return true;
	}

	void SetDefaultConfig()
	{
		for (int i = 0; i < NUM_AXES; ++i)
		{
			Axes[i].DeadZone = DEFAULT_DEADZONE;
			Axes[i].Multiplier = 1.0f;

			// Every axis on this device is one half of a thumbstick or trackpad,
			// so use the same per-orientation digital thresholds i_dijoy gives to
			// a stick rather than the generic JOYTHRESH_DEFAULT.
			Axes[i].DigitalThreshold = AxisSources[i] == X ? JOYTHRESH_STICK_X : JOYTHRESH_STICK_Y;
			Axes[i].ResponseCurvePreset = JOYCURVE_DEFAULT;
			Axes[i].ResponseCurve = JOYCURVE[JOYCURVE_DEFAULT];

			Axes[i].DefaultDigitalThreshold = Axes[i].DigitalThreshold;
			Axes[i].DefaultResponseCurvePreset = Axes[i].ResponseCurvePreset;
		}
	}

	bool GetEnabled()
	{
		return true;
	}

	void SetEnabled(bool enabled)
	{
	}

	bool AllowsEnabledInBackground()
	{
		return true;
	}

	bool GetEnabledInBackground()
	{
		return true;
	}

	void SetEnabledInBackground(bool enabled)
	{
	}

	FString GetIdentifier()
	{
		return "OpenVR";
	}

	struct AxisInfo
	{
		float Multiplier;
		float DeadZone;
		float DigitalThreshold, DefaultDigitalThreshold;
		EJoyCurve ResponseCurvePreset, DefaultResponseCurvePreset;
		CubicBezier ResponseCurve;
		FString Name;
	};
	
	float Multiplier;
	AxisInfo Axes[NUM_AXES];


	int axisTrackpad = -1;
	int axisJoystick = -1;
	int axisTrigger = -1;
};

class FOpenVRJoystickManager : public FJoystickCollection
{
public:
	bool GetDevice()
	{
		return true;
	}
	void ProcessInput()
	{
		m_device.ProcessInput();
	}
	void AddAxes(float axes[NUM_AXIS_CODES])
	{
		m_device.AddAxes(axes);
	}
	float GetYaw()
	{
		return m_device.GetYaw();
	}
	float GetPitch()
	{
		return m_device.GetPitch();
	}
	float GetDirectionalMove()
	{
		return m_device.GetDirectionalMove();
	}
	void GetDevices(TArray<IJoystickConfig *> &sticks)
	{
		sticks.Push(&m_device);
	}
	IJoystickConfig *Rescan()
	{
		return &m_device;
	}

	FOpenVRJoystick m_device;
};

void I_StartupOpenVR()
{
	if (JoyDevices[INPUT_OpenVR] == NULL)
	{
		FJoystickCollection *joys = new FOpenVRJoystickManager;
		if (joys->GetDevice())
		{
			JoyDevices[INPUT_OpenVR] = joys;
			event_t ev = { EV_DeviceChange };
			D_PostEvent(&ev);
		}
	}
}

float I_OpenVRGetYaw()
{
	if (JoyDevices[INPUT_OpenVR] != NULL)
	{
		return ((FOpenVRJoystickManager*)JoyDevices[INPUT_OpenVR])->GetYaw();
	}
	return 0;
}

float I_OpenVRGetPitch()
{
	if (JoyDevices[INPUT_OpenVR] != NULL)
	{
		return ((FOpenVRJoystickManager*)JoyDevices[INPUT_OpenVR])->GetPitch();
	}
	return 0;
}

float I_OpenVRGetDirectionalMove()
{
	if (JoyDevices[INPUT_OpenVR] != NULL)
	{
		return ((FOpenVRJoystickManager*)JoyDevices[INPUT_OpenVR])->GetDirectionalMove();
	}
	return 0;
}
