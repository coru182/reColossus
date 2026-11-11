#!/usr/bin/env bash
# reColossus: the game's code recompiled by ./setup.sh, as one file, so that a later build can skip the pipeline
# (./setup.sh --generated <file>). It holds code made from the player's disc: keep it private (the CI keeps it in a
# release of the private repository that holds the disc image, never in this repository or its caches).
#
#   tools/bundle.sh key           the bundle's key: what the recompiled code depends on (Celosia's commit and the
#                                 pipeline's GENERATION; patches/ only change the PS5 build, not the desktop runner)
#   tools/bundle.sh pack <file>   work/celosia/build/{runner,vu1rec} and work/game_hashes.inc into <file> (.tar.gz)
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
case ${1:-} in
key)
    rev=$(sed -n 's/^CELOSIA_REV=\([0-9a-f]*\).*/\1/p' "$ROOT/setup.sh")
    gen=$(sed -n 's/^GENERATION=\([0-9]*\).*/\1/p' "$ROOT/tools/bundle.sh")
    echo "${rev:0:12}-g$gen"
    ;;
pack)
    out=${2:?usage: tools/bundle.sh pack <file>}
    [ -d "$ROOT/work/celosia/build/runner" ] && [ -f "$ROOT/work/game_hashes.inc" ] || { echo "bundle: run ./setup.sh first"; exit 1; }
    tar -czf "$out" -C "$ROOT/work" celosia/build/runner celosia/build/vu1rec game_hashes.inc
    echo "bundle: $out ($(du -h "$out" | cut -f1))"
    ;;
*)
    echo "usage: tools/bundle.sh key | pack <file>"
    exit 2
    ;;
esac
# Raised when the pipeline itself changes (setup.sh's part after "Celosia's pipeline", or a patch that changes the
# Linux runner), so older bundles are not used.
GENERATION=2
