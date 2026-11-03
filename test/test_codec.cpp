#include "panasonic_codec.h"

#include <stdio.h>
#include <string.h>

static int g_failed = 0;

static void expect(bool cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void test_poll_frame()
{
    uint8_t payload[10] = {};
    uint8_t frame[16] = {};
    size_t n = pacodec::build_frame(0x70, payload, 10, frame, sizeof(frame));
    expect(n == 13, "poll length");
    expect(frame[0] == 0x70 && frame[1] == 0x0A && frame[12] == 0x86, "poll bytes");
    expect(pacodec::checksum_ok(frame, n), "poll checksum");
    frame[4] ^= 0x01;
    expect(!pacodec::checksum_ok(frame, n), "broken checksum");
}

static void test_command_order()
{
    uint8_t image[10] = {};
    image[0] = 0x34;
    pacodec::apply_op(image, pacodec::KIND_MODE, pacodec::MODE_COOL, 0, 0);
    pacodec::apply_op(image, pacodec::KIND_POWER, 0, 0, 0);
    expect(image[0] == 0x30, "cool then power off keeps the mode nibble");

    memset(image, 0, sizeof(image));
    image[0] = 0x44;
    pacodec::apply_op(image, pacodec::KIND_POWER, 0, 0, 0);
    pacodec::apply_op(image, pacodec::KIND_MODE, pacodec::MODE_COOL, 0, 0);
    expect(image[0] == 0x34, "power off then cool turns the unit on");
}

static void test_fan_preset_and_nanoex()
{
    uint8_t image[10] = {};
    image[5] = 0x52;  // powerful + econavi + nanoeX
    pacodec::apply_op(image, pacodec::KIND_FAN, pacodec::FAN_S3, 0, 0);
    expect(image[3] == 0x50, "fan speed 3");
    expect(image[5] == 0x50, "fan clears preset and keeps econavi and nanoeX");

    pacodec::apply_op(image, pacodec::KIND_PRESET, pacodec::PRESET_POWERFUL, 0, 0);
    expect(image[5] == 0x52, "preset after fan is kept");

    pacodec::apply_op(image, pacodec::KIND_NANOEX, 0, 0, 0);
    expect(image[5] == 0x12, "nanoeX off keeps econavi and powerful");
}

static void test_temperature_and_swing()
{
    uint8_t image[10] = {};
    pacodec::apply_op(image, pacodec::KIND_TEMP, 0, 24.5f, 0);
    expect(image[1] == 49, "24.5 C raw");
    pacodec::apply_op(image, pacodec::KIND_TEMP, 0, 24.0f, 2);
    expect(image[1] == 44, "offset is removed before the raw byte");

    pacodec::apply_op(image, pacodec::KIND_SWING, pacodec::SWING_BOTH, 0, 0);
    pacodec::apply_op(image, pacodec::KIND_VSWING, pacodec::VSWING_UP, 0, 0);
    expect(image[4] == 0x1D, "vertical vane keeps the horizontal nibble");
    expect(!pacodec::apply_op(image, pacodec::KIND_VSWING, pacodec::VSWING_UNKNOWN, 0, 0), "unknown vane");
    expect(image[4] == 0x1D, "unknown vane does not move the vane");
}

int main()
{
    test_poll_frame();
    test_command_order();
    test_fan_preset_and_nanoex();
    test_temperature_and_swing();
    if (g_failed != 0) {
        fprintf(stderr, "%d failed\n", g_failed);
        return 1;
    }
    return 0;
}
