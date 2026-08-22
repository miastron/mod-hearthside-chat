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
    if (g_HsHttpServerPrivateKey.empty())
        return false;

    // Constant-time comparison -- the token backs an admin control surface,
    // so a length/early-exit timing leak is worth closing even though this
    // server binds loopback by default.
    if (token.size() != g_HsHttpServerPrivateKey.size())
        return false;

    unsigned char diff = 0;
    for (size_t i = 0; i < token.size(); ++i)
        diff |= static_cast<unsigned char>(token[i]) ^ static_cast<unsigned char>(g_HsHttpServerPrivateKey[i]);
    return diff == 0;
}
