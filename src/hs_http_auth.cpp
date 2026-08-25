#include "hs_http_auth.h"
#include "hs_config.h"

std::string Hs_ExtractBearerToken(const std::string& authorizationHeader)
{
    static const std::string kPrefix = "Bearer ";
    if (authorizationHeader.size() <= kPrefix.size())
        return "";
    if (authorizationHeader.compare(0, kPrefix.size(), kPrefix) != 0)
        return "";
    return authorizationHeader.substr(kPrefix.size());
}

bool Hs_ValidateBearerToken(const std::string& token)
{
    // Snapshot, not a direct read of the global. This runs on the HTTP
    // server thread; HttpServerPrivateKey live-reloads from the world thread
    // on `.reload config`, and the loop below indexes the string character by
    // character. A reassignment mid-loop is an out-of-bounds read of a freed
    // buffer on the one code path that decides whether a request is
    // authenticated. See hs_config.h's cross-thread section.
    const std::string key = Hs_ConfigString(g_HsHttpServerPrivateKey);
    if (key.empty())
        return false;

    // Constant-time comparison -- the token backs an admin control surface,
    // so a length/early-exit timing leak is worth closing even though this
    // server binds loopback by default.
    if (token.size() != key.size())
        return false;

    unsigned char diff = 0;
    for (size_t i = 0; i < token.size(); ++i)
        diff |= static_cast<unsigned char>(token[i]) ^ static_cast<unsigned char>(key[i]);
    return diff == 0;
}
