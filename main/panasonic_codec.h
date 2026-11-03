#pragma once

// CZ-TACG1 control-byte updates. No ESP-IDF types, so the host test can compile it.

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace pacodec {

static constexpr uint8_t KIND_POWER = 1;
static constexpr uint8_t KIND_MODE = 2;
static constexpr uint8_t KIND_TEMP = 3;
static constexpr uint8_t KIND_FAN = 4;
static constexpr uint8_t KIND_SWING = 5;
static constexpr uint8_t KIND_PRESET = 6;
static constexpr uint8_t KIND_VSWING = 7;
static constexpr uint8_t KIND_HSWING = 8;
static constexpr uint8_t KIND_NANOEX = 9;
static constexpr uint8_t KIND_ECO = 10;
static constexpr uint8_t KIND_ECONAVI = 11;
static constexpr uint8_t KIND_MILD = 12;
static constexpr uint8_t KIND_POLL = 13;

static constexpr uint8_t MODE_OFF = 0;
static constexpr uint8_t MODE_AUTO = 1;
static constexpr uint8_t MODE_COOL = 2;
static constexpr uint8_t MODE_HEAT = 3;
static constexpr uint8_t MODE_DRY = 4;
static constexpr uint8_t MODE_FAN = 5;

static constexpr uint8_t FAN_AUTO = 0;
static constexpr uint8_t FAN_S1 = 1;
static constexpr uint8_t FAN_S2 = 2;
static constexpr uint8_t FAN_S3 = 3;
static constexpr uint8_t FAN_S4 = 4;
static constexpr uint8_t FAN_S5 = 5;

static constexpr uint8_t SWING_OFF = 0;
static constexpr uint8_t SWING_HORIZONTAL = 1;
static constexpr uint8_t SWING_VERTICAL = 2;
static constexpr uint8_t SWING_BOTH = 3;

static constexpr uint8_t PRESET_NONE = 0;
static constexpr uint8_t PRESET_POWERFUL = 1;
static constexpr uint8_t PRESET_QUIET = 2;

static constexpr uint8_t VSWING_UNKNOWN = 0;
static constexpr uint8_t VSWING_UP = 1;
static constexpr uint8_t VSWING_UP_CENTER = 2;
static constexpr uint8_t VSWING_CENTER = 3;
static constexpr uint8_t VSWING_DOWN_CENTER = 4;
static constexpr uint8_t VSWING_DOWN = 5;
static constexpr uint8_t VSWING_SWING = 6;
static constexpr uint8_t VSWING_AUTO = 7;

static constexpr uint8_t HSWING_UNKNOWN = 0;
static constexpr uint8_t HSWING_LEFT = 1;
static constexpr uint8_t HSWING_LEFT_CENTER = 2;
static constexpr uint8_t HSWING_CENTER = 3;
static constexpr uint8_t HSWING_RIGHT_CENTER = 4;
static constexpr uint8_t HSWING_RIGHT = 5;
static constexpr uint8_t HSWING_AUTO = 6;

inline uint8_t mode_byte(uint8_t mode)
{
    switch (mode) {
    case MODE_COOL:
        return 0x34;
    case MODE_HEAT:
        return 0x44;
    case MODE_DRY:
        return 0x24;
    case MODE_FAN:
        return 0x64;
    case MODE_AUTO:
        return 0x04;
    default:
        return 0x00;
    }
}

inline uint8_t fan_byte(uint8_t fan)
{
    switch (fan) {
    case FAN_S1:
        return 0x30;
    case FAN_S2:
        return 0x40;
    case FAN_S3:
        return 0x50;
    case FAN_S4:
        return 0x60;
    case FAN_S5:
        return 0x70;
    default:
        return 0xA0;
    }
}

inline uint8_t swing_byte(uint8_t swing)
{
    switch (swing) {
    case SWING_BOTH:
        return 0xFD;
    case SWING_VERTICAL:
        return 0xF6;
    case SWING_HORIZONTAL:
        return 0x3D;
    default:
        return 0x36;
    }
}

inline uint8_t vswing_high(uint8_t swing)
{
    switch (swing) {
    case VSWING_DOWN:
        return 0x50;
    case VSWING_DOWN_CENTER:
        return 0x40;
    case VSWING_UP_CENTER:
        return 0x20;
    case VSWING_UP:
        return 0x10;
    case VSWING_SWING:
        return 0xE0;
    case VSWING_AUTO:
        return 0xF0;
    case VSWING_CENTER:
        return 0x30;
    default:
        return 0;
    }
}

inline uint8_t hswing_low(uint8_t swing)
{
    switch (swing) {
    case HSWING_LEFT:
        return 0x09;
    case HSWING_LEFT_CENTER:
        return 0x0A;
    case HSWING_RIGHT_CENTER:
        return 0x0B;
    case HSWING_RIGHT:
        return 0x0C;
    case HSWING_AUTO:
        return 0x0D;
    case HSWING_CENTER:
        return 0x06;
    default:
        return 0;
    }
}

inline uint8_t preset_low(uint8_t preset)
{
    switch (preset) {
    case PRESET_POWERFUL:
        return 0x02;
    case PRESET_QUIET:
        return 0x04;
    default:
        return 0x00;
    }
}

// Apply one command onto the 10-byte control image, in the order commands arrive.
// Returns false when the command does not change the image.
inline bool apply_op(uint8_t cmd[10], uint8_t kind, uint8_t u8, float temp, int8_t indoor_offset)
{
    switch (kind) {
    case KIND_POWER:
        if (u8) {
            if ((cmd[0] & 0x0F) == 0) {
                cmd[0] = (uint8_t)((cmd[0] & 0xF0) | 0x04);
            }
        } else {
            cmd[0] = (uint8_t)(cmd[0] & 0xF0);
        }
        return true;
    case KIND_MODE:
        if (u8 == MODE_OFF) {
            cmd[0] = (uint8_t)(cmd[0] & 0xF0);
        } else {
            cmd[0] = mode_byte(u8);
        }
        return true;
    case KIND_TEMP: {
        float c = temp - (float)indoor_offset;
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
        return true;
    }
    case KIND_FAN:
        // Powerful / Quiet lives in the same byte and is cleared with a fan change.
        cmd[5] = (uint8_t)(cmd[5] & 0xF0);
        cmd[3] = fan_byte(u8);
        return true;
    case KIND_PRESET:
        cmd[5] = (uint8_t)((cmd[5] & 0xF0) | preset_low(u8));
        return true;
    case KIND_SWING:
        cmd[4] = swing_byte(u8);
        return true;
    case KIND_VSWING:
        if (u8 == VSWING_UNKNOWN) {
            return false;
        }
        cmd[4] = (uint8_t)((cmd[4] & 0x0F) | vswing_high(u8));
        return true;
    case KIND_HSWING:
        if (u8 == HSWING_UNKNOWN) {
            return false;
        }
        cmd[4] = (uint8_t)((cmd[4] & 0xF0) | hswing_low(u8));
        return true;
    case KIND_NANOEX:
        cmd[5] = u8 ? (uint8_t)(cmd[5] | 0x40) : (uint8_t)(cmd[5] & (uint8_t)~0x40);
        return true;
    case KIND_ECONAVI:
        cmd[5] = u8 ? (uint8_t)(cmd[5] | 0x10) : (uint8_t)(cmd[5] & (uint8_t)~0x10);
        return true;
    case KIND_ECO:
        cmd[8] = u8 ? 0x40 : 0x00;
        return true;
    case KIND_MILD:
        cmd[2] = u8 ? 0x7F : 0x80;
        return true;
    default:
        return false;
    }
}

inline bool checksum_ok(const uint8_t *frame, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + frame[i]);
    }
    return len > 0 && sum == 0;
}

inline size_t build_frame(uint8_t header, const uint8_t *payload, size_t len, uint8_t *out, size_t cap)
{
    if (out == nullptr || len > 255 || len + 3 > cap) {
        return 0;
    }
    out[0] = header;
    out[1] = (uint8_t)len;
    if (len > 0 && payload != nullptr) {
        memcpy(out + 2, payload, len);
    }
    uint8_t sum = 0;
    for (size_t i = 0; i < len + 2; i++) {
        sum = (uint8_t)(sum + out[i]);
    }
    out[len + 2] = (uint8_t)(0 - sum);
    return len + 3;
}

}  // namespace pacodec
