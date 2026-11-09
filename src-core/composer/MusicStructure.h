#pragma once

#include "composer/SongAnalysis.h"

#include <string>
#include <vector>

// The parts of a song read from its lyrics. The audio alone finds where the music
// changes, but not what each part is: a chorus is what is sung again, a verse is what
// is sung once, and what is played without words is an intro, an interlude or an
// ending. Deterministic and local; it never invents a time the alignment did not give.
namespace composer {

struct LyricAlignment;

struct SungLine {
    int startMS = 0;
    int endMS = 0;
    std::string text;
};

struct StructureResult {
    bool applied = false;
    std::string reason; // why not, when it was not applied
    int lines = 0;
    int choruses = 0;
    int verses = 0;
    int families = 0; // groups of lines sung more than once in separate places
    std::vector<std::string> notes;
};

[[nodiscard]] std::vector<SungLine> TimedLinesFrom(const LyricAlignment& alignment);

// The lowercase words of a line without accents or punctuation, for comparing two
// lines heard with small differences.
[[nodiscard]] std::vector<std::string> LineWords(const std::string& text);

// Lays the sections out again from the lines, inside the music the analysis found,
// and keeps every section a person corrected. Without at least one group of lines sung
// again in a separate place, nothing changes: an unrepeated text says nothing about
// which part is the chorus.
StructureResult RefineSectionsWithLyrics(SongAnalysis& analysis, const std::vector<SungLine>& lines);

} // namespace composer
