#!/bin/bash
# Rebuild MicroPython WASM (ksm variant) and copy to ksm-mpy-web.
# Run from ports/webassembly/ in WSL: bash build-sim.sh

set -e

EMSDK="$(realpath ../../../emsdk)"
MPY_CROSS="$(realpath ../../mpy-cross)"
OUT="$(realpath ../../../ksm-mpy-web/public/mp)"

# Activate Emscripten
source "$EMSDK/emsdk_env.sh"

# Build mpy-cross if missing
if [ ! -f "$MPY_CROSS/mpy-cross" ]; then
    echo "Building mpy-cross..."
    make -C "$MPY_CROSS" -j$(nproc)
fi

# Build WASM
make VARIANT=ksm -j$(nproc)

# Patch micropython.mjs for JSPI compatibility (node available via emsdk env).
node patch-mjs-jspi.js

# Copy output
mkdir -p "$OUT"
cp build-ksm/micropython.mjs build-ksm/micropython.wasm "$OUT/"
echo "Done — WASM copied to $OUT"
