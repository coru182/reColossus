#!/usr/bin/env bash
# reColossus: the Vulkan renderer's shaders (ps5/vk/*.vert, *.frag) compiled to SPIR-V, as one C++ header the renderer
# includes (native_vk_spirv.h): glslangValidator (glslang-tools), Vulkan 1.0 SPIR-V.
#   tools/spirv.sh <out dir>
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:?usage: tools/spirv.sh <out dir>}
mkdir -p "$OUT/spirv"
command -v glslangValidator >/dev/null || { echo "spirv: glslangValidator is missing (apt install glslang-tools)"; exit 1; }
header=$OUT/native_vk_spirv.h
tmp=$header.tmp
echo "// written by tools/spirv.sh from ps5/vk; do not edit" > "$tmp"
echo "#pragma once" >> "$tmp"
echo "#include <cstdint>" >> "$tmp"
build() { # name source [defines...]
    local name=$1 src=$2; shift 2
    glslangValidator -V --target-env vulkan1.0 "$@" --vn "$name" -o "$OUT/spirv/$name.h" "$ROOT/ps5/vk/$src" > "$OUT/spirv/$name.log" ||
        { cat "$OUT/spirv/$name.log"; exit 1; }
    sed -e 's/^const uint32_t/static const uint32_t/' -e '/#pragma once/d' "$OUT/spirv/$name.h" >> "$tmp"
}
build kVkMainVert main.vert
build kVkMainVertSkinned main.vert -DSKINNED
build kVkMainFrag main.frag
build kVkQuadVert quad.vert
build kVkQuadFrag quad.frag
mv "$tmp" "$header"
echo "spirv: $header"
