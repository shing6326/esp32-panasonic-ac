#!/usr/bin/env python3
"""Wrap an ESP-IDF app binary as a Zigbee OTA image for ESP-PAC."""

import argparse
import struct
import sys
from pathlib import Path

MANUFACTURER = 0x5041
IMAGE_TYPE = 0x0001
HEADER_STRING = b"ESP-PAC ZB-PANASONIC-AC"


def version_to_u32(text):
    parts = text.split(".")
    if len(parts) != 3:
        raise SystemExit("version must be major.minor.patch, for example 1.0.1")
    numbers = []
    for part in parts:
        if not part.isdigit():
            raise SystemExit("version must be major.minor.patch, for example 1.0.1")
        number = int(part)
        if number > 255:
            raise SystemExit("each version component must fit in one byte")
        numbers.append(number)
    major, minor, patch = numbers
    return (major << 24) | (minor << 16) | (patch << 8)


def build_ota(app, file_version):
    header_string = HEADER_STRING[:32].ljust(32, b"\x00")
    fixed = struct.pack(
        "<IHHHHHIH",
        0x0BEEF11E,
        0x0100,
        56,
        0,
        MANUFACTURER,
        IMAGE_TYPE,
        file_version,
        2,
    )
    subelement = struct.pack("<HI", 0, len(app)) + app
    image_size = 56 + len(subelement)
    header = fixed + header_string + struct.pack("<I", image_size)
    if len(header) != 56:
        raise SystemExit("OTA header must be 56 bytes when no optional fields are set")
    image = header + subelement
    if len(image) != image_size:
        raise SystemExit("OTA image size field does not match the file")
    return image


def self_test():
    image = build_ota(b"\x11\x22\x33\x44", version_to_u32("1.0.1"))
    ident, header_version, header_length, field, manuf, image_type, file_version, stack = struct.unpack_from(
        "<IHHHHHIH", image, 0
    )
    image_size = struct.unpack_from("<I", image, 52)[0]
    tag, length = struct.unpack_from("<HI", image, header_length)
    checks = [
        ident == 0x0BEEF11E,
        header_version == 0x0100,
        header_length == 56,
        field == 0,
        manuf == MANUFACTURER,
        image_type == IMAGE_TYPE,
        file_version == 0x01000100,
        stack == 2,
        image.startswith(b"\x1e\xf1\xee\x0b"),
        image[20:52].split(b"\x00", 1)[0] == HEADER_STRING,
        image_size == len(image),
        tag == 0,
        length == 4,
        image[header_length + 6 :] == b"\x11\x22\x33\x44",
    ]
    if not all(checks):
        raise SystemExit("OTA self-test failed")
    if version_to_u32("1.0.0") != 0x01000000:
        raise SystemExit("version encoding failed")


def main():
    if "--self-test" in sys.argv:
        self_test()
        return
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True, help="major.minor.patch, for example 1.0.1")
    args = parser.parse_args()
    app = args.app.read_bytes()
    if not app:
        raise SystemExit(f"{args.app} is empty")
    image = build_ota(app, version_to_u32(args.version))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"{args.output}: {len(image)} bytes, file version 0x{version_to_u32(args.version):08x}")


if __name__ == "__main__":
    main()
