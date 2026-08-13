/*
 * KSM1 board early init and VM hook
 *
 * Sets REGOUT0 to 3.3V on first boot (required for SK6812 LEDs).
 * Shutdown/wake logic is handled in freeze/main.py (runs after MicroPython
 * is fully initialized, so I2C works reliably there).
 *
 * KSM1_vm_hook() is called by MICROPY_VM_HOOK_LOOP every 200 bytecodes.
 * It processes pending USB CDC data (so Ctrl+C interrupts tight Python loops)
 * and owns the power-off gesture: a 2s MENU hold, detected here so it works
 * even inside a hung Python loop that never cooperates with the scanner.
 *
 * GPREGRET is a one-reset intent latch (survives reset + System OFF wake, and
 * the Adafruit bootloader passes it through unless it is the 0x57 UF2 magic):
 *   0xA1 RUN_USER  — main.py runs /eeprom/app.py once, then clears the flag
 *   0xA2 POWER_OFF — main.py enters System OFF, then clears the flag
 *   0x57 BOOTLOADER — reserved Adafruit UF2 magic; never write it here
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

#define KSM1_MENU_PORT1_BIT     (10u)     // MENU = P1.10
#define KSM1_HOLD_MS_POWER_OFF  (2000u)
#define KSM1_GPREGRET_POWER_OFF (0xA2u)

// MENU reads low when pressed (active-low, PULL_UP).
static inline bool ksm1_menu_down(void) {
    return (NRF_P1->IN & (1u << KSM1_MENU_PORT1_BIT)) == 0;
}

void KSM1_vm_hook(void) {
    static uint32_t hold_start_ms = 0;
    static bool was_down = false;

    // 1. Drain any USB CDC data that didn't fit in the ring buffer when it arrived.
    //    tud_cdc_rx_cb already called mp_sched_keyboard_interrupt() for Ctrl+C bytes;
    //    this just ensures nothing is stranded if the buffer was temporarily full.
    mp_usbd_cdc_poll_interfaces(0);

    // 2. Power-off gesture: MENU held 2s. Runs from the VM hook so it works even
    //    when user Python is stuck in a tight loop. On trigger, latch POWER_OFF
    //    and reset — but only AFTER MENU is released, so main.py never boots the
    //    power-off path with MENU still down (that double-edge stuck the board on
    //    the earlier attempt). This is the single owner of power-off.
    if (ksm1_menu_down()) {
        uint32_t now = mp_hal_ticks_ms();
        if (!was_down) {
            was_down = true;
            hold_start_ms = now;
        } else if ((uint32_t)(now - hold_start_ms) >= KSM1_HOLD_MS_POWER_OFF) {
            NRF_POWER->GPREGRET = KSM1_GPREGRET_POWER_OFF;
            while (ksm1_menu_down()) {
                // spin until released, then reset into the power-off boot
            }
            __disable_irq();
            NVIC_SystemReset();
        }
    } else {
        was_down = false;
    }

    // 3. Process the pending KeyboardInterrupt (or any other scheduled event).
    //    This is what actually raises the exception inside the Python VM.
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
