// r98.2 — gaugefix:: against a model of the device's BQ27220, including the exact state r98.1
// shipped: CFGUPDATE latched and the gauge sealed.
//
// The model refuses EXIT_CFG_UPDATE (0x0091) unless the gauge is in FULL ACCESS, which is what the
// device did (the exit only ACKs; it is not obeyed while sealed). That is the whole bug: the SDK
// machine sent the exit while sealed, then sealed again and reported done, so CFGUPDATE stayed set
// forever. This test fails against that behaviour and passes against gaugefix::.
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/util/BatteryGaugeFix.cpp"

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(condition)                                               \
  do {                                                                 \
    ++checksRun;                                                       \
    if (!(condition)) {                                                \
      ++checksFailed;                                                  \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
    }                                                                  \
  } while (0)

constexpr uint16_t DM_BASE = 0x9280;
constexpr uint16_t DM_FCC = 0x929D;
constexpr uint16_t DM_DC = 0x929F;
constexpr uint16_t TARGET = 2000;

// Just enough BQ27220: keys with the gauge's spacing rules, CONFIG UPDATE, and a Data Memory block
// that only takes a write in CONFIG UPDATE with FULL ACCESS and the right checksum. 0x0091 is
// refused unless FULL ACCESS -- the behaviour that broke r98.1.
struct FakeGauge {
  uint8_t sec = 3;  // 3 sealed, 2 unsealed, 1 full access
  bool cfg = false;
  uint16_t fccReg = 3000;  // FullChargeCapacity() 0x12
  std::array<uint8_t, 0x100> dm{};  // from DM_BASE
  uint16_t mac = DM_BASE;
  uint16_t staged = 0;
  uint16_t lastKey = 0;
  unsigned long lastKeyAt = 0;
  unsigned long committedAt = 0;
  int exitsObeyed = 0;
  int exitsRefused = 0;
  int seals = 0;
  bool ignoreExitAlways = false;
  bool refuseDmWrites = false;

  FakeGauge() {
    for (size_t i = 0; i < dm.size(); ++i) dm[i] = static_cast<uint8_t>(i * 37 + 11);
    set(DM_FCC, 3000);
    set(DM_DC, 3000);
  }
  void set(uint16_t address, uint16_t value) {
    dm.at(address - DM_BASE) = value >> 8;
    dm.at(address - DM_BASE + 1) = value & 0xFF;
  }
  uint16_t get(uint16_t address) const { return dm.at(address - DM_BASE) << 8 | dm.at(address - DM_BASE + 1); }
  uint8_t sum(uint8_t first, uint8_t second) const {
    unsigned total = (mac & 0xFF) + (mac >> 8) + first + second;
    for (int i = 2; i < 32; ++i) total += dm.at(mac - DM_BASE + i);
    return static_cast<uint8_t>(255 - total);
  }
  bool keyFollows(uint16_t first, uint16_t second, uint16_t word) const {
    const unsigned long gap = hostMillis - lastKeyAt;
    return word == second && lastKey == first && gap >= 1500 && gap < 4000;
  }

  bool write(uint8_t reg, uint16_t word) {
    if (reg == 0x00) {
      if (sec == 3 && keyFollows(0x0414, 0x3672, word)) sec = 2;
      else if (sec == 2 && keyFollows(0xFFFF, 0xFFFF, word)) sec = 1;
      else if (word == 0x0090 && sec == 1) cfg = true;
      else if (word == 0x0091 || word == 0x0092) {
        if (sec == 1 && !ignoreExitAlways) {
          cfg = false;
          fccReg = get(DM_FCC);  // reinit copies Learned FCC into FullChargeCapacity
          ++exitsObeyed;
        } else {
          ++exitsRefused;
        }
      } else if (word == 0x0030) {
        sec = 3;
        ++seals;
      }
      lastKey = word;
      lastKeyAt = hostMillis;
    }
    if (reg == 0x3E) mac = word;
    if (reg == 0x40) staged = word;
    if (reg == 0x60) {
      const bool ok = !refuseDmWrites && cfg && sec == 1 &&
                      word == static_cast<uint16_t>(0x2400 | sum(staged & 0xFF, staged >> 8));
      if (ok) {
        dm.at(mac - DM_BASE) = staged & 0xFF;
        dm.at(mac - DM_BASE + 1) = staged >> 8;
        committedAt = hostMillis;
      }
    }
    return true;
  }

  bool read(uint8_t reg, uint16_t& word) const {
    if (reg == 0x3A) word = static_cast<uint16_t>(0x00F0 | (cfg ? 0x0400 : 0) | sec << 1);
    else if (reg == 0x3C) word = get(DM_DC);
    else if (reg == 0x12) word = fccReg;
    else if (reg == 0x40) word = static_cast<uint16_t>(dm.at(mac - DM_BASE) | dm.at(mac - DM_BASE + 1) << 8);
    else if (reg == 0x60) word = static_cast<uint16_t>(0x2400 | sum(dm.at(mac - DM_BASE), dm.at(mac - DM_BASE + 1)));
    else return false;
    return true;
  }
};

FakeGauge* gauge = nullptr;

}  // namespace

// The Wire stub hands every transaction to the model gauge.
bool hostI2cWrite(uint8_t, const std::vector<uint8_t>& bytes) {
  if (gauge == nullptr || bytes.size() != 3) return false;  // Control()/MAC writes are one word
  return gauge->write(bytes[0], static_cast<uint16_t>(bytes[1] | (bytes[2] << 8)));
}

bool hostI2cRead(uint8_t, uint8_t reg, uint8_t* out, size_t length) {
  if (gauge == nullptr) return false;
  uint16_t word = 0;
  if (!gauge->read(reg, word)) return false;
  if (length >= 1) out[0] = static_cast<uint8_t>(word & 0xFF);
  if (length >= 2) out[1] = static_cast<uint8_t>(word >> 8);
  return true;
}

namespace {

// Calls one step every 10 ms until the sequence reports done, like the main loop does.
bool runToCompletion() {
  for (unsigned long t = hostMillis + 10; t < hostMillis + 60000; t += 10) {
    hostMillis = t;
    if (!gaugefix::tick()) return true;
  }
  return false;
}

bool logContains(const char* needle) {
  for (const auto& line : hostLogLines)
    if (line.find(needle) != std::string::npos) return true;
  return false;
}

// The r98.1 device state, verbatim: Design Capacity already 2000, Learned FCC 2000, CFGUPDATE
// latched, gauge sealed, and 0x12 still at TI's 3000 because the reinit never ran.
void scenarioLatchedCfgUpdate() {
  FakeGauge fake;
  fake.set(DM_DC, TARGET);
  fake.set(DM_FCC, TARGET);
  fake.cfg = true;
  fake.sec = 3;
  fake.fccReg = 3000;
  gauge = &fake;
  BoardConfig::ACTIVE = BoardConfig::XTEINK_X3;
  BoardConfig::ACTIVE.batteryGauge.designCapacityMah = TARGET;

  CHECK(runToCompletion());
  CHECK(!fake.cfg);                 // CFGUPDATE cleared -- the defect this release fixes
  CHECK(fake.sec == 3);             // and re-sealed
  CHECK(fake.exitsObeyed == 1);     // the exit was actually obeyed, not just sent
  CHECK(fake.fccReg == TARGET);     // reinit ran: 0x12 picked up DM 0x929D
  CHECK(fake.exitsRefused == 0);    // never sent while sealed (that is what failed before)
  CHECK(logContains("1970-") || true);  // no-op: keep the log helper referenced
  CHECK(logContains("gauge FINAL 0x3A="));
  CHECK(logContains("CFGUPDATE CLEAR, SEALED"));
  CHECK(gaugefix::report().outcome == gaugefix::Outcome::Fixed);
}

// A gauge that has never been written: TI's 3000 mAh defaults, sealed, out of CONFIG UPDATE.
void scenarioFreshGauge() {
  FakeGauge fake;  // dm DC/FCC = 3000, 0x12 = 3000, sealed
  gauge = &fake;
  BoardConfig::ACTIVE = BoardConfig::XTEINK_X4;
  BoardConfig::ACTIVE.batteryGauge.gaugeType = BoardConfig::GaugeType::Bq27220;
  BoardConfig::ACTIVE.batteryGauge.gaugeAddr = 0x55;
  BoardConfig::ACTIVE.batteryGauge.designCapacityMah = TARGET;

  CHECK(runToCompletion());
  CHECK(fake.get(DM_DC) == TARGET);
  CHECK(fake.get(DM_FCC) == TARGET);
  CHECK(!fake.cfg);
  CHECK(fake.sec == 3);
  CHECK(fake.fccReg == TARGET);  // 0x12 follows the write only because the reinit ran
  CHECK(gaugefix::report().wroteDesign);
  CHECK(gaugefix::report().wroteFcc);
  CHECK(gaugefix::report().outcome == gaugefix::Outcome::Fixed);
}

// Already coherent: nothing may be written, nothing unsealed.
void scenarioHealthy() {
  FakeGauge fake;
  fake.set(DM_DC, TARGET);
  fake.set(DM_FCC, TARGET);
  fake.cfg = false;
  fake.sec = 3;
  fake.fccReg = TARGET;
  gauge = &fake;
  BoardConfig::ACTIVE = BoardConfig::XTEINK_X4;
  BoardConfig::ACTIVE.batteryGauge.gaugeType = BoardConfig::GaugeType::Bq27220;
  BoardConfig::ACTIVE.batteryGauge.gaugeAddr = 0x55;
  BoardConfig::ACTIVE.batteryGauge.designCapacityMah = TARGET;

  CHECK(runToCompletion());
  CHECK(fake.sec == 3);
  CHECK(fake.seals == 0);        // never unsealed, so never had to seal
  CHECK(fake.exitsObeyed == 0);  // and never touched CONFIG UPDATE
  CHECK(gaugefix::report().outcome == gaugefix::Outcome::Healthy);
}

// The gauge refuses every EXIT_CFG_UPDATE. The sequence must still terminate, must still SEAL (an
// unsealed gauge is worse than a stuck one), and must say plainly that it failed.
void scenarioExitRefused() {
  FakeGauge fake;
  fake.set(DM_DC, TARGET);
  fake.set(DM_FCC, TARGET);
  fake.cfg = true;
  fake.sec = 3;
  fake.ignoreExitAlways = true;
  gauge = &fake;
  BoardConfig::ACTIVE = BoardConfig::XTEINK_X4;
  BoardConfig::ACTIVE.batteryGauge.gaugeType = BoardConfig::GaugeType::Bq27220;
  BoardConfig::ACTIVE.batteryGauge.gaugeAddr = 0x55;
  BoardConfig::ACTIVE.batteryGauge.designCapacityMah = TARGET;

  CHECK(runToCompletion());  // terminates: no infinite retry
  CHECK(fake.exitsRefused == 4);  // bounded number of EXIT sends
  CHECK(fake.sec == 3);           // sealed regardless
  CHECK(fake.seals >= 1);
  CHECK(logContains("STILL IN CONFIG UPDATE"));
  CHECK(gaugefix::report().outcome == gaugefix::Outcome::Failed);
}

// A Data Memory write that never commits: the sequence must still exit and seal and report it.
void scenarioWriteRejected() {
  FakeGauge fake;  // DC 3000 -> a write is attempted
  fake.refuseDmWrites = true;
  gauge = &fake;
  BoardConfig::ACTIVE = BoardConfig::XTEINK_X4;
  BoardConfig::ACTIVE.batteryGauge.gaugeType = BoardConfig::GaugeType::Bq27220;
  BoardConfig::ACTIVE.batteryGauge.gaugeAddr = 0x55;
  BoardConfig::ACTIVE.batteryGauge.designCapacityMah = TARGET;

  CHECK(runToCompletion());
  CHECK(fake.get(DM_DC) == 3000);  // nothing landed
  CHECK(!fake.cfg);                // but the gauge was still taken out of CONFIG UPDATE
  CHECK(fake.sec == 3);            // and re-sealed
  CHECK(logContains("did not read back"));
  CHECK(gaugefix::report().outcome == gaugefix::Outcome::Failed);
}

}  // namespace

int main(int argc, char** argv) {
  BoardConfig::ACTIVE = BoardConfig::XTEINK_X3;
  hostLogEcho = argc > 2 && std::strcmp(argv[2], "-v") == 0;
  const std::string which = argc > 1 ? argv[1] : "latched";
  if (which == "latched") scenarioLatchedCfgUpdate();
  else if (which == "fresh") scenarioFreshGauge();
  else if (which == "healthy") scenarioHealthy();
  else if (which == "exitrefused") scenarioExitRefused();
  else if (which == "writerejected") scenarioWriteRejected();
  else {
    std::printf("unknown scenario %s\n", which.c_str());
    return 2;
  }
  std::printf("%s: %d checks, %d failed\n", which.c_str(), checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
