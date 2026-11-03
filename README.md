# ESP32 Zigbee Panasonic AC (CZ-TACG1)

Replace the Panasonic **CZ-TACG1** Wi-Fi module with an ESP32-C6. The board connects to the indoor unit **CN-CNT** UART, joins the network as a Zigbee router, and is controlled through Zigbee2MQTT / Home Assistant.

Supported protocol: CZ-TACG1 / CN-CNT. DNSK-P11 / CN-WLAN is out of scope.

The UART protocol comes from the CNT implementation in [esphome-panasonic-ac](https://github.com/DomiStyle/esphome-panasonic-ac). The Zigbee thermostat, router role, factory reset, and OTA client follow [esp32-midea-ac](https://github.com/pirsasha/esp32-midea-ac).

## Features

- Power, auto, cool, heat, dry, and fan
- Target temperature 16–30°C in 0.5°C steps. Setting a temperature while the unit is off turns it on and restores the previous mode
- Fan speed auto / 1 / 2 / 3 / 4 / 5
- Vertical, horizontal, and combined swing, plus fixed vane positions
- Powerful and Quiet
- Indoor and outdoor temperature
- nanoeX, Eco, ECONAVI, and Mild Dry (enable only when the remote has that button)
- Defrost status and the power estimate reported by the AC
- Hold BOOT for 5 seconds to leave the Zigbee network

## Wiring

Disconnect mains power before opening the indoor unit. The control board is live at mains voltage.

CN-CNT is a 5 V UART. The ESP32 is 3.3 V, so both TX and RX go through a bidirectional level shifter. Keep the ESP pins off the AC connector.

| AC CN-CNT | Level shifter | ESP32-C6 |
| --- | --- | --- |
| 5 V | HV | 5 V / VIN |
| GND | GND (common ground on both sides) | GND |
| RX (data from the ESP) | HV1 ↔ LV1 | GPIO5 (TX, changeable in menuconfig) |
| TX (data to the ESP) | HV2 ↔ LV2 | GPIO8 (RX, changeable in menuconfig) |
| — | LV VCC | 3.3 V |

On ESP32-C6, GPIO8 is a ROM-log strap. Idle-high UART leaves the ROM log enabled. A level shifter that holds GPIO8 low while the chip resets silences that log. GPIO9 is the BOOT strap and the factory-reset button; the shifter must not drive it.

Directions are from the ESP: ESP TX goes to the AC RX. The firmware uses **9600 8E1** (even parity), the same setting as the original CZ-TACG1 module.

GPIO9 is the BOOT button on most development boards and is used for factory reset.

## Build

Use ESP-IDF 5.5.1 through 5.5.x. CI builds with 5.5.6. The prebuilt Zigbee library is 1.6.8. ESP-IDF 6.0 needs Zigbee SDK 2.x, which is a different API. The partition table is sized for 2 MB flash and can also be flashed to a 4 MB module.

```bash
. $IDF_PATH/export.sh
cd esp32-panasonic-ac
idf.py set-target esp32c6
idf.py menuconfig            # Component config -> Panasonic CZ-TACG1
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

`menuconfig` sets the TX, RX, and BOOT pins, plus the indoor and outdoor temperature offsets. An offset is added to a temperature read from the AC. The indoor offset is subtracted from the target temperature before it is sent to the AC.

## Zigbee2MQTT

1. Copy `zigbee2mqtt/panasonic-ac.js` into the Zigbee2MQTT `data/` directory.
2. Add this to `configuration.yaml`:

```yaml
external_converters:
  - panasonic-ac.js
```

3. Restart Zigbee2MQTT and enable Permit join.
4. After power-up the module joins as a router. The identity is `ESP-PAC` / `ZB-PANASONIC-AC`. A device paired with the earlier `Panasonic` manufacturer name has to leave the network and join again.
5. If it was joined to another network, hold BOOT (GPIO9) for 5 seconds. That clears the Zigbee data and starts steering again. If permit-join is closed, steering retries after 1 s, 2 s, 5 s, then every 10 s.

Home Assistant shows a thermostat. Fan speed, swing, Powerful / Quiet, nanoeX, and the other controls appear as additional entities on the same device.

## Optional features

nanoeX, Eco, ECONAVI, and Mild Dry map to separate fields in the CZ-TACG1 status bytes. Leave a feature off when the remote has no button for it. The indoor unit may reject the command, and the result is undefined.

Changing the fan speed clears Powerful / Quiet. The ESPHome component does the same, because those presets override the fan speed.

## CI

`.github/workflows/ci.yml` builds the ESP32-C6 firmware on every pull request and every push to `main`, and compiles the host codec test. The binaries stay in the Actions artifact for 14 days.

A `v*` tag attaches the same images to a GitHub Release. A green run means the firmware linked and the app fits in the 640 KB OTA slot. It does not mean the image was tested on an air conditioner.

## OTA

The Zigbee OTA manufacturer code is `0x5041`. The ESP32-C6 image type is `0x0001`. Firmware `1.0.0` is stored as `0x01000000`.
