#pragma once

// Persistence for the debug-mode switch.
//
// Deliberately NOT a CrossPointSettings field: that struct persists through the keyed SettingInfo
// table, where a wrong entry fails silently (the setting just never round-trips) and every new row
// needs an i18n string. This is a one-byte flag file instead -- no schema, no translations, and it
// survives the power cycle that every flash requires.
namespace debugflag {

bool enabled();
void setEnabled(bool on);

}  // namespace debugflag