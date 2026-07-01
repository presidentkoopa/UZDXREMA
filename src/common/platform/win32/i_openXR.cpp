#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "i_input.h"

#ifdef USE_OPENXR

#include "common/rendering/vulkan/stereo3d/vk_openxrdevice.h"

namespace
{
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

	const EJoyAxis DefaultMap[NUM_AXES] =
	{
		JOYAXIS_Side,
		JOYAXIS_Side,
		JOYAXIS_Forward,
		JOYAXIS_Forward,
		JOYAXIS_Yaw,
		JOYAXIS_Yaw,
		JOYAXIS_Up,
		JOYAXIS_Up,
	};

	class FOpenXRJoystick : public IJoystickConfig
	{
	public:
		FOpenXRJoystick()
		{
			SetDefaultConfig();
			Multiplier = 1.0f;
			M_LoadJoystickConfig(this);
		}

		~FOpenXRJoystick()
		{
			M_SaveJoystickConfig(this);
		}

		FString GetName() override { return "OpenXR"; }
		float GetSensitivity() override { return Multiplier; }
		void SetSensitivity(float scale) override { Multiplier = scale; }
		int GetNumAxes() override { return NUM_AXES; }

		float GetAxisDeadZone(int axis) override
		{
			return unsigned(axis) < NUM_AXES ? Axes[axis].DeadZone : 0.0f;
		}

		EJoyAxis GetAxisMap(int axis) override
		{
			return unsigned(axis) < NUM_AXES ? Axes[axis].GameAxis : JOYAXIS_None;
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

		void SetAxisDeadZone(int axis, float v) override
		{
			if (unsigned(axis) < NUM_AXES) Axes[axis].DeadZone = v;
		}

		void SetAxisMap(int axis, EJoyAxis map) override
		{
			if (unsigned(axis) < NUM_AXES) Axes[axis].GameAxis = map;
		}

		void SetAxisScale(int axis, float v) override
		{
			if (unsigned(axis) < NUM_AXES) Axes[axis].Multiplier = v;
		}

		bool GetEnabled() override { return true; }
		void SetEnabled(bool enabled) override { (void)enabled; }
		bool AllowsEnabledInBackground() override { return true; }
		bool GetEnabledInBackground() override { return true; }
		void SetEnabledInBackground(bool enabled) override { (void)enabled; }
		bool IsSensitivityDefault() override { return Multiplier == 1.0f; }
		bool IsAxisDeadZoneDefault(int axis) override { return Axes[axis].DeadZone == DEFAULT_DEADZONE; }
		bool IsAxisMapDefault(int axis) override { return Axes[axis].GameAxis == DefaultMap[axis]; }
		bool IsAxisScaleDefault(int axis) override { return Axes[axis].Multiplier == 1.0f; }

		void SetDefaultConfig() override
		{
			for (int i = 0; i < NUM_AXES; ++i)
			{
				Axes[i].GameAxis = DefaultMap[i];
				Axes[i].DeadZone = DEFAULT_DEADZONE;
				Axes[i].Multiplier = 1.0f;
			}
		}

		FString GetIdentifier() override { return "OpenXR"; }

	private:
		struct AxisInfo
		{
			float Multiplier;
			float DeadZone;
			EJoyAxis GameAxis;
			FString Name;
		};

		float Multiplier = 1.0f;
		AxisInfo Axes[NUM_AXES];
	};

	class FOpenXRJoystickManager : public FJoystickCollection
	{
	public:
		bool GetDevice() override
		{
			return true;
		}

		void AddAxes(float axes[NUM_JOYAXIS]) override
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
