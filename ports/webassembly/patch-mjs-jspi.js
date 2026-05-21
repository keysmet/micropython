// Post-build patch: make micropython.mjs compatible with JSPI.
// Run from ports/webassembly/ after building the standard variant.
const fs = require('fs');
const path = require('path');

const mjsPath = path.join(__dirname, 'build-standard/micropython.mjs');
let mjs = fs.readFileSync(mjsPath, 'utf8');

// 1. Remove .isAsync from imports that don't need to suspend.
//    Only emscripten_sleep stays Suspending (actual sleep mechanism).
const ASYNC_IMPORTS = ['___syscall_poll', '_fd_sync'];
for (const name of ASYNC_IMPORTS) {
    const pat = `${name}.isAsync = true;`;
    if (!mjs.includes(pat)) {
        console.error(`PATCH FAILED: ${pat} not found in micropython.mjs`);
        process.exit(1);
    }
    mjs = mjs.replace(pat, `${name}.isAsync = false;`);
}

// 2. Replace emscripten_scan_registers with a no-op.
//    The asyncify implementation reads Asyncify.currData which is invalid in JSPI
//    mode (WASM never suspends via asyncify, so currData is stale/null).
const OLD_SCAN = `  var _emscripten_scan_registers = (func) => {
      return Asyncify.handleSleep((wakeUp) => {
        // We must first unwind, so things are spilled to the stack. Then while
        // we are pausing we do the actual scan. After that we can resume. Note
        // how using a timeout here avoids unbounded call stack growth, which
        // could happen if we tried to scan the stack immediately after unwinding.
        safeSetTimeout(() => {
          var stackBegin = Asyncify.currData + 12;
          var stackEnd = HEAPU32[((Asyncify.currData)>>2)];
          getWasmTableEntry(func)(stackBegin, stackEnd);
          wakeUp();
        }, 0);
      });
    };
  _emscripten_scan_registers.isAsync = true;`;
const NEW_SCAN = `  var _emscripten_scan_registers = (func) => {};
  _emscripten_scan_registers.isAsync = false;`;
if (!mjs.includes(OLD_SCAN)) {
    console.error('PATCH FAILED: emscripten_scan_registers pattern not found in micropython.mjs');
    process.exit(1);
}
mjs = mjs.replace(OLD_SCAN, NEW_SCAN);

// 3. Make pyimport() and runPythonAsync() work via WebAssembly.promising().
//    Both call WASM functions that use emscripten_sleep, which requires a JSPI
//    fiber context. Module.ccall({ async: true }) uses the asyncify path which
//    returns undefined instead of a Promise in JSPI mode.
const OLD_PYIMPORT = `    proxy_js_init();
    const pyimport = (name) => {
        const value = Module._malloc(3 * 4);
        Module.ccall(
            "mp_js_do_import",
            "null",
            ["string", "pointer"],
            [name, value],
        );
        return proxy_convert_mp_to_js_obj_jsside_with_free(value);
    };`;

const NEW_PYIMPORT = `    proxy_js_init();
    const _asyncDoImport = WebAssembly.promising(Module.wasmExports['mp_js_do_import']);
    const _asyncDoExec = WebAssembly.promising(Module.wasmExports['mp_js_do_exec_async']);
    const pyimport = async (name) => {
        const value = Module._malloc(3 * 4);
        const nameLen = Module.lengthBytesUTF8(name);
        const nameBuf = Module._malloc(nameLen + 1);
        Module.stringToUTF8(name, nameBuf, nameLen + 1);
        await _asyncDoImport(nameBuf, value);
        Module._free(nameBuf);
        return proxy_convert_mp_to_js_obj_jsside_with_free(value);
    };`;

if (!mjs.includes(OLD_PYIMPORT)) {
    console.error('PATCH FAILED: pyimport pattern not found in micropython.mjs');
    process.exit(1);
}
mjs = mjs.replace(OLD_PYIMPORT, NEW_PYIMPORT);

// 4. Replace ccall({ async: true }) in runPythonAsync with _asyncDoExec.
const OLD_EXEC = `            await Module.ccall(
                "mp_js_do_exec_async",
                "number",
                ["pointer", "number", "pointer"],
                [buf, len, value],
                { async: true },
            );`;
const NEW_EXEC = `            await _asyncDoExec(buf, len, value);`;
if (!mjs.includes(OLD_EXEC)) {
    console.error('PATCH FAILED: runPythonAsync ccall pattern not found in micropython.mjs');
    process.exit(1);
}
mjs = mjs.replace(OLD_EXEC, NEW_EXEC);

// 5. Await pyimport('__main__') in the return object.
const OLD_DICT = `            __dict__: pyimport("__main__").__dict__,`;
const NEW_DICT = `            __dict__: (await pyimport("__main__")).__dict__,`;
if (!mjs.includes(OLD_DICT)) {
    console.error('PATCH FAILED: __main__ pyimport pattern not found');
    process.exit(1);
}
mjs = mjs.replace(OLD_DICT, NEW_DICT);

fs.writeFileSync(mjsPath, mjs);
console.log('OK — pyimport+runPythonAsync use promising(), scan_registers is a no-op, only emscripten_sleep is Suspending');
