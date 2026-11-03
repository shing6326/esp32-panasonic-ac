#include <limits>
#include <math.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_analog_input.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_ota.h"
#include "zcl/esp_zigbee_zcl_thermostat.h"

#include "panasonic.h"

static const char *TAG = "ZB_PAC";

static constexpr uint8_t ZIGBEE_ENDPOINT = 1;
static constexpr int64_t FACTORY_RESET_HOLD_MS = 5000;

static constexpr uint16_t OTA_MANUFACTURER = 0x5041;
#if CONFIG_IDF_TARGET_ESP32H2
static constexpr uint16_t OTA_IMAGE_TYPE = 0x0002;
#elif CONFIG_IDF_TARGET_ESP32C6
static constexpr uint16_t OTA_IMAGE_TYPE = 0x0001;
#else
#error "Set the target to esp32c6 or esp32h2"
#endif
static constexpr uint16_t OTA_HW_VERSION = 0x0100;
static constexpr uint8_t OTA_MAX_DATA_SIZE = 223;
static constexpr int OTA_ELEMENT_HEADER_LEN = 6;
static constexpr int OTA_QUERY_INTERVAL_MIN = 60;

static constexpr uint16_t ATTR_POWER = 0xF000;
static constexpr uint16_t ATTR_MODE = 0xF001;
static constexpr uint16_t ATTR_FAN = 0xF002;
static constexpr uint16_t ATTR_SWING = 0xF003;
static constexpr uint16_t ATTR_PRESET = 0xF004;
static constexpr uint16_t ATTR_NANOEX = 0xF005;
static constexpr uint16_t ATTR_INDOOR = 0xF006;
static constexpr uint16_t ATTR_OUTDOOR = 0xF007;
static constexpr uint16_t ATTR_TARGET = 0xF008;
static constexpr uint16_t ATTR_FW = 0xF009;
static constexpr uint16_t ATTR_ECO = 0xF00A;
static constexpr uint16_t ATTR_ECONAVI = 0xF00B;
static constexpr uint16_t ATTR_MILD = 0xF00C;
static constexpr uint16_t ATTR_DEFROST = 0xF00D;
static constexpr uint16_t ATTR_POWER_W = 0xF00E;
static constexpr uint16_t ATTR_VSWING = 0xF00F;
static constexpr uint16_t ATTR_HSWING = 0xF010;

static bool g_zb_ready = false;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool ac_power = false;
static uint8_t ac_mode = 0;
static uint8_t ac_last_mode = (uint8_t)panasonic::Mode::Cool;
static uint8_t ac_fan = (uint8_t)panasonic::Fan::Auto;
static uint8_t ac_swing = (uint8_t)panasonic::Swing::Off;
static uint8_t ac_preset = (uint8_t)panasonic::Preset::None;
static uint8_t ac_vswing = (uint8_t)panasonic::VSwing::Unknown;
static uint8_t ac_hswing = (uint8_t)panasonic::HSwing::Unknown;
static bool ac_nanoex = false;
static bool ac_eco = false;
static bool ac_econavi = false;
static bool ac_mild = false;
static bool ac_defrost = false;
static bool ac_defrost_valid = false;
static float ac_indoor = std::numeric_limits<float>::quiet_NaN();
static float ac_outdoor = std::numeric_limits<float>::quiet_NaN();
static float ac_target = 24.0f;
static bool ac_set_valid = false;
static bool ac_indoor_valid = false;
static bool ac_outdoor_valid = false;
static float ac_power_w = std::numeric_limits<float>::quiet_NaN();
static bool ac_power_valid = false;
static int16_t thermostat_local_temp = (int16_t)0x8000;
static int16_t thermostat_outdoor_temp = (int16_t)0x8000;
static int16_t thermostat_cooling_setpoint = 2400;
static int16_t thermostat_heating_setpoint = 2400;
static int16_t thermostat_min_heat_setpoint = 1600;
static int16_t thermostat_max_heat_setpoint = 3000;
static int16_t thermostat_min_cool_setpoint = 1600;
static int16_t thermostat_max_cool_setpoint = 3000;
static uint8_t thermostat_control_sequence = ESP_ZB_ZCL_THERMOSTAT_CONTROL_SEQ_OF_OPERATION_COOLING_AND_HEATING_4_PIPES;
static uint8_t thermostat_system_mode = ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF;
static uint8_t thermostat_running_mode = 0x00;
static uint8_t thermostat_ac_type = 0x04;
static uint8_t thermostat_louver_position = ESP_ZB_ZCL_THERMOSTAT_LOUVER_FULLY_CLOSED;
static uint32_t firmware_version = 0x01000000;  // 1.0.0

static int16_t celsius_to_zcl(float celsius)
{
    return (int16_t)lroundf(celsius * 100.0f);
}

static float normalize_target_temp(float celsius)
{
    if (celsius < 16.0f) {
        celsius = 16.0f;
    } else if (celsius > 30.0f) {
        celsius = 30.0f;
    }
    return roundf(celsius * 2.0f) / 2.0f;
}

static uint8_t ac_mode_to_system_mode(uint8_t mode)
{
    switch (mode) {
    case (uint8_t)panasonic::Mode::Off:
        return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF;
    case (uint8_t)panasonic::Mode::Cool:
        return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_COOL;
    case (uint8_t)panasonic::Mode::Heat:
        return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_HEAT;
    case (uint8_t)panasonic::Mode::Dry:
        return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_DRY;
    case (uint8_t)panasonic::Mode::Fan:
        return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_FAN_ONLY;
    case (uint8_t)panasonic::Mode::Auto:
    default:
        return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_AUTO;
    }
}

static uint8_t system_mode_to_ac_mode(uint8_t mode)
{
    switch (mode) {
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF:
        return (uint8_t)panasonic::Mode::Off;
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_COOL:
        return (uint8_t)panasonic::Mode::Cool;
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_HEAT:
        return (uint8_t)panasonic::Mode::Heat;
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_DRY:
        return (uint8_t)panasonic::Mode::Dry;
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_FAN_ONLY:
        return (uint8_t)panasonic::Mode::Fan;
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_AUTO:
    default:
        return (uint8_t)panasonic::Mode::Auto;
    }
}

static void update_attr(uint16_t cluster, uint16_t attr, void *value)
{
    esp_zb_zcl_set_attribute_val(ZIGBEE_ENDPOINT, cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr, value, false);
}

static bool near_temp(float a, float b)
{
    return fabsf(a - b) < 0.05f;
}

struct Published {
    bool power;
    uint8_t mode;
    uint8_t fan;
    uint8_t swing;
    uint8_t preset;
    uint8_t vswing;
    uint8_t hswing;
    bool nanoex;
    bool eco;
    bool econavi;
    bool mild;
    bool defrost;
    bool defrost_valid;
    bool set_valid;
    bool indoor_valid;
    bool outdoor_valid;
    bool power_valid;
    float target;
    float indoor;
    float outdoor;
    float power_w;
    int16_t local_temp;
    int16_t outdoor_temp;
};

static Published g_published = {};
static bool g_published_valid = false;

static bool state_changed(const Published &now)
{
    if (!g_published_valid) {
        return true;
    }
    const Published &prev = g_published;
    if (prev.power != now.power || prev.mode != now.mode || prev.fan != now.fan || prev.swing != now.swing ||
        prev.preset != now.preset || prev.vswing != now.vswing || prev.hswing != now.hswing ||
        prev.nanoex != now.nanoex || prev.eco != now.eco || prev.econavi != now.econavi || prev.mild != now.mild ||
        prev.defrost_valid != now.defrost_valid || (now.defrost_valid && prev.defrost != now.defrost) ||
        prev.set_valid != now.set_valid || (now.set_valid && !near_temp(prev.target, now.target)) ||
        prev.indoor_valid != now.indoor_valid || (now.indoor_valid && !near_temp(prev.indoor, now.indoor)) ||
        prev.outdoor_valid != now.outdoor_valid || (now.outdoor_valid && !near_temp(prev.outdoor, now.outdoor)) ||
        prev.power_valid != now.power_valid || (now.power_valid && !near_temp(prev.power_w, now.power_w))) {
        return true;
    }
    return false;
}

static Published capture_state()
{
    Published now = {};
    taskENTER_CRITICAL(&s_state_mux);
    now.power = ac_power;
    now.mode = ac_mode;
    now.fan = ac_fan;
    now.swing = ac_swing;
    now.preset = ac_preset;
    now.vswing = ac_vswing;
    now.hswing = ac_hswing;
    now.nanoex = ac_nanoex;
    now.eco = ac_eco;
    now.econavi = ac_econavi;
    now.mild = ac_mild;
    now.defrost = ac_defrost;
    now.defrost_valid = ac_defrost_valid;
    now.set_valid = ac_set_valid;
    now.indoor_valid = ac_indoor_valid;
    now.outdoor_valid = ac_outdoor_valid;
    now.power_valid = ac_power_valid;
    now.target = ac_target;
    now.indoor = ac_indoor;
    now.outdoor = ac_outdoor;
    now.power_w = ac_power_w;
    now.local_temp = thermostat_local_temp;
    now.outdoor_temp = thermostat_outdoor_temp;
    taskEXIT_CRITICAL(&s_state_mux);
    return now;
}

static bool publish_all_attrs(bool take_lock)
{
    if (!g_zb_ready) {
        return false;
    }
    Published now = capture_state();
    taskENTER_CRITICAL(&s_state_mux);
    bool changed = state_changed(now);
    taskEXIT_CRITICAL(&s_state_mux);
    if (!changed) {
        return true;
    }
    if (take_lock && esp_zb_lock_acquire(pdMS_TO_TICKS(200)) != ESP_OK) {
        ESP_LOGW(TAG, "Zigbee lock busy, skip state publish");
        return false;
    }

    thermostat_system_mode = ac_mode_to_system_mode(now.mode);
    thermostat_running_mode = now.power ? thermostat_system_mode : 0x00;
    thermostat_louver_position = now.swing == (uint8_t)panasonic::Swing::Off
                                     ? ESP_ZB_ZCL_THERMOSTAT_LOUVER_FULLY_CLOSED
                                     : ESP_ZB_ZCL_THERMOSTAT_LOUVER_FULLY_OPEN;
    if (now.set_valid) {
        thermostat_cooling_setpoint = celsius_to_zcl(now.target);
        thermostat_heating_setpoint = thermostat_cooling_setpoint;
    }

    if (now.indoor_valid) {
        update_attr(ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID, &now.local_temp);
        update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_INDOOR, &now.indoor);
    }
    if (now.outdoor_valid) {
        update_attr(ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_ATTR_THERMOSTAT_OUTDOOR_TEMPERATURE_ID, &now.outdoor_temp);
        update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_OUTDOOR, &now.outdoor);
    }
    if (now.set_valid) {
        update_attr(ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID,
                    &thermostat_cooling_setpoint);
        update_attr(ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID,
                    &thermostat_heating_setpoint);
        update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_TARGET, &now.target);
    }
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID, &thermostat_system_mode);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_ATTR_THERMOSTAT_RUNNING_MODE_ID, &thermostat_running_mode);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_LOUVER_POSITION_ID, &thermostat_louver_position);

    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_POWER, &now.power);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_MODE, &now.mode);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_FAN, &now.fan);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_SWING, &now.swing);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_PRESET, &now.preset);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_NANOEX, &now.nanoex);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_FW, &firmware_version);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_ECO, &now.eco);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_ECONAVI, &now.econavi);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_MILD, &now.mild);
    if (now.defrost_valid) {
        update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_DEFROST, &now.defrost);
    }
    if (now.power_valid) {
        update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_POWER_W, &now.power_w);
    }
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_VSWING, &now.vswing);
    update_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ATTR_HSWING, &now.hswing);

    if (take_lock) {
        esp_zb_lock_release();
    }
    taskENTER_CRITICAL(&s_state_mux);
    g_published = now;
    g_published_valid = true;
    taskEXIT_CRITICAL(&s_state_mux);
    return true;
}

static void on_panasonic_state(const panasonic::State &state)
{
    if (!state.valid) {
        return;
    }

    taskENTER_CRITICAL(&s_state_mux);
    ac_power = state.power_on;
    ac_mode = (uint8_t)state.mode;
    if (state.mode != panasonic::Mode::Off) {
        ac_last_mode = ac_mode;
    }
    ac_fan = (uint8_t)state.fan;
    ac_swing = (uint8_t)state.swing;
    ac_preset = (uint8_t)state.preset;
    ac_vswing = (uint8_t)state.vswing;
    ac_hswing = (uint8_t)state.hswing;
    ac_nanoex = state.nanoex;
    ac_eco = state.eco;
    ac_econavi = state.econavi;
    ac_mild = state.mild_dry;
    if (state.set_valid) {
        ac_target = state.set_temp;
        ac_set_valid = true;
    }
    if (state.indoor_valid) {
        ac_indoor = state.indoor_temp;
        ac_indoor_valid = true;
        thermostat_local_temp = celsius_to_zcl(ac_indoor);
    }
    if (state.outdoor_valid) {
        ac_outdoor = state.outdoor_temp;
        ac_outdoor_valid = true;
        thermostat_outdoor_temp = celsius_to_zcl(ac_outdoor);
    }
    if (state.defrost_valid) {
        ac_defrost = state.defrost;
        ac_defrost_valid = true;
    }
    if (state.power_valid) {
        ac_power_w = state.power_w;
        ac_power_valid = true;
    }
    taskEXIT_CRITICAL(&s_state_mux);
    publish_all_attrs(true);
}

static bool attr_bool(const esp_zb_zcl_set_attr_value_message_t *message)
{
    return message->attribute.data.value && *((const uint8_t *)message->attribute.data.value) != 0;
}

static uint8_t attr_u8(const esp_zb_zcl_set_attr_value_message_t *message)
{
    return message->attribute.data.value ? *((const uint8_t *)message->attribute.data.value) : 0;
}

static int16_t attr_i16(const esp_zb_zcl_set_attr_value_message_t *message)
{
    return message->attribute.data.value ? *((const int16_t *)message->attribute.data.value) : 0;
}

static float attr_single(const esp_zb_zcl_set_attr_value_message_t *message)
{
    return message->attribute.data.value ? *((const float *)message->attribute.data.value) : 0.0f;
}

static void remember_on_mode()
{
    if (ac_mode == (uint8_t)panasonic::Mode::Off) {
        ac_mode = ac_last_mode;
    }
    ac_power = true;
}

static void apply_power(bool on)
{
    taskENTER_CRITICAL(&s_state_mux);
    ac_power = on;
    if (on) {
        remember_on_mode();
    } else {
        ac_mode = (uint8_t)panasonic::Mode::Off;
    }
    taskEXIT_CRITICAL(&s_state_mux);
    panasonic::set_power(on);
}

static void apply_mode(uint8_t mode)
{
    if (mode > (uint8_t)panasonic::Mode::Fan) {
        return;
    }
    taskENTER_CRITICAL(&s_state_mux);
    ac_mode = mode;
    ac_power = mode != (uint8_t)panasonic::Mode::Off;
    if (ac_power) {
        ac_last_mode = mode;
    }
    taskEXIT_CRITICAL(&s_state_mux);
    panasonic::set_mode((panasonic::Mode)mode);
}

static void apply_target(float celsius)
{
    float normalized = normalize_target_temp(celsius);
    taskENTER_CRITICAL(&s_state_mux);
    ac_target = normalized;
    ac_set_valid = true;
    remember_on_mode();
    taskEXIT_CRITICAL(&s_state_mux);
    panasonic::set_target_temp(normalized);
    panasonic::set_power(true);
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty attribute message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG,
                        "Attribute write failed with status %d", message->info.status);
    ESP_RETURN_ON_FALSE(message->info.dst_endpoint == ZIGBEE_ENDPOINT, ESP_OK, TAG, "Other endpoint");
    ESP_RETURN_ON_FALSE(message->attribute.data.value, ESP_OK, TAG, "Empty value");

    if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT) {
        switch (message->attribute.id) {
        case ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID:
            apply_mode(system_mode_to_ac_mode(attr_u8(message)));
            break;
        case ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID:
        case ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID:
            apply_target((float)attr_i16(message) / 100.0f);
            break;
        case ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_LOUVER_POSITION_ID: {
            uint8_t swing = attr_u8(message) == ESP_ZB_ZCL_THERMOSTAT_LOUVER_FULLY_CLOSED
                                ? (uint8_t)panasonic::Swing::Off
                                : (uint8_t)panasonic::Swing::Vertical;
            taskENTER_CRITICAL(&s_state_mux);
            ac_swing = swing;
            taskEXIT_CRITICAL(&s_state_mux);
            panasonic::set_swing((panasonic::Swing)swing);
            break;
        }
        default:
            break;
        }
        publish_all_attrs(false);
        return ESP_OK;
    }

    if (message->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT) {
        return ESP_OK;
    }

    switch (message->attribute.id) {
    case ATTR_POWER:
        apply_power(attr_bool(message));
        break;
    case ATTR_MODE:
        apply_mode(attr_u8(message));
        break;
    case ATTR_FAN:
        if (attr_u8(message) <= (uint8_t)panasonic::Fan::S5) {
            uint8_t fan = attr_u8(message);
            taskENTER_CRITICAL(&s_state_mux);
            ac_fan = fan;
            ac_preset = (uint8_t)panasonic::Preset::None;
            taskEXIT_CRITICAL(&s_state_mux);
            panasonic::set_fan((panasonic::Fan)fan);
        }
        break;
    case ATTR_SWING:
        if (attr_u8(message) <= (uint8_t)panasonic::Swing::Both) {
            uint8_t swing = attr_u8(message);
            taskENTER_CRITICAL(&s_state_mux);
            ac_swing = swing;
            taskEXIT_CRITICAL(&s_state_mux);
            panasonic::set_swing((panasonic::Swing)swing);
        }
        break;
    case ATTR_PRESET:
        if (attr_u8(message) <= (uint8_t)panasonic::Preset::Quiet) {
            uint8_t preset = attr_u8(message);
            taskENTER_CRITICAL(&s_state_mux);
            ac_preset = preset;
            taskEXIT_CRITICAL(&s_state_mux);
            panasonic::set_preset((panasonic::Preset)preset);
        }
        break;
    case ATTR_NANOEX: {
        bool nanoex = attr_bool(message);
        taskENTER_CRITICAL(&s_state_mux);
        ac_nanoex = nanoex;
        taskEXIT_CRITICAL(&s_state_mux);
        panasonic::set_nanoex(nanoex);
        break;
    }
    case ATTR_ECO: {
        bool eco = attr_bool(message);
        taskENTER_CRITICAL(&s_state_mux);
        ac_eco = eco;
        taskEXIT_CRITICAL(&s_state_mux);
        panasonic::set_eco(eco);
        break;
    }
    case ATTR_ECONAVI: {
        bool econavi = attr_bool(message);
        taskENTER_CRITICAL(&s_state_mux);
        ac_econavi = econavi;
        taskEXIT_CRITICAL(&s_state_mux);
        panasonic::set_econavi(econavi);
        break;
    }
    case ATTR_MILD: {
        bool mild = attr_bool(message);
        taskENTER_CRITICAL(&s_state_mux);
        ac_mild = mild;
        taskEXIT_CRITICAL(&s_state_mux);
        panasonic::set_mild_dry(mild);
        break;
    }
    case ATTR_TARGET:
        apply_target(attr_single(message));
        break;
    case ATTR_VSWING:
        if (attr_u8(message) <= (uint8_t)panasonic::VSwing::Auto) {
            uint8_t vswing = attr_u8(message);
            taskENTER_CRITICAL(&s_state_mux);
            ac_vswing = vswing;
            taskEXIT_CRITICAL(&s_state_mux);
            panasonic::set_vswing((panasonic::VSwing)vswing);
        }
        break;
    case ATTR_HSWING:
        if (attr_u8(message) <= (uint8_t)panasonic::HSwing::Auto) {
            uint8_t hswing = attr_u8(message);
            taskENTER_CRITICAL(&s_state_mux);
            ac_hswing = hswing;
            taskEXIT_CRITICAL(&s_state_mux);
            panasonic::set_hswing((panasonic::HSwing)hswing);
        }
        break;
    default:
        break;
    }

    publish_all_attrs(false);
    return ESP_OK;
}

static const esp_partition_t *s_ota_partition = nullptr;
static esp_ota_handle_t s_ota_handle = 0;
static bool s_ota_tagid_received = false;

static esp_err_t ota_extract_element(uint32_t total_size, const void *payload, uint16_t payload_size, void **outbuf,
                                     uint16_t *outlen)
{
    static uint16_t tagid = 0;
    if (!s_ota_tagid_received) {
        if (payload == nullptr || payload_size <= OTA_ELEMENT_HEADER_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        tagid = *(const uint16_t *)payload;
        uint32_t length = *(const uint32_t *)((const uint8_t *)payload + sizeof(tagid));
        if ((length + OTA_ELEMENT_HEADER_LEN) != total_size) {
            return ESP_ERR_INVALID_ARG;
        }
        s_ota_tagid_received = true;
        *outbuf = (void *)((const uint8_t *)payload + OTA_ELEMENT_HEADER_LEN);
        *outlen = payload_size - OTA_ELEMENT_HEADER_LEN;
    } else {
        *outbuf = (void *)payload;
        *outlen = payload_size;
    }
    if (tagid != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void ota_abort()
{
    if (s_ota_handle != 0) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
    }
    s_ota_tagid_received = false;
}

static esp_err_t zb_ota_upgrade_status_handler(esp_zb_zcl_ota_upgrade_value_message_t message)
{
    static uint32_t total_size = 0;
    static uint32_t offset = 0;
    if (message.info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        return ESP_OK;
    }
    switch (message.upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        ESP_LOGI(TAG, "OTA start");
        s_ota_tagid_received = false;
        offset = 0;
        s_ota_partition = esp_ota_get_next_update_partition(nullptr);
        ESP_RETURN_ON_FALSE(s_ota_partition, ESP_ERR_NOT_FOUND, TAG, "No OTA partition");
        return esp_ota_begin(s_ota_partition, OTA_SIZE_UNKNOWN, &s_ota_handle);
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
        total_size = message.ota_header.image_size;
        offset += message.payload_size;
        if (message.payload_size && message.payload) {
            uint16_t out_len = 0;
            void *out_buf = nullptr;
            esp_err_t ret = ota_extract_element(total_size, message.payload, message.payload_size, &out_buf, &out_len);
            if (ret != ESP_OK) {
                ota_abort();
                return ret;
            }
            if (out_len) {
                ret = esp_ota_write(s_ota_handle, out_buf, out_len);
                if (ret != ESP_OK) {
                    ota_abort();
                    return ret;
                }
            }
        }
        break;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
        if (offset != total_size) {
            ota_abort();
            offset = 0;
            total_size = 0;
            return ESP_FAIL;
        }
        offset = 0;
        total_size = 0;
        s_ota_tagid_received = false;
        break;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH: {
        esp_err_t ret = esp_ota_end(s_ota_handle);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = esp_ota_set_boot_partition(s_ota_partition);
        if (ret != ESP_OK) {
            return ret;
        }
        esp_restart();
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        return zb_attribute_handler((const esp_zb_zcl_set_attr_value_message_t *)message);
    case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
        return zb_ota_upgrade_status_handler(*(const esp_zb_zcl_ota_upgrade_value_message_t *)message);
    default:
        break;
    }
    return ESP_OK;
}

extern "C" void factory_reset_cb(uint8_t)
{
    esp_zb_factory_reset();
}

static void factory_reset_button_task(void *)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << CONFIG_PANASONIC_FACTORY_RESET_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    int64_t pressed_since = 0;
    bool reset_done = false;
    while (true) {
        bool pressed = gpio_get_level((gpio_num_t)CONFIG_PANASONIC_FACTORY_RESET_GPIO) == 0;
        int64_t now = esp_log_timestamp();
        if (pressed) {
            if (pressed_since == 0) {
                pressed_since = now;
            } else if (!reset_done && g_zb_ready && now - pressed_since >= FACTORY_RESET_HOLD_MS) {
                if (esp_zb_lock_acquire(pdMS_TO_TICKS(200)) == ESP_OK) {
                    esp_zb_scheduler_alarm(factory_reset_cb, 0, 0);
                    esp_zb_lock_release();
                    reset_done = true;
                    ESP_LOGW(TAG, "Factory reset: leaving the Zigbee network");
                }
            }
        } else {
            pressed_since = 0;
            reset_done = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void add_analog(esp_zb_attribute_list_t *list, uint16_t id, uint8_t type, uint8_t access, void *value)
{
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(list, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, id, type, access, value));
}

static void config_report(uint16_t cluster, uint16_t attr)
{
    esp_zb_zcl_reporting_info_t info = {};
    info.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    info.ep = ZIGBEE_ENDPOINT;
    info.cluster_id = cluster;
    info.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    info.attr_id = attr;
    info.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    info.u.send_info.min_interval = 1;
    info.u.send_info.max_interval = 300;
    info.u.send_info.def_min_interval = 1;
    info.u.send_info.def_max_interval = 300;
    info.u.send_info.delta.u8 = 0;
    esp_err_t err = esp_zb_zcl_update_reporting_info(&info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reporting 0x%04x/0x%04x failed: %s", cluster, attr, esp_err_to_name(err));
    }
}

static void config_reports()
{
    const uint16_t therm[] = {
        ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_OUTDOOR_TEMPERATURE_ID,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID,
    };
    for (uint16_t id : therm) {
        config_report(ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, id);
    }
    const uint16_t analog[] = {
        ATTR_POWER, ATTR_MODE,     ATTR_FAN,    ATTR_SWING, ATTR_PRESET, ATTR_NANOEX, ATTR_INDOOR,
        ATTR_OUTDOOR, ATTR_TARGET, ATTR_FW,     ATTR_ECO,   ATTR_ECONAVI, ATTR_MILD, ATTR_DEFROST,
        ATTR_POWER_W, ATTR_VSWING, ATTR_HSWING,
    };
    for (uint16_t id : analog) {
        config_report(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, id);
    }
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    xTaskCreate(factory_reset_button_task, "factory_reset", 2048, nullptr, 5, nullptr);

    ESP_LOGI(TAG, "ESP-PAC CZ-TACG1 Zigbee bridge 1.0.0");

    esp_zb_platform_config_t platform_config = {};
    platform_config.radio_config.radio_mode = ZB_RADIO_MODE_NATIVE;
    platform_config.host_config.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE;
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_config));

    esp_zb_cfg_t zb_cfg = {};
    zb_cfg.esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER;
    zb_cfg.install_code_policy = false;
    zb_cfg.nwk_cfg.zczr_cfg.max_children = 10;
    esp_zb_init(&zb_cfg);

    static char model_id[] = {15, 'Z', 'B', '-', 'P', 'A', 'N', 'A', 'S', 'O', 'N', 'I', 'C', '-', 'A', 'C'};
    static char manuf_name[] = {7, 'E', 'S', 'P', '-', 'P', 'A', 'C'};

    esp_zb_basic_cluster_cfg_t basic_cfg = {};
    basic_cfg.zcl_version = 3;
    basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
    esp_zb_attribute_list_t *basic_attr = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(basic_attr, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model_id));
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(basic_attr, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manuf_name));

    esp_zb_thermostat_cluster_cfg_t therm_cfg = {};
    therm_cfg.local_temperature = thermostat_local_temp;
    therm_cfg.occupied_cooling_setpoint = thermostat_cooling_setpoint;
    therm_cfg.occupied_heating_setpoint = thermostat_heating_setpoint;
    therm_cfg.control_sequence_of_operation = thermostat_control_sequence;
    therm_cfg.system_mode = thermostat_system_mode;
    esp_zb_attribute_list_t *therm_attr = esp_zb_thermostat_cluster_create(&therm_cfg);
    ESP_ERROR_CHECK(esp_zb_thermostat_cluster_add_attr(therm_attr, ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_HEAT_SETPOINT_LIMIT_ID,
                                                       &thermostat_min_heat_setpoint));
    ESP_ERROR_CHECK(esp_zb_thermostat_cluster_add_attr(therm_attr, ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_HEAT_SETPOINT_LIMIT_ID,
                                                       &thermostat_max_heat_setpoint));
    ESP_ERROR_CHECK(esp_zb_thermostat_cluster_add_attr(therm_attr, ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_COOL_SETPOINT_LIMIT_ID,
                                                       &thermostat_min_cool_setpoint));
    ESP_ERROR_CHECK(esp_zb_thermostat_cluster_add_attr(therm_attr, ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_COOL_SETPOINT_LIMIT_ID,
                                                       &thermostat_max_cool_setpoint));
    ESP_ERROR_CHECK(esp_zb_thermostat_cluster_add_attr(therm_attr, ESP_ZB_ZCL_ATTR_THERMOSTAT_OUTDOOR_TEMPERATURE_ID,
                                                       &thermostat_outdoor_temp));
    ESP_ERROR_CHECK(esp_zb_thermostat_cluster_add_attr(therm_attr, ESP_ZB_ZCL_ATTR_THERMOSTAT_RUNNING_MODE_ID,
                                                       &thermostat_running_mode));
    ESP_ERROR_CHECK(esp_zb_thermostat_cluster_add_attr(therm_attr, ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_TYPE_ID, &thermostat_ac_type));
    ESP_ERROR_CHECK(esp_zb_thermostat_cluster_add_attr(therm_attr, ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_LOUVER_POSITION_ID,
                                                       &thermostat_louver_position));

    esp_zb_analog_input_cluster_cfg_t analog_cfg = {};
    analog_cfg.out_of_service = false;
    analog_cfg.present_value = 0;
    esp_zb_attribute_list_t *analog_attr = esp_zb_analog_input_cluster_create(&analog_cfg);
    uint8_t rw = ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING;
    uint8_t ro = ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING;
    add_analog(analog_attr, ATTR_POWER, ESP_ZB_ZCL_ATTR_TYPE_BOOL, rw, &ac_power);
    add_analog(analog_attr, ATTR_MODE, ESP_ZB_ZCL_ATTR_TYPE_U8, rw, &ac_mode);
    add_analog(analog_attr, ATTR_FAN, ESP_ZB_ZCL_ATTR_TYPE_U8, rw, &ac_fan);
    add_analog(analog_attr, ATTR_SWING, ESP_ZB_ZCL_ATTR_TYPE_U8, rw, &ac_swing);
    add_analog(analog_attr, ATTR_PRESET, ESP_ZB_ZCL_ATTR_TYPE_U8, rw, &ac_preset);
    add_analog(analog_attr, ATTR_NANOEX, ESP_ZB_ZCL_ATTR_TYPE_BOOL, rw, &ac_nanoex);
    add_analog(analog_attr, ATTR_INDOOR, ESP_ZB_ZCL_ATTR_TYPE_SINGLE, ro, &ac_indoor);
    add_analog(analog_attr, ATTR_OUTDOOR, ESP_ZB_ZCL_ATTR_TYPE_SINGLE, ro, &ac_outdoor);
    add_analog(analog_attr, ATTR_TARGET, ESP_ZB_ZCL_ATTR_TYPE_SINGLE, rw, &ac_target);
    add_analog(analog_attr, ATTR_FW, ESP_ZB_ZCL_ATTR_TYPE_U32, ro, &firmware_version);
    add_analog(analog_attr, ATTR_ECO, ESP_ZB_ZCL_ATTR_TYPE_BOOL, rw, &ac_eco);
    add_analog(analog_attr, ATTR_ECONAVI, ESP_ZB_ZCL_ATTR_TYPE_BOOL, rw, &ac_econavi);
    add_analog(analog_attr, ATTR_MILD, ESP_ZB_ZCL_ATTR_TYPE_BOOL, rw, &ac_mild);
    add_analog(analog_attr, ATTR_DEFROST, ESP_ZB_ZCL_ATTR_TYPE_BOOL, ro, &ac_defrost);
    add_analog(analog_attr, ATTR_POWER_W, ESP_ZB_ZCL_ATTR_TYPE_SINGLE, ro, &ac_power_w);
    add_analog(analog_attr, ATTR_VSWING, ESP_ZB_ZCL_ATTR_TYPE_U8, rw, &ac_vswing);
    add_analog(analog_attr, ATTR_HSWING, ESP_ZB_ZCL_ATTR_TYPE_U8, rw, &ac_hswing);

    esp_zb_ota_cluster_cfg_t ota_cfg = {};
    ota_cfg.ota_upgrade_file_version = firmware_version;
    ota_cfg.ota_upgrade_downloaded_file_ver = firmware_version;
    ota_cfg.ota_upgrade_manufacturer = OTA_MANUFACTURER;
    ota_cfg.ota_upgrade_image_type = OTA_IMAGE_TYPE;
    esp_zb_attribute_list_t *ota_attr = esp_zb_ota_cluster_create(&ota_cfg);
    esp_zb_zcl_ota_upgrade_client_variable_t ota_var = {};
    ota_var.timer_query = ESP_ZB_ZCL_OTA_UPGRADE_QUERY_TIMER_COUNT_DEF;
    ota_var.hw_version = OTA_HW_VERSION;
    ota_var.max_data_size = OTA_MAX_DATA_SIZE;
    uint16_t ota_server_addr = 0xffff;
    uint8_t ota_server_ep = 0xff;
    ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(ota_attr, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID, &ota_var));
    ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(ota_attr, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID, &ota_server_addr));
    ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(ota_attr, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID, &ota_server_ep));

    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(clusters, basic_attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_thermostat_cluster(clusters, therm_attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_analog_input_cluster(clusters, analog_attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_ota_cluster(clusters, ota_attr, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));

    esp_zb_endpoint_config_t ep_config = {};
    ep_config.endpoint = ZIGBEE_ENDPOINT;
    ep_config.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    ep_config.app_device_id = ESP_ZB_HA_THERMOSTAT_DEVICE_ID;
    ep_config.app_device_version = 1;
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, clusters, ep_config));
    ESP_ERROR_CHECK(esp_zb_device_register(ep_list));
    esp_zb_core_action_handler_register(zb_action_handler);

    g_zb_ready = true;
    config_reports();
    ESP_ERROR_CHECK(esp_zb_start(false));
    ESP_ERROR_CHECK(panasonic::init(CONFIG_PANASONIC_UART_TX_GPIO, CONFIG_PANASONIC_UART_RX_GPIO,
                                    (int8_t)CONFIG_PANASONIC_INDOOR_OFFSET, (int8_t)CONFIG_PANASONIC_OUTDOOR_OFFSET,
                                    on_panasonic_state));

    esp_zb_stack_main_loop();
}

extern "C" void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    esp_zb_bdb_start_top_level_commissioning(mode_mask);
}

static uint8_t s_steer_step = 0;

static uint32_t steer_delay_ms()
{
    static const uint32_t delays[] = {1000, 2000, 5000, 10000};
    uint32_t delay = delays[s_steer_step];
    if (s_steer_step + 1 < 4) {
        s_steer_step++;
    }
    return delay;
}

static void schedule_steering()
{
    uint32_t delay = steer_delay_ms();
    ESP_LOGW(TAG, "Steering retry in %lu ms", (unsigned long)delay);
    esp_zb_scheduler_alarm(bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, delay);
}

static void joined_network()
{
    s_steer_step = 0;
    ESP_LOGI(TAG, "On Zigbee network, pan 0x%04x channel %d addr 0x%04x", esp_zb_get_pan_id(),
             esp_zb_get_current_channel(), esp_zb_get_short_address());
    panasonic::request_status();
}

static void ota_match_server_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr, uint8_t endpoint, void *)
{
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "OTA server found at 0x%04x ep %d", addr, endpoint);
        esp_zb_ota_upgrade_client_query_interval_set(ZIGBEE_ENDPOINT, OTA_QUERY_INTERVAL_MIN);
        esp_zb_ota_upgrade_client_query_image_req(addr, endpoint);
    }
}

static void ota_find_server(void)
{
    static uint16_t ota_cluster = ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE;
    esp_zb_zdo_match_desc_req_param_t req = {};
    req.addr_of_interest = 0x0000;
    req.dst_nwk_addr = 0x0000;
    req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    req.num_in_clusters = 0;
    req.num_out_clusters = 1;
    req.cluster_list = &ota_cluster;
    esp_zb_zdo_match_cluster(&req, ota_match_server_cb, nullptr);
}

extern "C" void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *signal = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)*signal;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack ready");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Factory new, start steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                joined_network();
                ota_find_server();
            }
        } else {
            ESP_LOGW(TAG, "Zigbee init failed (%s)", esp_err_to_name(err_status));
            schedule_steering();
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            joined_network();
            ota_find_server();
        } else {
            ESP_LOGW(TAG, "Steering failed (%s). Permit join on the coordinator.", esp_err_to_name(err_status));
            schedule_steering();
        }
        break;
    default:
        ESP_LOGI(TAG, "Zigbee signal %d, status %s", (int)sig_type, esp_err_to_name(err_status));
        break;
    }
}
