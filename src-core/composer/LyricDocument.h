#pragma once

#include "composer/ComposerTypes.h"

#include <string>
#include <vector>

// The lyrics of a song as a person wrote them: blocks, lines and words, with any times
// the source already had. Nothing here guesses a time or a pronunciation, and the
// text of every line is kept exactly as given.
namespace composer {

inline constexpr int LYRIC_DOCUMENT_SCHEMA_VERSION = 1;

// Only what the person wrote: a block is a chorus because someone tagged it so.
enum class LyricBlockKind {
    Unlabeled,
    Intro,
    Verse,
    PreChorus,
    Chorus,
    Bridge,
    Instrumental,
    Outro,
    Other // tagged with a name none of the above matches; `label` keeps it
};

struct LyricWord {
    std::string text; // as written, punctuation included
    std::string key;  // lowercase, without surrounding punctuation: what is pronounced
};

struct LyricLine {
    // Stable while the line's own text does not change: its lowercase key and how
    // many identical lines come before it. Inserting a verse elsewhere keeps it.
    std::string id;
    std::string text;
    std::vector<LyricWord> words;
    std::string voice;
    bool instrumental = false; // "(instrumental)", "♪", or a blank timed line
    int startMS = -1;          // -1: the source had no time
    int endMS = -1;
    std::string timingSource; // "lrc", "srt", "track", or empty
};

struct LyricBlock {
    LyricBlockKind kind = LyricBlockKind::Unlabeled;
    std::string label; // the tag as written, "" when there was none
    int number = 0;    // "[Estrofa 2]": 2
    std::vector<LyricLine> lines;
};

struct LyricDocument {
    int schemaVersion = LYRIC_DOCUMENT_SCHEMA_VERSION;
    std::string format; // "text", "lrc", "srt", "track"
    std::string language = "und"; // "es", "en" or "und"
    double languageConfidence = 0;
    std::vector<LyricBlock> blocks;
    std::vector<std::string> warnings;

    [[nodiscard]] std::vector<const LyricLine*> Lines() const;
    [[nodiscard]] bool Empty() const;
    [[nodiscard]] bool HasTimes() const;
};

// Plain UTF-8 text. "[Estribillo]"-style tags, "[Voz: Ana]" and blank lines are
// optional; without them every paragraph is an unlabeled block.
[[nodiscard]] LyricDocument ParsePlainLyrics(const std::string& text);
// Synced lyrics, through xLights' own LRC reader.
[[nodiscard]] LyricDocument ParseLrcLyrics(const std::string& text);
// SubRip subtitles, through xLights' own SRT reader.
[[nodiscard]] LyricDocument ParseSrtLyrics(const std::string& text);
// The phrases (and words, when present) of an xLights lyrics track.
[[nodiscard]] LyricDocument LyricsFromTrack(const TimingTrackSpec& track);
// By content: an LRC timestamp or an SRT arrow decide, otherwise plain text.
[[nodiscard]] LyricDocument ParseLyrics(const std::string& text);

[[nodiscard]] std::vector<LyricWord> SplitLyricWords(const std::string& line);
[[nodiscard]] std::string LyricKey(const std::string& line);
[[nodiscard]] const char* BlockKindId(LyricBlockKind kind);

struct LanguageGuess {
    std::string code = "und";
    double confidence = 0;
};

// Common words and Spanish letters, nothing more: enough to tell Spanish from English
// lyrics, and to say "und" rather than guess on anything else.
[[nodiscard]] LanguageGuess DetectLyricLanguage(const std::string& text);

} // namespace composer
