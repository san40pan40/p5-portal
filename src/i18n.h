#pragma once
/*
 * I18n — interface translations (Ukrainian and English, built into the firmware so the panel
 * works offline, e.g. during first setup). All strings live in i18n_strings.h.
 *
 * Usage:  T(MENU_STATUS)  -> "Статус" or "Status", depending on the current language.
 * The language switches instantly (no restart) and is stored in NVS by Config.
 */
#include <Arduino.h>

namespace I18n {

enum class Lang : uint8_t { UK = 0, EN = 1 };

// Ids of all strings, generated from i18n_strings.h
enum Str : uint16_t {
#define X(id, uk, en) id,
#include "i18n_strings.h"
#undef X
  STR_COUNT
};

void        setLang(Lang l);
Lang        lang();
const char *t(Str id);
bool        parseLang(const String &code, Lang &out);   // "uk" / "en"

}  // namespace I18n

// Short form for the pages: T(MENU_STATUS)
#define T(id) I18n::t(I18n::id)
