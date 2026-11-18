# r163 item 2.3 — app-engine Phase 2 (implemented)

Authority: `docs/engineering/APP_ENGINE_REPAIR_PLAN.md` §4 Phase 2 (in the planning repo),
defects D1/D2/D11, S1/S2/S4. This file records what the CODE does, not what was planned.

## 1. `device(what)` — syscall 15, bytecode version 3

| where | what |
|---|---|
| `src/engines/core/Opcodes.h` | `kSyscallCount` 15 → 16; `{"device", 1}` appended; `kSysDevice`=15 and the `kDevice*` / `kOrientation*` selector constants; the amendment note (why, the two readings, why v3) |
| `src/engines/core/HostPorts.h` | `DevicePorts { ctx, width, height, orientation, lineHeight(font) }` and `HostPorts::device` |
| `src/engines/core/Syscalls.cpp` | `SyscallBinder::deviceCall` — arity/enum checked, unbound or disagreeing device = `E_SYS_DENIED`, never a 0 |
| `src/engines/core/Program.h` | `kBytecodeVersion` 2 → 3 (with the "no compatibility path" note) |
| `src/engines/host/HostCanvas.h/.cpp` | `devicePorts(orientation)` — size + line height read from the SAME `Surface` the canvas paints |
| `src/activities/EngineHostActivity.cpp` | fills `ports_.device` from `canvas_->devicePorts(...)`; a version refusal now names both versions on the fault screen and in one `LOG_ERR` |
| `src/activities/EngineHostActivity.h` | `versionDetail_[64]` |

Selector ABI: 0 width, 1 height, 2 orientation (0 landscape / 1 portrait), 3 line height of the
**default** face (font 0 — the arity is 1, so a font cannot be an argument).

Refusals (all `E_SYS_DENIED`, i.e. loud, never a plausible wrong number): unbound device;
`device.width/height` ≠ `canvas.panelW/H`; orientation not 0/1; no text face; line height ≤ 0.
`what` outside 0..3 or a non-integer argument is `E_SYS_ARG`.

## 2. The version bump is loud

* `decodeCpb` (`src/engines/host/CpbLoad.cpp`) compares the header word before parsing a byte of
  code → `E_V_VERSION`.
* `verifyProgram` (`Program.cpp`) refuses any version ≠ 3 → `E_V_VERSION`, and that is an E04
  shelf row (`manifestCode`).
* `EngineHostActivity` prints `bytecode v2; this build speaks v3 -- rebuild it with
  tools/vm/build_apps.py` on the fault screen and in the log.
* Tools: `cpvm_format.py` `VERSION = 3`, `model/cpvm_model.py` `K_BYTECODE_VERSION = 3`, and every
  `app.json` says `"bytecode": 3`. `build_apps.py` refuses a manifest that disagrees.

## 3. `apps_layout` is the provider

`tools/vm/apps_layout.py` gained, after the existing per-app mirrors (which stay the authority for
script-defined hit sets):

* `frame(w, h)` — status band (40 px, shrinks with the canvas), content box, bottom bar (32 px);
* `grid(lay, w, h, use_frame)` — `cols/rows/cell` fitted to the canvas, **shrink-only**;
* `layout_for(app, w, h) -> {cells, hits, clears, frame, grid}` — `clears` is ALWAYS the whole
  canvas (D2);
* `provider_check(apps)` + `apps_layout.py --check` — both panels, prints one PASS/FAIL line;
* `CANVAS_CLEAR_STATEMENT` — the one statement `build_apps.py` requires of `main()`.

`build_apps.py` runs `provider_check` per app, so every build is also a layout proof.

`panel_max` is deleted from every manifest (31 in `tools/vm/apps`; 26 of them declared it), and
`apps_rules.frame_of` no longer reads it: an app's frame is the canvas it was given unless it
declares `panel_w`/`panel_h`.

## 4. The standard frame's canvas clear

All 31 apps now start `main()` with

```cps
clear(0, 0, device(0), device(1));
```

and `build_apps.py::check_canvas_clear` refuses a source that does not. It is a runtime statement
rather than an emitted literal on purpose: a literal (`1216`) re-creates D2 in portrait, and
CPscript `const` initialisers must be compile-time values.

## 5. Host constants now match the device

| constant | was (tools) | device | now |
|---|---|---|---|
| globals | `K_MAX_GLOBALS = 256` (validator, format, sema, codegen) | `kMaxGlobals = 192` | 192, one source (`cpvm_format.py`), validator reads it |
| code words | 8192 accepted by every host proof; `build_apps` workspace check said 5120 | `kWorkspaceCodeWords = 5072` (decode refuses above it with E_V_SCRATCH) | `K_WORKSPACE_CODE_WORDS = 5072`; `validator.validate()` refuses above it with E_V_SCRATCH |

This immediately caught `instapaper` declaring `code_words_max: 5120` (over the 5,072-word
workspace) — it had been passing every host proof and could never have loaded on the device.

## 6. Host proofs

See the commit message / r163 proof report for the actual summary lines.
