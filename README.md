# VoodooI2CHID (fork)

`VoodooI2CHID.kext` is the I2C-HID satellite of [VoodooI2C](https://github.com/VoodooI2C/VoodooI2C). It drives HID-over-I2C devices such as precision touchpads, touchscreens, styluses and sensors.

This fork is used as the `VoodooI2C Satellites/VoodooI2CHID` submodule of [amane1234/VoodooI2C_APIC](https://github.com/amane1234/VoodooI2C_APIC), which lets VoodooI2C use APIC interrupt pins above `0x2f`. See that repository's README for the full background, tested hardware and setup.

## Changes compared to upstream

### Polling idle back-off for other event drivers

In polling mode, `simulateInterrupt()` reads the device on a timer:

| State | Interval |
|---|---|
| Touch activity | 2 ms (`INTERRUPT_SIMULATOR_TIMEOUT_BUSY`) |
| No activity seen yet | 5 ms (`INTERRUPT_SIMULATOR_TIMEOUT`) |
| No activity for 1.5 s | 50 ms (`INTERRUPT_SIMULATOR_TIMEOUT_IDLE`) |

The decision uses `last_multi_touch_event`, and upstream only `VoodooI2CMultitouchHIDEventDriver` updates it. With other event drivers, such as AlpsHID, it stays `0`, so polling reads the device every 5 ms forever. On a Dell Comet Lake-U with an Alps touchpad this was about 2,000 I2C controller interrupts per second while idle.

`getInputReport()` now treats an input report whose content changed as touch activity and updates `last_multi_touch_event`. Empty reports are skipped before that point, so polling backs off to 50 ms after 1.5 s without new reports. The first touch after a long idle period can be up to 50 ms late; after that the interval returns to 2 ms.

### Fixes

- `VoodooI2CHIDDevice::open()` recorded the client before `super::open()` and without checking for duplicates, so a failed or repeated open left an entry that `close()` never removed. `stop()` waited for the list to empty with no timeout, which hung termination, sleep and shutdown. Only successful opens are tracked now, once each, and the wait in `stop()` is bounded to 5 s.
- The Precision Touchpad, sensor and accelerometer `setPowerState()` returned `kIOReturnInvalid` for a foreign device. The callback returns an acknowledgement delay, so they return `kIOPMAckImplied` now.
- Sensor `withElement()` released a sensor whose `start()` failed while it was still attached, leaving it in the registry. It is detached first now, and the allocations are checked.
- `getInputReport()` checks the I2C read result, skips reports of 2 bytes or less, and checks the buffer allocation.

## Building

This kext is not built on its own. It includes headers from the VoodooI2C tree (`../../../VoodooI2C/...`, `../../../Dependencies/...`), so build it as part of VoodooI2C_APIC:

```bash
git clone --recursive -b main https://github.com/amane1234/VoodooI2C_APIC.git
```

Then follow the "Building" steps in the VoodooI2C_APIC README. `VoodooI2CHID.kext` is written to `build/Release/` next to `VoodooI2C.kext`.

## License

GPL, the same as upstream VoodooI2C. See the VoodooI2C `LICENSE.txt`.
