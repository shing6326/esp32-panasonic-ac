# ESP32 Zigbee Panasonic 冷氣（CZ-TACG1）

用 ESP32-C6 或 ESP32-H2 取代 Panasonic **CZ-TACG1** Wi-Fi 模組。模組接室內機 **CN-CNT** UART，自己以 Zigbee router 加入網絡，再用 Zigbee2MQTT / Home Assistant 控制冷氣。

只支援 CZ-TACG1 / CN-CNT。不支援 DNSK-P11 / CN-WLAN。

UART 協定來自 [esphome-panasonic-ac](https://github.com/DomiStyle/esphome-panasonic-ac) 的 CNT 實作。Zigbee thermostat、router、恢復原廠設定和 OTA 的做法參考 [esp32-midea-ac](https://github.com/pirsasha/esp32-midea-ac)。

## 功能

- 開關、自動、冷凍、製熱、抽濕、送風
- 目標溫度 16–30°C，步進 0.5°C。設定溫度時如果冷氣是關的，會一併開機，並沿用上次模式
- 風速 auto / 1 / 2 / 3 / 4 / 5
- 上下、左右、同時擺動，以及風向固定位置
- Powerful、Quiet
- 室內溫度、室外溫度
- nanoeX、Eco、ECONAVI、Mild Dry（遙控器有這些鍵才可開）
- 除霜狀態、冷氣回報的估計功率
- 按住 BOOT 5 秒離開 Zigbee 網絡

## 接線

斷開冷氣電源再開蓋。主板有市電，不要帶電操作。

CN-CNT 是 5 V UART。ESP32 是 3.3 V，TX 和 RX 都要經過雙向電平轉換器。不要把 ESP 的腳直接接到冷氣。

| 冷氣 CN-CNT | 電平轉換 | ESP32-C6 / H2 |
| --- | --- | --- |
| 5 V | HV | 5 V / VIN |
| GND | GND（高低壓兩側共地） | GND |
| RX（收 ESP 的資料） | HV1 ↔ LV1 | GPIO5（TX，可在 menuconfig 改） |
| TX（送給 ESP 的資料） | HV2 ↔ LV2 | GPIO8（RX，可在 menuconfig 改） |
| — | LV 的 VCC | 3.3 V |

RX / TX 以 ESP 的方向為準：ESP 的 TX 接到冷氣的 RX。韌體固定 **9600 8E1**（偶校驗），和 CZ-TACG1 原廠模組一樣。

GPIO9 是多數開發板的 BOOT 鍵，用來恢復原廠設定。

## 編譯

需要 ESP-IDF 5.3 或更新，以及支援的目標晶片。Flash 用 2 MB 的分割表，4 MB 模組也可以燒。

```bash
. $IDF_PATH/export.sh
cd esp32-panasonic-ac
idf.py set-target esp32c6    # 或 esp32h2
idf.py menuconfig            # Component config -> Panasonic CZ-TACG1
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

`menuconfig` 可以改 TX、RX、BOOT 腳，以及室內／室外溫度偏移。偏移會加在讀回來的溫度上，設定溫度時會先減掉室內偏移再送給冷氣。

## 加入 Zigbee2MQTT

1. 把 `zigbee2mqtt/panasonic-ac.js` 複製到 Zigbee2MQTT 的 `data/` 目錄。
2. 在 `configuration.yaml` 加入：

```yaml
external_converters:
  - panasonic-ac.js
```

3. 重啟 Zigbee2MQTT，打開 Permit join。
4. 模組上電後會以 router 自動尋網。識別字是 `Panasonic` / `ZB-PANASONIC-AC`。
5. 若之前入過別的網絡，按住 BOOT（GPIO9）5 秒，它會清掉 Zigbee 資料並重新尋網。

Home Assistant 會出現恆溫器。風速、擺風、Powerful / Quiet、nanoeX 等在同一個設備的其他實體。

## 不要亂開的功能

nanoeX、Eco、ECONAVI、Mild Dry 對應 CZ-TACG1 狀態字裡的獨立欄位。遙控器沒有該按鍵就不要開，冷氣可能不接受，行為也不確定。

改風速時，Powerful / Quiet 會被清掉。這和原廠 ESPHome 元件一樣，因為這兩個 preset 會蓋過風速。

## OTA

Zigbee OTA manufacturer code 是 `0x5041`。ESP32-C6 的 image type 是 `0x0001`，ESP32-H2 是 `0x0002`。韌體版本 `1.0.0` 的數值是 `0x01000000`。
