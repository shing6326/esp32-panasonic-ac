# ESP32 Zigbee Panasonic AC (CZ-TACG1)

Replace the Panasonic **CZ-TACG1** Wi-Fi module with an ESP32-C6. The board connects to the indoor unit **CN-CNT** UART, joins the network as a Zigbee router, and is controlled through Zigbee2MQTT / Home Assistant.

Supported protocol: CZ-TACG1 / CN-CNT. DNSK-P11 / CN-WLAN is out of scope.

This repository is MIT licensed. A build downloads Espressif's Zigbee libraries, and those libraries keep their own licenses.

## Features

- Power, auto, cool, heat, dry, and fan
- Target temperature 16–30°C in 1°C steps. Setting a temperature while the unit is off turns it on and restores the previous mode
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
| RX (data from the ESP) | HV1 ↔ LV1 | GPIO23 (TX, changeable in menuconfig) |
| TX (data to the ESP) | HV2 ↔ LV2 | GPIO22 (RX, changeable in menuconfig) |
| — | LV VCC | 3.3 V |

GPIO8 is a ROM-log strap and GPIO9 is the BOOT strap and the factory-reset button. Keep the level shifter off both pins. A shifter that holds GPIO8 low while the chip resets silences the ROM log.

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

1. Copy the contents of `zigbee2mqtt/` into `/root/config/zigbee2mqtt/`. That places `external_converters/panasonic-ac.js` and `device_icons/ZB-PANASONIC-AC.png` beside `configuration.yaml`. Zigbee2MQTT loads every `.js` file in `external_converters`. Do not add an `external_converters` list to `configuration.yaml`.
2. On Zigbee2MQTT 2.11.0 or later, a new installation does not load that directory until external JavaScript is enabled:

```yaml
advanced:
  enable_external_js: true
```

3. Restart Zigbee2MQTT. After the built-in definition is released, delete `external_converters/panasonic-ac.js` so the two definitions do not both match the device.

## Device icon

Zigbee2MQTT selects the picture from the device `model`. Every `ZB-PANASONIC-AC` uses `https://www.zigbee2mqtt.io/images/devices/ZB-PANASONIC-AC.png` once that file is published. No IEEE address is required, and a newly paired board picks it up on its own.

A file in `device_icons` does not apply to the whole model. Zigbee2MQTT treats that directory as a per-device override, so each board still needs its own `icon: device_icons/ZB-PANASONIC-AC.png` entry. `device_options.icon` is also the wrong place: it would replace the picture of every device on the network.

## Reset and pairing

A new board joins by itself after power-up. A board that already belongs to a network stays there until it is reset.

1. In the Zigbee2MQTT frontend, turn on **Permit join** and leave it open while the board looks for a network.
2. Power the ESP32-C6 and wait until the firmware is running. Do not hold BOOT while the chip is resetting. GPIO9 is the download-mode strap, and holding it at reset stops the firmware from starting.
3. To pair a board that has joined before, press and hold BOOT (GPIO9, active low) for 5 seconds. The firmware logs `Factory reset: leaving the Zigbee network`, erases the Zigbee network data, and starts steering. A shorter press does nothing. The IEEE address does not change, so Zigbee2MQTT shows the same device again.
4. If permit join is closed, steering fails and retries after 1 second, 2 seconds, 5 seconds, then every 10 seconds. Open permit join and wait for the next attempt. Another power cycle is not required.
5. The device joins as a router. The identity is `ESP-PAC` / `ZB-PANASONIC-AC`. A device paired with the earlier `Panasonic` manufacturer name has to leave the network and join again so Zigbee2MQTT loads this definition.

Home Assistant shows a thermostat. Fan speed, swing, Powerful / Quiet, nanoeX, and the other controls appear as additional entities on the same device.

## Optional features

nanoeX, Eco, ECONAVI, and Mild Dry map to separate fields in the CZ-TACG1 status bytes. Leave a feature off when the remote has no button for it. The indoor unit may reject the command, and the result is undefined.

Changing the fan speed clears Powerful / Quiet, because those presets share the fan field.

## CI

`.github/workflows/ci.yml` builds the ESP32-C6 firmware on every pull request and every push to `main`, and compiles the host codec test. The binaries stay in the Actions artifact for 14 days.

A `v*` tag attaches a flashable set to a GitHub Release: the app, bootloader, partition table, `ota_data_initial`, `flash_args_esp32c6`, and `zb-panasonic-ac-<version>.ota`. A green run means the firmware linked and the app fits in the 640 KB OTA slot. It does not mean the image was tested on an air conditioner.

Download the release files into one directory and flash them together:

```bash
esptool.py --chip esp32c6 -p /dev/ttyACM0 write_flash @flash_args_esp32c6
```

The first image has to be written over USB. Zigbee OTA can only replace a board that is already running this firmware.

## OTA

The Zigbee OTA manufacturer code is `0x5041` (20545). The ESP32-C6 image type is `0x0001`. Firmware `1.0.6` is stored as `0x01000600`. Zigbee2MQTT offers an update only when the file version is greater than the version on the device. A board running `1.0.0` through `1.0.5` can take `1.0.6`.

`PROJECT_VER` in `CMakeLists.txt` and `firmware_version` in `main/main.cpp` use the same three bytes. `1.0.6` is `0x01000600`. The release workflow wraps `build/zb_panasonic_ac.bin` into `zb-panasonic-ac-<version>.ota` with manufacturer `0x5041`, image type `0x0001`, and tag `0x0000`. Point Zigbee2MQTT at the index:

```yaml
ota:
  zigbee_ota_override_index_location: https://raw.githubusercontent.com/shing6326/esp32-panasonic-ac/main/ota/index.json
```

```json
[
  {
    "url": "https://github.com/shing6326/esp32-panasonic-ac/releases/download/v1.0.6/zb-panasonic-ac-1.0.6.ota",
    "manufacturerCode": 20545,
    "imageType": 1,
    "fileVersion": 16778752
  }
]
```

`16778752` is `0x01000600`. That file is attached to the `v1.0.6` release. `v1.0.0` has no OTA file.
