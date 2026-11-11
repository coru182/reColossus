#!/usr/bin/env bash
# reColossus setup: the pinned dependencies and the PS5 toolchain, then the game's code recompiled from your own disc
# image by Celosia's pipeline (static recompilation with PS2Recomp, run on this Linux machine).
#
#   ./setup.sh /path/to/SotC.iso            your disc image (USA, SCUS-97472), or a .zip holding it; then ./build-ps5.sh
#   ./setup.sh --toolchain                  only the dependencies and the PS5 toolchain (no disc needed)
#   ./setup.sh --generated bundle.tar.gz    the game's code from a bundle an earlier run packed (tools/bundle.sh)
#
# deps/        Celosia (Shadow of the Colossus on PS2Recomp), in its Linux port (x86-64: upstream Celosia now builds
#              for ARM only: Apple silicon and the Vita), zlib, and reUndr-ps5 with its PS5 toolchain (PayloadSDK,
#              RADV built for the PS5, the packaging tool), as reKing pins them
# work/celosia Celosia with patches/ applied: its PS2Recomp tree builds both the Linux runner (for the pipeline) and the
#              PS5 runtime; build/runner and build/vu1rec hold the C++ recompiled from your disc
# work/game_hashes.inc  the hashes of the disc's boot ELF and modules, which the app checks the player's disc against
#
# The pipeline runs the game on this machine a few times (Celosia captures each code module as the game loads it, then
# records the VU1 microcode it uploads): it needs a display, so it runs under xvfb-run when there is none.
# REUNDR=/path/to/reUndr-ps5 uses a reUndr-ps5 checkout whose toolchain is already built (that build takes an hour or
# more and needs Docker).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DEPS=$ROOT/deps
WORK=$ROOT/work
CELOSIA_REV=43ea358a61a7e99553c7f15f442832ef89b7f870   # GenissonEmilio/ShadowOfTheColossusPC, 2026-10-31
REUNDR_REV=91de6198be34c956069e9b5d9ce00aed532a73ab    # as reKing 1.0.5 pins it
ZLIB_REV=51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf      # v1.3.1
JOBS=${JOBS:-$(nproc)}
export PATH=/usr/lib/llvm-18/bin:$PATH

MODE=${1:-}
case $MODE in
--toolchain) ;;
--generated) BUNDLE=${2:-}; [ -f "$BUNDLE" ] || { echo "usage: ./setup.sh --generated bundle.tar.gz"; exit 2; }
             BUNDLE=$(cd "$(dirname "$BUNDLE")" && pwd)/$(basename "$BUNDLE") ;;
"")          echo "usage: ./setup.sh /path/to/SotC.iso   (your own disc image of Shadow of the Colossus, PS2, USA)"
             echo "       ./setup.sh --toolchain          (the dependencies and the PS5 toolchain only)"
             echo "       ./setup.sh --generated b.tar.gz (the game's code from an earlier run's bundle)"
             exit 2 ;;
*)           [ -f "$MODE" ] || { echo "setup: $MODE not found"; exit 2; }
             ISO=$(cd "$(dirname "$MODE")" && pwd)/$(basename "$MODE") ;;
esac
for tool in git cmake ninja clang-18 clang++-18 ld.lld llvm-objcopy-18 llvm-objdump-18 python3 patch zip; do
    command -v "$tool" >/dev/null || { echo "setup: $tool is missing (README.md, Building)"; exit 1; }
done

# a repository at a pinned commit: name, url, commit
pin() {
    local dir=$DEPS/$1
    [ -d "$dir/.git" ] || git clone --filter=blob:none --no-checkout "$2" "$dir"
    git -C "$dir" checkout --quiet --detach "$3" 2>/dev/null ||
        { git -C "$dir" fetch --quiet origin && git -C "$dir" checkout --quiet --detach "$3"; }
}
mkdir -p "$DEPS"
pin celosia https://github.com/GenissonEmilio/ShadowOfTheColossusPC.git "$CELOSIA_REV"
pin zlib https://github.com/madler/zlib.git "$ZLIB_REV"

# reUndr-ps5's pinned PS5 toolchain, built by its own scripts, as reKing's setup.sh does
REUNDR=${REUNDR:-$DEPS/reUndr-ps5}
[ "$REUNDR" != "$DEPS/reUndr-ps5" ] || pin reUndr-ps5 https://github.com/bystritskiy/reUndr-ps5.git "$REUNDR_REV"
VK=$REUNDR/build/deps/PS5_Vulkan
if [ ! -f "$VK/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" ] || [ ! -f "$VK/runtime/libc.prx" ] ||
   [ ! -f "$REUNDR/build/deps/compiler-rt/libclang_rt.builtins-x86_64.a" ]; then
    command -v docker >/dev/null || { echo "setup: building reUndr-ps5's toolchain needs Docker"; exit 1; }
    (cd "$REUNDR" && python3 tools/rebuild-deps.py --llvm-bin /usr/lib/llvm-18/bin --jobs "$JOBS")
fi
if [ ! -x "$REUNDR/build/ps5-native-tool" ]; then
    # reUndr's script names macOS's Homebrew clang: the same script, with this machine's clang++-18
    mkdir -p "$REUNDR/build"
    sed "s#/opt/homebrew/opt/llvm@18/bin/clang++#$(command -v clang++-18)#" "$REUNDR/tools/build-native-tool.py" \
        > "$REUNDR/build/build-native-tool-linux.py"
    (cd "$REUNDR" && python3 build/build-native-tool-linux.py)
fi
if [ "$MODE" = --toolchain ]; then
    echo "setup: the toolchain is ready"
    exit 0
fi

# the game's tree: Celosia with this port's changes (the PS5 build of its runtime)
C=$WORK/celosia
rm -rf "$C"
mkdir -p "$C"
git -C "$DEPS/celosia" archive --format=tar HEAD | tar -x -C "$C"
for p in "$ROOT"/patches/celosia-*.patch; do patch -s -d "$C" -p1 < "$p"; done
chmod +x "$C/celosia"

if [ "$MODE" = --generated ]; then
    tar -xzf "$BUNDLE" -C "$WORK"
    [ -d "$C/build/runner" ] && [ -f "$WORK/game_hashes.inc" ] || { echo "setup: $BUNDLE is not a reColossus bundle"; exit 1; }
    echo "setup: the game's code from $(basename "$BUNDLE"); now ./build-ps5.sh"
    exit 0
fi

# Celosia's pipeline on Linux: the disc extracted into work/celosia/disc, the boot ELF and each module captured and
# recompiled, the Linux runner built, and the VU1 microcode recorded and compiled. The flags are the ones the Linux
# port builds with; no FFmpeg, no debug UI.
for tool in bsdtar unzip pkg-config; do
    command -v "$tool" >/dev/null || { echo "setup: $tool is missing (README.md, Building)"; exit 1; }
done
export CELOSIA_JOBS=$JOBS
export CELOSIA_CMAKE_ARGS="-DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_CXX_FLAGS=-msse4.1
 -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_CCACHE=OFF
 -DPS2X_ENABLE_SCCACHE=OFF"
run=()
if [ -z "${DISPLAY:-}" ]; then
    command -v xvfb-run >/dev/null || { echo "setup: no display: install xvfb (README.md, Building)"; exit 1; }
    run=(xvfb-run -a -s "-screen 0 1280x720x24")
fi
# the scripted run that records the VU1 microcode: longer than Celosia's 40 s (a CI machine draws slower), pressing
# Cross on the title screen now and then so that a new game starts
export PS2X_INPUT_SCRIPT=${PS2X_INPUT_SCRIPT:-"20:DOWN:0.3;22:CROSS:0.3;30:CROSS:0.3;40:CROSS:0.3;60:CROSS:0.3;80:CROSS:0.3"}
sed -i 's/cmd_autotest 40 >/cmd_autotest ${CELOSIA_VU1_SECONDS:-150} >/' "$C/celosia"
# the emulated GS, so that the game builds its PS2 display lists and runs its VU1 microcode: with the Linux runner's
# default native renderer the VU1 never runs and nothing is recorded (0.1.0 to 0.1.4 had no compiled microcode). The
# patched runner leaves the native renderer off with this.
export RECOLOSSUS_EMULATED_GS=1
(cd "$C" && "${run[@]}" ./celosia setup --iso "$ISO")
# the VU1 microcode the scripted run recorded, and what the run printed about it
dumps=$(find "$C/build/vu1" -maxdepth 1 -name '*.bin' 2>/dev/null | wc -l || true)
compiled=$(find "$C/build/vu1rec" -maxdepth 1 -type f 2>/dev/null | wc -l || true)
echo "setup: VU1 microcode: $dumps image(s) recorded, $compiled file(s) of recompiled code"
if [ -f "$C/logs/autotest.log" ]; then
    echo "setup: the scripted run's VU1 lines (last ones):"
    grep -E "vu1|VU1" "$C/logs/autotest.log" | tail -40 || true
fi
if [ "$compiled" -eq 0 ]; then
    echo "setup: no VU1 microcode was recompiled (the scripted run recorded none)"
    exit 1
fi
python3 "$ROOT/tools/game_hashes.py" "$C/disc" "$WORK/game_hashes.inc"
echo "setup: done; now ./build-ps5.sh"
