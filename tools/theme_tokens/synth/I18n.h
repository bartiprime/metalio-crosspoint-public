// HOST STUB for tools/theme_tokens/check_theme_syntax.sh. NOT part of the firmware
// build: it lives under tools/ and is only ever reached through that script's -I.
// Its job is to let a theme .cpp be type-checked on the host against the REAL
// BaseTheme.h; every renderer signature below is copied from the real header.
#pragma once
enum class StrId { STR_NO_OPEN_BOOK, STR_START_READING };
class I18n { public: static I18n& getInstance(); const char* get(StrId) const; };
#define tr(id) I18n::getInstance().get(StrId::id)
