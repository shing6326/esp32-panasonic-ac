#include "panasonic.h"

#include <string.h>
#include <math.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace panasonic {

static const char *TAG = "cz_tacg1";

static constexpr uart_port_t UART = UART_NUM_1;
static constexpr int UART_BAUD = 9600;
static constexpr int RX_RING = 1024;
static constexpr size_t RX_MAX = 128;
static constexpr int64_t RX_GAP_MS = 20;
static constexpr int64_t CMD_INTERVAL_MS = 250;
static constexpr int64_t POLL_INTERVAL_MS = 5000;
static constexpr int64_t SILENCE_WARN_MS = 30000;

static constexpr uint8_t CTRL_HEADER = 0xF0;
static constexpr uint8_t POLL_HEADER = 0x70;

enum CmdKind : uint8_t {
    CMD_POWER = 1,
    CMD_MODE,
    CMD_TEMP,
    CMD_FAN,
    CMD_SWING,
    CMD_PRESET,
    CMD_VSWING,
    CMD_HSWING,
    CMD_NANOEX,
    CMD_ECO,
    CMD_ECONAVI,
    CMD_MILD,
    CMD_POLL,
};

struct Cmd {
    uint8_t kind;
    uint8_t u8;
    float f;
};

static constexpr uint16_t F_POWER = 1u << 0;
static constexpr uint16_t F_MODE = 1u << 1;
static constexpr uint16_t F_TEMP = 1u << 2;
static constexpr uint16_t F_FAN = 1u << 3;
static constexpr uint16_t F_SWING = 1u << 4;
static constexpr uint16_t F_PRESET = 1u << 5;
static constexpr uint16_t F_VSWING = 1u << 6;
static constexpr uint16_t F_HSWING = 1u << 7;
static constexpr uint16_t F_NANOEX = 1u << 8;
static constexpr uint16_t F_ECO = 1u << 9;
static constexpr uint16_t F_ECONAVI = 1u << 10;
static constexpr uint16_t F_MILD = 1u << 11;

struct Desired {
    uint16_t mask;
    bool power;
    uint8_t mode;
    float temp;
    uint8_t fan;
    uint8_t swing;
    uint8_t preset;
    uint8_t vswing;
    uint8_t hswing;
    bool nanoex;
    bool eco;
    bool econavi;
    bool mild;
};

static QueueHandle_t g_queue = nullptr;
static state_cb_t g_cb = nullptr;
static int8_t g_indoor_offset = 0;
static int8_t g_outdoor_offset = 0;
static uint8_t g_shadow[10] = {};
static bool g_shadow_valid = false;
static Desired g_desired = {};
static bool g_poll_asap = false;

static int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

static esp_err_t enqueue(uint8_t kind, uint8_t u8, float f)
{
    if (g_queue == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    Cmd cmd = {};
    cmd.kind = kind;
    cmd.u8 = u8;
    cmd.f = f;
    if (xQueueSend(g_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void capture(const Cmd &cmd)
{
    switch (cmd.kind) {
    case CMD_POWER:
        g_desired.mask |= F_POWER;
        g_desired.power = cmd.u8 != 0;
        break;
    case CMD_MODE:
        g_desired.mask |= F_MODE;
        g_desired.mode = cmd.u8;
        break;
    case CMD_TEMP:
        g_desired.mask |= F_TEMP;
        g_desired.temp = cmd.f;
        break;
    case CMD_FAN:
        g_desired.mask |= F_FAN;
        g_desired.fan = cmd.u8;
        break;
    case CMD_SWING:
        g_desired.mask |= F_SWING;
        g_desired.swing = cmd.u8;
        break;
    case CMD_PRESET:
        g_desired.mask |= F_PRESET;
        g_desired.preset = cmd.u8;
        break;
    case CMD_VSWING:
        g_desired.mask |= F_VSWING;
        g_desired.vswing = cmd.u8;
        break;
    case CMD_HSWING:
        g_desired.mask |= F_HSWING;
        g_desired.hswing = cmd.u8;
        break;
    case CMD_NANOEX:
        g_desired.mask |= F_NANOEX;
        g_desired.nanoex = cmd.u8 != 0;
        break;
    case CMD_ECO:
        g_desired.mask |= F_ECO;
        g_desired.eco = cmd.u8 != 0;
        break;
    case CMD_ECONAVI:
        g_desired.mask |= F_ECONAVI;
        g_desired.econavi = cmd.u8 != 0;
        break;
    case CMD_MILD:
        g_desired.mask |= F_MILD;
        g_desired.mild = cmd.u8 != 0;
        break;
    case CMD_POLL:
        g_poll_asap = true;
        break;
    default:
        break;
    }
}

static void send_frame(uint8_t header, const uint8_t *payload, size_t len, bool log_info)
{
    uint8_t frame[16];
    if (len + 3 > sizeof(frame)) {
        return;
    }

    frame[0] = header;
    frame[1] = (uint8_t)len;
    memcpy(frame + 2, payload, len);

    uint8_t sum = 0;
    for (size_t i = 0; i < len + 2; i++) {
        sum += frame[i];
    }
    frame[len + 2] = (uint8_t)(0 - sum);

    uart_write_bytes(UART, frame, len + 3);
    uart_wait_tx_done(UART, pdMS_TO_TICKS(100));

    if (log_info) {
        ESP_LOGI(TAG, "TX control");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, len + 3, ESP_LOG_INFO);
    } else {
        ESP_LOGD(TAG, "TX poll");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, len + 3, ESP_LOG_DEBUG);
    }
}

static bool verify_packet(const uint8_t *data, size_t len)
{
    if (len < 12) {
        return false;
    }
    if (data[0] != CTRL_HEADER && data[0] != POLL_HEADER) {
        return false;
    }
    if (data[1] != len - 3) {
        return false;
    }
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum == 0;
}

static Mode mode_from_nibble(uint8_t nib)
{
    switch (nib) {
    case 0x00:
        return Mode::Auto;
    case 0x03:
        return Mode::Cool;
    case 0x04:
        return Mode::Heat;
    case 0x02:
        return Mode::Dry;
    case 0x06:
        return Mode::Fan;
    default:
        ESP_LOGW(TAG, "unknown mode nibble 0x%X", nib);
        return Mode::Auto;
    }
}

static Fan fan_from_byte(uint8_t speed)
{
    switch (speed) {
    case 0xA0:
        return Fan::Auto;
    case 0x30:
        return Fan::S1;
    case 0x40:
        return Fan::S2;
    case 0x50:
        return Fan::S3;
    case 0x60:
        return Fan::S4;
    case 0x70:
        return Fan::S5;
    default:
        ESP_LOGW(TAG, "unknown fan 0x%02X", speed);
        return Fan::Auto;
    }
}

static VSwing vswing_from_nibble(uint8_t nib)
{
    switch (nib) {
    case 0x0E:
        return VSwing::Swing;
    case 0x0F:
        return VSwing::Auto;
    case 0x01:
        return VSwing::Up;
    case 0x02:
        return VSwing::UpCenter;
    case 0x03:
        return VSwing::Center;
    case 0x04:
        return VSwing::DownCenter;
    case 0x05:
        return VSwing::Down;
    case 0x00:
        return VSwing::Unknown;
    default:
        ESP_LOGD(TAG, "unknown vertical swing 0x%X", nib);
        return VSwing::Unknown;
    }
}

static HSwing hswing_from_nibble(uint8_t nib)
{
    switch (nib) {
    case 0x0D:
        return HSwing::Auto;
    case 0x09:
        return HSwing::Left;
    case 0x0A:
        return HSwing::LeftCenter;
    case 0x06:
        return HSwing::Center;
    case 0x0B:
        return HSwing::RightCenter;
    case 0x0C:
        return HSwing::Right;
    case 0x00:
        return HSwing::Unknown;
    default:
        ESP_LOGD(TAG, "unknown horizontal swing 0x%X", nib);
        return HSwing::Unknown;
    }
}

static Preset preset_from_byte(uint8_t preset)
{
    switch (preset & 0x0F) {
    case 0x02:
        return Preset::Powerful;
    case 0x04:
        return Preset::Quiet;
    default:
        return Preset::None;
    }
}

static bool read_sensor(const uint8_t *data, size_t len, size_t index, int8_t offset, float *out)
{
    if (index >= len || data[index] == 0x80) {
        return false;
    }
    int16_t corrected = (int16_t)(int8_t)data[index] + offset;
    if (corrected > 100 || corrected < -40) {
        return false;
    }
    *out = (float)corrected;
    return true;
}

static State parse_state(const uint8_t *frame, size_t len)
{
    State st = {};
    st.valid = true;
    st.power_on = (g_shadow[0] & 0x0F) != 0;
    st.mode = st.power_on ? mode_from_nibble((g_shadow[0] >> 4) & 0x0F) : Mode::Off;
    st.fan = fan_from_byte(g_shadow[3]);

    uint8_t v_nib = (g_shadow[4] >> 4) & 0x0F;
    uint8_t h_nib = g_shadow[4] & 0x0F;
    st.vswing = vswing_from_nibble(v_nib);
    st.hswing = hswing_from_nibble(h_nib);
    bool v_auto = v_nib == 0x0F;
    bool h_auto = h_nib == 0x0D;
    if (v_auto && h_auto) {
        st.swing = Swing::Both;
    } else if (v_auto) {
        st.swing = Swing::Vertical;
    } else if (h_auto) {
        st.swing = Swing::Horizontal;
    } else {
        st.swing = Swing::Off;
    }

    st.preset = preset_from_byte(g_shadow[5]);
    st.nanoex = (((g_shadow[5] >> 4) & 0x04) == 0x04);
    st.econavi = (g_shadow[5] & 0x10) != 0;
    st.eco = g_shadow[8] == 0x40;
    st.mild_dry = g_shadow[2] == 0x7F;

    float set_temp = g_shadow[1] * 0.5f + (float)g_indoor_offset;
    if (set_temp <= 100.0f && set_temp >= 0.0f) {
        st.set_temp = set_temp;
    }

    st.indoor_valid = read_sensor(frame, len, 18, g_indoor_offset, &st.indoor_temp) ||
                      read_sensor(frame, len, 21, g_indoor_offset, &st.indoor_temp);
    st.outdoor_valid = read_sensor(frame, len, 19, g_outdoor_offset, &st.outdoor_temp) ||
                       read_sensor(frame, len, 22, g_outdoor_offset, &st.outdoor_temp);

    if (len >= 15) {
        st.defrost = frame[14] == 0x02;
        st.defrost_valid = true;
    }
    if (len > 30) {
        int32_t raw = (int32_t)frame[28] + ((int32_t)frame[29] << 8);
        st.power_w = (float)(raw - (int32_t)(int8_t)frame[30]);
        st.power_valid = true;
    }
    return st;
}

static bool handle_rx(const uint8_t *data, size_t len)
{
    ESP_LOGD(TAG, "RX %u bytes", (unsigned)len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_DEBUG);

    if (!verify_packet(data, len)) {
        ESP_LOGD(TAG, "drop invalid frame");
        return false;
    }
    if (data[0] != POLL_HEADER || data[1] < 10) {
        ESP_LOGD(TAG, "ignore frame header 0x%02X", data[0]);
        return false;
    }

    memcpy(g_shadow, data + 2, 10);
    g_shadow_valid = true;

    State st = parse_state(data, len);
    static State prev = {};
    bool changed = !prev.valid || prev.power_on != st.power_on || prev.mode != st.mode || prev.fan != st.fan ||
                   prev.swing != st.swing || prev.preset != st.preset || prev.set_temp != st.set_temp ||
                   prev.nanoex != st.nanoex || prev.eco != st.eco || prev.econavi != st.econavi ||
                   prev.mild_dry != st.mild_dry || prev.defrost != st.defrost;
    prev = st;
    if (changed) {
        ESP_LOGI(TAG, "power=%d mode=%u fan=%u swing=%u preset=%u set=%.1f in=%.1f out=%.1f",
                 st.power_on, (unsigned)st.mode, (unsigned)st.fan, (unsigned)st.swing, (unsigned)st.preset,
                 st.set_temp, st.indoor_valid ? st.indoor_temp : -99.0f, st.outdoor_valid ? st.outdoor_temp : -99.0f);
    }

    if (g_cb != nullptr) {
        g_cb(st);
    }
    return true;
}

static uint8_t mode_byte(uint8_t mode)
{
    switch ((Mode)mode) {
    case Mode::Cool:
        return 0x34;
    case Mode::Heat:
        return 0x44;
    case Mode::Dry:
        return 0x24;
    case Mode::Fan:
        return 0x64;
    case Mode::Auto:
        return 0x04;
    case Mode::Off:
    default:
        return 0x00;
    }
}

static uint8_t fan_byte(uint8_t fan)
{
    switch ((Fan)fan) {
    case Fan::S1:
        return 0x30;
    case Fan::S2:
        return 0x40;
    case Fan::S3:
        return 0x50;
    case Fan::S4:
        return 0x60;
    case Fan::S5:
        return 0x70;
    case Fan::Auto:
    default:
        return 0xA0;
    }
}

static uint8_t swing_byte(uint8_t swing)
{
    switch ((Swing)swing) {
    case Swing::Both:
        return 0xFD;
    case Swing::Vertical:
        return 0xF6;
    case Swing::Horizontal:
        return 0x3D;
    case Swing::Off:
    default:
        return 0x36;
    }
}

static uint8_t vswing_high(uint8_t swing)
{
    switch ((VSwing)swing) {
    case VSwing::Down:
        return 0x50;
    case VSwing::DownCenter:
        return 0x40;
    case VSwing::UpCenter:
        return 0x20;
    case VSwing::Up:
        return 0x10;
    case VSwing::Swing:
        return 0xE0;
    case VSwing::Auto:
        return 0xF0;
    case VSwing::Center:
    case VSwing::Unknown:
    default:
        return 0x30;
    }
}

static uint8_t hswing_low(uint8_t swing)
{
    switch ((HSwing)swing) {
    case HSwing::Left:
        return 0x09;
    case HSwing::LeftCenter:
        return 0x0A;
    case HSwing::RightCenter:
        return 0x0B;
    case HSwing::Right:
        return 0x0C;
    case HSwing::Auto:
        return 0x0D;
    case HSwing::Center:
    case HSwing::Unknown:
    default:
        return 0x06;
    }
}

static uint8_t preset_low(uint8_t preset)
{
    switch ((Preset)preset) {
    case Preset::Powerful:
        return 0x02;
    case Preset::Quiet:
        return 0x04;
    case Preset::None:
    default:
        return 0x00;
    }
}

static bool build_control(uint8_t cmd[10])
{
    if (!g_shadow_valid) {
        return false;
    }
    memcpy(cmd, g_shadow, 10);

    if (g_desired.mask & F_MODE) {
        if ((Mode)g_desired.mode == Mode::Off) {
            cmd[0] &= 0xF0;
        } else {
            cmd[0] = mode_byte(g_desired.mode);
        }
    }
    if (g_desired.mask & F_TEMP) {
        float c = g_desired.temp - (float)g_indoor_offset;
        if (c < 16.0f) {
            c = 16.0f;
        } else if (c > 30.0f) {
            c = 30.0f;
        }
        int raw = (int)lroundf(c * 2.0f);
        if (raw < 32) {
            raw = 32;
        } else if (raw > 60) {
            raw = 60;
        }
        cmd[1] = (uint8_t)raw;
    }
    if (g_desired.mask & F_FAN) {
        // Powerful / Quiet replaces the fan speed, same as the ESPHome component.
        if ((cmd[5] & 0x0F) != 0 && (g_desired.mask & F_PRESET) == 0) {
            cmd[5] &= 0xF0;
        }
        cmd[3] = fan_byte(g_desired.fan);
    }
    if (g_desired.mask & F_PRESET) {
        cmd[5] = (uint8_t)((cmd[5] & 0xF0) | preset_low(g_desired.preset));
    }
    if (g_desired.mask & F_SWING) {
        cmd[4] = swing_byte(g_desired.swing);
    }
    if ((g_desired.mask & F_VSWING) != 0 && (VSwing)g_desired.vswing != VSwing::Unknown) {
        cmd[4] = (uint8_t)((cmd[4] & 0x0F) + vswing_high(g_desired.vswing));
    }
    if ((g_desired.mask & F_HSWING) != 0 && (HSwing)g_desired.hswing != HSwing::Unknown) {
        cmd[4] = (uint8_t)((cmd[4] & 0xF0) + hswing_low(g_desired.hswing));
    }
    // Byte 5 is a bitfield: low nibble preset, 0x10 ECONAVI, 0x40 nanoeX.
    if (g_desired.mask & F_NANOEX) {
        uint8_t keep = cmd[5] & 0x1F;
        cmd[5] = g_desired.nanoex ? (uint8_t)(keep | 0x40) : keep;
    }
    if (g_desired.mask & F_ECONAVI) {
        cmd[5] = g_desired.econavi ? (uint8_t)(cmd[5] | 0x10) : (uint8_t)(cmd[5] & ~0x10);
    }
    if (g_desired.mask & F_ECO) {
        cmd[8] = g_desired.eco ? 0x40 : 0x00;
    }
    if (g_desired.mask & F_MILD) {
        cmd[2] = g_desired.mild ? 0x7F : 0x80;
    }
    if (g_desired.mask & F_POWER) {
        if (g_desired.power) {
            if ((cmd[0] & 0x0F) == 0) {
                cmd[0] = (uint8_t)((cmd[0] & 0xF0) | 0x04);
            }
        } else {
            cmd[0] &= 0xF0;
        }
    }
    return true;
}

static void protocol_task(void *)
{
    uint8_t rx[RX_MAX];
    size_t rx_len = 0;
    int64_t last_rx = 0;
    int64_t last_tx = now_ms() - POLL_INTERVAL_MS + 500;
    int64_t last_good = 0;
    int64_t last_warn = 0;

    while (true) {
        uint8_t tmp[64];
        // Short wait so a frame still arriving at 9600 baud is not cut in half.
        // A frame is complete only after RX_GAP_MS of silence, same as the ESPHome driver.
        int n = uart_read_bytes(UART, tmp, sizeof(tmp), pdMS_TO_TICKS(10));
        int64_t now = now_ms();
        if (n > 0) {
            if (rx_len + (size_t)n > RX_MAX) {
                ESP_LOGW(TAG, "RX overflow, dropping %u bytes", (unsigned)rx_len);
                rx_len = 0;
            } else {
                memcpy(rx + rx_len, tmp, (size_t)n);
                rx_len += (size_t)n;
            }
            last_rx = now;
        } else if (rx_len > 0 && now - last_rx >= RX_GAP_MS) {
            if (handle_rx(rx, rx_len)) {
                last_good = now;
            }
            rx_len = 0;
        }

        Cmd cmd;
        while (xQueueReceive(g_queue, &cmd, 0) == pdTRUE) {
            capture(cmd);
        }

        now = now_ms();
        if (last_good == 0 || now - last_good > SILENCE_WARN_MS) {
            if (now - last_warn > SILENCE_WARN_MS) {
                ESP_LOGW(TAG, "no valid CZ-TACG1 response yet; check CN-CNT wiring, 5 V level shifter and even parity");
                last_warn = now;
            }
        }

        bool sent = false;
        if (g_desired.mask != 0 && g_shadow_valid && now - last_tx >= CMD_INTERVAL_MS) {
            uint8_t payload[10];
            if (build_control(payload)) {
                send_frame(CTRL_HEADER, payload, sizeof(payload), true);
                g_desired.mask = 0;
                last_tx = now;
                sent = true;
            }
        }
        if (!sent && now - last_tx >= CMD_INTERVAL_MS &&
            (g_poll_asap || now - last_tx >= POLL_INTERVAL_MS)) {
            static const uint8_t poll[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            send_frame(POLL_HEADER, poll, sizeof(poll), false);
            g_poll_asap = false;
            last_tx = now;
        }
    }
}

esp_err_t init(int tx_gpio, int rx_gpio, int8_t indoor_offset, int8_t outdoor_offset, state_cb_t cb)
{
    if (g_queue != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tx_gpio < 0 || rx_gpio < 0 || tx_gpio == rx_gpio) {
        return ESP_ERR_INVALID_ARG;
    }

    uart_config_t cfg = {};
    cfg.baud_rate = UART_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_EVEN;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(UART, RX_RING, 0, 0, nullptr, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(UART, &cfg);
    if (err != ESP_OK) {
        uart_driver_delete(UART);
        return err;
    }
    err = uart_set_pin(UART, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        uart_driver_delete(UART);
        return err;
    }

    g_cb = cb;
    g_indoor_offset = indoor_offset;
    g_outdoor_offset = outdoor_offset;
    g_queue = xQueueCreate(24, sizeof(Cmd));
    if (g_queue == nullptr) {
        uart_driver_delete(UART);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(protocol_task, "panasonic", 6144, nullptr, 8, nullptr) != pdPASS) {
        vQueueDelete(g_queue);
        g_queue = nullptr;
        uart_driver_delete(UART);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "CZ-TACG1 on UART%d TX=%d RX=%d, 9600 8E1", (int)UART, tx_gpio, rx_gpio);
    return ESP_OK;
}

esp_err_t set_power(bool on)
{
    return enqueue(CMD_POWER, on ? 1 : 0, 0);
}

esp_err_t set_mode(Mode mode)
{
    return enqueue(CMD_MODE, (uint8_t)mode, 0);
}

esp_err_t set_target_temp(float celsius)
{
    return enqueue(CMD_TEMP, 0, celsius);
}

esp_err_t set_fan(Fan fan)
{
    return enqueue(CMD_FAN, (uint8_t)fan, 0);
}

esp_err_t set_swing(Swing swing)
{
    return enqueue(CMD_SWING, (uint8_t)swing, 0);
}

esp_err_t set_preset(Preset preset)
{
    return enqueue(CMD_PRESET, (uint8_t)preset, 0);
}

esp_err_t set_vswing(VSwing swing)
{
    return enqueue(CMD_VSWING, (uint8_t)swing, 0);
}

esp_err_t set_hswing(HSwing swing)
{
    return enqueue(CMD_HSWING, (uint8_t)swing, 0);
}

esp_err_t set_nanoex(bool on)
{
    return enqueue(CMD_NANOEX, on ? 1 : 0, 0);
}

esp_err_t set_eco(bool on)
{
    return enqueue(CMD_ECO, on ? 1 : 0, 0);
}

esp_err_t set_econavi(bool on)
{
    return enqueue(CMD_ECONAVI, on ? 1 : 0, 0);
}

esp_err_t set_mild_dry(bool on)
{
    return enqueue(CMD_MILD, on ? 1 : 0, 0);
}

esp_err_t request_status()
{
    return enqueue(CMD_POLL, 0, 0);
}

}  // namespace panasonic
