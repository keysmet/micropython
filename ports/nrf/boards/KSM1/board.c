/*
 * KSM1 board early init, VM hook, and hang watchdog
 *
 * Sets REGOUT0 to 3.3V on first boot (required for SK6812 LEDs).
 * All boot/shutdown/wake logic lives in freeze/main.py (it runs after MicroPython
 * is fully initialized, so I2C to the EEPROM works). Boot intent is stored in an
 * EEPROM byte, not GPREGRET — reading NRF_POWER->GPREGRET via machine.mem32
 * hard-faults on this board.
 *
 * KSM1_vm_hook() is called by MICROPY_VM_HOOK_LOOP every 200 bytecodes. It drains
 * USB CDC (so Ctrl+C interrupts tight loops), runs pending callbacks, and runs the
 * hang watchdog. There is no background key-scan timer: keys are scanned inside
 * ksm.tick() (the single cooperative yield point). A well-behaved script calls
 * tick() regularly; each call feeds the watchdog (KSM1_watchdog_feed, wired from
 * the `board` module — see modksm.c / freeze/ksm.py). If tick() is not called for
 * KSM1_WATCHDOG_MS, the script is stuck in a non-yielding loop: we write the MENU
 * boot intent to the EEPROM and reset, so the board recovers into MENU mode
 * instead of hanging until the battery dies.
 *
 * Writing the EEPROM from here is safe: the VM hook runs *between* bytecodes on the
 * main thread (it is not an interrupt), so the I2C bus is idle — unlike the old
 * key-scan Timer ISR, which is why intent-latching was kept out of it before.
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

// ── Hang watchdog ────────────────────────────────────────────────────────────
// Must match freeze/main.py: EEPROM at 0x50, intent byte at address 0x0000,
// INTENT_MENU = 0xA2, and the EEPROM lives on I2C(0).
#define KSM1_WATCHDOG_MS        (1000u)
#define KSM1_EEPROM_I2C_ID      (0)
#define KSM1_EEPROM_ADDR        (0x50)
#define KSM1_INTENT_ADDR        (0x0000)
#define KSM1_INTENT_MENU        (0xA2)

// Blocking raw I2C TX on a configured TWI instance (defined in modules/machine/i2c.c).
extern int machine_hard_i2c_raw_tx(int id, uint16_t addr, const uint8_t *buf, size_t len);

// Last tick() timestamp, or 0 when the watchdog is disarmed. The watchdog only
// guards USER mode (where an untrusted script can hang): main.py arms it right
// before the user loop and disarms it on the way out. MENU mode / boot / the REPL
// leave it disarmed, so they can sit without ticking and never reboot.
static uint32_t ksm1_last_feed_ms = 0;

static uint32_t ksm1_now_nonzero(void) {
    uint32_t now = mp_hal_ticks_ms();
    return now ? now : 1;   // 0 is the "disarmed" sentinel
}

// Arm the watchdog (main.py, entering USER mode).
void KSM1_watchdog_arm(void) {
    ksm1_last_feed_ms = ksm1_now_nonzero();
}

// Refresh the deadline — but only while armed. ksm.tick() calls this every yield;
// in MENU mode (disarmed) it is a no-op, so MENU ticks never arm the watchdog.
void KSM1_watchdog_feed(void) {
    if (ksm1_last_feed_ms != 0) {
        ksm1_last_feed_ms = ksm1_now_nonzero();
    }
}

// Disarm (main.py, leaving the user loop — normal exit, or Ctrl+C to the REPL for
// a USB upload — and before the power-off / System OFF sequence stops ticking).
void KSM1_watchdog_disarm(void) {
    ksm1_last_feed_ms = 0;
}

static void ksm1_watchdog_reboot_to_menu(void) {
    // Latch MENU intent so the reboot lands in MENU mode instead of re-running the
    // (hung) script and hanging again. main.py reads and clears this byte at boot.
    uint8_t frame[3] = { KSM1_INTENT_ADDR >> 8, KSM1_INTENT_ADDR & 0xFF, KSM1_INTENT_MENU };
    machine_hard_i2c_raw_tx(KSM1_EEPROM_I2C_ID, KSM1_EEPROM_ADDR, frame, sizeof(frame));
    mp_hal_delay_ms(10);   // EEPROM write cycle must complete before we reset
    NVIC_SystemReset();
}

void KSM1_vm_hook(void) {
    // Drain any USB CDC data that didn't fit in the ring buffer when it arrived.
    // tud_cdc_rx_cb already called mp_sched_keyboard_interrupt() for Ctrl+C bytes;
    // this just ensures nothing is stranded if the buffer was temporarily full.
    mp_usbd_cdc_poll_interfaces(0);

    // Process the pending KeyboardInterrupt (or any other scheduled event) — this
    // is what actually raises the exception inside the Python VM.
    mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);

    // Hang watchdog: while armed (USER mode), if ksm.tick() hasn't refreshed the
    // deadline for KSM1_WATCHDOG_MS the script is stuck in a non-yielding loop →
    // reboot into MENU. Disarmed (== 0) in MENU/boot/REPL, so those never trip it.
    if (ksm1_last_feed_ms != 0 &&
        (uint32_t)(mp_hal_ticks_ms() - ksm1_last_feed_ms) > KSM1_WATCHDOG_MS) {
        ksm1_watchdog_reboot_to_menu();
    }
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
