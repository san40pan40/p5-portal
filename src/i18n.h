#pragma once
/*
 * I18n — the UI texts (English only). All texts are in i18n_strings.h, so they stay in one place.
 * Usage: T(MENU_STATUS) -> "Status".
 */
#include <Arduino.h>

namespace I18n {

// Ids of all strings, made from i18n_strings.h
enum Str : uint16_t {
#define X(id, text) id,
#include "i18n_strings.h"
#undef X
  STR_COUNT
};

const char *t(Str id);

}  // namespace I18n

// Short form for the pages: T(MENU_STATUS)
#define T(id) I18n::t(I18n::id)
