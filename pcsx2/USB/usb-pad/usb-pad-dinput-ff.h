// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "USB/usb-pad/usb-pad.h"
#include "Input/DInputSource.h"

namespace usb_pad
{
	// Force feedback through DirectInput, as used up to v1.7.3727 (USB/usb-pad/dx/dinput.cpp).
	// Needed for wheels such as the OpenFFBoard, where SDL haptic gives no or erratic FFB.
	class DInputFFDevice final : public FFDevice
	{
	public:
		~DInputFFDevice() override;

		static std::unique_ptr<DInputFFDevice> Create(const std::string_view device);

		void SetConstantForce(int level) override;
		void SetSpringForce(const parsed_ff_data& ff) override;
		void SetDamperForce(const parsed_ff_data& ff) override;
		void SetFrictionForce(const parsed_ff_data& ff) override;
		void SetAutoCenter(int value) override;
		void SetGain(int percent) override;
		void DisableForce(EffectID force) override;

	private:
		// Effect parameter blocks are referenced by pointer from DIEFFECT, so they must not move.
		struct Effect
		{
			wil::com_ptr_nothrow<IDirectInputEffect> effect;
			DIEFFECT params = {};
			DICONSTANTFORCE constant = {};
			DICONDITION condition = {};
			bool created = false;
		};

		DInputFFDevice(IDirectInputDevice8W* device);

		bool CreateEffects(const std::string_view device);
		bool CreateEffect(Effect& fx, REFGUID type, const char* name);
		void StopEffect(Effect& fx);
		void SetConditionForce(Effect& fx, const char* name, const parsed_ff_data& ff);
		void UpdateEffect(Effect& fx, const char* name);

		wil::com_ptr_nothrow<IDirectInputDevice8W> m_device;

		DWORD m_axes[1] = {DIJOFS_X};
		LONG m_direction[1] = {0};

		Effect m_constant;
		Effect m_spring;
		Effect m_damper;
		Effect m_friction;

		int m_last_constant_level = 0;
	};
} // namespace usb_pad
