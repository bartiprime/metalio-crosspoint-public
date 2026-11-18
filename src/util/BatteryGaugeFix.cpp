// r98.2 — BQ27220 capacity load with an unconditional, verified exit from CONFIG UPDATE.
//
// The state machine r98.1 shipped (freeink-sdk BatteryMonitor::loadDesignCapacity()) left the
// gauge sitting in CFGUPDATE=1 with SEC=sealed. The exact dead end, from that code:
//
//   close():  if (cfg) { cfg = false; if (writeReg16(CONTROL, wrote ? 0x0091 : 0x0092)) next(8); }
//             writeReg16(CONTROL, 0x0030);   // SEALED -- reached even when the exit failed
//
//   1. step 0 sees Design Capacity == target and CFGUPDATE already set ("an earlier load was cut
//      short") and calls close() WITHOUT unsealing. The gauge is sealed, so it refuses
//       EXIT_CFG_UPDATE (0x0091) -- the write only ACKs, it is not obeyed -- and the machine is
//      dropped to step 8 with s.cfg already cleared.
//   2. step 8 waits up to 5 s for CFGUPDATE to clear. It cannot, because the exit was refused.
//      On timeout the code falls into the design-capacity branch and writes DM again (refused:
//      DM writes need FULL ACCESS and CFGUPDATE), then calls close() a second time.
//   3. close() now has s.cfg == false, so it sends SEALED (0x0030) -- which the gauge accepts --
//      and marks step 9 (done). CFGUPDATE stays latched. Every later boot repeats 1-3.
//
// Consequences seen on the device: Impedance Track suspended, so RemainingCapacity (0x10) froze
// at 2853 mAh while charging at +257 mA; DM reads were incoherent (0x929F read 2000, then 3280,
// the second from whichever block the gauge's MAC pointer had advanced to); and 0x12 stayed at
// TI's 3000 default because EXIT_CFG_UPDATE_REINIT -- the only event that copies DM Learned FCC
// into FullChargeCapacity -- had never actually run.
//
// This module owns the lane instead. Sequence, all of it in OUR tree (the submodule stays
// pristine, and main.cpp no longer calls the SDK machine):
//
//   probe 0x3A/0x3C/0x12  ->  already healthy? log and stop, touch nothing
//   unseal (0x0414 then 0x3672, 1.6 s apart -- this gauge ignores keys sent back to back)
//   full access (0xFFFF, 0xFFFF, 1.6 s apart)
//   ENTER_CFG_UPDATE (0x0090), verify CFGUPDATE=1
//   write DM 0x929D (Learned FCC) then 0x929F (Design Capacity), checksum-adjusted, read back
//   EXIT_CFG_UPDATE_REINIT (0x0091)  -> POLL 0x3A up to 4x, re-sending 0x0091 each time
//   SEALED (0x0030)                  -> POLL 0x3A until SEC reads sealed (one 0x0020 fallback)
//   final 0x3A logged, always
//
// Every failure -- a refused key, a failed DM write, a write that reads back wrong, an I2C error --
// jumps to the SAME exit lane, so the loop cannot leave the gauge unsealed, half-written, or in
// CONFIG UPDATE. The exit and the seal are the last two things this code does, on every path.
#include "util/BatteryGaugeFix.h"

#include "platform/PlatformFeatures.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <Wire.h>

namespace gaugefix {
namespace {

// --- BQ27220 registers/keys (TRM SLUUBD4A 6.1) ---------------------------------
constexpr uint8_t REG_CONTROL = 0x00;
constexpr uint8_t REG_FULL_CHARGE_CAPACITY = 0x12;
constexpr uint8_t REG_OPERATION_STATUS = 0x3A;
constexpr uint8_t REG_DESIGN_CAPACITY = 0x3C;
constexpr uint8_t REG_MAC_CONTROL = 0x3E;
constexpr uint8_t REG_MAC_DATA = 0x40;
constexpr uint8_t REG_MAC_DATA_SUM = 0x60;  // high byte = MACDataLen()

constexpr uint16_t CTRL_SEALED = 0x0030;
// Some BQ27xxx-family parts seal with 0x0020 rather than 0x0030. Tried once as a fallback, only
// when 0x0030 did not take effect, so the end state is verified rather than assumed.
constexpr uint16_t CTRL_SEALED_ALT = 0x0020;
constexpr uint16_t CTRL_ENTER_CFG_UPDATE = 0x0090;
constexpr uint16_t CTRL_EXIT_CFG_UPDATE_REINIT = 0x0091;
constexpr uint16_t KEY_UNSEAL_1 = 0x0414;
constexpr uint16_t KEY_UNSEAL_2 = 0x3672;
constexpr uint16_t KEY_FULL_ACCESS_1 = 0xFFFF;
constexpr uint16_t KEY_FULL_ACCESS_2 = 0xFFFF;

constexpr uint16_t DM_LEARNED_FCC = 0x929D;
constexpr uint16_t DM_DESIGN_CAPACITY = 0x929F;

// OperationStatus 0x3A: CFGUPDATE = bit 10, SEC[2:1] = security state.
constexpr uint16_t OPSTAT_CFGUPDATE = 0x0400;
constexpr uint8_t SEC_FULL_ACCESS = 1;
constexpr uint8_t SEC_UNSEALED = 2;
constexpr uint8_t SEC_SEALED = 3;  // sealed-at-rest code this gauge reports (SDK host model, TRM)

// A learning cycle moves Learned FCC at most 256 mAh (TRM 1.1.3); one learned against TI's
// 3000 mAh default stays far above a 2000 mAh cell.
constexpr unsigned long KEY_GAP_MS = 1600;      // this gauge ignores keys sent back to back
constexpr unsigned long SETTLE_MS = 300;        // AFTER a Control() write
constexpr unsigned long BLOCK_SETTLE_MS = 10;   // MAC block select -> data read
constexpr unsigned long DM_WRITE_GAP_MS = 600;  // block select right after a block write is missed
constexpr int EXIT_SENDS = 4;
constexpr int SEAL_SENDS = 3;

enum Step : uint8_t {
    kIdle = 0,
    kProbe,
    kUnseal1,
    kUnseal2,
    kCheckUnsealed,
    kFullAccess1,
    kFullAccess2,
    kCheckFullAccess,
    kEnterCfg,
    kCheckCfg,
    kWriteFcc,
    kWriteDesign,
    kCheckDesign,
    kExit,
    kPollExit,
    kSeal,
    kPollSeal,
    kFinal,
    kDone
};

struct State {
    Step step = kIdle;
    unsigned long at = 0;
    bool armed = false;
    bool cfgSeen = false;
    bool healthy = false;
    uint8_t tries = 0;
    uint8_t unsealTries = 0;
    uint8_t accessTries = 0;
    uint8_t cfgTries = 0;
    uint8_t sealTries = 0;
    bool failed = false;
    const char* failedWhy = "";
    Report report;
} s;

TwoWire& bus() {
#if defined(SOC_I2C_NUM) && SOC_I2C_NUM > 1
    if (BoardConfig::ACTIVE.batteryGauge.i2cBus == 1) return Wire1;
#endif
    return Wire;
}

uint8_t address() { return BoardConfig::ACTIVE.batteryGauge.gaugeAddr; }

uint16_t bswap16(const uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }

bool readReg16(const uint8_t reg, uint16_t& out) {
    TwoWire& w = bus();
    w.beginTransmission(address());
    w.write(reg);
    if (w.endTransmission(false) != 0) return false;
    if (w.requestFrom(address(), static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) return false;
    const uint8_t lo = w.read();
    const uint8_t hi = w.read();
    out = static_cast<uint16_t>(lo) | static_cast<uint16_t>(hi << 8);
    return true;
}

bool writeReg16(const uint8_t reg, const uint16_t value) {
    TwoWire& w = bus();
    delayMicroseconds(66);  // BQ27220 bus-free time between transactions
    w.beginTransmission(address());
    w.write(reg);
    w.write(static_cast<uint8_t>(value & 0xFF));
    w.write(static_cast<uint8_t>(value >> 8));
    return w.endTransmission(true) == 0;
}

bool selectBlock(const uint16_t dmAddress) { return writeReg16(REG_MAC_CONTROL, dmAddress); }

// One MAC read of a DM word: select the block, read the big-endian word, then the block checksum.
// MACDataSum() advances the gauge's block pointer, so every read re-selects.
bool dmReadOnce(const uint16_t dmAddress, uint16_t& value, uint16_t& sum) {
    if (!selectBlock(dmAddress)) return false;
    delay(BLOCK_SETTLE_MS);
    uint16_t raw = 0;
    if (!readReg16(REG_MAC_DATA, raw)) return false;
    if (!readReg16(REG_MAC_DATA_SUM, sum)) return false;
    value = bswap16(raw);
    return true;
}

// Writes one big-endian word into Data Memory, moving the block checksum by exactly the bytes that
// change (the same arithmetic the SDK uses: no full-block rewrite, no guessing). Caller must be in
// CONFIG UPDATE with FULL ACCESS; the read-back proves the write landed.
bool dmWriteWord(const uint16_t dmAddress, const uint16_t value, uint16_t& readBack) {
    uint16_t old = 0;
    uint16_t sum = 0;
    if (!dmReadOnce(dmAddress, old, sum)) return false;
    if (old == value) {
        readBack = old;
        return true;
    }
    const uint16_t raw = bswap16(value);  // byte order as the gauge stores it
    const uint16_t oldRaw = bswap16(old);
    const uint8_t checksum = static_cast<uint8_t>((sum & 0xFF) + (oldRaw & 0xFF) + (oldRaw >> 8) - (raw & 0xFF) -
                                                 (raw >> 8));
    // Re-select: the checksum read above moved the gauge to the next block.
    if (!selectBlock(dmAddress)) return false;
    delay(BLOCK_SETTLE_MS);
    if (!writeReg16(REG_MAC_DATA, raw)) return false;
    if (!writeReg16(REG_MAC_DATA_SUM, static_cast<uint16_t>((sum & 0xFF00) | checksum))) return false;
    delay(BLOCK_SETTLE_MS);
    uint16_t check = 0;
    uint16_t checkSum = 0;
    if (!dmReadOnce(dmAddress, check, checkSum)) return false;
    readBack = check;
    return check == value;
}

bool readOpStatus(uint16_t& status) { return readReg16(REG_OPERATION_STATUS, status); }

void setStep(const Step step, const unsigned long gapMs) {
    s.step = step;
    s.at = millis() + gapMs;
}

void logFinal(const char* outcome);

// Every failure lands here. It does not return: it goes to the exit lane, which always sends
// EXIT_CFG_UPDATE (with reinit) and then SEALED, verifies both, and logs the final 0x3A.
void fail(const char* why) {
    if (!s.failed) {
        s.failed = true;
        s.failedWhy = why;
    }
    LOG_ERR("BATT", "gauge load: %s -> forcing EXIT_CFG_UPDATE + SEAL anyway", why);
    setStep(kExit, 0);
}

Report& rep() { return s.report; }

void logFinal(const char* outcome) {
    Report& r = rep();
    uint16_t status = 0;
    const bool ok = readOpStatus(status);
    if (ok) {
        r.opStatus = status;
        r.cfgUpdate = (status & OPSTAT_CFGUPDATE) != 0;
        r.security = static_cast<uint8_t>((status >> 1) & 3);
        r.sealed = r.security == SEC_SEALED;
    }
    uint16_t dc = 0;
    uint16_t fcc = 0;
    if (readReg16(REG_DESIGN_CAPACITY, dc)) r.designCapacity = dc;
    if (readReg16(REG_FULL_CHARGE_CAPACITY, fcc)) r.fullChargeCapacity = fcc;

    const bool clean = ok && !r.cfgUpdate && r.sealed;
    r.outcome = s.healthy ? Outcome::Healthy : ((clean && !s.failed) ? Outcome::Fixed : Outcome::Failed);
    if (r.outcome == Outcome::Healthy) {
        LOG_INF("BATT", "gauge ok: already coherent, nothing written");
    }
    LOG_INF("BATT",
            "gauge FINAL 0x3A=0x%04X CFGUPDATE=%u SEC=%u -> %s | 0x3C=%u 0x12=%u | wrote FCC=%d DC=%d %s%s",
            static_cast<unsigned>(r.opStatus), r.cfgUpdate ? 1u : 0u, static_cast<unsigned>(r.security),
            clean ? "CFGUPDATE CLEAR, SEALED" : (r.cfgUpdate ? "STILL IN CONFIG UPDATE" : "NOT SEALED"),
            static_cast<unsigned>(r.designCapacity), static_cast<unsigned>(r.fullChargeCapacity),
            r.wroteFcc ? 1 : 0, r.wroteDesign ? 1 : 0, s.failed ? "failure=" : "", s.failed ? s.failedWhy : outcome);
    if (!clean) {
        LOG_ERR("BATT",
                "gauge left imperfect (0x3A=0x%04X): %s. RemainingCapacity stays frozen and DM reads are "
                "unreliable until this clears; re-run by rebooting (the sequence retries at every boot)",
                static_cast<unsigned>(r.opStatus), s.failed ? s.failedWhy : "exit or seal did not verify");
    }
}

// ---- one step -----------------------------------------------------------------
bool stepOnce() {
    const unsigned long now = millis();
    if (s.step == kDone || s.step == kIdle) return false;
    if (static_cast<long>(now - s.at) < 0) return true;

    uint16_t status = 0;
    const auto unusable = [&](const bool readOk) {
        if (readOk) return false;
        fail("gauge not answering on 0x3A");
        return true;
    };

    switch (s.step) {
        case kProbe: {
            uint16_t dc = 0;
            uint16_t fcc = 0;
            if (unusable(readOpStatus(status))) return true;
            if (status == 0xFFFF || status == 0x0000) {
                // 0xFFFF = nothing on the bus; not our call to retry forever.
                s.failed = true;
                s.failedWhy = "0x3A reads 0xFFFF/0x0000 (no gauge)";
                s.step = kDone;
                rep().opStatus = status;
                LOG_ERR("BATT", "gauge load: %s, nothing to do", s.failedWhy);
                return false;
            }
            s.report.opStatus = status;
            const bool cfg = (status & OPSTAT_CFGUPDATE) != 0;
            const uint8_t sec = static_cast<uint8_t>((status >> 1) & 3);
            const bool dcOk = readReg16(REG_DESIGN_CAPACITY, dc);
            const bool fccOk = readReg16(REG_FULL_CHARGE_CAPACITY, fcc);
            const uint16_t target = BoardConfig::ACTIVE.batteryGauge.designCapacityMah;
            const bool fccTooHigh = fccOk && fcc > static_cast<uint16_t>(target + target / 4);
            LOG_INF("BATT", "gauge probe 0x3A=0x%04X CFGUPDATE=%u SEC=%u 0x3C=%u 0x12=%u target=%u", status,
                    cfg ? 1u : 0u, static_cast<unsigned>(sec), dcOk ? dc : 0u, fccOk ? fcc : 0u,
                    static_cast<unsigned>(target));
            // Healthy = out of CONFIG UPDATE, sealed, Design Capacity already the pack's rating and
            // FullChargeCapacity plausible. Then there is nothing to write and nothing to unseal for.
            if (!cfg && sec == SEC_SEALED && dcOk && dc == target && !fccTooHigh) {
                s.healthy = true;
                s.step = kFinal;
                return true;
            }
            s.cfgSeen = cfg;
            if (cfg) {
                LOG_ERR("BATT",
                        "gauge is in CONFIG UPDATE (0x3A=0x%04X): Impedance Track is suspended, 0x10 is frozen and "
                        "DM reads are unreliable. Unsealing to clear it",
                        status);
            }
            setStep(kUnseal1, 0);
            return true;
        }

        case kUnseal1:
            if (!writeReg16(REG_CONTROL, KEY_UNSEAL_1)) {
                fail("unseal key 1 write failed");
                return true;
            }
            setStep(kUnseal2, KEY_GAP_MS);
            return true;

        case kUnseal2:
            if (!writeReg16(REG_CONTROL, KEY_UNSEAL_2)) {
                fail("unseal key 2 write failed");
                return true;
            }
            setStep(kCheckUnsealed, KEY_GAP_MS);
            return true;

        case kCheckUnsealed:
            if (unusable(readOpStatus(status))) return true;
            if (((status >> 1) & 3) == SEC_FULL_ACCESS) {  // already fully accessible
                setStep(kEnterCfg, 0);
                return true;
            }
            if (((status >> 1) & 3) != SEC_UNSEALED) {
                if (++s.unsealTries < 2) {
                    LOG_ERR("BATT", "gauge did not unseal (0x3A=0x%04X), retrying the key pair", status);
                    setStep(kUnseal1, 0);
                    return true;
                }
                fail("gauge would not unseal");
                return true;
            }
            setStep(kFullAccess1, 0);
            return true;

        case kFullAccess1:
            if (!writeReg16(REG_CONTROL, KEY_FULL_ACCESS_1)) {
                fail("full-access key 1 write failed");
                return true;
            }
            setStep(kFullAccess2, KEY_GAP_MS);
            return true;

        case kFullAccess2:
            if (!writeReg16(REG_CONTROL, KEY_FULL_ACCESS_2)) {
                fail("full-access key 2 write failed");
                return true;
            }
            setStep(kCheckFullAccess, KEY_GAP_MS);
            return true;

        case kCheckFullAccess:
            if (unusable(readOpStatus(status))) return true;
            if (((status >> 1) & 3) != SEC_FULL_ACCESS) {
                if (++s.accessTries < 2) {
                    LOG_ERR("BATT", "gauge did not reach FULL ACCESS (0x3A=0x%04X), retrying", status);
                    setStep(kFullAccess1, 0);
                    return true;
                }
                fail("gauge would not reach FULL ACCESS");
                return true;
            }
            setStep(kEnterCfg, 0);
            return true;

        case kEnterCfg:
            if (!writeReg16(REG_CONTROL, CTRL_ENTER_CFG_UPDATE)) {
                fail("ENTER_CFG_UPDATE write failed");
                return true;
            }
            setStep(kCheckCfg, SETTLE_MS);
            return true;

        case kCheckCfg:
            if (unusable(readOpStatus(status))) return true;
            if ((status & OPSTAT_CFGUPDATE) == 0) {
                if (++s.cfgTries < 2) {
                    setStep(kEnterCfg, 0);
                    return true;
                }
                fail("gauge would not enter CONFIG UPDATE");
                return true;
            }
            s.cfgSeen = true;
            setStep(kWriteFcc, 0);
            return true;

        case kWriteFcc: {
            const uint16_t target = BoardConfig::ACTIVE.batteryGauge.designCapacityMah;
            uint16_t fcc = 0;
            uint16_t sum = 0;
            if (!dmReadOnce(DM_LEARNED_FCC, fcc, sum)) {
                fail("Learned FCC read failed");
                return true;
            }
            if (fcc <= static_cast<uint16_t>(target + target / 4)) {  // already sane, leave it
                setStep(kWriteDesign, DM_WRITE_GAP_MS);
                return true;
            }
            uint16_t back = 0;
            if (!dmWriteWord(DM_LEARNED_FCC, target, back)) {
                fail("Learned FCC write did not read back");
                return true;
            }
            rep().wroteFcc = true;
            LOG_INF("BATT", "gauge DM 0x929D Learned FCC %u -> %u (read back %u)", fcc, target, back);
            setStep(kWriteDesign, DM_WRITE_GAP_MS);
            return true;
        }

        case kWriteDesign: {
            const uint16_t target = BoardConfig::ACTIVE.batteryGauge.designCapacityMah;
            uint16_t dc = 0;
            uint16_t sum = 0;
            if (!dmReadOnce(DM_DESIGN_CAPACITY, dc, sum)) {
                fail("Design Capacity read failed");
                return true;
            }
            if (dc == target) {
                LOG_INF("BATT", "gauge DM 0x929F Design Capacity already %u", dc);
                setStep(kExit, 0);
                return true;
            }
            uint16_t back = 0;
            if (!dmWriteWord(DM_DESIGN_CAPACITY, target, back)) {
                fail("Design Capacity write did not read back");
                return true;
            }
            rep().wroteDesign = true;
            LOG_INF("BATT", "gauge DM 0x929F Design Capacity %u -> %u (read back %u)", dc, target, back);
            setStep(kCheckDesign, DM_WRITE_GAP_MS);
            return true;
        }

        case kCheckDesign: {
            uint16_t dc = 0;
            uint16_t sum = 0;
            const uint16_t target = BoardConfig::ACTIVE.batteryGauge.designCapacityMah;
            if (!dmReadOnce(DM_DESIGN_CAPACITY, dc, sum) || dc != target) {
                LOG_ERR("BATT", "Design Capacity read-back %u != %u (keeping the write, exiting anyway)", dc, target);
            }
            setStep(kExit, 0);
            return true;
        }

        // ---- the lane that runs on EVERY path -----------------------------------------------
        case kExit:
            s.tries = 1;
            if (!writeReg16(REG_CONTROL, CTRL_EXIT_CFG_UPDATE_REINIT)) {
                LOG_ERR("BATT", "EXIT_CFG_UPDATE write NAKed, will retry");
            }
            setStep(kPollExit, SETTLE_MS);
            return true;

        case kPollExit:
            if (!readOpStatus(status)) {
                LOG_ERR("BATT", "0x3A unreadable while polling EXIT_CFG_UPDATE");
                status = 0xFFFF;
            }
            if ((status & OPSTAT_CFGUPDATE) == 0) {
                rep().exited = true;
                LOG_INF("BATT", "gauge left CONFIG UPDATE (0x3A=0x%04X)", status);
                setStep(kSeal, 0);
                return true;
            }
            if (s.tries < EXIT_SENDS) {
                ++s.tries;
                writeReg16(REG_CONTROL, CTRL_EXIT_CFG_UPDATE_REINIT);
                setStep(kPollExit, 250);
                return true;
            }
            LOG_ERR("BATT", "EXIT_CFG_UPDATE did not clear CFGUPDATE after %d sends (0x3A=0x%04X)", EXIT_SENDS,
                    status);
            setStep(kSeal, 0);  // still seal: an unsealed gauge is worse than a stuck one
            return true;

        case kSeal:
            s.sealTries = 1;
            if (!writeReg16(REG_CONTROL, CTRL_SEALED)) LOG_ERR("BATT", "SEALED write NAKed, will retry");
            setStep(kPollSeal, SETTLE_MS);
            return true;

        case kPollSeal:
            if (!readOpStatus(status)) status = 0xFFFF;
            if (((status >> 1) & 3) == SEC_SEALED) {
                rep().resealed = true;
                LOG_INF("BATT", "gauge re-sealed (0x3A=0x%04X)", status);
                setStep(kFinal, 0);
                return true;
            }
            if (s.sealTries <= SEAL_SENDS) {
                const uint16_t cmd = s.sealTries == 1 ? CTRL_SEALED_ALT : CTRL_SEALED;
                ++s.sealTries;
                LOG_ERR("BATT", "gauge not sealed yet (0x3A=0x%04X), sending 0x%04X", status,
                        static_cast<unsigned>(cmd));
                writeReg16(REG_CONTROL, cmd);
                setStep(kPollSeal, 400);
                return true;
            }
            LOG_ERR("BATT", "gauge did not re-seal after %d sends (0x3A=0x%04X)", SEAL_SENDS + 1, status);
            setStep(kFinal, 0);
            return true;

        case kFinal: {
            const unsigned long start = s.report.elapsedMs;
            logFinal(s.healthy ? "healthy" : (s.failed ? s.failedWhy : "written, exited, sealed"));
            rep().elapsedMs = millis() - start;
            s.step = kDone;
            return false;
        }

        default:
            s.step = kDone;
            return false;
    }
}

}  // namespace

bool arm() {
    const auto& g = BoardConfig::ACTIVE.batteryGauge;
#if FREEINK_BATTERY_I2C_GAUGE
    if (g.gaugeType != BoardConfig::GaugeType::Bq27220) return false;
    if (g.gaugeAddr == 0 || g.designCapacityMah == 0) return false;
    return true;
#else
    (void)g;
    return false;
#endif
}

bool tick() {
    if (!s.armed) {
        if (!arm()) return false;
        s.armed = true;
        s.report.elapsedMs = millis();
        s.step = kProbe;
        s.at = millis() + 250;  // let the gauge settle after boot-time rail work
        LOG_INF("BATT", "gauge capacity load armed (target %u mAh)",
                static_cast<unsigned>(BoardConfig::ACTIVE.batteryGauge.designCapacityMah));
        return true;
    }
    return stepOnce();
}

void finishNow() {
    if (!s.armed) return;
    // Blocking backstop. The steps are bounded, so this always terminates; if the caller's
    // deadline already fired, jump straight to the lane that cannot leave the gauge open.
    if (s.step != kDone && s.step != kExit && s.step != kPollExit && s.step != kSeal && s.step != kPollSeal &&
        s.step != kFinal) {
        fail("caller deadline reached mid-sequence");
    }
    for (int i = 0; i < 400 && s.step != kDone; ++i) {
        stepOnce();
        if (s.step != kDone) delay(25);
    }
    if (s.step != kDone) {
        writeReg16(REG_CONTROL, CTRL_EXIT_CFG_UPDATE_REINIT);
        writeReg16(REG_CONTROL, CTRL_SEALED);
        logFinal("backstop");
        s.step = kDone;
    }
}

Report report() { return s.report; }

bool readDmWord(const uint16_t dmAddress, DmWord& out) {
    out = {};
    out.address = dmAddress;
    if (!arm()) {
        out.verdict = Verdict::Unavailable;
        out.why = "no BQ27220 with a target capacity on this profile";
        return false;
    }
    // r98.3 -- MAC transfers move ONE 32-byte block (TRM 6.1: MACData() streams the block that
    // MACDataControl selected, MACDataSum() checksums it and advances the pointer). A 16-bit word
    // whose address is the LAST byte of a block therefore takes its second byte from a block the
    // transfer did not select, so the value is not reproducible -- which is exactly what `batt`
    // showed: 0x929D (block 0x9280, offset 0x1D, word inside the block) reads clean, while
    // 0x929F (offset 0x1F, the block's final byte) returned two different values on two reads.
    // Nothing about that needs the gauge unsealed, and unsealing to read it is refused: a READ
    // must never change gauge state. Design Capacity is already available and validated from the
    // 0x3C command register, so say so rather than printing INVALID.
    constexpr uint16_t kMacBlockMask = 0x1Fu;  // 32-byte blocks
    if ((dmAddress & kMacBlockMask) == kMacBlockMask) {
        static char why[192];
        snprintf(why, sizeof(why),
                 "16-bit word starts on the LAST byte of MAC block 0x%04X, so it crosses the block "
                 "boundary: NOT reliably readable while sealed (a read never unseals the gauge). "
                 "Use 0x3C, the command-register copy, which is validated",
                 static_cast<unsigned>(dmAddress & ~kMacBlockMask));
        out.verdict = Verdict::Unavailable;
        out.why = why;
        return false;
    }
    uint16_t v1 = 0, s1 = 0, v2 = 0, s2 = 0;
    if (!dmReadOnce(dmAddress, v1, s1)) {
        out.verdict = Verdict::Unavailable;
        out.why = "MAC read failed (sealed, or no FULL ACCESS)";
        return false;
    }
    if (!dmReadOnce(dmAddress, v2, s2)) {
        out.value = v1;
        out.sum = s1;
        out.verdict = Verdict::Invalid;
        out.why = "second MAC read failed";
        return false;
    }
    out.value = v1;
    out.value2 = v2;
    out.sum = s1;
    out.sum2 = s2;
    if (v1 != v2 || s1 != s2) {
        out.verdict = Verdict::Invalid;
        out.why = "two independent reads disagree -- block data is not coherent";
        return false;
    }
    out.verdict = Verdict::Ok;
    out.why = "two reads agree, checksum stable";
    return true;
}

}  // namespace gaugefix
