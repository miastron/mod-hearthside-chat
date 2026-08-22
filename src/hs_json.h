#ifndef MOD_HS_JSON_H
#define MOD_HS_JSON_H

// ---------------------------------------------------------------------------
// Bundled nlohmann/json isolation wrapper.
//
// This module vendors its own copy of nlohmann/json (deps/nlohmann/json.hpp).
// Because the library is header-only, including it unmodified would reopen
// the same `nlohmann` namespace already opened by other modules in this
// worldserver binary (mod-playerbots-characters bundles its own copy too) and
// risk ODR clashes. Relocate it into `hs_nlohmann` for the duration of the
// include only, the same technique used for cpp-httplib (see hs_llm.cpp).
//
// Usage: include "hs_json.h" instead of <nlohmann/json.hpp> and refer to the
// JSON type as `hs_json`.
// ---------------------------------------------------------------------------

#define nlohmann hs_nlohmann
#include <nlohmann/json.hpp>
#undef nlohmann

using hs_json = hs_nlohmann::json;

#endif // MOD_HS_JSON_H
