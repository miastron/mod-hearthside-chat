#ifndef MOD_HS_LOG_H
#define MOD_HS_LOG_H

// The module's four logger categories, as constants rather than 77 hand-typed
// string literals across 15 files (review item 18).
//
// AzerothCore resolves logger names by walking dot-separated parents, so
// `Logger.module` in the worldserver conf catches all four while an operator
// can still silence per-reply traces without losing backend-outage errors --
// see the LOGGING block at the top of conf/mod_hearthside_chat.conf.dist.
// That parent-walk is also why a typo never fails loudly: a message logged to
// "module.hearthsde" simply routes to the root appender instead, which is
// what makes a literal the wrong thing to type by hand at every call site.
//
// Header-only and dependency-free; include it wherever LOG_* is called.
inline constexpr char const* kHsLog          = "module.hearthside";
inline constexpr char const* kHsLogChat      = "module.hearthside.chat";
inline constexpr char const* kHsLogGenerator = "module.hearthside.generator";
inline constexpr char const* kHsLogLlm       = "module.hearthside.llm";

#endif // MOD_HS_LOG_H
