#!/usr/bin/env bash
# reColossus: tests/vk_check.cpp built and run (Linux: clang-18, Mesa's llvmpipe and lavapipe, Vulkan validation layers;
# apt: mesa-vulkan-drivers libvulkan-dev vulkan-validationlayers glslang-tools libegl-dev libgl-dev).
#   tests/vk_check.sh <Celosia tree> <out dir> [frames]
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
C=${1:?usage: tests/vk_check.sh <Celosia tree> <out dir> [frames]}
OUT=${2:?usage: tests/vk_check.sh <Celosia tree> <out dir> [frames]}
R=$C/PS2Recomp/ps2xRuntime
mkdir -p "$OUT/obj"
"$ROOT/tools/spirv.sh" "$OUT/gen" > /dev/null
FLAGS=(-std=c++20 -O1 -g -I"$R/include" -I"$R/src/lib/native" -I"$C/PS2Recomp/ps2xIOP/include" -I"$OUT/gen")
for src in "$R/src/lib/native/native_renderer.cpp" "$R/src/lib/native/native_renderer_gl.cpp" "$ROOT/ps5/vk/native_renderer_vk.cpp" "$ROOT/tests/vk_check.cpp"; do
    clang++-18 "${FLAGS[@]}" -c "$src" -o "$OUT/obj/$(basename "$src" .cpp).o" &
done
wait
clang++-18 "$OUT"/obj/*.o -o "$OUT/vk_check" -lEGL -lOpenGL -ldl -lpthread
export VK_INSTANCE_LAYERS=${VK_INSTANCE_LAYERS-VK_LAYER_KHRONOS_validation}
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1)}
"$OUT/vk_check" "$OUT" "${3:-6}"
