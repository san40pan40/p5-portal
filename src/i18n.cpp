#include "i18n.h"

namespace I18n {

// A const array of pointers to string literals: it stays in flash
static const char *const TEXT[] = {
#define X(id, text) text,
#include "i18n_strings.h"
#undef X
};

static_assert(sizeof(TEXT) / sizeof(TEXT[0]) == STR_COUNT, "text table out of sync");

const char *t(Str id) { return id < STR_COUNT ? TEXT[id] : ""; }

}  // namespace I18n
