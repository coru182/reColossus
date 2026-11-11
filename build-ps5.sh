#!/usr/bin/env bash
# reColossus: builds the PS5 app (PPSA99071) into build-ps5/PPSA99071-<version>.zip, after ./setup.sh, from
#   work/celosia   Celosia with patches/ applied: PS2Recomp's runtime and IOP, the game's own C++ (game/overrides), and
#                  build/runner + build/vu1rec, the C++ recompiled from the player's own disc
#   ps5/           the PS5 host: the app's entry and first start, and the raylib subset the runtime draws, reads the
#                  controller and plays its sound through (VideoOut, ScePad, SceAudioOut)
#   package/       what the app carries besides the program: sce_sys (param.json, icon), licences, README.txt
#   deps/          zlib, and reUndr-ps5's PS5 toolchain (PayloadSDK, RADV, the packaging tool)
# Stages: ./build-ps5.sh [compile] [link] [package]  (all three when none is named). JOBS=<n> compiles n at a time.
# The link and the package are reKing's build-ps5.sh steps (from reUndr-ps5's tools/link.sh and package.py).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DEPS=$ROOT/deps
C=$ROOT/work/celosia
PKG=$ROOT/package
REUNDR=${REUNDR:-$DEPS/reUndr-ps5}
VK=$REUNDR/build/deps/PS5_Vulkan
SDK=$VK/.deps/native/ps5-payload-sdk
OUT=$ROOT/build-ps5
TITLE=PPSA99071
VERSION=0.1.4
JOBS=${JOBS:-$(nproc)}
export PATH=/usr/lib/llvm-18/bin:$PATH
STAGES=("$@"); [ ${#STAGES[@]} -gt 0 ] || STAGES=(compile link package)
has() { local s; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }

[ -d "$C/build/runner" ] && [ -f "$ROOT/work/game_hashes.inc" ] ||
    { echo "build: run ./setup.sh /path/to/SotC.iso first"; exit 1; }
mkdir -p "$OUT/gen"
# the toolchain file reads the SDK from the environment too: CMake's own test projects (check_ipo_supported) don't get
# the cache variable
export PS5_PAYLOAD_SDK=$SDK
TOOLCHAIN=(-DCMAKE_TOOLCHAIN_FILE="$REUNDR/ps5/cmake/ps5.cmake" -DPS5_PAYLOAD_SDK="$SDK"
           -DCMAKE_C_COMPILER=/usr/lib/llvm-18/bin/clang -DCMAKE_CXX_COMPILER=/usr/lib/llvm-18/bin/clang++)
# Zen 2; the code as reKing compiles the game's (-O2); exceptions and RTTI on (the PS5 target leaves RTTI off): the
# runtime's scheduler switches the game's threads by throwing
FLAGS="-O2 -DNDEBUG -march=znver2 -fno-strict-aliasing -ffunction-sections -fdata-sections -fexceptions -frtti"

if has compile; then
    cp "$ROOT/work/game_hashes.inc" "$OUT/gen/game_hashes.inc"
    # zlib (the runtime's scene recorder), once
    if [ ! -f "$OUT/zlib/libz.a" ]; then
        cmake -S "$DEPS/zlib" -B "$OUT/zlib" -G Ninja "${TOOLCHAIN[@]}" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -ffunction-sections -fdata-sections" -DZLIB_BUILD_EXAMPLES=OFF > /dev/null
        cmake --build "$OUT/zlib" --target zlibstatic -j "$JOBS"
    fi
    # PS2Recomp's runtime, IOP and the game (ps2EntryRunner), and the PS5 host: static libraries the link takes
    cmake -S "$C/PS2Recomp" -B "$OUT/cmake" -G Ninja "${TOOLCHAIN[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS_RELEASE="$FLAGS -I$OUT/gen -DRECOLOSSUS_VERSION_RAW=$VERSION" \
        -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -march=znver2 -ffunction-sections -fdata-sections" \
        -DPS2X_PS5=ON -DPS2X_PS5_DIR="$ROOT/ps5" \
        -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
        -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF \
        -DPS2X_ENABLE_IOP_RPC_TRACE=OFF -DPS2X_ENABLE_CCACHE=OFF -DPS2X_ENABLE_SCCACHE=OFF \
        "-DPS2X_RUNNER_DIRS=$C/build/runner;$C/build/vu1rec;$C/game/overrides" > "$OUT/configure.log" 2>&1 ||
        { tail -40 "$OUT/configure.log"; echo "compile: configure failed ($OUT/configure.log)"; exit 1; }
    # -k 0: every failing source in one run, so one round of fixes covers them all
    cmake --build "$OUT/cmake" --target ps2EntryRunner ps2x_ps5_host ps2_runtime ps2_iop -j "$JOBS" -- -k 0 2>&1 |
        tee "$OUT/compile.log" | grep -E "error|FAILED|^\[[0-9]+/[0-9]+\]" | awk 'NR % 50 == 1 || /error|FAILED/'
    [ "${PIPESTATUS[0]}" -eq 0 ] || { grep -E "error:" "$OUT/compile.log" | sort | uniq -c | sort -rn | head -80; echo "compile: failed ($OUT/compile.log)"; exit 1; }
    echo "compile: done"
fi

if has link; then
    # reUndr's link (as reKing links): RADV linked in whole, the payload SDK's libc++/libc++abi/libunwind, its system
    # module stubs, one export map
    work=$OUT/link; mkdir -p "$work/obj" "$work/stubs"
    archive=$VK/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a
    resource=$(clang-18 --print-resource-dir); rt="$REUNDR/build/deps/compiler-rt/libclang_rt.builtins-x86_64.a"
    if ! cmp -s "$rt" "$resource/lib/linux/libclang_rt.builtins-x86_64.a"; then
        { mkdir -p "$resource/lib/linux" && cp "$rt" "$resource/lib/linux/"; } 2>/dev/null ||
            sudo sh -c "mkdir -p '$resource/lib/linux' && cp '$rt' '$resource/lib/linux/'"
    fi
    for pair in 'libSceAgc agc_canary_link_stub.c' 'libSceAgcDriver agc_driver_canary_link_stub.c'; do
        read -r library source <<<"$pair"
        "$SDK/bin/prospero-clang" -std=c11 -O2 -fPIC -c "$VK/vendor/ps5/sdk/stubs/$source" -o "$work/obj/$library.o"
        "$SDK/bin/prospero-lld" --shared -soname "$library.prx" -o "$work/stubs/$library.so" "$work/obj/$library.o"
    done
    export LLVM_CONFIG=/usr/bin/llvm-config-18 PS5_CLANG=/usr/bin/clang-18
    source "$VK/tools/radv-link.sh"
    radv_link_recipe "$VK" "$SDK" "$archive"
    inputs=(); for item in "${radv_link_inputs[@]}"; do case "$item" in --start-group|--end-group) ;; *) inputs+=("$item") ;; esac; done
    flags=(); skip=0
    for item in "${radv_link_flags[@]}"; do
        if ((skip)); then skip=0; continue; fi
        if [[ $item == --version-script ]]; then skip=1; continue; fi
        flags+=("$item")
    done
    # libc.a's raw-syscall readlink, pathconf and fork share an object the link needs: bound to ps5/posix_ps5.c's
    for name in readlink pathconf fork; do flags+=("--defsym=$name=rc_$name"); done
    { printf '{local:\n_Zn*; _Zd*;\n'
      for item in "${flags[@]}"; do [[ $item == --defsym=* ]] && { n=${item#--defsym=}; printf '%s;\n' "${n%%=*}"; }; done
      printf '*;};\n'; } > "$work/exports.map"
    L=$OUT/cmake
    host=("$L/ps2xRuntime/libps2x_ps5_host.a" "$L/ps2xRuntime/libps2EntryRunner.a")
    libs=("$L/ps2xRuntime/libps2_runtime.a" "$L/ps2xIOP/libps2_iop.a" "$OUT/zlib/libz.a")
    for f in "${host[@]}" "${libs[@]}"; do [ -f "$f" ] || { echo "link: $f is missing (run the compile stage)"; exit 1; }; done
    stubs=(); for f in "$SDK"/target/lib/*.so; do case "$f" in *libScePosixForWebKit.so|*libSceGLSlimVSH.so|*libc_stub_weak.so) ;; *) stubs+=("$f") ;; esac; done
    # the game's code and the host whole (the overrides register themselves as the program starts); the hand-written
    # overrides replace recompiled functions of the same name, as on the Vita (--allow-multiple-definition); every
    # thread with a stack the recompiled code fits in (posix_ps5.c)
    "$SDK/bin/prospero-lld" "${radv_linker_script[@]}" "${flags[@]}" --version-script "$work/exports.map" \
        --exclude-libs=ALL --no-dynamic-linker --gc-sections --allow-multiple-definition --wrap=pthread_create --wrap=chdir \
        -e _start -o "$work/llvm-pie.elf" \
        "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
        --whole-archive "${host[@]}" --no-whole-archive \
        --start-group "${libs[@]}" "${inputs[@]}" "$SDK/target/lib/libc.a" "$SDK/target/lib/libpthread.a" --end-group \
        --as-needed "${stubs[@]}" > "$work/link.log" 2>&1 || true
    grep -E "error|undefined" "$work/link.log" | head -60 || true
    [ -s "$work/llvm-pie.elf" ] && [ "$work/llvm-pie.elf" -nt "$work/exports.map" ] || { echo "link: failed (see $work/link.log)"; exit 1; }
    # a PS5 title may not issue a syscall itself (SYSTEM_ILLEGAL_FUNCTION_CALL): none may be left in the program
    llvm-objdump-18 -d --no-show-raw-insn "$work/llvm-pie.elf" | awk '/^[0-9a-f]+ <.*>:$/ {fn=$2} /\tsyscall/ {print fn}' \
        | sort | uniq -c > "$work/raw_syscalls.txt"
    if [ -s "$work/raw_syscalls.txt" ]; then echo "link: raw syscalls left in:"; cat "$work/raw_syscalls.txt"; exit 1; fi
    echo "link: no raw syscalls"
    echo "link: $(du -h "$work/llvm-pie.elf" | cut -f1) $work/llvm-pie.elf"
    # the program's symbols and segments, to read the addresses of a crash on the console (the app logs where it
    # was loaded): names and addresses only, nothing of the game's code
    llvm-nm-18 -n -C --defined-only "$work/llvm-pie.elf" > "$OUT/symbols.txt"
    llvm-readelf-18 -lW "$work/llvm-pie.elf" > "$OUT/segments.txt"
    # what the program imports from the system's modules (one that doesn't exist there jumps nowhere when called)
    llvm-nm-18 -u "$work/llvm-pie.elf" | awk '{print $NF}' > "$work/imports.txt"
    { echo; echo "Imports:"; cat "$work/imports.txt"; } >> "$OUT/segments.txt"
fi

if has package; then
    # reUndr's tools/package.py steps: module info and imports, a signed fake SELF, the PS5 libc module, sce_sys
    work=$OUT/link; native=$REUNDR/build/ps5-native-tool; app=$OUT/app/$TITLE
    [ -x "$native" ] || { echo "package: the native tool is not built ($native)"; exit 1; }
    rm -rf "$app"; mkdir -p "$app/sce_module"
    llvm-objcopy-18 --strip-debug "$work/llvm-pie.elf" "$work/llvm-pie.stripped.elf"
    "$native" link --in "$work/llvm-pie.stripped.elf" --out "$work/eboot.elf" --stub-dir "$SDK/target/lib" \
        --stub "$work/stubs/libSceAgc.so" --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
        --companion-sdk 0x08050001 --file-name eboot.elf > "$work/convert.log" 2>&1
    "$native" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F > "$work/sign.log" 2>&1
    cp "$VK/runtime/libc.prx" "$app/sce_module/libc.prx"
    # every libc import must be in the PS5's libc module: one that isn't jumps to an unmapped address when called
    python3 "$ROOT/tools/check_imports.py" "$app/eboot.bin" "$app/sce_module/libc.prx" "$work/imports.txt"
    for f in "$app/eboot.bin" "$app/sce_module/libc.prx"; do
        "$native" self --inspect --file "$f" | grep -q "integrity: valid" || { echo "package: $f is not a valid fake SELF"; exit 1; }
    done
    cp -r "$PKG/sce_sys" "$app/sce_sys"
    for f in icon0.png pic0.dds pic1.dds; do if [ -f "$ROOT/art/$f" ]; then cp "$ROOT/art/$f" "$app/sce_sys/$f"; fi; done
    mkdir -p "$app/licenses" && cp "$PKG"/licenses/* "$app/licenses/"
    cp "$PKG/README.txt" "$app/README.txt"
    (cd "$OUT/app" && rm -f "../$TITLE-$VERSION.zip" && zip -qr "../$TITLE-$VERSION.zip" "$TITLE")
    echo "package: $OUT/$TITLE-$VERSION.zip"
fi
