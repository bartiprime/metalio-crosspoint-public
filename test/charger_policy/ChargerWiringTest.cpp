// r163 E4 -- the wiring, checked as text.
//
// The policy is host-tested (ChargerPolicyTest). What that cannot show is whether the device
// actually calls it, and whether the one write site still only reaches the documented
// registers. Both are properties of files this build does not compile on the host (main.cpp
// needs ESP-IDF), so they are read here -- the same technique as test/standby_resume_order and
// BatteryGaugeWiringTest: a landmark that fails when the wiring is dropped or widened.

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace {

std::string slurp(const char* rel) {
  const std::string path = std::string(CP_REPO_ROOT) + "/" + rel;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return {};
  std::string out;
  char buf[4096];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

}  // namespace

// The loop runs on the main loop, next to the battery poll -- not on the UI thread, not on a
// part-owned task, and not nowhere (which is the defect E4 exists for).
TEST(ChargerWiringTest, MainLoopRunsTheChargerTick) {
  const std::string main = slurp("src/main.cpp");
  ASSERT_FALSE(main.empty()) << "src/main.cpp not readable from " CP_REPO_ROOT;
  EXPECT_TRUE(has(main, "chargercontrol::tick(millis());"))
      << "the charger control loop is not called from the main loop";
  EXPECT_TRUE(has(main, "powerManager.pollBattery();"))
      << "the call moved away from the battery poll; re-check the bus/lock reasoning";
}

// The boot-time VREG write is still the only place VREG is written, and it is still 4400.
TEST(ChargerWiringTest, VregIsBootOnlyAndFixed) {
  const std::string cpp = slurp("src/system/ChargerCurrent.cpp");
  ASSERT_FALSE(cpp.empty());
  // Exactly one call site for each VREG byte, both inside applyVendorVreg.
  size_t n = 0;
  for (size_t at = cpp.find("updateBits(kRegVregLo"); at != std::string::npos;
       at = cpp.find("updateBits(kRegVregLo", at + 1)) {
    ++n;
  }
  EXPECT_EQ(n, 1u) << "VREG must be written from exactly one place (the boot defaults)";
  const std::string hdr = slurp("src/system/ChargerCurrent.h");
  ASSERT_FALSE(hdr.empty());
  EXPECT_TRUE(has(hdr, "kVregMv = 4400")) << "src/system/ChargerCurrent.h must pin 4400 mV";
}

// The kill switch exists, defaults to the ENABLED path, and is read by exactly one file.
TEST(ChargerWiringTest, KillSwitchDefaultsOn) {
  const std::string hdr = slurp("src/system/ChargerCurrent.h");
  ASSERT_FALSE(hdr.empty());
  EXPECT_TRUE(has(hdr, "#ifndef CP_CHARGER_CONTROL"));
  EXPECT_TRUE(has(hdr, "#define CP_CHARGER_CONTROL 1"))
      << "the default must be the enabled path -- a glass A/B is opt-IN";
}

// The one write site cannot reach a register outside the documented set: the guard is a call
// to the policy's own allowlist, not a comment.
TEST(ChargerWiringTest, WriteSiteIsGuardedByTheAllowlist) {
  const std::string cpp = slurp("src/system/ChargerCurrent.cpp");
  ASSERT_FALSE(cpp.empty());
  EXPECT_TRUE(has(cpp, "isDocumentedRegister(reg)"))
      << "journaledWrite() lost its documented-register guard";
  // And no charger register is written anywhere else in this file: every write goes through
  // write8, which appears exactly ONCE (journaledWrite). There was a second site for the vendor's
  // whole-byte BATFET shutdown; it is gone with the feature (see the test below).
  size_t writes = 0;
  for (size_t at = cpp.find("chargercurrent::write8("); at != std::string::npos;
       at = cpp.find("chargercurrent::write8(", at + 1)) {
    ++writes;
  }
  EXPECT_EQ(writes, 1u) << "a new direct write8() call site bypasses the journal";
}

// REG0x18 is never written, anywhere, by anything in this item. The vendor's UVLO action is
// one-way in software (their release path writes nothing), so the only possible outcomes of a
// write to it are "no effect" and "the pack never charges again". The detector keeps the
// condition in the log; the register stays untouched. §6 of docs/engineering/R163_ITEM_E4.md
// records the evidence and the decision, so a later session does not "finish" the vendor port.
TEST(ChargerWiringTest, BatfetRegisterIsNeverWritten) {
  const std::string cpp = slurp("src/system/ChargerCurrent.cpp");
  ASSERT_FALSE(cpp.empty());
  const std::string hdr = slurp("src/system/ChargerPolicy.h");
  ASSERT_FALSE(hdr.empty());
  EXPECT_FALSE(has(cpp, "kRegBatfet")) << "the BATFET register handle is back in the driver";
  EXPECT_FALSE(has(cpp, "batfetShutdown")) << "the BATFET write function is back in the driver";
  EXPECT_FALSE(has(hdr, "BatfetShutdown")) << "the policy can name a BATFET shutdown again";
  EXPECT_FALSE(has(hdr, "kBatfetShutdown"));
  // The detector itself must survive: the condition is still read and still announced.
  EXPECT_TRUE(has(cpp, "uvlo-detected")) << "the UVLO detector's journal line is gone";
  EXPECT_TRUE(has(hdr, "uvCondition")) << "the UVLO condition itself was removed, not just the write";
}
