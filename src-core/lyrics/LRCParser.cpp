/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "LRCParser.h"
#include "PhonemeDictionary.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace lrc {

namespace {

bool AllDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// "mm:ss", "mm:ss.x", "mm:ss.xx", "mm:ss.xxx" (and ':' before the fraction, which some
// tools write). False when it is not a time at all; `negative` when it starts with '-'.
bool ParseStamp(const std::string& tag, int& ms, bool& negative) {
    negative = false;
    std::string t = tag;
    if (!t.empty() && t[0] == '-') {
        negative = true;
        t.erase(t.begin());
    }
    const size_t colon = t.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    const std::string minutes = t.substr(0, colon);
    std::string rest = t.substr(colon + 1);
    std::string fraction;
    const size_t dot = rest.find_first_of(".:");
    if (dot != std::string::npos) {
        fraction = rest.substr(dot + 1);
        rest = rest.substr(0, dot);
    }
    if (!AllDigits(minutes) || !AllDigits(rest) || rest.size() > 2 || (dot != std::string::npos && (!AllDigits(fraction) || fraction.size() > 3))) {
        return false;
    }
    const int seconds = static_cast<int>(std::strtol(rest.c_str(), nullptr, 10));
    if (seconds >= 60) {
        return false;
    }
    int fractionMS = 0;
    if (!fraction.empty()) {
        fractionMS = static_cast<int>(std::strtol(fraction.c_str(), nullptr, 10));
        fractionMS *= fraction.size() == 1 ? 100 : fraction.size() == 2 ? 10 : 1;
    }
    ms = static_cast<int>(std::strtol(minutes.c_str(), nullptr, 10)) * 60000 + seconds * 1000 + fractionMS;
    return true;
}

bool IsMetadataTag(const std::string& tag) {
    const size_t colon = tag.find(':');
    if (colon == std::string::npos || colon == 0) {
        return false;
    }
    return std::all_of(tag.begin(), tag.begin() + static_cast<std::ptrdiff_t>(colon), [](unsigned char c) { return std::isalpha(c) != 0 || c == '#'; });
}

} // namespace

ParsedLRC ParseLRCDetailed(const std::string& syncedLyrics) {
    ParsedLRC out;
    std::string source = syncedLyrics;
    if (source.rfind("\xEF\xBB\xBF", 0) == 0) {
        source.erase(0, 3);
        out.hadBom = true;
    }
    std::istringstream stream(source);
    std::string line;
    int number = 0;
    while (std::getline(stream, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        size_t pos = 0;
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
            ++pos;
        }
        if (pos >= line.size() || line[pos] != '[') {
            if (pos < line.size()) {
                out.issues.push_back({ "untimed-line", number, line });
            }
            continue;
        }
        // One or more tags at the start: "[00:12.00][01:30.00]text" is the same text twice.
        std::vector<int> times;
        bool metadataOnly = true;
        while (pos < line.size() && line[pos] == '[') {
            const size_t close = line.find(']', pos);
            if (close == std::string::npos) {
                out.issues.push_back({ "malformed-time", number, line.substr(pos) });
                pos = line.size();
                break;
            }
            const std::string tag = line.substr(pos + 1, close - pos - 1);
            pos = close + 1;
            int ms = 0;
            bool negative = false;
            if (ParseStamp(tag, ms, negative)) {
                metadataOnly = false;
                ++out.timestampCount;
                if (negative) {
                    out.issues.push_back({ "negative-time", number, "[" + tag + "]" });
                } else {
                    times.push_back(ms);
                }
            } else if (IsMetadataTag(tag)) {
                ++out.metadataCount;
                const std::string key = tag.substr(0, tag.find(':'));
                const std::string value = tag.substr(tag.find(':') + 1);
                if (key == "offset") {
                    char* end = nullptr;
                    const long offset = std::strtol(value.c_str(), &end, 10);
                    if (end == value.c_str()) {
                        out.issues.push_back({ "malformed-offset", number, "[" + tag + "]" });
                    } else {
                        out.offsetMS = static_cast<int>(offset);
                        out.hadOffset = true;
                    }
                }
            } else {
                metadataOnly = false;
                out.issues.push_back({ "malformed-time", number, "[" + tag + "]" });
            }
        }
        if (metadataOnly && times.empty()) {
            continue;
        }
        std::string text = pos < line.size() ? line.substr(pos) : std::string();
        // Trim a single leading space — typical LRC files use `[mm:ss.xx] text`.
        if (!text.empty() && text.front() == ' ') {
            text.erase(text.begin());
        }
        // Enhanced LRC word times "<mm:ss.xx>" are left out of the text.
        size_t angle;
        while ((angle = text.find('<')) != std::string::npos && text.find('>', angle) != std::string::npos) {
            int ms = 0;
            bool negative = false;
            const size_t close = text.find('>', angle);
            if (!ParseStamp(text.substr(angle + 1, close - angle - 1), ms, negative)) {
                break;
            }
            text.erase(angle, close - angle + 1);
            out.hadWordTimes = true;
        }
        for (int ms : times) {
            out.lines.push_back({ ms, text, number });
        }
    }
    // "[offset:+500]" moves every line 500 ms earlier, as LRC players read it.
    if (out.hadOffset && out.offsetMS != 0) {
        for (ParsedLRC::Line& l : out.lines) {
            l.timeMS -= out.offsetMS;
        }
        for (const ParsedLRC::Line& l : out.lines) {
            if (l.timeMS < 0) {
                out.issues.push_back({ "negative-time", l.number, "offset " + std::to_string(out.offsetMS) });
            }
        }
        out.lines.erase(std::remove_if(out.lines.begin(), out.lines.end(), [](const ParsedLRC::Line& l) { return l.timeMS < 0; }), out.lines.end());
    }
    return out;
}

std::vector<std::pair<int, std::string>> ParseLRC(const std::string& syncedLyrics) {
    std::vector<std::pair<int, std::string>> result;
    for (const ParsedLRC::Line& line : ParseLRCDetailed(syncedLyrics).lines) {
        result.push_back({ line.timeMS, line.text });
    }
    return result;
}

namespace {
// Replace each occurrence of `from` (multi-byte UTF-8 sequence) with `to`.
// Plain `std::string::replace`-driven loop — small inputs, doesn't justify
// pulling in a regex engine.
void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}
} // namespace

bool SanitizePhraseText(std::string& text) {
    // Common Unicode quote-equivalents that the LRCLIB / vendor
    // text often contains. Replace with ASCII so the downstream
    // XML write doesn't have to worry about escapes.
    ReplaceAll(text, "\xE2\x80\x99", "'");  // U+2019 RIGHT SINGLE QUOTATION MARK
    ReplaceAll(text, "\xC8\x98",     "'");  // U+0218 LATIN CAPITAL S WITH COMMA BELOW (LRC mojibake guard)
    ReplaceAll(text, "\xE2\x80\x9C", "\""); // U+201C LEFT DOUBLE QUOTATION MARK
    ReplaceAll(text, "\xE2\x80\x9D", "\""); // U+201D RIGHT DOUBLE QUOTATION MARK

    // Strip out characters that are illegal in phrase labels
    // (XML-attribute-hostile + the renderer's parser doesn't like them).
    text.erase(std::remove(text.begin(), text.end(), '"'), text.end());
    text.erase(std::remove(text.begin(), text.end(), '<'), text.end());
    text.erase(std::remove(text.begin(), text.end(), '>'), text.end());

    // Trim leading + trailing whitespace.
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(text.begin(), text.end(), notSpace);
    auto last = std::find_if(text.rbegin(), text.rend(), notSpace).base();
    if (first >= last) {
        text.clear();
    } else {
        text = std::string(first, last);
    }

    if (text.empty()) return false;

    PhonemeDictionary::InsertSpacesAfterPunctuation(text);
    return !text.empty();
}

} // namespace lrc
