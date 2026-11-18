#pragma once

#include <cstddef>
#include <cstdint>

#include "I18nKeys.h"
/**
 * Internationalization (i18n) system for CrossPoint Reader
 */

class I18n {
 public:
  static I18n& getInstance();

  // Disable copy
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Get localized string by ID
  const char* get(StrId id) const;

  const char* operator[](StrId id) const { return get(id); }

  Language getLanguage() const { return _language; }
  void setLanguage(Language lang);
  const char* getLanguageName(Language lang) const;
  static Language languageFromCode(const char* code);

  // Get all unique characters used in a specific language
  // Returns a sorted string of unique characters
  static const char* getCharacterSet(Language lang);

 private:
  I18n() : _language(Language::EN) {}

  Language _language;
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()

/**
 * Fill `out` from a translated template WITHOUT ever handing that template to printf.
 *
 * A translation is text, not a format string: one that contains a bare '%' -- "100%", "% Complete" --
 * used as a printf format makes snprintf read a variadic argument that was never passed (undefined
 * behaviour, usually a fault). This substitutes the numbers for the template's numeric verbs in order
 * and copies every other byte verbatim, including a '%' that is not followed by a verb. Values are
 * always rendered unsigned, so a template that says %d and an unsigned value cannot disagree.
 *
 * Only numeric verbs (%d, %u) are consumed; `%%` is a literal percent. If the template carries more
 * verbs than `count`, the surplus is left as written (a wrong-looking line, never a fault).
 */
char* i18nFormatUnsigned(char* out, const size_t n, const char* tmpl, const unsigned* values, const int count);
