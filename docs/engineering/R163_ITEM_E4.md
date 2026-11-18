# r163 item E4 — charger control (implemented)

Authority: `docs/engineering/UPSTREAM_VENDOR_SWEEP_2026_11_16.md` §Battery, and the vendor
document it cites, `docs/battery-soc-and-charge.md` §4.1/§4.2 with `cx25601n.cc`
(`charger_vreg_task`, `kick_recharge`, `hw_init_defaults`). This file records what the CODE
does, what it refuses to do, and what only glass can settle.

**This is the first item in this firmware that writes a charger register to change behaviour
at run time.** Before it, the charger was written once per boot (`applyChoice`,
`applyVendorVreg`) and never again.

## 1. Shape

| where | what |
|---|---|
| `src/system/ChargerPolicy.h` | the whole decision table, as arithmetic: `Machine::step(Sample) -> Plan`. No I/O, no Wire, no time source. Host-tested. |
| `src/system/ChargerCurrent.cpp` | the only Wire in the item: one sample (status 0x1E, vbat 0x08, current 0x0C), one plan, at most that plan's writes, one journal line per write, through one guarded write site. |
| `src/system/ChargerCurrent.h` | `chargercontrol::{tick,lastReason,writeCount}` and the `CP_CHARGER_CONTROL` kill switch. |
| `src/main.cpp:4915` | `chargercontrol::tick(millis());` — next to `powerManager.pollBattery()`, on the main loop. |
| `test/charger_policy/` | `ChargerPolicyTest` (27) and `ChargerWiringTest` (5). |

The tick gates itself: 2 s while `Vbat < 4400`, 10 s at/above it (§4.2, `cx25601n.cc:172`).
On the other passes it is one `millis()` compare.

## 2. Every register this item writes

| register | field | value | when | journal `label` |
|---|---|---|---|---|
| `0x04`[7:3] + `0x05`[3:0] | VREG | **4400 mV, fixed** | once per boot, from `applyVendorVreg` — **not** from the loop | `VREG lo` / `VREG hi` |
| `0x14` bit 2 | EN_TERM | 0 at boot; then 0 while `Vbat ≤ 4380`, 1 at `Vbat ≥ 4400` | every poll, only when the value differs | `EN_TERM=0` / `EN_TERM=1` |
| `0x16` bit 5 (+4) | EN_CHG (HIZ) | 1 at boot; 0 for exactly one poll during a kick, then 1 | kick step 1 / restore | `EN_CHG=0,HIZ=0` / `EN_CHG=1,HIZ=0` |

Nothing else. `0x18` is **not** in this item's write set at all — see §6.1.
`chargerpolicy::isDocumentedRegister()` is the allowlist (four registers, compile-time),
`journaledWrite()` refuses anything outside it, and the wiring test asserts there is exactly
one `write8` call site in the file (the masked write) and that the BATFET register is never
written by any path.

Every write is read-modify-write, is preceded by a read that doubles as the presence test, and is **read back**
before it is claimed. A target value already in the register means **no I/O at all** — that
is what makes "at most one write per poll" structural rather than a promise.

Journal line, one per write, `[PWR]`:

```
charger-write REG0x14 old=0x05 new=0x01 EN_TERM=0 reason=vbat below VREG 4400mV -> termination disabled | vbat=4279mV stat=CC i=812mA vbus=1
```

`charger-write` is the grep token. `reason` is the policy's own sentence; the tail is the
measurement the decision was made from.

## 3. The rules, and the one place each differs from the vendor

| vendor rule (§4.2) | here | why |
|---|---|---|
| VREG fixed 4400, never IR-adaptive | same; the loop cannot touch VREG | §4.1: adaptive raising caused the CC↔CV software oscillation |
| `EN_TERM` off while `Vbat < VREG`, on at/above | **20 mV hysteresis**: on at ≥ 4400, off at ≤ 4380, hold between | a bare comparison at a 2 s poll toggles forever on a pack the CV loop is holding at the cut-off. 20 mV is the gauge's own full/exit width (§3.2). Proven: `TermDoesNotOscillateOnADitheringPack` |
| kick on `CHG_STAT=NOT` + I≈0 + `V < VREG` | same, plus: two consecutive agreeing polls, one kick per 60 s, never with `EN_TERM` armed | one flaky `CHG_STAT` read becomes a charge cycle on the vendor's single-sample rule; the vendor also kicks every poll with no rate limit |
| poll 2 s below / 10 s above | same; a failed read polls at **10 s**, never 2 s | a poll loop, not a retry loop (fail-safe: no storm) |
| UVLO `no VBUS + Vbat ≤ 3100` | **detected and journaled, never acted on** (no `0x18` write). The condition keeps the vendor's witnesses: `Vbat ≥ 2500`, VBUS must have been READ as 0, and the gauge must not show current into the pack, on two agreeing polls | the vendor's action is one-way in software (§6.1). `VBUS_STAT` is documented to report "no input" while a cable is in (§4.1, DPDM off), so the current reading is also what keeps the *detector* honest |

## 4. What this item refuses

* **Adaptive VREG** (§4.1) — not implemented in any form; VREG has exactly one value.
* **The vendor's `HIZ` clear as a separate step** (`cx25601n.cc:144,150` write bit 4 to 0
  twice). It is folded into the `EN_CHG` byte write as "HIZ must be 0 for EN_CHG to act",
  and it is written **only when the bit is set**, which it never is in our image — the
  vendor's two redundant writes are an artefact of their bit-helper, not a policy.
* **ITERM / charge-timer / IPRECHG / DPDM writes** — they are `hw_init_defaults` values from
  the vendor's init, and this item's remit is the runtime loop. Changing ITERM without a
  pack is exactly the kind of blind write the operator ruled out.
* **OTG** — unrelated to charging, untouched.
* **A blocking kick with the vendor's 200 ms / 500 ms delays** — the kick is two polls
  (EN_CHG off, then on), because 700 ms of `vTaskDelay` has no place on the main loop. The
  off window is one poll interval (≤ 2 s), which is longer than the vendor's 200 ms and has
  no electrical consequence: the part is not charging during `CHG_STAT=NOT`.
* **Running during sleep** — the loop is main-loop only, so a standby leaves the registers
  where the last poll left them. This matches the vendor (they suspend their task too,
  `cx25601n_vreg_suspend_for_sleep`).

## 5. Kill switch

`CP_CHARGER_CONTROL`, default **1** (the enabled path), defined in
`src/system/ChargerCurrent.h`. Set it to `0` in the build flags for a glass A/B; nothing else
in the image reads it.

* **ON (shipped):** the tick runs; boot writes VREG=4400 and `EN_TERM=0` (disarmed), and the
  loop owns `EN_TERM` from its first poll.
* **OFF:** `tick()` is compiled out (it returns false before touching Wire, and the state,
  the helpers and the journal go with it). `applyVendorVreg()` then writes VREG=4400 and
  **`EN_TERM=1`** — the vendor's init default and the exact pre-E4 behaviour. Restoring the
  switch restores: one boot-time write of VREG and `EN_TERM=1`, no runtime charger traffic,
  no kick, no UVLO, no `charger-write` lines.

Note the boot state flips with the switch: ON boots disarmed (safer — a charge below the
cut-off cannot be terminated before the loop arms it), OFF boots armed (what shipped until
now).

## 6. UVLO / BATFET: DELIBERATELY NOT IMPLEMENTED

**This is the decision, not an omission. Do not "finish" the vendor port by adding it.**

Evidence, read from the vendor tree (`vendor/main/boards/common/power/cx25601n.cc`):

* `:402-411` `cx25601n_enter_batfet_shutdown()` does exactly one thing:
  `write_byte(REG_CHG_CTRL3 /*0x18*/, 0x01)`.
* `:186-198` the UVLO branch calls it; the release branch (`:197-199`) only clears a **RAM
  flag** (`s_uv_shutdown_done = false`) when vbat rises above 3200 mV. **It writes nothing to
  `0x18`.**
* There is no other write to bit 0 of that register anywhere in their tree (`grep
  REG_CHG_CTRL3`: `:332` clears bit 2, `:478`/`:492` set/clear bit 6, `:511` reads bit 6).

So in software the shutdown is **one-way**. The vendor presumably relies on the part restoring
the FET on adapter insertion or POR; that is a hardware assumption **nobody has verified**, and
`0x18` bit 0 is the one control whose failure mode is *the pack never charges again and no code
path can fix it*. A control that can permanently stop charging is worse than no control.

What r163 does instead:

* keeps the **detection** (no VBUS + `vbat ≤ 3100 mV`, the vendor's 2500 mV floor and the
  gauge-current witness, two agreeing polls),
* publishes it as `Plan::uvlo` and journals **one** `[PWR] uvlo-detected:` line per episode
  naming the condition and the measurement, so a session is judged from the log,
* suspends the rest of the loop while the condition stands (no `EN_TERM` change, no kick at a
  pack with no charger on it) — what the vendor's latch did, without the latch,
* and **refuses the write**: `0x18` is outside `isDocumentedRegister()`, the driver has no
  BATFET function, and `ChargerWiringTest.BatfetRegisterIsNeverWritten` fails if any of that
  comes back.

There is no orderly shutdown to take instead: this firmware has no low-battery power-off path
to hook (the only shutdown paths are the user's power key, the idle/deep-sleep timeout and the
park/rescue sequence, none of which is a battery-critical action). If one is added later, the
UVLO detector is the right trigger for it — a screen-and-sleep, not a register write.

## 7. What could go wrong on hardware, and what to watch

1. **A `charger-write` line every poll.** The most likely failure mode is oscillation the
   host tests cannot see because the inputs differ on glass — e.g. `CHG_STAT` alternating
   `NOT`/`CC` between polls while the kick's 60 s limit holds `EN_CHG`. **Watch:** two or
   more `REG0x16` lines inside 60 s, or the same `REG0x14` line twice inside 10 s. Any
   repeat is a bug; the journal carries `old`/`new` so a no-op is not logged at all.
2. **An `uvlo-detected` line** means the pack read ≤ 3100 mV with no input and no charge
   current, twice. Nothing was written: the device keeps running as the vendor's firmware would
   not. **Watch:** the measurements in the same line (`vbat=`, `vbus=`, `i=`). If `vbus=0` is
   logged while a cable is visibly in, the current witness did its job or failed; that is the
   case to report, not to retune. A `uvlo-detected` line while charging is the one false
   positive to look for, and it can only come from a cable the gauge shows no current from.
3. **Charging stops after a kick.** If `EN_CHG=0` is logged with no `EN_CHG=1` within ~10 s,
   the bus died between the two polls and the restore never ran. The policy restores
   unconditionally at the *next* poll, so the fix is patience — but if the log shows the
   pair missing, the part is not charging and needs a replug.
4. **`EN_TERM` stuck on below the cut-off.** Would show as a charge that terminates at ~4.1 V
   and never resumes, with no `EN_TERM=0` line. Means the 0x14 write failed (`not written` /
   `READ-BACK MISMATCH` earlier in the log).
5. **A `charger-write` line with `vbat=?`.** The gauge read failed but the status read did
   not; only `EN_TERM` can be written in that state, and the tests pin that no kick and no
   UVLO announcement can fire without the voltage.

What the operator should look at first: `grep charger-write` on a full charge cycle. A
healthy session is **one or two lines per charge** (`EN_TERM=1` at ~4400, `EN_TERM=0` when
the pack drops to ~4380 after unplug, plus a kick pair if the pack sat under load).

## 8. Host proofs

`test/charger_policy` — **32 tests**:

* `ChargerPolicyTest` (27): VREG pinned 4400; the four-register allowlist (and 0x18 refused);
  the closed five-action set (there is no BATFET action); the EN_TERM boundary at exactly
  4400/4399 and 4380/4381; the 1000-poll dither proof (zero writes); one disarm per walk down
  the band; convergence from every 100 mV step 3300→4500; poll cadence; the kick's six
  refusals (stat, current +6, current +5 as the boundary, unknown current, unknown stat,
  V ≥ VREG, `EN_TERM` armed); the 60 s rate limit (58 s of polls refused, 60 s allowed); the
  unconditional restore on a dead bus; **UVLO as a detector**: two agreeing polls before the
  announcement, the 3100/3101 boundary, five refusals (input present, VBUS unreadable, 40 mA
  into the pack, below the 2500 mV floor, one dissenting poll), `Action::None` on every poll
  of a 40-poll episode (no EN_TERM change, no kick, `kickCount() == 0`), and the level
  clearing by itself with no latch; a failed read writing nothing and keeping state; one write
  in a ten-minute quiet session.
* `ChargerWiringTest` (5): `main.cpp` calls the tick next to the battery poll; VREG is
  written from exactly one place and is 4400; the kill switch exists and defaults ON; the one
  write site is guarded by the allowlist and is the only `write8` caller; and
  `BatfetRegisterIsNeverWritten` pins §6 — no `kRegBatfet`, no `batfetShutdown`, no
  `BatfetShutdown` action, while the detector (`uvlo-detected`, `uvCondition`) stays.

`ChargerCurrent.cpp` was syntax-checked against stub `Wire.h`/`Logging.h`/`BoardConfig.h` in
both switch positions (ESP build not run in this batch by instruction).
