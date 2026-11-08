#include "composer/AssistTest.h"

#include "ai/IServiceSettingsStore.h"
#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"
#include "composer/MusicAssist.h"
#include "composer/GenerationValidator.h"
#include "utils/ExternalHooks.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace composer {

namespace {

constexpr int FRAME_MS = 25;

// Answers in the order given, and keeps every request it was sent.
class StandIn final : public IHttpClient {
public:
    void Answer(int status, const std::string& body, std::map<std::string, std::string> headers = {}) {
        HttpResponse r;
        r.status = status;
        r.body = body;
        r.headers = std::move(headers);
        _answers.push_back(std::move(r));
    }
    void NoNetwork() {
        HttpResponse r;
        r.transportError = "Could not resolve host";
        _answers.push_back(std::move(r));
    }
    HttpResponse Send(const HttpRequest& request) override {
        requests.push_back(request);
        if (_answers.empty()) {
            HttpResponse r;
            r.transportError = "no answer prepared";
            return r;
        }
        HttpResponse r = _answers.front();
        _answers.pop_front();
        return r;
    }
    std::vector<HttpRequest> requests;

private:
    std::deque<HttpResponse> _answers;
};

class MemoryStore final : public IServiceSettingsStore {
public:
    std::string getString(std::string_view, const std::string& d) const override { return d; }
    int getInt(std::string_view, int d) const override { return d; }
    bool getBool(std::string_view, bool d) const override { return d; }
    void setString(std::string_view, const std::string&) override {}
    void setInt(std::string_view, int) override {}
    void setBool(std::string_view, bool) override {}
    std::string getSecret(std::string_view name) const override {
        const auto it = secrets.find(std::string(name));
        return it != secrets.end() ? it->second : std::string();
    }
    void setSecret(std::string_view name, const std::string& token) override { secrets[std::string(name)] = token; }
    std::map<std::string, std::string> secrets;
};

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value == '\0') {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
}

std::string FakeKey(const std::string& prefix, size_t length) {
    std::string out = prefix;
    for (size_t i = 0; i < length; ++i) {
        out += static_cast<char>('a' + i % 26);
    }
    return out;
}

std::string Header(const HttpRequest& r, const std::string& name) {
    for (const auto& [k, v] : r.headers) {
        if (k == name) {
            return v;
        }
    }
    return {};
}

int Parts(const HttpRequest& r, const std::string& name, const std::string& value) {
    return static_cast<int>(std::count_if(r.multipart.begin(), r.multipart.end(), [&](const HttpRequest::Part& p) { return p.name == name && (value.empty() || p.value == value); }));
}

const char* WHISPER = R"({
  "task": "transcribe", "language": "spanish", "duration": 12.5,
  "text": " Una canción de amor, eh, la Navidad",
  "segments": [ { "id": 0, "start": 0.9, "end": 6.0, "text": " Una canción de amor, eh,", "avg_logprob": -0.15, "no_speech_prob": 0.01 },
                { "id": 1, "start": 7.8, "end": 10.2, "text": " la Navidad", "avg_logprob": -0.4, "no_speech_prob": 0.02 } ],
  "words": [ { "word": " canción", "start": 1.6, "end": 2.4 }, { "word": " Una", "start": 1.0, "end": 1.5 }, { "word": " de", "start": 2.5, "end": 2.7 },
             { "word": " amor,", "start": 2.8, "end": 3.9 }, { "word": " eh,", "start": 4.0, "end": 4.3 }, { "word": "", "start": 4.4, "end": 4.5 },
             { "word": " mal", "start": -1, "end": 2 }, { "word": " la", "start": 8.0, "end": 8.3 }, { "word": " Navida", "start": 8.4, "end": 9.6 } ]
})";

class Suite {
public:
    explicit Suite(const fs::path& root) :
        _root(root) {}

    int Run() {
        TestRetries();
        TestKeys();
        TestLrclib();
        TestGroq();
        TestGemini();
        TestCacheAndFiles();
        TestTranscriptLrc();
        TestTranscriptAlignment();
        TestExport();
        TestCommand();
        printf("composer-assist-test: %d checks passed, %d failed (%s)\n", _passed, _failed, _root.string().c_str());
        printf("composer-assist-test: every service was a stand-in; no real request was made and no key was used\n");
        return _failed == 0 ? 0 : 1;
    }

private:
    void Check(bool ok, const std::string& what) {
        printf("  %s %s\n", ok ? "OK   " : "FAILED", what.c_str());
        fflush(stdout);
        ok ? ++_passed : ++_failed;
    }

    void Title(const char* title) {
        printf("=== %s\n", title);
        fflush(stdout);
    }

    RetryPolicy Policy() {
        RetryPolicy p;
        p.sleep = [this](int s) { _slept.push_back(s); };
        return p;
    }

    void TestRetries() {
        Title("Answers that are errors: tried again only when it can help, and never for ever");
        HttpRequest request;
        request.url = "https://example.invalid/";
        {
            StandIn s;
            s.Answer(200, "{}");
            AssistFailure f;
            (void)SendWithRetry(s, request, Policy(), f);
            Check(f.error == AssistError::None && f.attempts == 1, "200: once");
        }
        {
            StandIn s;
            _slept.clear();
            s.Answer(429, "slow down", { { "retry-after", "2" } });
            s.Answer(200, "{}");
            AssistFailure f;
            (void)SendWithRetry(s, request, Policy(), f);
            Check(f.error == AssistError::None && f.attempts == 2 && _slept == std::vector<int> { 2 }, "429 with Retry-After 2: waits 2 s and tries again");
        }
        {
            StandIn s;
            _slept.clear();
            s.Answer(429, "", { { "retry-after", "600" } });
            AssistFailure f;
            (void)SendWithRetry(s, request, Policy(), f);
            Check(f.error == AssistError::RateLimited && f.attempts == 1 && f.retryAfterS == 600 && _slept.empty(), "429 asking for 10 minutes: not waited, reported with the time asked");
        }
        {
            StandIn s;
            _slept.clear();
            s.Answer(429, "", { { "retry-after", "Wed, 21 Oct 2026 07:28:00 GMT" } });
            AssistFailure f;
            (void)SendWithRetry(s, request, Policy(), f);
            Check(f.error == AssistError::RateLimited && _slept.empty(), "a Retry-After date is not waited for");
        }
        {
            StandIn s;
            _slept.clear();
            for (int i = 0; i < 5; ++i) {
                s.Answer(503, "unavailable");
            }
            AssistFailure f;
            (void)SendWithRetry(s, request, Policy(), f);
            Check(f.error == AssistError::ServerError && f.attempts == 3 && s.requests.size() == 3 && _slept == std::vector<int>({ 1, 2 }), "5xx: three tries in all, 1 s and 2 s apart");
        }
        {
            StandIn s;
            s.NoNetwork();
            s.Answer(200, "{}");
            AssistFailure f;
            (void)SendWithRetry(s, request, Policy(), f);
            Check(f.error == AssistError::None && f.attempts == 2, "no network once: tried again");
        }
        for (const auto& [status, error] : std::vector<std::pair<int, AssistError>> { { 401, AssistError::Unauthorized }, { 403, AssistError::Forbidden }, { 413, AssistError::TooLarge }, { 400, AssistError::BadResponse } }) {
            StandIn s;
            s.Answer(status, "no");
            s.Answer(200, "{}");
            AssistFailure f;
            (void)SendWithRetry(s, request, Policy(), f);
            Check(f.error == error && f.attempts == 1 && s.requests.size() == 1, fmt::format("{}: {} at once, never repeated", status, AssistErrorId(error)));
        }
    }

    void TestKeys() {
        Title("Keys: the secure store, then the environment; never in a message");
        const char* ENV = "COMPOSER_ASSIST_TEST_KEY";
        SetEnv(ENV, "");
        MemoryStore store;
        Check(ResolveApiKey(&store, "Groq", ENV).source == "none" && ResolveApiKey(nullptr, "Groq", ENV).value.empty(), "no key anywhere: none, and nothing invented");
        SetEnv(ENV, "env-key-123456");
        const ApiKey fromEnv = ResolveApiKey(&store, "Groq", ENV);
        Check(fromEnv.source == "environment" && fromEnv.value == "env-key-123456", "the environment variable when the store has none");
        store.setSecret(SecretStoreName("Groq"), "store-key-654321");
        const ApiKey fromStore = ResolveApiKey(&store, "Groq", ENV);
        Check(fromStore.source == "secure-store" && fromStore.value == "store-key-654321", "the secure store first");
        Check(SecretStoreName("Gemini") == "xLightsServiceSettingsGeminiApiKey", "Gemini's entry is the one xLights' AI settings already use");
        SetEnv(ENV, "");
        // Put together at run time, so no key-shaped text sits in the source for a scanner to flag.
        const std::string leaked = "401 Invalid API Key " + FakeKey("gsk_", 24) + "; Authorization: Bearer " + FakeKey("sk-", 21) + " key=" + FakeKey("AIza", 31) + " custom store-key-654321";
        const std::string clean = RedactSecrets(leaked, { "store-key-654321" });
        Check(clean.find("gsk_") == std::string::npos && clean.find("sk-abc") == std::string::npos && clean.find("AIza") == std::string::npos && clean.find("store-key") == std::string::npos,
              "keys of every shape are replaced: " + clean);
    }

    void TestLrclib() {
        Title("LRCLIB: read only, checked against the song, and chosen by a person");
        LrclibRecord r;
        std::string error;
        Check(ParseLrclibRecord(R"({"id":7,"name":"Old","trackName":"Noche","artistName":"Coro","albumName":null,"duration":181.0,"instrumental":true,"hasWordSync":false,"plainLyrics":null,"syncedLyrics":null,"lyricsfile":"version: 1"})", r, error) &&
                  r.instrumental && r.plainLyrics.empty() && r.albumName.empty() && r.lyricsfile == "version: 1",
              "nulls read as empty, instrumental kept");
        Check(!ParseLrclibRecord("<html>", r, error), "an answer that is not a record is refused");

        const std::string record = fmt::format(R"({{"id":11,"trackName":"Canción de Año","artistName":"Coro Niño","albumName":"Fiestas","duration":200,"instrumental":false,"hasWordSync":true,"plainLyrics":"Una noche\nde Navidad","syncedLyrics":"{}","lyricsfile":"lines: []"}})",
                                               "[00:01.00]Una noche\\n[00:04.50]de Navidad\\n");
        StandIn s;
        s.Answer(200, record);
        LrclibQuery q { "Canción de Año", "Coro Niño", "Fiestas", 201.4 };
        const LrclibResult found = LookupLrclib(s, q, Policy());
        Check(found.via == "get" && s.requests.size() == 1 && found.matches.size() == 1 && found.matches[0].Plausible() && found.matches[0].record.hasWordSync,
              "/api/get with a plausible answer: one request, title, artist, album and duration agree");
        if (!s.requests.empty()) {
            const std::string& url = s.requests[0].url;
            Check(url.find("track_name=Canci%C3%B3n%20de%20A%C3%B1o") != std::string::npos && url.find("&duration=201") != std::string::npos && s.requests[0].method == "GET",
                  "accents encoded in the URL, duration in whole seconds, a GET: " + url);
        }
        Check(found.matches.empty() || ParseLyricsFile("x.lrc", found.matches[0].record.syncedLyrics).Lines().size() == 2, "its synced lyrics read as any LRC file");

        StandIn search;
        search.Answer(404, R"({"code":404,"name":"TrackNotFound","message":"Failed to find specified track"})");
        search.Answer(200, R"json([{"id":1,"trackName":"Canción de Año","artistName":"Coro Niño","duration":205,"syncedLyrics":"[00:01.00]a"},
                              {"id":2,"trackName":"Canción de Año (feat. Otro)","artistName":"Coro Niño","duration":202.0,"syncedLyrics":"[00:01.00]a"},
                              {"id":3,"trackName":"Otra cosa","artistName":"Nadie","duration":201}])json");
        search.Answer(200, "[]");
        const LrclibResult searched = LookupLrclib(search, q, Policy());
        Check(searched.via == "search" && searched.matches.size() == 3 && searched.matches[0].record.id == 2, "404 from get: searched, the closest first (\"feat.\" ignored, 0.6 s off)");
        if (searched.matches.size() == 3) {
            const LrclibMatch& far = *std::find_if(searched.matches.begin(), searched.matches.end(), [](const LrclibMatch& m) { return m.record.id == 1; });
            const LrclibMatch& other = *std::find_if(searched.matches.begin(), searched.matches.end(), [](const LrclibMatch& m) { return m.record.id == 3; });
            Check(!far.durationMatches && far.titleMatches && !far.Plausible(), "3.6 s longer: shown, but not as a match");
            Check(!other.titleMatches && !other.Plausible(), "another song: not a match");
        }
        Check(std::all_of(search.requests.begin(), search.requests.end(), [](const HttpRequest& r) { return r.method == "GET"; }), "only reads: nothing is ever sent to LRCLIB");

        const LrclibQuery guess = GuessLrclibQuery("C:/música/Chiquetere ( Una musiquita que suena tal que asi ) - Seba Maza [mefO4CV957I].mp3", 134.07);
        Check(guess.artist == "Chiquetere ( Una musiquita que suena tal que asi )" && guess.track == "Seba Maza", "a downloaded file's name gives both halves (the order is not known)");
        StandIn swapped;
        swapped.Answer(404, "{}");
        swapped.Answer(200, "[]");
        swapped.Answer(200, R"([{"id":9,"trackName":"Chiquetere","artistName":"Seba Maza","duration":134,"syncedLyrics":"[00:01.00]a"}])");
        const LrclibResult both = LookupLrclib(swapped, guess, Policy());
        Check(!both.matches.empty() && both.matches[0].record.id == 9 && both.matches[0].Plausible(), "so both orders are searched, and the song is found");

        StandIn offline;
        for (int i = 0; i < 9; ++i) {
            offline.NoNetwork();
        }
        const LrclibResult none = LookupLrclib(offline, q, Policy());
        Check(none.matches.empty() && none.failure.error == AssistError::Network, "offline: no lyrics and a network error, nothing else breaks");
    }

    void TestGroq() {
        Title("Groq Whisper: only with consent and a key, checked before sending");
        const fs::path song = _root / "cancion.wav";
        std::ofstream(song, std::ios::binary) << std::string(4096, 'x');
        ApiKey key { FakeKey("gsk_", 24), "environment" };
        TranscribeOptions options;
        Transcript t;
        AssistFailure f;
        StandIn s;
        Check(!TranscribeWithGroq(s, song.string(), key, false, options, Policy(), t, f) && f.error == AssistError::NoConsent && s.requests.empty(), "no consent: nothing is sent");
        Check(!TranscribeWithGroq(s, song.string(), ApiKey(), true, options, Policy(), t, f) && f.error == AssistError::NoCredentials && s.requests.empty(), "no key: nothing is sent, and it says why");
        TranscribeOptions small = options;
        small.maxBytes = 1000;
        const bool sent = TranscribeWithGroq(s, song.string(), key, true, small, Policy(), t, f);
        Check(!sent && f.error == AssistError::FileTooLarge && s.requests.empty(), "over the size limit: refused before uploading (" + f.message + ")");
        const fs::path wma = _root / "cancion.wma";
        std::ofstream(wma, std::ios::binary) << "x";
        Check(!TranscribeWithGroq(s, wma.string(), key, true, options, Policy(), t, f) && f.error == AssistError::UnsupportedFile && s.requests.empty(), "a format Groq does not take: refused before uploading");

        s.Answer(200, WHISPER);
        const bool ok = TranscribeWithGroq(s, song.string(), key, true, options, Policy(), t, f);
        Check(ok && s.requests.size() == 1, "with consent and a key: one request");
        if (!s.requests.empty()) {
            const HttpRequest& r = s.requests[0];
            Check(r.method == "POST" && r.url == "https://api.groq.com/openai/v1/audio/transcriptions" && Header(r, "Authorization") == "Bearer " + key.value, "POST to Groq, the key only in the header");
            Check(Parts(r, "model", "whisper-large-v3") == 1 && Parts(r, "response_format", "verbose_json") == 1 && Parts(r, "timestamp_granularities[]", "word") == 1 &&
                      Parts(r, "timestamp_granularities[]", "segment") == 1 && Parts(r, "language", "es") == 1,
                  "whisper-large-v3, verbose_json, word and segment times, Spanish");
            Check(r.multipart.front().filePath == song.string() && r.multipart.front().mimeType == "audio/wav", "the song sent from where it is");
        }
        Check(t.words.size() == 7 && t.words[0].text == "Una" && t.words[1].text == "canción" && t.durationMS == 12500 && t.segments.size() == 2 && t.provider == "groq",
              fmt::format("verbose_json read: {} words in time order, empty and negative ones left out", t.words.size()));
        Transcript bad;
        std::string error;
        Check(!ParseWhisperVerboseJson(R"({"text":"hola","segments":[]})", bad, error), "without word times the transcription is refused: " + error);

        StandIn denied;
        denied.Answer(401, R"({"error":{"message":"Invalid API Key )" + key.value + R"(","type":"invalid_request_error","code":"invalid_api_key"}})");
        Check(!TranscribeWithGroq(denied, song.string(), key, true, options, Policy(), t, f) && f.error == AssistError::Unauthorized && f.message.find(key.value) == std::string::npos && denied.requests.size() == 1,
              "401: reported once, and the key the service echoed back is not in the message");
        StandIn tooBig;
        tooBig.Answer(413, R"({"error":{"message":"Request Entity Too Large"}})");
        Check(!TranscribeWithGroq(tooBig, song.string(), key, true, options, Policy(), t, f) && f.error == AssistError::TooLarge, "413 from the service: too large");
    }

    void TestGemini() {
        Title("Gemini: a description of the song, never trusted further than it says");
        const fs::path song = _root / "cancion.wav";
        ApiKey key { FakeKey("AIza", 31), "secure-store" };
        CharacterOptions options;
        SongCharacter c;
        AssistFailure f;
        StandIn s;
        Check(!DescribeWithGemini(s, song.string(), key, false, options, Policy(), c, f) && f.error == AssistError::NoConsent && s.requests.empty(), "no consent: nothing is sent");
        const std::string answer = R"({"candidates":[{"content":{"parts":[{"text":"```json\n{\"genre\":\"Villancico pop\",\"moods\":[\"alegre\",\"festivo\"],\"energy\":\"high\",\"vocalPresence\":\"lead\",\"textures\":[\"campanas\"],\"suggestedTheme\":\"christmas\",\"themeConfidence\":0.82,\"sections\":[{\"startSeconds\":0,\"endSeconds\":8.5,\"label\":\"Intro\",\"energy\":\"low\"},{\"startSeconds\":8.5,\"endSeconds\":4,\"label\":\"roto\"},{\"startSeconds\":8.5,\"endSeconds\":40,\"label\":\"estribillo\",\"energy\":\"high\"}]}\n```"}],"role":"model"},"finishReason":"STOP"}]})";
        s.Answer(200, answer);
        const bool ok = DescribeWithGemini(s, song.string(), key, true, options, Policy(), c, f);
        Check(ok && c.genre == "villancico pop" && c.energy == "high" && c.vocalPresence == "lead" && c.suggestedTheme == "christmas" && c.themeConfidence == 0.82 && c.moods.size() == 2,
              "the answer read field by field (fenced JSON accepted)");
        Check(c.sections.size() == 2 && c.sections[1].startMS == 8500 && std::count(c.warnings.begin(), c.warnings.end(), "section-dropped") == 1, "a section ending before it starts is dropped, and said");
        if (!s.requests.empty()) {
            const HttpRequest& r = s.requests[0];
            Check(r.url == "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent" && Header(r, "x-goog-api-key") == key.value && r.body.find(key.value) == std::string::npos,
                  "gemini-3.8-flash by generateContent, the key in its header and not in the body");
            Check(r.body.find("\"inline_data\"") != std::string::npos && r.body.find("audio/wav") != std::string::npos && r.body.find("\"responseMimeType\":\"application/json\"") != std::string::npos &&
                      r.body.find("responseJsonSchema") != std::string::npos,
                  "the song inline, and JSON with a schema asked for");
        }
        const auto parse = [](const std::string& inner) {
            SongCharacter out;
            std::string error;
            const std::string body = fmt::format(R"({{"output_text":{}}})", nlohmann::json(inner).dump());
            (void)ParseGeminiCharacter(body, out, error);
            return out;
        };
        const SongCharacter unsure = parse(R"({"genre":"","moods":[],"energy":"super","vocalPresence":"lead","suggestedTheme":"christmas","themeConfidence":0.3,"sections":[]})");
        Check(unsure.suggestedTheme == "uncertain" && unsure.genre == "unknown" && unsure.energy == "unknown" && std::count(unsure.warnings.begin(), unsure.warnings.end(), "theme-low-confidence") == 1,
              "a theme at 0.3 is \"uncertain\"; an energy outside the list is \"unknown\" (Interactions-style answer read too)");
        const SongCharacter invented = parse(R"({"genre":"synth","moods":[],"energy":"low","vocalPresence":"none","suggestedTheme":"vaporwave","themeConfidence":0.95,"sections":[]})");
        Check(invented.suggestedTheme == "uncertain" && std::count(invented.warnings.begin(), invented.warnings.end(), "theme-not-known") == 1, "a theme outside the catalogue is not taken, however sure");
        SongCharacter blocked;
        std::string error;
        Check(!ParseGeminiCharacter(R"({"promptFeedback":{"blockReason":"OTHER"}})", blocked, error), "a blocked request is an error: " + error);
        StandIn limited;
        limited.Answer(429, "{}", { { "retry-after", "3" } });
        limited.Answer(429, "{}", { { "retry-after", "3" } });
        limited.Answer(429, "{}", { { "retry-after", "3" } });
        limited.Answer(200, answer);
        Check(!DescribeWithGemini(limited, song.string(), key, true, options, Policy(), c, f) && f.error == AssistError::RateLimited && limited.requests.size() == 3, "429 three times: it stops at three");
    }

    void TestCacheAndFiles() {
        Title("The cache: same inputs, same key; the answers saved and read back");
        const std::string a = AssistCacheKey("audio1", "lyrics1", "groq", "whisper-large-v3", "es");
        Check(a == AssistCacheKey("audio1", "lyrics1", "groq", "whisper-large-v3", "es"), "the same inputs give the same key");
        Check(a != AssistCacheKey("audio2", "lyrics1", "groq", "whisper-large-v3", "es") && a != AssistCacheKey("audio1", "lyrics2", "groq", "whisper-large-v3", "es") &&
                  a != AssistCacheKey("audio1", "lyrics1", "groq", "whisper-large-v3-turbo", "es") && a != AssistCacheKey("audio1", "lyrics1", "groq", "whisper-large-v3", "en"),
              "the song, the lyrics, the model and the options each change it");
        Check(AssistCachePath("C:/show", a, "transcript").find("ShowComposer") != std::string::npos, "kept in the show's Show Composer cache");
        Transcript t;
        std::string error;
        (void)ParseWhisperVerboseJson(WHISPER, t, error);
        t.provider = "groq";
        t.cacheKey = a;
        Transcript back;
        Check(FromJson(ToJson(t), back, error) && back.words.size() == t.words.size() && back.words[1].text == "canción" && back.cacheKey == a && back.segments.size() == 2, "a transcript saved and read back");
        SongCharacter c;
        c.genre = "pop";
        c.suggestedTheme = "christmas";
        c.themeConfidence = 0.7;
        c.sections.push_back({ 0, 1000, "intro", "low" });
        SongCharacter cb;
        Check(FromJson(ToJson(c), cb, error) && cb.suggestedTheme == "christmas" && cb.sections.size() == 1 && ToJson(c).find("\"interpretation\": true") != std::string::npos,
              "a description saved and read back, marked as interpretation");
        Check(Base64Encode("") == "" && Base64Encode("f") == "Zg==" && Base64Encode("fo") == "Zm8=" && Base64Encode("foo") == "Zm9v" && Base64Encode("foobar") == "Zm9vYmFy", "base64 as RFC 4648");
    }

    void TestTranscriptLrc() {
        Title("Lyrics made from what was heard, for a song with none");
        Transcript t;
        t.durationMS = 60000;
        t.segments = { { " Subtítulos realizados por la comunidad de Amara.org", 0, 4000, -0.9, 0.3 },
                       { "Con mi burrito vengo cantando", 5000, 8000, -0.2, 0.05 },
                       { "algo que no se entiende", 12000, 14000, -1.4, 0.2 },
                       { "Ya voy, ya voy", 20000, 22000, -1.6, 0.9 },
                       { "uno dos tres cuatro cinco seis siete ocho", 30000, 42000, -0.3, 0.1 },
                       { "y la última se sale", 57000, 63000, -0.3, 0.1 },
                       { "voy voy camino camino de Belén", 15000, 18000, -0.2, 0.05 },
                       { "Tuqui, tuqui, tuqui, tuqui, ta", 24000, 27000, -0.2, 0.05 } };
        const int ws[][2] = { { 30000, 30500 }, { 30600, 31000 }, { 31200, 31600 }, { 31800, 32300 }, { 36000, 36500 }, { 36600, 37200 }, { 37300, 37800 }, { 37900, 41800 } };
        const char* names[] = { "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho" };
        for (int i = 0; i < 8; ++i) {
            t.words.push_back({ names[i], ws[i][0], ws[i][1], -1 });
        }
        const TranscriptLrc lrc = TranscriptToLrc(t, 60000);
        const auto has = [&](const std::string& s) { return std::any_of(lrc.lines.begin(), lrc.lines.end(), [&](const TranscriptLrcLine& l) { return l.text == s; }); };
        Check(lrc.dropped.size() == 2 && !has("Subtítulos realizados por la comunidad de Amara.org") && !has("Ya voy, ya voy"),
              "words written over music and a segment with no voice are left out, and said so");
        Check(has("Con mi burrito vengo cantando") && !lrc.lines.empty() && !lrc.lines.front().doubtful, "a clear sung line is kept as heard");
        const auto line = [&](const std::string& s) {
            const auto it = std::find_if(lrc.lines.begin(), lrc.lines.end(), [&](const TranscriptLrcLine& l) { return l.text == s; });
            return it != lrc.lines.end() ? &*it : nullptr;
        };
        Check(line("algo que no se entiende") != nullptr && line("algo que no se entiende")->doubtful && !line("algo que no se entiende")->why.empty(), "an unsure line is kept and marked doubtful, with why");
        Check(has("uno dos tres cuatro") && has("cinco seis siete ocho"), "a 12 s line is split where the singing pauses");
        Check(line("voy voy camino camino de Belén") != nullptr && line("voy voy camino camino de Belén")->doubtful && line("Tuqui, tuqui, tuqui, tuqui, ta") != nullptr &&
                  !line("Tuqui, tuqui, tuqui, tuqui, ta")->doubtful,
              "words heard twice in a row (two voices at once) make a line doubtful; a refrain with commas does not");
        Check(lrc.lines.back().endMS <= 60000 && lrc.lrc.find("[01:00.00]") != std::string::npos, "nothing goes past the end of the song");
        Check(lrc.lrc.rfind("[00:05.00]Con mi burrito vengo cantando\n", 0) == 0 && lrc.lrc.find("[00:08.00]\n") != std::string::npos, "LRC lines with their times, and an empty line where a pause follows");
        Check(lrc.lrc.find("Amara") == std::string::npos, "nothing invented, nothing filled in");
    }

    void TestTranscriptAlignment() {
        Title("A transcription matched to the lyrics, which stay the reference");
        const LyricDocument lyrics = ParsePlainLyrics("Una canción de amor\nla Navidad\nY la luna se va\n");
        Transcript t;
        std::string error;
        (void)ParseWhisperVerboseJson(WHISPER, t, error);
        // The third line was never sung, but "la" is heard far away, alone.
        t.words.push_back({ "la", 30000, 30300, -1 });
        const TranscriptAlignment a = AlignTranscriptToLyrics(lyrics, t);
        Check(a.lyricWords == 11 && a.matchedWords == 6, fmt::format("{} of {} lyric words heard", a.matchedWords, a.lyricWords));
        const auto anchor = [&](const std::string& prefix) -> const WordAnchor* {
            for (const WordAnchor& w : a.anchors) {
                if (w.wordId.find(prefix) != std::string::npos) {
                    return &w;
                }
            }
            return nullptr;
        };
        const WordAnchor* cancion = anchor("/1:canción");
        const WordAnchor* navidad = anchor(":navidad");
        Check(cancion != nullptr && cancion->startMS == 1600 && cancion->endMS == 2400, "\"canción\" at the time it was heard, its accent and comma no obstacle");
        Check(navidad != nullptr && navidad->startMS == 8400, "\"Navida\" heard for \"Navidad\": one letter off in a long word still matches");
        Check(anchor(":luna") == nullptr && anchor("#1/1:la") == nullptr, "a line not sung gets no time, and a lone \"la\" 20 s away is not taken for it");
        Check(a.extraWords >= 1 && std::count(a.warnings.begin(), a.warnings.end(), "lines-not-heard:1") == 1, "the filler \"eh\" is heard but not in the lyrics; the missing line is reported");
        Check(std::all_of(a.anchors.begin(), a.anchors.end(), [](const WordAnchor& w) { return w.confidence > 0 && w.confidence <= 0.85; }), "word confidence never above 0.85");
        bool ordered = true;
        for (size_t i = 1; i < a.anchors.size(); ++i) {
            ordered = ordered && a.anchors[i].startMS >= a.anchors[i - 1].startMS;
        }
        Check(ordered, "matched in the order sung");

        AlignerInput in;
        in.document = &lyrics;
        in.anchors = &a.anchors;
        in.durationMS = 12500;
        const LyricAlignment aligned = AlignLyrics(in);
        Check(aligned.provider == AlignmentProvider::Transcript && aligned.quality == "transcribed", fmt::format("provider {}, quality {}", ProviderId(aligned.provider), aligned.quality));
        if (aligned.phrases.size() == 3) {
            Check(aligned.phrases[0].startMS == 1000 && aligned.phrases[0].source == "transcript" && aligned.phrases[2].startMS == -1, "the heard lines are timed from their words; the unsung one is not timed");
        }
        int fromTranscript = 0;
        for (const AlignedWord& w : aligned.words) {
            fromTranscript += w.source == "transcript" ? 1 : 0;
        }
        Check(fromTranscript == 6, fmt::format("{} words keep the heard times", fromTranscript));
        const TimingTrackSpec track = BuildLyricTrack(aligned, "Letra");
        Check(ValidateLyrics(aligned, track, in.durationMS, FRAME_MS).ErrorCount() == 0, "the three layers pass the lyrics checks");

        LyricCorrection mark;
        mark.level = LyricCorrection::Level::Word;
        mark.targetId = cancion != nullptr ? cancion->wordId : "";
        mark.startMS = 1500;
        mark.endMS = 2500;
        in.corrections = { mark };
        const LyricAlignment corrected = AlignLyrics(in);
        const auto moved = std::find_if(corrected.words.begin(), corrected.words.end(), [&](const AlignedWord& w) { return w.id == mark.targetId; });
        Check(moved != corrected.words.end() && moved->startMS == 1500 && moved->source == "manual", "a word a person moved wins over what was heard");

        const LyricDocument none = ParsePlainLyrics("Nada que ver aquí\n");
        const TranscriptAlignment unrelated = AlignTranscriptToLyrics(none, t);
        Check(unrelated.matchedWords == 0 && std::count(unrelated.warnings.begin(), unrelated.warnings.end(), "transcript-low-coverage") == 1, "lyrics of another song: nothing matched, and said");
    }

    void TestExport() {
        Title("LRC written from the times");
        const std::string lrc = ExportLrc({ { 4500, "de Navidad" }, { 1000, "Una noche" }, { -1, "sin tiempo" } }, "Prueba", "Coro");
        Check(lrc.find("[ti:Prueba]") != std::string::npos && lrc.find("[00:01.00]Una noche\n[00:04.50]de Navidad\n") != std::string::npos && lrc.find("sin tiempo") == std::string::npos,
              "sorted, two decimals, untimed lines left out");
        const LyricDocument back = ParseLyricsFile("x.lrc", lrc);
        Check(back.Lines().size() == 2 && back.Lines()[1]->startMS == 4500 && back.ErrorCount() == 0, "and read back unchanged");
    }

    static std::string Read(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    void TestCommand() {
        Title("The command the window runs: consent, keys, cache and offline");
        const fs::path show = _root / "show";
        std::error_code ec;
        fs::create_directories(show, ec);
        const fs::path song = _root / "cancion.wav";
        AssistCommandOptions o;
        o.audioPath = song.string();
        o.showDir = show.string();
        o.action = "transcribe";
        o.outPath = (_root / "out-1.json").string();
        MemoryStore keys;
        keys.setSecret(SecretStoreName("Groq"), FakeKey("gsk_", 24));
        StandIn s;
        s.Answer(200, WHISPER);
        Check(RunAssistCommand(o, s, &keys, Policy()) == 2 && s.requests.empty() && Read(o.outPath).find("no-consent") != std::string::npos, "without the person's yes: refused, nothing sent, and said in the answer");
        o.allowUpload = true;
        Check(RunAssistCommand(o, s, &keys, Policy()) == 0 && s.requests.size() == 1, "with it: transcribed");
        Transcript t;
        std::string error;
        Check(FromJson(Read(o.outPath), t, error) && t.words.size() == 7 && !t.cacheKey.empty(), "the answer is a transcript, with its cache key");
        const std::string cached = AssistCachePath(show.string(), t.cacheKey, "transcript");
        Check(FileExists(cached) && Read(cached).find(FakeKey("gsk_", 24)) == std::string::npos, "kept in the show's cache, with no key in it");
        StandIn offline;
        o.outPath = (_root / "out-2.json").string();
        Check(RunAssistCommand(o, offline, nullptr, Policy()) == 0 && offline.requests.empty() && FromJson(Read(o.outPath), t, error), "the same song again: from the cache, with no network and no key");

        AssistCommandOptions d = o;
        d.action = "describe";
        d.outPath = (_root / "out-3.json").string();
        StandIn none;
        Check(RunAssistCommand(d, none, &keys, Policy()) == 2 && none.requests.empty() && Read(d.outPath).find("no-credentials") != std::string::npos, "no Gemini key: refused before sending");

        AssistCommandOptions l;
        l.action = "lrclib";
        l.audioPath = (_root / "Coro Niño - Canción de Año.mp3").string();
        l.durationS = 201.4;
        l.outPath = (_root / "out-4.json").string();
        StandIn lrclib;
        lrclib.Answer(200, R"json({"id":11,"trackName":"Canción de Año","artistName":"Coro Niño","albumName":"","duration":200,"instrumental":false,"hasWordSync":false,"plainLyrics":"Una","syncedLyrics":"[00:01.00]Una","lyricsfile":""})json");
        const int code = RunAssistCommand(l, lrclib, nullptr, Policy());
        const nlohmann::json answer = nlohmann::json::parse(Read(l.outPath), nullptr, false);
        Check(code == 0 && !answer.is_discarded() && answer["matches"].size() == 1 && answer["matches"][0].value("plausible", false) && answer["matches"][0].value("syncedLyrics", std::string()) == "[00:01.00]Una",
              "LRCLIB from the file name: one plausible match, its lyrics in the answer for the person to pick");
    }

    fs::path _root;
    int _passed = 0;
    int _failed = 0;
    std::vector<int> _slept;
};

} // namespace

int RunAssistTest(const std::string& workDir) {
    fs::path root;
    std::error_code ec;
    for (int i = 0; i < 1000; ++i) {
        root = fs::path(workDir) / fmt::format("composer-assist-test-{}", i);
        if (!fs::exists(root, ec)) {
            break;
        }
    }
    fs::create_directories(root, ec);
    if (ec) {
        printf("composer-assist-test: cannot create %s\n", root.string().c_str());
        return 1;
    }
    Suite suite(root);
    return suite.Run();
}

} // namespace composer
