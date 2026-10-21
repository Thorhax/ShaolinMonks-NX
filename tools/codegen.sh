#!/bin/bash
# codegen.sh <default.xbe> [out]: builds the recompiler for this machine and translates the game's XBE to C++
# out defaults to build/generated/shaolin. The XBE must be the USA/Europe release upstream supports.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
XBE=$(realpath "${1:?usage: codegen.sh <default.xbe> [out]}")
OUT=${2:-$ROOT/build/generated/shaolin}
UP=$ROOT/third_party/ShaolinMonksRecomp
SDK=$ROOT/third_party/XboxXBERecomp
want=48240902bf4a04eb89376217c529531032d3370749bcce91a194878cc49db74a
have=$(sha256sum "$XBE" | cut -d' ' -f1)
[ "$have" = "$want" ] || { echo "unknown XBE ($have): only the USA/Europe release is supported" >&2; exit 1; }
cmake -S "$ROOT/tools/host" -B "$ROOT/build/host" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build/host" --target xbe_recomp
mkdir -p "$OUT"
# the "unimplemented" lines are data the analysis decodes as code, the same on every host
"$ROOT/build/host/xbe_recomp" "$UP/config/game.toml" "$XBE" "$OUT" --xdk "$SDK/xdk" > "$ROOT/build/codegen.log" 2>&1
echo "generated $(ls "$OUT"/x86_recomp.*.cpp | wc -l) partitions in $OUT"
# the executable checks the game it finds is the release it was made from
printf 'extern const char kImageSha1[] = "%s";\n' "$(sha1sum "$XBE" | cut -d' ' -f1)" > "$OUT/xbe_sha1.cpp"
