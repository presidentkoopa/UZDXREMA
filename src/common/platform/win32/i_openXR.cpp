#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "i_input.h"

#ifdef USE_OPENXR

#include "common/rendering/vulkan/stereo3d/vk_openxrdevice.h"

namespace
{
	// Deliberately larger than JOYDEADZONE_DEFAULT: VR trackpads and thumbsticks
	// rest noisily, so this fork has always used a wider default dead zone.
	// (Kept as fork behaviour rather than switching to JOYDEADZONE_DEFAULT.)
	constexpr float DEFAULT_DEADZONE = 0.25f;

	enum Hand
	{
		ON,
		OFF
	};

	enum Source
	{
		STICK,
		PAD
	};

	enum Axis
	{
		X,
		Y
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

	const Hand Hands[NUM_AXES] = { OFF, OFF, OFF, OFF, ON, ON, ON, ON };
	const Source Sources[NUM_AXES] = { PAD, STICK, PAD, STICK, PAD, STICK, PAD, STICK };
	const Axis AxisSources[NUM_AXES] = { X, X, Y, Y, X, X, Y, Y };

	// ------------------------------------------------------------------------
	// FORK NOTE -- former DefaultMap[NUM_AXES] table (removed, not lost).
	//
	// Up to UZDoom 5.0.0-rc.2 the joystick config interface owned the
	// axis-to-game-function mapping (EJoyAxis / GetAxisMap / SetAxisMap /
	// IsAxisMapDefault). That whole concept was deleted upstream: axis-to-
	// function binding now lives in the bindings system, addressed by axis
	// codes (see NUM_AXIS_CODES / AXIS_CODE_* in keydef.h), not by a per-device
	// table. There is therefore no longer any place in IJoystickConfig to
	// express what this fork's DefaultMap[] said.
	//
	// The mapping this fork shipped, recorded verbatim so it can be rebuilt:
	//
	//     OFF_HAND_PAD_X    -> JOYAXIS_Side      (strafe)
	//     OFF_HAND_STICK_X  -> JOYAXIS_Side      (strafe)
	//     OFF_HAND_PAD_Y    -> JOYAXIS_Forward   (move forward/back)
	//     OFF_HAND_STICK_Y  -> JOYAXIS_Forward   (move forward/back)
	//     ON_HAND_PAD_X     -> JOYAXIS_Yaw       (turn)
	//     ON_HAND_STICK_X   -> JOYAXIS_Yaw       (turn)
	//     ON_HAND_PAD_Y     -> JOYAXIS_Up        (fly up/down)
	//     ON_HAND_STICK_Y   -> JOYAXIS_Up        (fly up/down)
	//
	// i.e. off hand = movement (X strafe, Y forward), on hand = turn (X) and
	// vertical (Y). Note "on hand"/"off hand" are resolved at runtime against
	// s3d::OpenXROnHandIsRight(), so the physical left/right assignment follows
	// the player's handedness setting, not a fixed controller.
	//
	// TODO: re-express the above as DEFAULT AXIS-CODE BINDINGS (the +/- axis
	// codes this device contributes in AddAxes(), bound to the movement/turn
	// commands) so the out-of-the-box VR control scheme matches again. Until
	// that is done the axes are reported to the config/menu but carry no
	// default game function.
	// ------------------------------------------------------------------------

	class FOpenXRJoystick : public IJoystickConfig
	{
	public:
		FOpenXRJoystick()
		{
			SetDefaultConfig();
			Multiplier = JOYSENSITIVITY_DEFAULT;
			M_LoadJoystickConfig(this);
		}

		~FOpenXRJoystick()
		{
			M_SaveJoystickConfig(this);
		}

		FString GetName() override { return "OpenXR"; }
		float GetSensitivity() override { return Multiplier; }
		void SetSensitivity(float scale) override { Multiplier = scale; }
		// This device has no haptics plumbing through the OpenXR input layer
		// yet, so mirror the DirectInput backend and report none.
		bool HasHaptics() override { return false; }
		float GetHapticsStrength() override { return JOYHAPSTRENGTH_DEFAULT; }
		void SetHapticsStrength(float strength) override { (void)strength; }
		bool IsHapticsStrengthDefault() override { return true; }

		int GetNumAxes() override { return NUM_AXES; }

		float GetAxisDeadZone(int axis) override
		{
			return unsigned(axis) < NUM_AXES ? Axes[axis].DeadZone : 0.0f;
		}

		const char* GetAxisName(int axis) override
		{
			FString& name = Axes[axis].Name;
			name = "";
			const bool onHandIsRight = s3d::OpenXROnHandIsRight();
			name += onHandIsRight ? (Hands[axis] == ON ? "Right " : "Left ")
				: (Hands[axis] == ON ? "Left " : "Right ");
			name += Sources[axis] == PAD ? "Trackpad " : "Thumbstick ";
			name += AxisSources[axis] == X ? "Horizontal" : "Vertical";
			return name.GetChars();
		}

		float GetAxisScale(int axis) override
		{
			return unsigned(axis) < NUM_AXES ? Axes[axis].Multiplier : 0.0f;
		}

		float GetAxisDigitalThreshold(int axis) override
		{
			return unsigned(axis) < NUM_AXES ? Axes[axis].DigitalThreshold : JOYTHRESH_DEFAULT;
		}

		EJoyCurve GetAxisResponseCurve(int axis) override
		{
			return unsigned(axis) < NUM_AXES ? Axes[axis].ResponseCurvePreset : JOYCURVE_DEFAULT;
		}

		float GetAxisResponseCurvePoint(int axis, int point) override
		{
			if (unsigned(axis) >= NUM_AXES || unsigned(point) >= 4) return 0.0f;
			return Axes[axis].ResponseCurve.pts[point];
		}

		void SetAxisDeadZone(int axis, float v) override
		{
			if (unsigned(axis) < NUM_AXES) Axes[axis].DeadZone = clamp(v, 0.0f, 1.0f);
		}

		void SetAxisScale(int axis, float v) override
		{
			if (unsigned(axis) < NUM_AXES) Axes[axis].Multiplier = v;
		}

		void SetAxisDigitalThreshold(int axis, float threshold) override
		{
			if (unsigned(axis) < NUM_AXES) Axes[axis].DigitalThreshold = threshold;
		}

		void SetAxisResponseCurve(int axis, EJoyCurve preset) override
		{
			if (unsigned(axis) >= NUM_AXES) return;
			if (preset >= NUM_JOYCURVE || preset < JOYCURVE_CUSTOM) return;
			Axes[axis].ResponseCurvePreset = preset;
			if (preset == JOYCURVE_CUSTOM) return;
			Axes[axis].ResponseCurve = JOYCURVE[preset];
		}

		void SetAxisResponseCurvePoint(int axis, int point, float value) override
		{
			if (unsigned(axis) < NUM_AXES && unsigned(point) < 4)
			{
				Axes[axis].ResponseCurvePreset = JOYCURVE_CUSTOM;
				Axes[axis].ResponseCurve.pts[point] = value;
			}
		}

		bool GetEnabled() override { return true; }
		void SetEnabled(bool enabled) override { (void)enabled; }
		bool AllowsEnabledInBackground() override { return true; }
		bool GetEnabledInBackground() override { return true; }
		void SetEnabledInBackground(bool enabled) override { (void)enabled; }
		bool IsSensitivityDefault() override { return Multiplier == JOYSENSITIVITY_DEFAULT; }

		bool IsAxisDeadZoneDefault(int axis) override
		{
			return unsigned(axis) < NUM_AXES ? Axes[axis].DeadZone == Axes[axis].DefaultDeadZone : true;
		}

		bool IsAxisScaleDefault(int axis) override
		{
			return unsigned(axis) < NUM_AXES ? Axes[axis].Multiplier == Axes[axis].DefaultMultiplier : true;
		}

		bool IsAxisDigitalThresholdDefault(int axis) override
		{
			return unsigned(axis) < NUM_AXES ? Axes[axis].DigitalThreshold == Axes[axis].DefaultDigitalThreshold : true;
		}

		bool IsAxisResponseCurveDefault(int axis) override
		{
			return unsigned(axis) < NUM_AXES ? Axes[axis].ResponseCurvePreset == Axes[axis].DefaultResponseCurvePreset : true;
		}

		void SetDefaultConfig() override
		{
			for (int i = 0; i < NUM_AXES; ++i)
			{
				Axes[i].DeadZone = DEFAULT_DEADZONE;
				Axes[i].Multiplier = JOYSENSITIVITY_DEFAULT;
				// Every axis here is one half of a thumbstick/trackpad, so use
				// the stick thresholds rather than JOYTHRESH_DEFAULT.
				Axes[i].DigitalThreshold = AxisSources[i] == X ? JOYTHRESH_STICK_X : JOYTHRESH_STICK_Y;
				Axes[i].ResponseCurvePreset = JOYCURVE_DEFAULT;
				Axes[i].ResponseCurve = JOYCURVE[JOYCURVE_DEFAULT];

				// Preserve defaults so the config saver can skip untouched values.
				Axes[i].DefaultDeadZone = Axes[i].DeadZone;
				Axes[i].DefaultMultiplier = Axes[i].Multiplier;
				Axes[i].DefaultDigitalThreshold = Axes[i].DigitalThreshold;
				Axes[i].DefaultResponseCurvePreset = Axes[i].ResponseCurvePreset;
			}
		}

		FString GetIdentifier() override { return "OpenXR"; }

	private:
		struct AxisInfo
		{
			float Multiplier, DefaultMultiplier;
			float DeadZone, DefaultDeadZone;
			float DigitalThreshold, DefaultDigitalThreshold;
			EJoyCurve ResponseCurvePreset, DefaultResponseCurvePreset;
			CubicBezier ResponseCurve;
			FString Name;
		};

		float Multiplier = JOYSENSITIVITY_DEFAULT;
		AxisInfo Axes[NUM_AXES];
	};

	class FOpenXRJoystickManager : public FJoystickCollection
	{
	public:
		bool GetDevice() override
		{
			return true;
		}

		// Signature must match FJoystickCollection::AddAxes, which now takes the
		// axis-code array (NUM_AXIS_CODES) instead of the old NUM_JOYAXIS
		// game-function array. Still a stub: the OpenXR device does not feed
		// axis values in through this path.
		void AddAxes(float axes[NUM_AXIS_CODES]) override
		{
			(void)axes;
		}

		void GetDevices(TArray<IJoystickConfig*>& sticks) override
		{
			if (s3d::OpenXRInputDeviceAvailable())
			{
				sticks.Push(&mDevice);
			}
		}

		IJoystickConfig* Rescan() override
		{
			return s3d::OpenXRInputDeviceAvailable() ? &mDevice : nullptr;
		}

	private:
		FOpenXRJoystick mDevice;
	};
}

void I_StartupOpenXR()
{
	if (JoyDevices[INPUT_OpenXR] == NULL)
	{
		JoyDevices[INPUT_OpenXR] = new FOpenXRJoystickManager;
	}
}

#endif
