// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Input/InputManager.h"
#include "USB/usb-pad/usb-pad-dinput-ff.h"

#include "common/Console.h"

#include <algorithm>
#include <climits>

// Ported from the DirectInput backend of v1.7.3727 (pcsx2/USB/usb-pad/dx/dinput.cpp).
// The conversion of the Logitech effect parameters to DirectInput units is kept identical on purpose,
// since that is the behaviour known to work with the OpenFFBoard.

namespace usb_pad
{
	static LONG ScaleSigned(int value, int source_max)
	{
		const s64 scaled = static_cast<s64>(value) * DI_FFNOMINALMAX / source_max;
		return static_cast<LONG>(std::clamp<s64>(scaled, -DI_FFNOMINALMAX, DI_FFNOMINALMAX));
	}

	static DWORD ScaleUnsigned(int value, int source_max)
	{
		const s64 scaled = static_cast<s64>(value) * DI_FFNOMINALMAX / source_max;
		return static_cast<DWORD>(std::clamp<s64>(scaled, 0, DI_FFNOMINALMAX));
	}

	DInputFFDevice::DInputFFDevice(IDirectInputDevice8W* device)
		: m_device(device)
	{
	}

	DInputFFDevice::~DInputFFDevice()
	{
		StopEffect(m_constant);
		StopEffect(m_spring);
		StopEffect(m_damper);
		StopEffect(m_friction);
	}

	std::unique_ptr<DInputFFDevice> DInputFFDevice::Create(const std::string_view device)
	{
		DInputSource* source = static_cast<DInputSource*>(InputManager::GetInputSourceInterface(InputSourceType::DInput));
		if (!source)
		{
			Console.Error("(DInputFFDevice) The DInput input source is not enabled. Enable it in Controller Settings > Global Settings to use '%.*s' for FF.",
				static_cast<int>(device.size()), device.data());
			return nullptr;
		}

		IDirectInputDevice8W* dev = source->GetDeviceForIdentifier(device);
		if (!dev)
		{
			Console.Error("(DInputFFDevice) No DirectInput device for '%.*s'. Cannot use FF.", static_cast<int>(device.size()), device.data());
			return nullptr;
		}

		DIDEVCAPS caps = {};
		caps.dwSize = sizeof(caps);
		if (SUCCEEDED(dev->GetCapabilities(&caps)) && !(caps.dwFlags & DIDC_FORCEFEEDBACK))
		{
			Console.Error("(DInputFFDevice) '%.*s' does not report force feedback support.", static_cast<int>(device.size()), device.data());
			return nullptr;
		}

		std::unique_ptr<DInputFFDevice> ret(new DInputFFDevice(dev));
		if (!ret->CreateEffects(device))
			return nullptr;

		return ret;
	}

	bool DInputFFDevice::CreateEffect(Effect& fx, REFGUID type, const char* name)
	{
		// Common parameters, identical to the old backend. Steering is assumed to be on the X axis.
		fx.params.dwSize = sizeof(fx.params);
		fx.params.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
		fx.params.dwDuration = INFINITE;
		fx.params.dwSamplePeriod = 0;
		fx.params.dwGain = DI_FFNOMINALMAX;
		fx.params.dwTriggerButton = DIEB_NOTRIGGER;
		fx.params.dwTriggerRepeatInterval = 0;
		fx.params.cAxes = 1;
		fx.params.rgdwAxes = m_axes;
		fx.params.rglDirection = m_direction;
		fx.params.dwStartDelay = 0;

		if (type == GUID_ConstantForce)
		{
			fx.params.cbTypeSpecificParams = sizeof(fx.constant);
			fx.params.lpvTypeSpecificParams = &fx.constant;
		}
		else
		{
			fx.params.cbTypeSpecificParams = sizeof(fx.condition);
			fx.params.lpvTypeSpecificParams = &fx.condition;
		}

		const HRESULT hr = m_device->CreateEffect(type, &fx.params, fx.effect.put(), nullptr);
		if (FAILED(hr))
		{
			Console.Warning("(DInputFFDevice) CreateEffect for %s failed: %08X%s", name, static_cast<u32>(hr),
				(hr == DIERR_NOTEXCLUSIVEACQUIRED) ? " (device is not acquired exclusively)" : "");
			return false;
		}

		fx.created = true;
		return true;
	}

	bool DInputFFDevice::CreateEffects(const std::string_view device)
	{
		// All effects are infinite, they are only updated and restarted through SetParameters().
		const bool constant_ok = CreateEffect(m_constant, GUID_ConstantForce, "constant");
		const bool spring_ok = CreateEffect(m_spring, GUID_Spring, "spring");
		const bool damper_ok = CreateEffect(m_damper, GUID_Damper, "damper");
		const bool friction_ok = CreateEffect(m_friction, GUID_Friction, "friction");

		if (!constant_ok && !spring_ok && !damper_ok && !friction_ok)
		{
			Console.Error("(DInputFFDevice) No force feedback effect could be created on '%.*s'.", static_cast<int>(device.size()), device.data());
			return false;
		}

		Console.WriteLn("(DInputFFDevice) '%.*s': constant=%d spring=%d damper=%d friction=%d", static_cast<int>(device.size()), device.data(),
			constant_ok, spring_ok, damper_ok, friction_ok);

		// The old backend started the constant force right away.
		if (m_constant.created)
			m_constant.effect->Start(1, 0);

		return true;
	}

	void DInputFFDevice::StopEffect(Effect& fx)
	{
		if (fx.created && fx.effect)
			fx.effect->Stop();
	}

	void DInputFFDevice::UpdateEffect(Effect& fx, const char* name)
	{
		const HRESULT hr = fx.effect->SetParameters(&fx.params, DIEP_TYPESPECIFICPARAMS | DIEP_START);
		if (FAILED(hr))
			Console.Warning("(DInputFFDevice) SetParameters for %s failed: %08X", name, static_cast<u32>(hr));
	}

	void DInputFFDevice::SetConstantForce(int level)
	{
		if (!m_constant.created)
			return;

		const int clamped = std::clamp(level, -32768, 32767);
		m_constant.constant.lMagnitude = ScaleSigned(invert_forces ? -clamped : clamped, SHRT_MAX);

		if (debug_log && clamped != m_last_constant_level)
		{
			Console.WriteLn("(DInputFFDevice) constant: level=%d invert=%d -> magnitude=%ld", clamped, invert_forces, m_constant.constant.lMagnitude);
			m_last_constant_level = clamped;
		}

		UpdateEffect(m_constant, "constant");
	}

	void DInputFFDevice::SetConditionForce(Effect& fx, const char* name, const parsed_ff_data& ff)
	{
		fx.condition.dwNegativeSaturation = ScaleUnsigned(ff.u.condition.left_saturation, SHRT_MAX);
		fx.condition.dwPositiveSaturation = ScaleUnsigned(ff.u.condition.right_saturation, SHRT_MAX);
		fx.condition.lNegativeCoefficient = ScaleSigned(ff.u.condition.left_coeff, SHRT_MAX);
		fx.condition.lPositiveCoefficient = ScaleSigned(ff.u.condition.right_coeff, SHRT_MAX);
		fx.condition.lOffset = ScaleSigned(ff.u.condition.center, SHRT_MAX);
		fx.condition.lDeadBand = ScaleUnsigned(ff.u.condition.deadband, USHRT_MAX);

		if (debug_log)
		{
			Console.WriteLn("(DInputFFDevice) %s: coeff=%d/%d sat=%d/%d center=%d deadband=%d -> coeff=%ld/%ld sat=%lu/%lu offset=%ld deadband=%ld", name,
				ff.u.condition.left_coeff, ff.u.condition.right_coeff, ff.u.condition.left_saturation, ff.u.condition.right_saturation,
				ff.u.condition.center, ff.u.condition.deadband, fx.condition.lNegativeCoefficient, fx.condition.lPositiveCoefficient,
				fx.condition.dwNegativeSaturation, fx.condition.dwPositiveSaturation, fx.condition.lOffset, fx.condition.lDeadBand);
		}

		UpdateEffect(fx, name);
	}

	void DInputFFDevice::SetSpringForce(const parsed_ff_data& ff)
	{
		if (m_spring.created)
			SetConditionForce(m_spring, "spring", ff);
	}

	void DInputFFDevice::SetDamperForce(const parsed_ff_data& ff)
	{
		if (m_damper.created)
			SetConditionForce(m_damper, "damper", ff);
	}

	void DInputFFDevice::SetFrictionForce(const parsed_ff_data& ff)
	{
		if (m_friction.created)
			SetConditionForce(m_friction, "friction", ff);
	}

	void DInputFFDevice::SetAutoCenter(int value)
	{
		DIPROPDWORD prop = {};
		prop.diph.dwSize = sizeof(prop);
		prop.diph.dwHeaderSize = sizeof(prop.diph);
		prop.diph.dwObj = 0;
		prop.diph.dwHow = DIPH_DEVICE;
		prop.dwData = (value > 0) ? DIPROPAUTOCENTER_ON : DIPROPAUTOCENTER_OFF;

		if (debug_log)
			Console.WriteLn("(DInputFFDevice) autocenter: value=%d -> %s", value, (value > 0) ? "on" : "off");

		const HRESULT hr = m_device->SetProperty(DIPROP_AUTOCENTER, &prop.diph);
		if (FAILED(hr))
			Console.Warning("(DInputFFDevice) Setting autocenter failed: %08X", static_cast<u32>(hr));
	}

	void DInputFFDevice::SetGain(int percent)
	{
		DIPROPDWORD prop = {};
		prop.diph.dwSize = sizeof(prop);
		prop.diph.dwHeaderSize = sizeof(prop.diph);
		prop.diph.dwObj = 0;
		prop.diph.dwHow = DIPH_DEVICE;
		prop.dwData = static_cast<DWORD>(std::clamp(percent, 0, 100)) * DI_FFNOMINALMAX / 100;

		if (debug_log)
			Console.WriteLn("(DInputFFDevice) gain: %d%% -> %lu", percent, prop.dwData);

		const HRESULT hr = m_device->SetProperty(DIPROP_FFGAIN, &prop.diph);
		if (FAILED(hr))
			Console.Warning("(DInputFFDevice) Setting FF gain failed: %08X", static_cast<u32>(hr));
	}

	void DInputFFDevice::DisableForce(EffectID force)
	{
		if (debug_log)
			Console.WriteLn("(DInputFFDevice) disable force %d", static_cast<int>(force));

		switch (force)
		{
			case EFF_CONSTANT:
				StopEffect(m_constant);
				break;
			case EFF_SPRING:
				StopEffect(m_spring);
				break;
			case EFF_DAMPER:
				StopEffect(m_damper);
				break;
			case EFF_FRICTION:
				StopEffect(m_friction);
				break;
			case EFF_RUMBLE:
			default:
				break;
		}
	}
} // namespace usb_pad
