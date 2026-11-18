#include "I18n.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "I18nStrings.h"

using namespace i18n_strings;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  // Use generated helper function - no hardcoded switch needed!
  const LangStrings lang = getLanguageStrings(_language);

  // If bit 15 of the offset is set, apply the offset to the English lookup table
  const uint16_t off = lang.offsets[index];
  if (off & 0x8000) return STRINGS_EN_DATA + (off & 0x7FFF);
  return lang.data + off;
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

Language I18n::languageFromCode(const char* code) {
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(code, LANGUAGE_CODES[i]) == 0) return static_cast<Language>(i);
  }
  return Language::EN;
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}

char* i18nFormatUnsigned(char* out, const size_t n, const char* tmpl, const unsigned* values, const int count) {
  if (out == nullptr || n == 0) return out;
  out[0] = '\0';
  if (tmpl == nullptr) return out;
  const int have = values != nullptr ? count : 0;

  std::string built;
  built.reserve(strlen(tmpl) + 24);
  int used = 0;
  for (size_t i = 0; tmpl[i] != '\0'; ++i) {
    if (tmpl[i] != '%') {
      built.push_back(tmpl[i]);
      continue;
    }
    const char verb = tmpl[i + 1];
    if ((verb == 'u' || verb == 'd') && used < have) {
      char num[12];
      std::snprintf(num, sizeof(num), "%u", values[used++]);
      built += num;
      ++i;
    } else if (verb == '%') {
      built.push_back('%');
      ++i;
    } else {
      // A bare '%' (or an unknown verb) in the translation: copy it as text. It can never make the
      // final snprintf read an argument.
      built.push_back('%');
    }
  }
  std::snprintf(out, n, "%s", built.c_str());
  return out;
}
