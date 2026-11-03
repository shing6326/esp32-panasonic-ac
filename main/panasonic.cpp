#include "panasonic.h"
#include "panasonic_codec.h"

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

static_assert((uint8_t)Mode::Off == pacodec::MODE_OFF, "mode enum");
static_assert((uint8_t)Mode::Fan == pacodec::MODE_FAN, "mode enum");
static_assert((uint8_t)Fan::S5 == pacodec::FAN_S5, "fan enum");
static_assert((uint8_t)Swing::Both == pacodec::SWING_BOTH, "swing enum");
static_assert((uint8_t)Preset::Quiet == pacodec::PRESET_QUIET, "preset enum");
static_assert((uint8_t)VSwing::Auto == pacodec::VSWING_AUTO, "vswing enum");
static_assert((uint8_t)HSwing::Auto == pacodec::HSWING_AUTO, "hswing enum");

enum CmdKind : uint8_t {
    CMD_POWER = pacodec::KIND_POWER,
    CMD_MODE = pacodec::KIND_MODE,
    CMD_TEMP = pacodec::KIND_TEMP,
    CMD_FAN = pacodec::KIND_FAN,
    CMD_SWING = pacodec::KIND_SWING,
    CMD_PRESET = pacodec::KIND_PRESET,
    CMD_VSWING = pacodec::KIND_VSWING,
    CMD_HSWING = pacodec::KIND_HSWING,
    CMD_NANOEX = pacodec::KIND_NANOEX,
    CMD_ECO = pacodec::KIND_ECO,
    CMD_ECONAVI = pacodec::KIND_ECONAVI,
    CMD_MILD = pacodec::KIND_MILD,
    CMD_POLL = pacodec::KIND_POLL,
};

struct Cmd {
    uint8_t kind;
    uint8_t u8;
    float f;
};

static constexpr int kMaxOps = 16;
static constexpr int64_t kCmdHoldMs = 800;

static QueueHandle_t g_queue = nullptr;
static state_cb_t g_cb = nullptr;
static int8_t g_indoor_offset = 0;
static int8_t g_outdoor_offset = 0;
static uint8_t g_shadow[10] = {};
static bool g_shadow_valid = false;
static Cmd g_ops[kMaxOps] = {};
static int g_ops_n = 0;
static bool g_poll_asap = false;
static int64_t g_cmd_ms = 0;

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
    if (cmd.kind == CMD_POLL) {
        g_poll_asap = true;
        return;
    }
    if (g_ops_n >= kMaxOps) {
        memmove(g_ops, g_ops + 1, sizeof(Cmd) * (kMaxOps - 1));
        g_ops_n = kMaxOps - 1;
    }
    g_ops[g_ops_n++] = cmd;
}

static void send_frame(uint8_t header, const uint8_t *payload, size_t len, bool log_info)
{
    uint8_t frame[16];
    size_t n = pacodec::build_frame(header, payload, len, frame, sizeof(frame));
    if (n == 0) {
        return;
    }

    uart_write_bytes(UART, frame, n);
    uart_wait_tx_done(UART, pdMS_TO_TICKS(100));

    if (log_info) {
        ESP_LOGI(TAG, "TX control");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, n, ESP_LOG_INFO);
    } else {
        ESP_LOGD(TAG, "TX poll");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, n, ESP_LOG_DEBUG);
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
    return pacodec::checksum_ok(data, len);
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
        ESP_LOGD(TAG, "unknown mode nibble 0x%X", nib);
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
        ESP_LOGD(TAG, "unknown fan 0x%02X", speed);
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

    float raw_set = g_shadow[1] * 0.5f;
    if (raw_set >= 16.0f && raw_set <= 30.0f) {
        st.set_temp = raw_set + (float)g_indoor_offset;
        st.set_valid = true;
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
        st.power_w = (float)(raw - (int32_t)frame[30]);
        st.power_valid = true;
    }
    return st;
}

static bool handle_rx(const uint8_t *data, size_t len, int64_t now)
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

    // A poll that was already in flight can arrive just after a command. Keep the
    // command image for a short window so the late poll does not undo it. Queued
    // commands are replayed on top of a newer poll, so a remote change still wins
    // as the base image.
    bool hold_command = g_ops_n == 0 && g_cmd_ms != 0 && now - g_cmd_ms < kCmdHoldMs;
    if (!hold_command) {
        memcpy(g_shadow, data + 2, 10);
        g_shadow_valid = true;
    }

    State st = parse_state(data, len);
    static State prev = {};
    bool changed = !prev.valid || prev.power_on != st.power_on || prev.mode != st.mode || prev.fan != st.fan ||
                   prev.swing != st.swing || prev.preset != st.preset || prev.vswing != st.vswing ||
                   prev.hswing != st.hswing || prev.set_valid != st.set_valid || prev.set_temp != st.set_temp ||
                   prev.indoor_valid != st.indoor_valid || prev.indoor_temp != st.indoor_temp ||
                   prev.outdoor_valid != st.outdoor_valid || prev.outdoor_temp != st.outdoor_temp ||
                   prev.power_valid != st.power_valid || prev.power_w != st.power_w || prev.nanoex != st.nanoex ||
                   prev.eco != st.eco || prev.econavi != st.econavi || prev.mild_dry != st.mild_dry ||
                   prev.defrost != st.defrost;
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

static bool build_control(uint8_t cmd[10])
{
    if (!g_shadow_valid || g_ops_n == 0) {
        return false;
    }
    memcpy(cmd, g_shadow, 10);
    bool changed = false;
    for (int i = 0; i < g_ops_n; i++) {
        if (pacodec::apply_op(cmd, g_ops[i].kind, g_ops[i].u8, g_ops[i].f, g_indoor_offset)) {
            changed = true;
        }
    }
    return changed && memcmp(cmd, g_shadow, 10) != 0;
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
        int n = uart_read_bytes(UART, tmp, sizeof(tmp), pdMS_TO_TICKS(RX_GAP_MS));
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
            if (handle_rx(rx, rx_len, now)) {
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
        if (g_ops_n > 0 && g_shadow_valid && now - last_tx >= CMD_INTERVAL_MS) {
            uint8_t payload[10];
            if (build_control(payload)) {
                send_frame(CTRL_HEADER, payload, sizeof(payload), true);
                memcpy(g_shadow, payload, sizeof(payload));
                g_cmd_ms = now;
                g_ops_n = 0;
                last_tx = now;
                sent = true;
            } else {
                g_ops_n = 0;
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
    if (xTaskCreate(protocol_task, "panasonic", 8192, nullptr, 8, nullptr) != pdPASS) {
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
