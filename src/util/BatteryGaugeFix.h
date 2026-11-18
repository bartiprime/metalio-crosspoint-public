#pragma once
#include <cstdint>

// r98.2 — BQ27220 capacity load with an UNCONDITIONAL, VERIFIED exit from CONFIG UPDATE.
//
// WHY THIS IS IN OUR TREE AND NOT IN freeink-sdk
//   BatteryMonitor::loadDesignCapacity() owns the only DM-write machine in the SDK, and its
//   close() lane is defective in a way no caller can reach around:
//     * it sends EXIT_CFG_UPDATE (0x0091) only while the gauge is assumed unsealed, so a gauge
//       already left in CFGUPDATE + SEALED (exactly the state r98.1 shipped) never exits — the
//       write is refused, the machine moves to step 8, waits 5 s for CFGUPDATE to clear, then
//       RE-WRITES Design Capacity and SEALS (0x0030) without ever re-sending the exit;
//     * the seal succeeds, CFGUPDATE stays latched, and every later boot repeats the same dead
//       end. That left RemainingCapacity frozen (IT suspended) and DM reads incoherent.
//   Editing the submodule is off the table (pinned), so this module owns the whole lane in our
//   tree: unseal, full access, CONFIG UPDATE, checksummed DM write, EXIT with reinit, POLL until
//   CFGUPDATE reads 0, SEAL, POLL until SEC reads sealed, and one final 0x3A in the log. Every
//   failure inside the sequence still falls into the exit+seal lane, so no path can leave the
//   gauge half-written, unsealed, or stuck in CONFIG UPDATE.
namespace gaugefix {

enum class Verdict : uint8_t { Ok, Invalid, Unavailable };

// One Data Memory word plus the block checksum read alongside it. `verdict` is the ONLY thing a
// caller may display on: a DM read is accepted only when two independent reads agree AND the
// checksum is stable across them. r98.1 printed whatever came back, which is how the same address
// showed 2000 and then 3280.
struct DmWord {
    uint16_t address = 0;
    uint16_t value = 0;   // big-endian DM word, host order
    uint16_t value2 = 0;  // the independent re-read, kept so a disagreement can be printed
    uint16_t sum = 0;     // MACDataSum() from the first read (low byte = block checksum)
    uint16_t sum2 = 0;
    Verdict verdict = Verdict::Unavailable;
    const char* why = "not read";
};

enum class Outcome : uint8_t { NotRun, Healthy, Fixed, Failed };

struct Report {
    Outcome outcome = Outcome::NotRun;
    uint16_t opStatus = 0;
    uint16_t designCapacity = 0;
    uint16_t fullChargeCapacity = 0;
    bool cfgUpdate = false;
    uint8_t security = 0;
    bool sealed = false;
    bool exited = false;
    bool resealed = false;
    bool wroteFcc = false;
    bool wroteDesign = false;
    unsigned long elapsedMs = 0;
};

// True when this board carries a BQ27220 with a non-zero target capacity (2000 mAh here).
bool arm();

// One short step (never blocks more than ~20 ms). True while the sequence still has work.
bool tick();

// Blocking backstop for the callers' deadline: drives the sequence to a seal, bounded (~10 s).
// Used only if tick() somehow has not finished; it ends in the same logged final state.
void finishNow();

Report report();

// Validated Data Memory word read (two independent reads, checksum must be stable).
bool readDmWord(uint16_t address, DmWord& out);

}  // namespace gaugefix
