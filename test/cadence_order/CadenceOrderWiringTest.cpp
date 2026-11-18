// CAD2 wiring: BOTH cadence pickers consume the one display-order table.
//
// The mapping itself is proven in CadenceOrderTest. What stays checkable only as
// text is that each surface actually reads it -- a picker that keeps its own copy of
// the order is exactly how Settings > Reader and the in-book More menu drift apart,
// which is the failure CAD2 exists to prevent.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readFile(const std::string& relativePath) {
  const std::string path = std::string(CP_REPO_ROOT) + "/" + relativePath;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << "cannot read " << path;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

// Settings > Reader: one builder, both rows, no positional Enum descriptor left.
TEST(CadenceOrderWiring, SettingsRowsGoThroughTheSharedBuilder) {
  const std::string list = readFile("src/SettingsList.cpp");
  EXPECT_TRUE(contains(list, "buildCadenceSetting(StrId::STR_REFRESH_FREQ"));
  EXPECT_TRUE(contains(list, "buildCadenceSetting(StrId::STR_QUICK_REFRESH_PAGES"));
  // The old positional rows would store the display index in the settings byte.
  EXPECT_FALSE(contains(list, "&CrossPointSettings::refreshFrequency,\n"));
  EXPECT_TRUE(contains(readFile("src/SettingsList.h"), "cadenceorder::storedForDisplay"));
  EXPECT_TRUE(contains(readFile("src/SettingsList.h"), "cadenceorder::positionOf"));
}

// The in-book More menu: the same table, and no private copy of the order.
TEST(CadenceOrderWiring, ReaderMenuReadsTheSameTable) {
  const std::string menu = readFile("src/activities/reader/EpubReaderMenuActivity.cpp");
  EXPECT_TRUE(contains(menu, "cadenceorder::positionOf"));
  EXPECT_TRUE(contains(menu, "cadenceorder::storedForDisplay"));
  EXPECT_TRUE(contains(menu, "cadenceorder::kLabels"));
  EXPECT_FALSE(contains(menu, "kCadenceLabels")) << "the menu must not keep its own order table";
}

// Persistence: the rows are dynamic now, so the two bytes must still be written and
// read by hand under their old keys -- a save that dropped them would reset the
// operator's cadence on the next boot.
TEST(CadenceOrderWiring, SettingsBytesKeepTheirKeys) {
  const std::string settings = readFile("src/CrossPointSettings.cpp");
  EXPECT_TRUE(contains(settings, "doc[\"refreshFrequency\"] = refreshFrequency;"));
  EXPECT_TRUE(contains(settings, "doc[\"quickRefreshPages\"] = quickRefreshPages;"));
  EXPECT_TRUE(contains(settings, "doc[\"refreshFrequency\"] | refreshFrequency"));
  EXPECT_TRUE(contains(settings, "doc[\"quickRefreshPages\"] | quickRefreshPages"));
}

// The enum itself must not move: CAD2 is ordering only.
TEST(CadenceOrderWiring, RefreshEnumIsUnrenumbered) {
  const std::string header = readFile("src/CrossPointSettings.h");
  EXPECT_TRUE(contains(header, "REFRESH_1 = 0,"));
  EXPECT_TRUE(contains(header, "REFRESH_5 = 1,"));
  EXPECT_TRUE(contains(header, "REFRESH_10 = 2,"));
  EXPECT_TRUE(contains(header, "REFRESH_15 = 3,"));
  EXPECT_TRUE(contains(header, "REFRESH_30 = 4,"));
  EXPECT_TRUE(contains(header, "REFRESH_NEVER = 5,"));
  EXPECT_TRUE(contains(header, "REFRESH_60 = 6,"));
}
