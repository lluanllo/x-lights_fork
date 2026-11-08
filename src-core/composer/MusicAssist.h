#pragma once

#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class IServiceSettingsStore;

// Optional help from outside the program: lyrics from LRCLIB, a transcription from
// Groq Whisper and a description of the song from Gemini. Each one is asked only
// when a person allows it, its answer is checked before anything uses it, and
// without a network or a key everything else keeps working as before.
//
// Nothing here writes a key anywhere: keys go in request headers, and every message
// that could reach a log goes through RedactSecrets first.
namespace composer {

inline constexpr const char* MUSIC_ASSIST_VERSION = "1.0.0";

// ------------------------------------------------------------------ transport

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    struct Part {
        std::string name;
        std::string value;    // a plain field...
        std::string filePath; // ...or a file sent from disk
        std::string mimeType;
    };
    std::vector<Part> multipart; // sent as multipart/form-data when not empty
    int timeoutS = 60;
};

struct HttpResponse {
    int status = 0;                            // 0: no answer at all
    std::map<std::string, std::string> headers; // names in lowercase
    std::string body;
    std::string transportError;
};

class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    [[nodiscard]] virtual HttpResponse Send(const HttpRequest& request) = 0;
};

// libcurl, the way the rest of xLights talks to the network.
[[nodiscard]] std::unique_ptr<IHttpClient> MakeCurlHttpClient();

enum class AssistError {
    None,
    NoCredentials,
    NoConsent,
    FileTooLarge,  // refused before sending anything
    UnsupportedFile,
    Unauthorized,  // 401: the key is wrong or expired
    Forbidden,     // 403: the key cannot use that model or service
    TooLarge,      // 413: the service refused the size
    RateLimited,   // 429, after the retries allowed
    ServerError,   // 5xx, after the retries allowed
    Network,
    NotFound,
    BadResponse    // an answer that does not have the shape asked for
};

struct AssistFailure {
    AssistError error = AssistError::None;
    int status = 0;
    int retryAfterS = -1;
    int attempts = 0;
    std::string message; // already redacted
};

struct RetryPolicy {
    int maxAttempts = 3;
    int maxWaitS = 20; // a Retry-After longer than this is not waited for
    std::function<void(int seconds)> sleep; // empty: the real clock
};

// 429 and 5xx and network failures are tried again, at most maxAttempts times in all;
// a 4xx is never repeated. Nothing loops for ever.
[[nodiscard]] HttpResponse SendWithRetry(IHttpClient& client, const HttpRequest& request, const RetryPolicy& policy, AssistFailure& failure);
[[nodiscard]] AssistError ErrorOfStatus(int status);
[[nodiscard]] const char* AssistErrorId(AssistError error);

// ----------------------------------------------------------------- credentials

struct ApiKey {
    std::string value;
    std::string source; // "secure-store", "environment" or "none"; never the key
};

// The secure store first (the same entry xLights' AI settings use for Gemini), then
// the environment variable. `store` may be null.
[[nodiscard]] ApiKey ResolveApiKey(const IServiceSettingsStore* store, const std::string& service, const std::string& environmentVariable);
[[nodiscard]] std::string SecretStoreName(const std::string& service);
// Every key-looking run of characters, and the given keys, replaced by "***".
[[nodiscard]] std::string RedactSecrets(const std::string& text, const std::vector<std::string>& keys = {});

// -------------------------------------------------------------------- LRCLIB

struct LrclibQuery {
    std::string track;
    std::string artist;
    std::string album;
    double durationS = 0; // 0: unknown
};

struct LrclibRecord {
    long long id = 0;
    std::string trackName;
    std::string artistName;
    std::string albumName;
    double durationS = 0;
    bool instrumental = false;
    bool hasWordSync = false;
    std::string plainLyrics;
    std::string syncedLyrics;
    std::string lyricsfile;
};

struct LrclibMatch {
    LrclibRecord record;
    bool titleMatches = false;
    bool artistMatches = false;
    bool albumMatches = false; // true when no album was asked for
    bool durationMatches = false; // within 2 s, or no duration to compare
    double durationDiffS = -1;
    double score = 0;
    [[nodiscard]] bool Plausible() const { return titleMatches && artistMatches && durationMatches; }
};

struct LrclibResult {
    std::vector<LrclibMatch> matches; // best first; a person picks one
    std::string via;                  // "get", "search" or ""
    AssistFailure failure;
};

[[nodiscard]] bool ParseLrclibRecord(const std::string& body, LrclibRecord& record, std::string& error);
[[nodiscard]] bool ParseLrclibSearch(const std::string& body, std::vector<LrclibRecord>& records, std::string& error);
[[nodiscard]] LrclibMatch CheckLrclibMatch(const LrclibQuery& query, const LrclibRecord& record);
// /api/get, then /api/search when it finds nothing. Only reads; nothing is published.
[[nodiscard]] LrclibResult LookupLrclib(IHttpClient& client, const LrclibQuery& query, const RetryPolicy& policy = {});
// Title and artist from "Artist - Title [id].mp3"-style file names; empty when unsure.
[[nodiscard]] LrclibQuery GuessLrclibQuery(const std::string& audioPath, double durationS);

// ------------------------------------------------------------------ transcripts

struct TranscriptWord {
    std::string text;
    int startMS = 0;
    int endMS = 0;
    double confidence = -1; // -1: the service gave none
};

struct TranscriptSegment {
    std::string text;
    int startMS = 0;
    int endMS = 0;
    double avgLogprob = 0;
    double noSpeechProb = 0;
};

struct Transcript {
    std::string provider; // "groq", "test"
    std::string model;
    std::string language;
    int durationMS = 0;
    std::string text;
    std::vector<TranscriptWord> words;
    std::vector<TranscriptSegment> segments;
    std::string cacheKey;
};

struct TranscribeOptions {
    std::string model = "whisper-large-v3";
    std::string language = "es";
    std::string endpoint = "https://api.groq.com/openai/v1/audio/transcriptions";
    long long maxBytes = 25LL * 1024 * 1024; // the free tier's attachment limit
};

[[nodiscard]] bool ParseWhisperVerboseJson(const std::string& body, Transcript& transcript, std::string& error);
[[nodiscard]] HttpRequest BuildGroqRequest(const std::string& audioPath, const std::string& key, const TranscribeOptions& options);
// `consent` is the person's yes in the window for this song; without it nothing is sent.
[[nodiscard]] bool TranscribeWithGroq(IHttpClient& client, const std::string& audioPath, const ApiKey& key, bool consent, const TranscribeOptions& options,
                                      const RetryPolicy& policy, Transcript& transcript, AssistFailure& failure);

[[nodiscard]] std::string ToJson(const Transcript& transcript);
[[nodiscard]] bool FromJson(const std::string& text, Transcript& transcript, std::string& error);

// Lyrics made from what a transcription heard, for a song with no lyrics of its own: one
// LRC line per sung segment, long ones split where the singing pauses, an empty line
// where a pause of 1.5 s or more follows, nothing past the song's end. Nothing is
// invented or filled in from memory: a segment the service says has no voice, or that
// is one of the sentences speech models write over music ("subtitles by...",
// "subscribe..."), is left out and said so; a segment it was unsure of is kept and
// marked doubtful, for a person to check.
struct TranscriptLrcLine {
    int startMS = 0;
    int endMS = 0;
    std::string text;
    bool doubtful = false;
    std::string why; // why doubtful
};

struct TranscriptLrc {
    std::string lrc;
    std::vector<TranscriptLrcLine> lines;
    std::vector<std::pair<std::string, std::string>> dropped; // text, reason
};

[[nodiscard]] TranscriptLrc TranscriptToLrc(const Transcript& transcript, int durationMS);

// --------------------------------------------------------------- song character

struct CharacterSection {
    int startMS = -1;
    int endMS = -1;
    std::string label; // as the model wrote it
    std::string energy; // "low", "medium", "high" or "unknown"
};

// What a listener would say about the song. Interpretation, not measurement: it is
// kept apart from SongAnalysis and never changes a beat, a section or a silence.
struct SongCharacter {
    std::string provider;
    std::string model;
    std::string genre = "unknown";
    std::vector<std::string> moods;
    std::string energy = "unknown";        // "low", "medium", "high", "varied" or "unknown"
    std::string vocalPresence = "unknown"; // "none", "some", "lead" or "unknown"
    std::vector<std::string> textures;
    std::string suggestedTheme = "uncertain"; // "christmas", "halloween", ..., or "uncertain"
    double themeConfidence = 0;
    std::vector<CharacterSection> sections;
    std::vector<std::string> warnings;
    std::string cacheKey;
};

struct CharacterOptions {
    std::string model = "gemini-3.8-flash";
    std::string endpoint = "https://generativelanguage.googleapis.com/v1beta/models";
    long long maxBytes = 14LL * 1024 * 1024; // base64 makes it ~19 MB, under the 20 MB request limit
};

// The themes a show can be built around; anything else the model says is "uncertain".
[[nodiscard]] const std::vector<std::string>& KnownThemes();
[[nodiscard]] std::string CharacterPrompt();
[[nodiscard]] std::string CharacterSchemaJson();
[[nodiscard]] HttpRequest BuildGeminiRequest(const std::string& audioBytes, const std::string& mimeType, const std::string& key, const CharacterOptions& options);
// The JSON text from a generateContent answer (or an Interactions one), checked
// field by field: a value outside what was asked becomes "unknown" with a warning.
[[nodiscard]] bool ParseGeminiCharacter(const std::string& body, SongCharacter& character, std::string& error);
[[nodiscard]] bool DescribeWithGemini(IHttpClient& client, const std::string& audioPath, const ApiKey& key, bool consent, const CharacterOptions& options, const RetryPolicy& policy,
                                      SongCharacter& character, AssistFailure& failure);
[[nodiscard]] std::string AudioMimeType(const std::string& path);

[[nodiscard]] std::string ToJson(const SongCharacter& character);
[[nodiscard]] bool FromJson(const std::string& text, SongCharacter& character, std::string& error);

// ------------------------------------------------------------------- the cache

// The same song, lyrics, provider, model, version and options give the same key, so
// a generation made later, offline, finds the answer it used.
[[nodiscard]] std::string AssistCacheKey(const std::string& audioHash, const std::string& lyricsHash, const std::string& provider, const std::string& model, const std::string& params);
[[nodiscard]] std::string AssistCachePath(const std::string& showDir, const std::string& key, const std::string& kind);

// --------------------------------------------------------- transcript to lyrics

struct TranscriptLineMatch {
    std::string lineId;
    int words = 0;
    int matched = 0;
    double confidence = 0; // matched share times the mean word confidence
};

struct TranscriptAlignment {
    std::vector<WordAnchor> anchors;
    std::vector<TranscriptLineMatch> lines;
    int lyricWords = 0;
    int matchedWords = 0;
    int transcriptWords = 0;
    int extraWords = 0; // heard but not in the lyrics: fillers, ad libs, repeats
    double coverage = 0; // matched / lyric words
    std::vector<std::string> warnings;
};

// The lyrics are the reference: their words, in their order, are matched to the
// words heard, allowing for punctuation, accents, small spelling differences,
// fillers and lines sung again or left out. A word with no match gets no time.
[[nodiscard]] TranscriptAlignment AlignTranscriptToLyrics(const LyricDocument& lyrics, const Transcript& transcript);
// For words: lowercase, no accents or punctuation; "" for a word with no letters.
[[nodiscard]] std::string MatchKey(const std::string& word);

// --------------------------------------------------------------------- LRC out

struct TimedLine {
    int startMS = -1;
    std::string text;
};
[[nodiscard]] std::string ExportLrc(const std::vector<TimedLine>& lines, const std::string& title = "", const std::string& artist = "");

[[nodiscard]] std::string Base64Encode(const std::string& bytes);

// ------------------------------------------------------------- the command

// What the window asks a child process for, so a slow or absent network never
// freezes it. The answer goes to `outPath` as JSON; a transcription or description is
// also kept in the show's cache, where a later generation finds it offline.
struct AssistCommandOptions {
    std::string action;   // "lrclib", "transcribe", "describe" or "transcript-lrc"
    std::string transcriptPath; // transcript-lrc: the transcription to make lyrics from
    std::string audioPath;
    std::string showDir;  // for the cache; empty: no cache
    std::string outPath;
    std::string query;    // lrclib: "Artist - Title"; empty: guessed from the file name
    double durationS = 0; // lrclib: the song's length, to check a match
    bool allowUpload = false; // the person said yes to sending this song, in the window
    std::string groqModel = "whisper-large-v3";
    std::string geminiModel = "gemini-3.8-flash";
    std::string language = "es";
};

// 0 answered, 1 failed (the reason in the JSON and on stdout), 2 refused before
// sending anything (no consent, no key, a file too large or of the wrong kind).
[[nodiscard]] int RunAssistCommand(const AssistCommandOptions& options, IHttpClient& client, const IServiceSettingsStore* keys, const RetryPolicy& policy = {});

} // namespace composer
