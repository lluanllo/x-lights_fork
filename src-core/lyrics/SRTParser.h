#pragma once

/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include <string>
#include <vector>

namespace srt {

struct Entry {
    long startMS = 0;
    long endMS = 0;
    std::string text; // the entry's lines joined with a space
};

// Parse SubRip subtitles (`1`, `00:00:06,580 --> 00:00:08,580`, text lines, blank
// line) in file order. Entries whose times cannot be read come back with 0 and 0,
// so callers can skip them. Wx-free; used by SequenceFile::ProcessSRT and the
// Show Composer's lyric import.
std::vector<Entry> ParseSRT(std::vector<std::string> lines);

} // namespace srt
