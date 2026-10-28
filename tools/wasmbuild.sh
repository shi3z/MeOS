#!/bin/sh
# Builds a MeOS WebAssembly app:   tools/wasmbuild.sh apps/wasm/hello.c [out.wasm]
# NAME.manifest next to NAME.c is added as the custom section "meos.manifest" (format: kernel/include/m5os/wasm.h).
# Needs LLVM with the wasm32 target, wasm-ld and llvm-objcopy (brew install llvm lld).
set -e
SRC=$1; OUT=${2:-${SRC%.c}.wasm}; MAN=${SRC%.c}.manifest
LLVM=${LLVM_BIN:-$(brew --prefix llvm 2>/dev/null)/bin}
[ -f "$MAN" ] || { echo "missing $MAN"; exit 1; }
"$LLVM/clang" --target=wasm32 -nostdlib -O2 -ffreestanding -Wall -Wextra -Wno-unused-function \
    -Wl,--no-entry -Wl,--export=_start -Wl,-z,stack-size=8192 -Wl,--allow-undefined -Wl,--initial-memory=65536 -Wl,--max-memory=16777216 \
    -o "$OUT" "$SRC"
"$LLVM/llvm-objcopy" --add-section "meos.manifest=$MAN" "$OUT"
ls -l "$OUT"
