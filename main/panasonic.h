// CZ-TACG1 / CN-CNT UART driver. DNSK-P11 / CN-WLAN is not implemented.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

namespace panasonic {

enum class Mode : uint8_t {
    Off = 0,
    Auto = 1,
    Cool = 2,
    Heat = 3,
    Dry = 4,
    Fan = 5,
};

enum class Fan : uint8_t {
    Auto = 0,
    S1 = 1,
    S2 = 2,
    S3 = 3,
    S4 = 4,
    S5 = 5,
};

enum class Swing : uint8_t {
    Off = 0,
    Horizontal = 1,
    Vertical = 2,
    Both = 3,
};

enum class Preset : uint8_t {
    None = 0,
    Powerful = 1,
    Quiet = 2,
};

enum class VSwing : uint8_t {
    Unknown = 0,
    Up = 1,
    UpCenter = 2,
    Center = 3,
    DownCenter = 4,
    Down = 5,
    Swing = 6,
    Auto = 7,
};

enum class HSwing : uint8_t {
    Unknown = 0,
    Left = 1,
    LeftCenter = 2,
    Center = 3,
    RightCenter = 4,
    Right = 5,
    Auto = 6,
};

struct State {
    bool valid;
    bool power_on;
    Mode mode;
    Fan fan;
    Swing swing;
    Preset preset;
    VSwing vswing;
    HSwing hswing;
    float set_temp;
    bool set_valid;
    float indoor_temp;
    float outdoor_temp;
    bool indoor_valid;
    bool outdoor_valid;
    bool nanoex;
    bool eco;
    bool econavi;
    bool mild_dry;
    bool defrost;
    bool defrost_valid;
    float power_w;
    bool power_valid;
};

typedef void (*state_cb_t)(const State &state);

// tx_gpio is the ESP pin wired to the AC RX input.
// rx_gpio is the ESP pin wired to the AC TX output.
// Offsets are added to reported temperatures and the indoor offset is
// subtracted from the target written back to the AC.
esp_err_t init(int tx_gpio, int rx_gpio, int8_t indoor_offset, int8_t outdoor_offset, state_cb_t cb);

esp_err_t set_power(bool on);
esp_err_t set_mode(Mode mode);
esp_err_t set_target_temp(float celsius);
esp_err_t set_fan(Fan fan);
esp_err_t set_swing(Swing swing);
esp_err_t set_preset(Preset preset);
esp_err_t set_vswing(VSwing swing);
esp_err_t set_hswing(HSwing swing);
esp_err_t set_nanoex(bool on);
esp_err_t set_eco(bool on);
esp_err_t set_econavi(bool on);
esp_err_t set_mild_dry(bool on);
esp_err_t request_status();

}  // namespace panasonic
