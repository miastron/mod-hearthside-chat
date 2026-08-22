#ifndef MOD_HS_HTTP_AUTH_H
#define MOD_HS_HTTP_AUTH_H

#include <string>

// PLAN.md §4.19 / §7 step 19. mod-playerbots-characters' HTTP server backs
// its bearer token with a per-account OTP-exchange/AES-encrypted session
// system -- real machinery for a genuine problem PBC has (many web-frontend
// users, each with their own WoW account, that requests need to be
// attributed to). This module has no such problem: it is a single-operator
// server-admin tool with one shared secret, matching PLAN.md's own
// "HttpServerPrivateKey **required**... a bearer token can back up the
// firewall rule" framing (the same shape as llama-server's own --api-key).
// So the check here is a direct constant-time comparison against
// HearthsideChat.HttpServerPrivateKey, not PBC's account-attribution system
// -- a deliberate simplification, not a partial port.

// Extracts the token from an "Authorization: Bearer <token>" header value.
// Returns "" if the header is missing or malformed.
std::string Hs_ExtractBearerToken(const std::string& authorizationHeader);

// Constant-time comparison against g_HsHttpServerPrivateKey (hs_config.h).
// Always false if the configured key is empty -- an operator must set a key
// before the server does anything but refuse every request, matching PBC's
// own "private key is required for the authorization layer" refusal.
bool Hs_ValidateBearerToken(const std::string& token);

#endif // MOD_HS_HTTP_AUTH_H
