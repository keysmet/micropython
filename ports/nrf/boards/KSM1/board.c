/*
 * KSM1 board early init and VM hook
 *
 * Sets REGOUT0 to 3.3V on first boot (required for SK6812 LEDs).
 * Shutdown/wake logic is handled in freeze/main.py (runs after MicroPython
 * is fully initialized, so I2C works reliably there).
 *
 * KSM1_vm_hook() is called by MICROPY_VM_HOOK_LOOP every 200 bytecodes.
 * It processes pending USB CDC data (so Ctrl+C interrupts tight Python loops)
 * and detects a 2s MENU hold to trigger the board power-off flow via GPREGRET.
 */

#include "nrf.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "shared/tinyusb/mp_usbd_cdc.h"
#if MICROPY_HW_USB_HID
#ifndef NO_QSTR
#include "tusb.h"
#endif
#endif

#define KSM1_MENU_PORT1_BIT       (10u)   // P1.10
#define KSM1_HOLD_MS_POWER_OFF    (2000u)
#define KSM1_GPREGRET_POWER_OFF   (0xA2u)

void KSM1_vm_hook(void) {
    static uint32_t menu_hold_start_ms = 0;
    static bool menu_was_down = false;

    // 1. Drain any USB CDC data that didn't fit in the ring buffer when it arrived.
    //    tud_cdc_rx_cb already called mp_sched_keyboard_interrupt() for Ctrl+C bytes;
    //    this just ensures nothing is stranded if the buffer was temporarily full.
    mp_usbd_cdc_poll_interfaces(0);

    // 2. Emergency power-off path: MENU held for 2s forces System OFF flow.
    //    This runs from the VM hook so it still works for tight Python loops.
    bool menu_down = ((NRF_P1->IN & (1u << KSM1_MENU_PORT1_BIT)) == 0);
    if (menu_down) {
        uint32_t now = mp_hal_ticks_ms();
        if (!menu_was_down) {
            menu_was_down = true;
            menu_hold_start_ms = now;
        } else if ((uint32_t)(now - menu_hold_start_ms) >= KSM1_HOLD_MS_POWER_OFF) {
            __disable_irq();
            NRF_POWER->GPREGRET = KSM1_GPREGRET_POWER_OFF;
            NVIC_SystemReset();
        }
    } else {
        menu_was_down = false;
    }

    // 3. Process the pending KeyboardInterrupt (or any other scheduled event).
    //    This is what actually raises the exception inside the Python VM.
    //    This keeps Ctrl+C and scheduler-driven events responsive.
    mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
}

void KSM1_board_enter_bootloader(void) {
    // Signal the Adafruit bootloader to enter UF2 mode on next reset.
    // 0x57 = DFU_MAGIC_UF2_RESET — checked by the bootloader in GPREGRET.
    __disable_irq();
    NRF_POWER->GPREGRET = 0x57;
    NVIC_SystemReset();
}

#if MICROPY_HW_USB_HID
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}
#endif

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

    // Write-protect the entire 1MB code flash via ACL.
    // ACL is cleared on system reset, so the Adafruit bootloader (which runs
    // before MicroPython on every reset) retains full write access for DFU/UF2.
    // Prevents Python-level writes via machine.mem32 or NVMC from corrupting
    // the SoftDevice, firmware, or ROMFS.
    NRF_ACL->ACL[0].ADDR = 0x00000000;
    NRF_ACL->ACL[0].SIZE = 0x00100000;    // 1MB — full code flash
    NRF_ACL->ACL[0].PERM = (1UL << 2);   // WRITE_Disable, READ_Allow
}
