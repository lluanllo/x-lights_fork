#include "composer/ComposerText.h"

namespace composer::text {

std::vector<char32_t> Decode(const std::string& utf8) {
    std::vector<char32_t> out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char lead = static_cast<unsigned char>(utf8[i]);
        size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 0;
        bool valid = length > 0 && i + length <= utf8.size();
        for (size_t k = 1; valid && k < length; ++k) {
            valid = (static_cast<unsigned char>(utf8[i + k]) & 0xC0) == 0x80;
        }
        if (!valid) {
            // A stray byte of legacy 8-bit text is read as Latin-1, which is what it
            // most likely was, rather than dropped.
            out.push_back(static_cast<char32_t>(lead));
            ++i;
            continue;
        }
        char32_t c = length == 1 ? lead : length == 2 ? (lead & 0x1F) : length == 3 ? (lead & 0x0F) : (lead & 0x07);
        for (size_t k = 1; k < length; ++k) {
            c = (c << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        }
        out.push_back(c);
        i += length;
    }
    return out;
}

std::string Encode(char32_t c) {
    std::string out;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}

std::string Encode(const std::vector<char32_t>& codePoints) {
    std::string out;
    for (char32_t c : codePoints) {
        out += Encode(c);
    }
    return out;
}

char32_t ToLower(char32_t c) {
    if (c >= U'A' && c <= U'Z') {
        return c + 32;
    }
    // Latin-1 capitals, except the multiplication sign.
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) {
        return c + 32;
    }
    // Latin Extended-A alternates capital and small letters.
    if (c >= 0x100 && c <= 0x17F && (c % 2) == 0 && c != 0x138) {
        return c + 1;
    }
    return c;
}

bool IsLetter(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7);
}

bool IsDigit(char32_t c) {
    return c >= U'0' && c <= U'9';
}

bool IsSpace(char32_t c) {
    return c == U' ' || c == U'\t' || c == U'\r' || c == U'\n' || c == 0xA0 || c == 0x2009 || c == 0x202F;
}

bool IsWordChar(char32_t c) {
    return IsLetter(c) || IsDigit(c) || c == U'\'' || c == 0x2019;
}

std::string LowerKey(const std::string& utf8) {
    std::vector<char32_t> cps = Decode(utf8);
    for (char32_t& c : cps) {
        c = ToLower(c);
    }
    return Encode(cps);
}

std::string Fold(const std::string& utf8) {
    std::string out;
    for (char32_t c : Decode(utf8)) {
        c = ToLower(c);
        switch (c) {
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: c = U'a'; break;
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: c = U'e'; break;
        case 0xEC: case 0xED: case 0xEE: case 0xEF: c = U'i'; break;
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: c = U'o'; break;
        case 0xF9: case 0xFA: case 0xFB: case 0xFC: c = U'u'; break;
        default: break;
        }
        out += Encode(c);
    }
    return out;
}

std::string Trim(const std::string& utf8) {
    const std::vector<char32_t> cps = Decode(utf8);
    size_t first = 0;
    size_t last = cps.size();
    while (first < last && IsSpace(cps[first])) {
        ++first;
    }
    while (last > first && IsSpace(cps[last - 1])) {
        --last;
    }
    return Encode(std::vector<char32_t>(cps.begin() + static_cast<std::ptrdiff_t>(first), cps.begin() + static_cast<std::ptrdiff_t>(last)));
}

} // namespace composer::text
