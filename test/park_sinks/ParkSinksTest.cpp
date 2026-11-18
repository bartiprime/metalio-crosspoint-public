// # r106 B0 — the park-sink build defaults, as the SHIPPING image carries them.
//
// src/platform/ParkSinks.cpp is pure C++ (the hardware legs fold out on a host build),
// so this compiles the real file: the defaults asserted here are literally the
// initialisers the firmware ships with.
//
// Why usb must be OFF (R103_GLASS_LOG §F3): with the pads cut, a park left COM4
// enumerated but unconfigurable (PermissionError(13, 'A device attached to the system is
// not functioning'), every command EMPTY after 0.2 s). A default that can cost the only
// instrument is not a measurement default. `exp park usb=1` still arms it at runtime.

#include "platform/ParkSinks.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

using parksink::Sink;

TEST(ParkSinks, UsbShipsDisarmed) {
  EXPECT_FALSE(parksink::buildDefault(Sink::Usb));
  // Fresh boot == no override, so the effective value IS the build default.
  EXPECT_FALSE(parksink::active(Sink::Usb));
  EXPECT_EQ(parksink::overrideValue(Sink::Usb), -1);
}

TEST(ParkSinks, FrontAndSdShipDisarmedSinceR118) {
  // r118: front and sd are default-DISARMED. Disarmed is the verified pre-r105 behaviour
  // (front: plain Frontlight.setOn(false); sd: pads stay claimed), and the arms remain live
  // so `exp park front=1` / `sd=1` re-enables either at runtime without a rebuild.
  EXPECT_FALSE(parksink::buildDefault(Sink::Front));
  EXPECT_FALSE(parksink::buildDefault(Sink::Sd));
  EXPECT_FALSE(parksink::active(Sink::Front));
  EXPECT_FALSE(parksink::active(Sink::Sd));
}

TEST(ParkSinks, RadioShipsDisarmedSinceR107) {
  // r107 P6: a park sink that can cost the console is not worth a measurement, so radio ships
  // OFF exactly like usb. `exp park radio=1` still arms it for one session.
  EXPECT_FALSE(parksink::buildDefault(Sink::Radio));
  EXPECT_FALSE(parksink::active(Sink::Radio));
  EXPECT_EQ(parksink::overrideValue(Sink::Radio), -1);
}

TEST(ParkSinks, RuntimeArmStillEnablesUsb) {
  // `exp park usb=1`: RAM-only, this session, exactly what the bench needs.
  parksink::setOverride(Sink::Usb, true);
  EXPECT_TRUE(parksink::active(Sink::Usb));
  EXPECT_EQ(parksink::overrideValue(Sink::Usb), 1);
  // ... and `exp park usb=0` / `exp clear` put it back to the (sourced) default.
  parksink::clearAll();
  EXPECT_FALSE(parksink::active(Sink::Usb));
  EXPECT_EQ(parksink::overrideValue(Sink::Usb), -1);
}

TEST(ParkSinks, StatusLineSaysDefaultOff) {
  // `exp status` / `exp park` print this table; the line has to read `default=off`.
  char buf[1024];
  const size_t n = parksink::status(buf, sizeof(buf));
  EXPECT_GT(n, 0u);
  EXPECT_NE(std::strstr(buf, "exp park usb=off default=off"), nullptr) << buf;
  EXPECT_NE(std::strstr(buf, "exp park front=off default=off"), nullptr) << buf;
  EXPECT_NE(std::strstr(buf, "exp park radio=off default=off"), nullptr) << buf;  // r107 P6
  EXPECT_NE(std::strstr(buf, "exp park sd=off default=off"), nullptr) << buf;  // r118
}

TEST(ParkSinks, SinkNamesAreTheCommandTokens) {
  EXPECT_STREQ(parksink::name(Sink::Usb), "usb");
  EXPECT_STREQ(parksink::name(Sink::Front), "front");
  EXPECT_STREQ(parksink::name(Sink::Radio), "radio");
  EXPECT_STREQ(parksink::name(Sink::Sd), "sd");
}

TEST(ParkSinks, NothingHasMovedYetOnAFreshBoot) {
  // The `sd` / `radio` read-outs must say "never-run", not "ok", before a park happens.
  EXPECT_EQ(parksink::lastSdRelease(), parksink::Verdict::Never);
  EXPECT_EQ(parksink::lastSdRestore(), parksink::Verdict::Never);
  EXPECT_FALSE(parksink::sdPadsReleased());
  EXPECT_EQ(parksink::lastRadioCut(), parksink::Verdict::Never);
  EXPECT_FALSE(parksink::radioDeinited());
}

}  // namespace
