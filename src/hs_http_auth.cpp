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

    // Constant-time comparison: the token backs an admin control surface,
    // so a length/early-exit timing leak is worth closing even though this
    // server binds loopback by default.
    //
    // Review item 9: the length test used to be an early `return false`,
    // directly under this comment. That short-circuit is itself the leak it
    // claims to close -- a wrong-length guess returned measurably sooner
    // than a right-length, wrong-content one, handing an attacker the
    // secret's length one probe at a time, which is the one fact the loop
    // below would otherwise never reveal. So the length difference is folded
    // into the same accumulator instead of branching on it, and the loop
    // runs key.size() times regardless of what was sent.
    //
    // The empty-token case still returns early: it carries no information
    // about the key (an empty Authorization header is distinguishable
    // without timing at all) and the modulo below needs a non-zero divisor.
    if (token.empty())
        return false;

    // size_t, not unsigned char: the accumulator starts as the *size* xor,
    // and truncating that to a byte would make a length difference of
    // exactly 256 fold to zero (a 1-byte token against a 257-byte key of the
    // same repeated character would then authenticate).
    size_t diff = token.size() ^ key.size();
    for (size_t i = 0; i < key.size(); ++i)
    {
        // Wrapping index: for the equal-length case this is the plain
        // token[i] comparison. For any other length the bytes compared are
        // meaningless, but `diff` is already non-zero from the size fold, so
        // the result is fixed and only the timing is being held constant.
        unsigned char sent = static_cast<unsigned char>(token[i % token.size()]);
        diff |= static_cast<size_t>(sent ^ static_cast<unsigned char>(key[i]));
    }
    return diff == 0;
}
