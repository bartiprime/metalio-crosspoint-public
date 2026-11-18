#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>

// Top-level groups on the Settings screen, in display order. End doubles as
// "no section" where one is optional.
//
// The order below IS the order the Settings list paints: SettingsListActivity
// walks SETTINGS_SECTION_TITLES / SECTION_ICONS by this enum's value, so a new
// section needs its title and icon at the same index. r99.9 (operator request
// A1): Display, Reader, Controls, Library, Connectivity, System -- reading
// preferences first, the device record last, with Debug (a row appended past
// the enum) closing the list.
enum class SettingsSection : uint8_t { Display, Reader, Controls, Library, Network, General, End };

inline constexpr size_t SETTINGS_SECTION_COUNT = static_cast<size_t>(SettingsSection::End);

inline constexpr StrId SETTINGS_SECTION_TITLES[SETTINGS_SECTION_COUNT] = {
    StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,       StrId::STR_CAT_CONTROLS,
    StrId::STR_LIBRARY,     StrId::STR_SETTINGS_NETWORK, StrId::STR_SETTINGS_GENERAL,
};
