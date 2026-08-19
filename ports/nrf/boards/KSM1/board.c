/*
 * KSM1 board early init and VM hook
 *
 * Sets REGOUT0 to 3.3V on first boot (required for SK6812 LEDs).
 * All boot/shutdown/wake logic lives in freeze/main.py (it runs after MicroPython
 * is fully initialized, so I2C to the EEPROM works). Boot intent is stored in an
 * EEPROM byte, not GPREGRET — reading NRF_POWER->GPREGRET via machine.mem32
 * hard-faults on this board.
 *
 * KSM1_vm_hook() is called by MICROPY_VM_HOOK_LOOP every 200 bytecodes; it only
 * drains USB CDC (so Ctrl+C interrupts tight loops) and runs pending callbacks.
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

void KSM1_vm_hook(void) {
    // Drain any USB CDC data that didn't fit in the ring buffer when it arrived.
    // tud_cdc_rx_cb already called mp_sched_keyboard_interrupt() for Ctrl+C bytes;
    // this just ensures nothing is stranded if the buffer was temporarily full.
    mp_usbd_cdc_poll_interfaces(0);

    // Process the pending KeyboardInterrupt (or any other scheduled event) — this
    // is what actually raises the exception inside the Python VM, and runs the
    // ksm key-scan timer callback.
    mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);

    // Note: power-off (2s MENU hold) is owned by Python (ksm.py's scanner latches
    // it, main.py runs the shutdown), because latching the boot intent needs I2C
    // to the EEPROM — impossible from here. See freeze/main.py.
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
