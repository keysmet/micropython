#!/bin/bash
# Rebuild MicroPython WASM (standard variant) and copy to ksm-launch.
# Run from ports/webassembly/: bash build-sim.sh

set -e
make VARIANT=standard
cp build-standard/micropython.mjs build-standard/micropython.wasm ../../../ksm-launch/public/mp/
echo "Done — WASM copied to ksm-launch/public/mp/"
