module("_boot.py", base_path="$(PORT_DIR)/modules/scripts", opt=3)
module("main.py", base_path="$(PORT_DIR)/freeze", opt=3)
include("$(MPY_DIR)/extmod/asyncio")
require("neopixel")
