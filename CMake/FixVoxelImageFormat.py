#!/usr/bin/env python3
"""Rewrite the voxel buffers' SPIR-V image format from Rgba32f to Rgba8.

Only needed on the glslc path. Defines.hlsl annotates the voxel buffers:

    #ifdef __spirv__
    #define VOXEL_IMAGE_FORMAT [[vk::image_format("rgba8")]]
    #endif

DXC defines __spirv__ and implements that attribute. glslang's HLSL front end
does neither, so under glslc the annotation vanishes and every `Buffer<float4>`
is emitted with SPIR-V's default sampled-image format, Rgba32f. The engine
stores one 32-bit RGBA8 texel per voxel and the buffer view says so, and a
shader that declares Rgba32f reads those 4 bytes as the first quarter of a
16-byte float4. Nothing errors: the voxel world simply decodes to garbage, so
the world and every model disappear and only the sky pass survives.

There is no glslc flag for this and no spirv-opt pass, so the format is
corrected in the binary directly. The rewrite is deliberately narrow - it only
touches OpTypeImage whose Dim is Buffer and whose format is Rgba32f, which
across all 24 shipped modules is exactly the VOXEL_BUFFER/VOXEL_RW_BUFFER
declarations and nothing else. Sampled 2D/3D images carry format Unknown and
are left alone.

Idempotent, so an incremental rebuild that reruns it is harmless.

Usage: FixVoxelImageFormat.py <module.spv>...
"""

import struct
import sys

SPIRV_MAGIC = 0x07230203
OP_TYPE_IMAGE = 25

# SPIR-V "Dim" operand.
DIM_BUFFER = 5

# SPIR-V "Image Format" operand.
FORMAT_RGBA32F = 1
FORMAT_RGBA8 = 4

# Word offsets within an OpTypeImage instruction.
WORD_DIM = 3
WORD_FORMAT = 8
MIN_WORD_COUNT = 9


def patch(path):
    with open(path, "rb") as handle:
        blob = handle.read()

    if len(blob) < 20:
        raise SystemExit("%s: too short to be a SPIR-V module" % path)

    magic = struct.unpack("<I", blob[:4])[0]

    if magic != SPIRV_MAGIC:
        # Big-endian SPIR-V is legal but nothing in this toolchain emits it,
        # and silently not patching would resurface as a black screen.
        raise SystemExit("%s: not a little-endian SPIR-V module" % path)

    words = list(struct.unpack("<%dI" % (len(blob) // 4), blob))

    index = 5  # Skip the five-word header.
    changed = 0

    while index < len(words):
        word_count = words[index] >> 16
        opcode = words[index] & 0xFFFF

        if word_count == 0:
            raise SystemExit("%s: zero-length instruction at word %d" % (path, index))

        if (
            opcode == OP_TYPE_IMAGE
            and word_count >= MIN_WORD_COUNT
            and words[index + WORD_DIM] == DIM_BUFFER
            and words[index + WORD_FORMAT] == FORMAT_RGBA32F
        ):
            words[index + WORD_FORMAT] = FORMAT_RGBA8
            changed += 1

        index += word_count

    if changed == 0:
        return 0

    with open(path, "wb") as handle:
        handle.write(struct.pack("<%dI" % len(words), *words))

    return changed


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)

    for path in argv[1:]:
        count = patch(path)

        if count:
            print("[shaders] %s: %d voxel buffer(s) Rgba32f -> Rgba8" % (path, count))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
