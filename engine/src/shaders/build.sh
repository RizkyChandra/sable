#!/bin/sh
# Regenerates the SPIR-V that engine/src/gpu.cpp embeds.
#
# The output is committed rather than built. SABLE_GPU is on whenever SDL3 is
# present, and requiring a shader compiler as well would mean the application
# CI job — which has SDL3 and no glslc — could no longer build the GPU backend
# at all. Run this by hand when a .comp or sable.glsl changes, and commit the
# .spv.inc beside it.
set -eu
cd "$(dirname "$0")"
for shader in composite dab; do
    glslc -O --target-env=vulkan1.0 -mfmt=c -o "$shader.spv.inc" "$shader.comp"
    echo "$shader.spv.inc: $(wc -c < "$shader.spv.inc") bytes"
done
