#ifndef MOD_HS_TEXT_H
#define MOD_HS_TEXT_H

#include <cctype>
#include <string>

// Chat-text normalization shared by the two matchers that compare a player's
// message against an authored phrase: hs_reflex.cpp (one-word reflexes) and
// hs_grounded.cpp (grounded questions). Both had byte-identical private
// copies of all three (review item 17); a change to what counts as
// whitespace or as a trailing mark has to apply to both or the two matchers
// silently disagree about the same message.
//
// Header-only and free of AzerothCore includes: both files' Tests/ harnesses
// build standalone.
namespace HsText
{
    inline std::string Hs_ToLowerAscii(const std::string& s)
    {
        std::string out = s;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // Trims outer whitespace and collapses internal whitespace runs to a
    // single space. Punctuation is left alone; callers decide how much of it
    // to strip, since the reflex BotQuestion family needs to keep a literal
    // trailing '?' for the bare "bot?" case while the Plain family strips it
    // freely.
    inline std::string Hs_NormalizeWhitespace(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        bool lastWasSpace = true; // skips leading whitespace too
        for (char c : s)
        {
            if (std::isspace(static_cast<unsigned char>(c)))
            {
                if (!lastWasSpace)
                    out.push_back(' ');
                lastWasSpace = true;
            }
            else
            {
                out.push_back(c);
                lastWasSpace = false;
            }
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    // One trailing '?', '!' or '.', not a run of them.
    inline std::string Hs_StripOneTrailingMark(const std::string& s)
    {
        if (!s.empty())
        {
            char last = s.back();
            if (last == '?' || last == '!' || last == '.')
                return s.substr(0, s.size() - 1);
        }
        return s;
    }

    // Chat hyperlinks (|cAARRGGBB|Hitem:...|h[Name]|h|r) reduced to "[Name]",
    // which is how a player reads them and the form training rows use. Raw
    // markup reached the model through history on 2026-10-09.
    inline std::string Hs_StripChatLinks(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();)
        {
            if (s[i] == '|' && i + 1 < s.size())
            {
                char code = s[i + 1];
                if (code == 'c' && i + 10 <= s.size()) { i += 10; continue; }   // |cAARRGGBB
                if (code == 'r') { i += 2; continue; }
                if (code == 'h') { i += 2; continue; }
                if (code == 'H')
                {
                    size_t end = s.find("|h", i + 2);
                    if (end == std::string::npos)
                        break;
                    i = end + 2;
                    continue;
                }
            }
            out.push_back(s[i++]);
        }
        return out;
    }
}

#endif // MOD_HS_TEXT_H
