include("$(PORT_DIR)/variants/manifest.py")

# Hardware stubs: make the nRF ksm.py run in WASM without modification.
module("neopixel.py",     base_path="$(PORT_DIR)/variants/ksm/stubs", opt=3)
module("_ksm_patches.py", base_path="$(PORT_DIR)/variants/ksm/stubs", opt=3)

# nRF sources — single source of truth shared with the firmware build.
module("pins.py", base_path="$(PORT_DIR)/../nrf/freeze", opt=3)
module("ksm.py",  base_path="$(PORT_DIR)/../nrf/freeze", opt=3)
