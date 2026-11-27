#include "i18n.h"

namespace I18n {

// Both tables are const arrays of pointers to string literals, so they stay in flash.
static const char *const UK[] = {
#define X(id, uk, en) uk,
#include "i18n_strings.h"
#undef X
};

static const char *const EN[] = {
#define X(id, uk, en) en,
#include "i18n_strings.h"
#undef X
};

static_assert(sizeof(UK) / sizeof(UK[0]) == STR_COUNT, "Ukrainian table out of sync");
static_assert(sizeof(EN) / sizeof(EN[0]) == STR_COUNT, "English table out of sync");

static Lang s_lang = Lang::UK;

void setLang(Lang l) { s_lang = l; }
Lang lang()          { return s_lang; }

const char *t(Str id) {
  if (id >= STR_COUNT) return "";
  return (s_lang == Lang::EN ? EN : UK)[id];
}

bool parseLang(const String &code, Lang &out) {
  if (code == "uk") { out = Lang::UK; return true; }
  if (code == "en") { out = Lang::EN; return true; }
  return false;
}

}  // namespace I18n
