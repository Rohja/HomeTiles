#pragma once

#include "src/devices/hardware_io_profile.h"

namespace DeviceWaveshareS3TouchLCD7 {

// The first hardware bring-up deliberately exposes no configurable GPIOs.
// Every documented pin is currently assigned to the RGB panel, I2C bus
// (touch + CH422G expander), backlight, SD card, or the RS485/CAN headers,
// none of which are wired into a generic HomeTiles IO-option tile yet.
inline constexpr Device::HardwareIoProfile kHardwareIoProfile{};

}  // namespace DeviceWaveshareS3TouchLCD7
