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
#include <utility>
#include <vector>

namespace lrc {

// Parse an `.lrc`-format synced-lyrics body (one or more lines
// shaped like `[mm:ss.xx] text…`) into a list of (millisecond,
// text) pairs in source order.
//
// Lines that don't open with `[` are skipped. Metadata tags like
// `[ti:Song Title]` (non-numeric prefix before the colon) are
// ignored. Timestamp formats accepted: `mm:ss`, `mm:ss.x`,
// `mm:ss.xx` (centiseconds), `mm:ss.xxx` (milliseconds). Empty
// trailing text is preserved as `("", time)` so callers can
// decide whether to drop it.
//
// Wx-free; called by both the desktop wx dialog
// (`src-ui-wx/sequencer/LRCLIBSearchDialog.cpp`) and the iPad
// bridge (`src-iPad/Bridge/XLLyricsImport.mm`).
std::vector<std::pair<int, std::string>> ParseLRC(const std::string& syncedLyrics);

// The same, keeping what went wrong. Lines come one per timestamp, so
// "[00:12.00][01:30.00]text" gives two; a UTF-8 BOM and CR line ends are
// accepted; "[offset:+500]" moves every line 500 ms earlier, as LRC players
// read it. Nothing is dropped silently: a tag that is not a valid time, a
// negative time and a non-empty line with no tag are each an issue.
struct ParsedLRC {
    struct Line {
        int timeMS = 0;
        std::string text;
        int number = 0; // 1-based line of the file
    };
    struct Issue {
        std::string code; // "malformed-time", "negative-time", "malformed-offset", "untimed-line"
        int line = 0;
        std::string text;
    };
    std::vector<Line> lines; // in the file's order
    std::vector<Issue> issues;
    int timestampCount = 0;
    int metadataCount = 0;
    int offsetMS = 0;
    bool hadOffset = false;
    bool hadBom = false;
    bool hadWordTimes = false;
};
ParsedLRC ParseLRCDetailed(const std::string& syncedLyrics);

// Apply the small text-cleanup pass the desktop runs before
// adding a phrase to a timing layer:
//   - Replace common Unicode quotes with ASCII (' " ").
//   - Strip remaining double-quotes and `<`/`>` (illegal in
//     phrase labels — they trip the XML save path).
//   - Run `PhonemeDictionary::InsertSpacesAfterPunctuation` so
//     adjacent words that are stuck to commas / periods break
//     into separate words on the breakdown pass.
//
// Mutates `text` in place. Returns true if the resulting string
// has any non-whitespace content (i.e. is worth writing).
bool SanitizePhraseText(std::string& text);

} // namespace lrc
