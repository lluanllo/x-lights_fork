/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "SRTParser.h"

#include <cstdlib>

#include "../utils/string_utils.h"

namespace srt {

namespace {

std::vector<std::string> SplitString(const std::string& s, char delim)
{
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= s.size()) {
        auto pos = s.find(delim, start);
        result.push_back(s.substr(start, pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return result;
}

std::string ReadSRTLine(const std::vector<std::string>& lines, size_t& idx, int linenum, long& startMS, long& endMS)
{
    startMS = 0;
    endMS = 0;

    if (idx >= lines.size()) return "";

    int l = std::strtol(lines[idx++].c_str(), nullptr, 10);
    while (idx < lines.size() && l < linenum) {
        l = std::strtol(lines[idx++].c_str(), nullptr, 10);
    }
    if (l > linenum) return "";
    if (idx >= lines.size()) return "";

    //00:00:06,580 --> 00:00:08,580
    std::string times = lines[idx++];
    if (times.find("-->") != std::string::npos)
    {
        auto c1 = SplitString(times, ':');
        if (c1.size() == 5) {
            int sH = std::strtol(c1[0].c_str(), nullptr, 10);
            int sM = std::strtol(c1[1].c_str(), nullptr, 10);
            auto c2 = SplitString(c1[2], ',');
            if (c2.size() == 2) {
                int sS = std::strtol(c2[0].c_str(), nullptr, 10);
                auto c3 = SplitString(c2[1], ' ');
                if (c3.size() == 3) {
                    int sMS = std::strtol(c3[0].c_str(), nullptr, 10);
                    int eH = std::strtol(c3[2].c_str(), nullptr, 10);
                    int eM = std::strtol(c1[3].c_str(), nullptr, 10);
                    auto c4 = SplitString(c1[4], ',');
                    if (c4.size() == 2) {
                        int eS = std::strtol(c4[0].c_str(), nullptr, 10);
                        int eMS_val = std::strtol(c4[1].c_str(), nullptr, 10);
                        startMS = sH * 3600000 + sM * 60000 + sS * 1000 + sMS;
                        endMS = eH * 3600000 + eM * 60000 + eS * 1000 + eMS_val;
                    }
                }
            }
        }
    }

    if (idx >= lines.size()) return "";

    std::string result;
    while (idx < lines.size() && !lines[idx].empty()) {
        if (!result.empty()) result += " ";
        result += Trim(lines[idx]);
        idx++;
    }
    if (idx < lines.size()) idx++; // skip blank line
    return result;
}

} // namespace

std::vector<Entry> ParseSRT(std::vector<std::string> lines)
{
    // A file saved on Windows and read elsewhere keeps its carriage returns, and a
    // line holding only "\r" would not end its entry.
    for (auto& line : lines) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
    }
    std::vector<Entry> entries;
    size_t idx = 0;
    int linenum = 1;
    while (idx < lines.size()) {
        Entry entry;
        entry.text = ReadSRTLine(lines, idx, linenum++, entry.startMS, entry.endMS);
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace srt
