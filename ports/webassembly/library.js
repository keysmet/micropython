/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017, 2018 Rami Ali
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

mergeInto(LibraryManager.library, {
    // This string will be emitted directly into the output file by Emscripten.
    mp_js_ticks_ms__postset: "var MP_JS_EPOCH = Date.now()",

    mp_js_ticks_ms: () => Date.now() - MP_JS_EPOCH,

    // Returns 1 if a keyboard interrupt should be raised, 0 otherwise.
    // Callers in C (VM hook, mphalport.c) are responsible for calling
    // mp_sched_keyboard_interrupt() + mp_handle_pending() on a non-zero return.
    // No JS→C calls here to avoid ASYNCIFY re-entrancy issues.
    mp_js_hook: () => {
        if (!ENVIRONMENT_IS_NODE) {
            if (globalThis._mpInterruptRequest) {
                globalThis._mpInterruptRequest = false;
                return 1;
            }
            if (++globalThis._mpHookCount > 100000) {
                globalThis._mpHookCount = 0;
                return 1;
            }
            return 0;
        }
        if (ENVIRONMENT_IS_NODE) {
            const mp_interrupt_char = Module.ccall(
                "mp_hal_get_interrupt_char",
                "number",
                ["number"],
                ["null"],
            );
            const fs = require("fs");

            const buf = Buffer.alloc(1);
            try {
                const n = fs.readSync(process.stdin.fd, buf, 0, 1);
                if (n > 0) {
                    if (buf[0] === mp_interrupt_char) {
                        Module.ccall(
                            "mp_sched_keyboard_interrupt",
                            "null",
                            ["null"],
                            ["null"],
                        );
                    } else {
                        process.stdout.write(String.fromCharCode(buf[0]));
                    }
                }
            } catch (e) {
                if (e.code === "EAGAIN") {
                } else {
                    throw e;
                }
            }
        }
        return 0;
    },

    // Called from mphalport.c after each emscripten_sleep chunk.
    // Resets the watchdog counter so legitimate sleep() calls are never flagged.
    mp_js_extend_watchdog: () => {
        globalThis._mpHookCount = 0;
    },

    mp_js_time_ms: () => Date.now(),

    // Node prior to v19 did not expose "crypto" as a global, so make sure it exists.
    mp_js_random_u32__postset:
        "if (globalThis.crypto === undefined) { globalThis.crypto = require('crypto'); }",

    mp_js_random_u32: () =>
        globalThis.crypto.getRandomValues(new Uint32Array(1))[0],
});
