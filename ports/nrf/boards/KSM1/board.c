/*
 * KSM1 board early init and VM hook
 *
 * Sets REGOUT0 to 3.3V on first boot (required for SK6812 LEDs).
 * Shutdown/wake logic is handled in freeze/main.py (runs after MicroPython
 * is fully initialized, so I2C works reliably there).
 *
 * KSM1_vm_hook() is called by MICROPY_VM_HOOK_LOOP every 200 bytecodes.
 * It processes pending USB CDC data (so Ctrl+C interrupts tight Python loops)
 * and detects a 2s MENU hold to force System OFF without going through Python.
 */

#include "nrf.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "shared/tinyusb/mp_usbd_cdc.h"

// P1.10 (MENU key, active low) and P0.07 (external power rail)
#define KSM1_MENU_PORT_BIT  (1UL << 10)
#define KSM1_PWR_ON_BIT     (1UL << 7)
// PIN_CNF value: input, pull-up, sense on low level — wakes from System OFF
#define KSM1_PIN_CNF_SENSE_LOW  (0x0003000CUL)

void KSM1_vm_hook(void) {
    // 1. Drain any USB CDC data that didn't fit in the ring buffer when it arrived.
    //    tud_cdc_rx_cb already called mp_sched_keyboard_interrupt() for Ctrl+C bytes;
    //    this just ensures nothing is stranded if the buffer was temporarily full.
    mp_usbd_cdc_poll_interfaces(0);

    // 2. Process the pending KeyboardInterrupt (or any other scheduled event).
    //    This is what actually raises the exception inside the Python VM.
    //    Timer callbacks (including the ksm key scanner) also run here, which is
    //    how MENU-hold sleep detection works even inside infinite loops.
    mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
}

void KSM1_board_enter_bootloader(void) {
    // Signal the Adafruit bootloader to enter UF2 mode on next reset.
    // 0x57 = DFU_MAGIC_UF2_RESET — checked by the bootloader in GPREGRET.
    __disable_irq();
    NRF_POWER->GPREGRET = 0x57;
    NVIC_SystemReset();
}

void KSM1_board_early_init(void) {
    // MDBT50Q runs in High Voltage mode (VDDH). UICR.REGOUT0 controls GPIO
    // output voltage and defaults to 1.8V (erased flash = 0xFFFFFFFF).
    // SK6812 LEDs on 2.0.5 run at ~5V so VIH ~3.25V — 1.8V GPIO can't drive them.
    // Set REGOUT0 to 3.3V on first boot and reset. UICR is flash, so this is permanent.
    if ((NRF_UICR->REGOUT0 & 0x7) != 5) { // 5 = 3.3V
        NRF_NVMC->CONFIG = 1;              // enable flash write
        NRF_UICR->REGOUT0 = (NRF_UICR->REGOUT0 & ~0x7UL) | 5;
        NRF_NVMC->CONFIG = 0;              // disable flash write
        NVIC_SystemReset();                // REGOUT0 change requires reset
    }
}
