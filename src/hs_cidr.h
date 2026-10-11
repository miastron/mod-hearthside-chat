#ifndef MOD_HS_CIDR_H
#define MOD_HS_CIDR_H

#include <cstdint>
#include <string>

// IPv4 allow-list matching for the HTTP API (HearthsideChat.HttpAllowedNetworks).
// Dependency-free so Tests/ can build it.

// "a.b.c.d" to a host-order address. False on anything else.
inline bool Hs_ParseIpv4(const std::string& text, uint32_t& out)
{
    uint32_t addr = 0;
    size_t   pos = 0;
    for (int octet = 0; octet < 4; ++octet)
    {
        size_t   start = pos;
        uint32_t value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9' && pos - start < 3)
            value = value * 10 + static_cast<uint32_t>(text[pos++] - '0');
        if (pos == start || value > 255)
            return false;
        addr = (addr << 8) | value;
        if (octet < 3)
        {
            if (pos >= text.size() || text[pos] != '.')
                return false;
            ++pos;
        }
    }
    if (pos != text.size())
        return false;
    out = addr;
    return true;
}

// Is `ip` inside any entry of `list`: comma-separated IPv4 addresses or
// a.b.c.d/N ranges, spaces ignored. An unparsable entry matches nothing, and
// an empty list allows nobody. An IPv4-mapped IPv6 address ("::ffff:a.b.c.d",
// what a dual-stack listener reports) is matched as the IPv4 address.
inline bool Hs_IpAllowed(std::string ip, const std::string& list)
{
    if (ip.compare(0, 7, "::ffff:") == 0)
        ip.erase(0, 7);
    uint32_t addr;
    if (!Hs_ParseIpv4(ip, addr))
        return false;

    size_t pos = 0;
    while (pos <= list.size())
    {
        size_t end = list.find(',', pos);
        if (end == std::string::npos)
            end = list.size();
        std::string entry;
        for (size_t i = pos; i < end; ++i)
            if (list[i] != ' ' && list[i] != '\t')
                entry += list[i];
        pos = end + 1;

        if (entry.empty())
            continue;
        unsigned bits = 32;
        size_t   slash = entry.find('/');
        if (slash != std::string::npos)
        {
            std::string len = entry.substr(slash + 1);
            if (len.empty() || len.size() > 2 || len.find_first_not_of("0123456789") != std::string::npos)
                continue;
            bits = static_cast<unsigned>(std::stoul(len));
            if (bits > 32)
                continue;
            entry.erase(slash);
        }
        uint32_t net;
        if (!Hs_ParseIpv4(entry, net))
            continue;
        uint32_t mask = bits == 0 ? 0 : ~uint32_t(0) << (32 - bits);
        if ((addr & mask) == (net & mask))
            return true;
    }
    return false;
}

#endif // MOD_HS_CIDR_H
