#!/usr/bin/env python3
"""Embed a SPIR-V binary in a C header as a uint32_t array.

The probes dlopen a Vulkan driver and hand it SPIR-V; embedding the module
means they have no runtime dependency on a shader compiler, and the committed
header means a normal build needs no glslang either. Only `make
pipeline_probe_shader` regenerates it.

Usage: spv_to_header.py <input.spv> <output.h> <symbol>
"""
import struct
import sys

SPIRV_MAGIC = 0x07230203


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)

    spv_path, header_path, symbol = sys.argv[1:4]

    with open(spv_path, "rb") as f:
        data = f.read()

    if len(data) % 4:
        sys.exit(f"{spv_path}: {len(data)} bytes is not a whole number of "
                 "32-bit words, so it is not SPIR-V")

    words = struct.unpack(f"<{len(data) // 4}I", data)

    # Checked rather than assumed: a big-endian module, or a GLSL file passed
    # by mistake, would otherwise be embedded happily and fail much later
    # inside the driver, where it looks like a driver bug.
    if words[0] != SPIRV_MAGIC:
        sys.exit(f"{spv_path}: first word is 0x{words[0]:08x}, expected "
                 f"0x{SPIRV_MAGIC:08x} - not a little-endian SPIR-V module")

    guard = symbol.upper() + "_H"
    out = [
        f"/* Generated from {spv_path} by tools/spv_to_header.py.",
        " * Do not edit - regenerate with: make pipeline_probe_shader",
        " */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <stdint.h>",
        "",
        f"static const uint32_t {symbol}[] = {{",
    ]

    for i in range(0, len(words), 8):
        row = " ".join(f"0x{w:08x}," for w in words[i:i + 8])
        out.append(f"   {row}")

    out += ["};", "", f"#endif /* {guard} */", ""]

    with open(header_path, "w", newline="\n") as f:
        f.write("\n".join(out))

    print(f"{header_path}: {len(words)} words")


if __name__ == "__main__":
    main()
