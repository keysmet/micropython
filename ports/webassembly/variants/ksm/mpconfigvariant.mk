include $(VARIANT_DIR)/../standard/mpconfigvariant.mk
FROZEN_MANIFEST := $(VARIANT_DIR)/manifest.py

# Native `audio` module — shares the sfxr synthesizer and dict parser with the
# nRF firmware (ports/nrf/drivers/sfxr) so the browser preview matches hardware.
# Sources are given relative to $(TOP) so the vpath/build rules resolve them and
# emit objects under $(BUILD)/ports/nrf/drivers/sfxr/.
INC += -I$(TOP)/ports/nrf/drivers/sfxr
SRC_C += \
	modaudio.c \
	ports/nrf/drivers/sfxr/sfxr.c \
	ports/nrf/drivers/sfxr/sfxr_mp.c \
