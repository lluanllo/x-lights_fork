#include "composer/MusicAssist.h"

#include "ai/IServiceSettingsStore.h"
#include "composer/ComposerText.h"
#include "composer/CompositionPlan.h"
#include "utils/ExternalHooks.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

namespace {

constexpr const char* USER_AGENT = "xLights-ShowComposer/1.0 (https://xlights.org)";

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

size_t WriteBody(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

size_t WriteHeader(char* data, size_t size, size_t count, void* user) {
    const std::string line(data, size * count);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
            value.pop_back();
        }
        (*static_cast<std::map<std::string, std::string>*>(user))[Lower(line.substr(0, colon))] = value;
    }
    return size * count;
}

class CurlHttpClient final : public IHttpClient {
public:
    CurlHttpClient() {
        static std::once_flag once;
        std::call_once(once, []() { curl_global_init(CURL_GLOBAL_ALL); });
    }

    HttpResponse Send(const HttpRequest& r) override {
        HttpResponse out;
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            out.transportError = "curl could not start";
            return out;
        }
        curl_slist* headers = nullptr;
        for (const auto& [name, value] : r.headers) {
            headers = curl_slist_append(headers, (name + ": " + value).c_str());
        }
        curl_mime* mime = nullptr;
        curl_easy_setopt(curl, CURLOPT_URL, r.url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(r.timeoutS));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out.headers);
        if (!r.multipart.empty()) {
            mime = curl_mime_init(curl);
            for (const HttpRequest::Part& p : r.multipart) {
                curl_mimepart* part = curl_mime_addpart(mime);
                curl_mime_name(part, p.name.c_str());
                if (!p.filePath.empty()) {
                    curl_mime_filedata(part, p.filePath.c_str());
                } else {
                    curl_mime_data(part, p.value.c_str(), CURL_ZERO_TERMINATED);
                }
                if (!p.mimeType.empty()) {
                    curl_mime_type(part, p.mimeType.c_str());
                }
            }
            curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        } else if (r.method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, r.body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(r.body.size()));
        }
        if (headers != nullptr) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
        const CURLcode code = curl_easy_perform(curl);
        if (code != CURLE_OK) {
            out.transportError = curl_easy_strerror(code);
        } else {
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            out.status = static_cast<int>(status);
        }
        curl_mime_free(mime);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return out;
    }
};

std::string UrlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += fmt::format("%{:02X}", c);
        }
    }
    return out;
}

std::string StringField(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double NumberField(const json& j, const char* key, double fallback = -1) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

long long FileSize(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(fs::path(path), ec);
    return ec ? -1 : static_cast<long long>(size);
}

std::string ReadBytes(const std::string& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    return bytes.str();
}

int Seconds(double s) {
    return static_cast<int>(std::lround(s * 1000.0));
}

// Without accents, punctuation, "(feat. …)" or "[…]": how a title is compared.
std::string TitleKey(const std::string& title) {
    std::string t = std::regex_replace(title, std::regex(R"(\[[^\]]*\]|\((feat|ft|with|prod)[^)]*\))", std::regex::icase), " ");
    std::string folded = text::Fold(t);
    std::string out;
    bool space = true;
    for (char32_t c : text::Decode(folded)) {
        if (text::IsLetter(c) || text::IsDigit(c)) {
            out += text::Encode(c);
            space = false;
        } else if (!space) {
            out += ' ';
            space = true;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

bool SameName(const std::string& a, const std::string& b) {
    const std::string x = TitleKey(a);
    const std::string y = TitleKey(b);
    if (x.empty() || y.empty()) {
        return false;
    }
    if (x == y) {
        return true;
    }
    // "Chiquetere" and "Chiquetere (Una musiquita que suena tal que así)" are the same song.
    const std::string& shorter = x.size() < y.size() ? x : y;
    const std::string& longer = x.size() < y.size() ? y : x;
    return shorter.size() >= 4 && (longer.rfind(shorter + " ", 0) == 0 || longer.find(" " + shorter) != std::string::npos);
}

bool SameArtist(const std::string& a, const std::string& b) {
    if (SameName(a, b)) {
        return true;
    }
    const auto split = [](const std::string& s) {
        std::set<std::string> names;
        std::string all = std::regex_replace(TitleKey(s), std::regex(R"(\b(feat|ft|featuring|with|y|and|x)\b)"), ",");
        std::stringstream in(std::regex_replace(all, std::regex(R"(\s*,\s*)"), ","));
        std::string name;
        while (std::getline(in, name, ',')) {
            if (name.size() >= 3) {
                names.insert(name);
            }
        }
        return names;
    };
    const std::set<std::string> x = split(a);
    for (const std::string& n : split(b)) {
        if (x.contains(n)) {
            return true;
        }
    }
    return false;
}

std::vector<char32_t> Letters(const std::string& s) {
    return text::Decode(s);
}

int EditDistance(const std::vector<char32_t>& a, const std::vector<char32_t>& b) {
    std::vector<int> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) {
        row[j] = static_cast<int>(j);
    }
    for (size_t i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            const int up = row[j];
            row[j] = std::min({ row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1) });
            diagonal = up;
        }
    }
    return row[b.size()];
}

// 1 for the same word, a little less for one letter off in a long word, 0 otherwise.
double Similarity(const std::string& a, const std::string& b) {
    if (a == b) {
        return 1.0;
    }
    const std::vector<char32_t> x = Letters(a);
    const std::vector<char32_t> y = Letters(b);
    const size_t shorter = std::min(x.size(), y.size());
    if (shorter < 4) {
        return 0;
    }
    const double sim = 1.0 - static_cast<double>(EditDistance(x, y)) / static_cast<double>(std::max(x.size(), y.size()));
    return sim >= 0.75 ? sim : 0;
}

std::string LrcTime(int ms) {
    const int cs = (ms + 5) / 10;
    return fmt::format("[{:02}:{:02}.{:02}]", cs / 6000, (cs / 100) % 60, cs % 100);
}

} // namespace

// ------------------------------------------------------------------ transport

std::unique_ptr<IHttpClient> MakeCurlHttpClient() {
    return std::make_unique<CurlHttpClient>();
}

AssistError ErrorOfStatus(int status) {
    if (status >= 200 && status < 300) {
        return AssistError::None;
    }
    switch (status) {
    case 0:
        return AssistError::Network;
    case 401:
        return AssistError::Unauthorized;
    case 403:
        return AssistError::Forbidden;
    case 404:
        return AssistError::NotFound;
    case 413:
        return AssistError::TooLarge;
    case 429:
        return AssistError::RateLimited;
    default:
        return status >= 500 ? AssistError::ServerError : AssistError::BadResponse;
    }
}

const char* AssistErrorId(AssistError error) {
    static const char* const IDS[] = { "none", "no-credentials", "no-consent", "file-too-large", "unsupported-file", "unauthorized", "forbidden", "too-large",
                                       "rate-limited", "server-error", "network", "not-found", "bad-response" };
    return IDS[static_cast<int>(error)];
}

HttpResponse SendWithRetry(IHttpClient& client, const HttpRequest& request, const RetryPolicy& policy, AssistFailure& failure) {
    failure = AssistFailure();
    HttpResponse response;
    const int attempts = std::max(1, policy.maxAttempts);
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        response = client.Send(request);
        failure.attempts = attempt;
        failure.status = response.status;
        failure.error = response.status == 0 ? AssistError::Network : ErrorOfStatus(response.status);
        if (failure.error == AssistError::None) {
            failure.message.clear();
            return response;
        }
        const std::string detail = response.status == 0 ? response.transportError : response.body.substr(0, 300);
        failure.message = RedactSecrets(fmt::format("{} {}: {}", AssistErrorId(failure.error), response.status, detail));
        const bool again = failure.error == AssistError::Network || failure.error == AssistError::RateLimited || failure.error == AssistError::ServerError;
        int wait = 1 << (attempt - 1);
        failure.retryAfterS = -1;
        const auto retryAfter = response.headers.find("retry-after");
        if (retryAfter != response.headers.end()) {
            char* end = nullptr;
            const long seconds = std::strtol(retryAfter->second.c_str(), &end, 10);
            // A date instead of seconds is not worth parsing: it is waited as too long.
            failure.retryAfterS = end != retryAfter->second.c_str() && seconds >= 0 ? static_cast<int>(seconds) : policy.maxWaitS + 1;
            wait = failure.retryAfterS;
        }
        if (!again || attempt == attempts || wait > policy.maxWaitS) {
            return response;
        }
        if (policy.sleep) {
            policy.sleep(wait);
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(wait));
        }
    }
    return response;
}

// ----------------------------------------------------------------- credentials

std::string SecretStoreName(const std::string& service) {
    // ServiceManager's own prefix and name, so a key saved in xLights' AI settings is found.
    return "xLightsServiceSettings" + service + "ApiKey";
}

ApiKey ResolveApiKey(const IServiceSettingsStore* store, const std::string& service, const std::string& environmentVariable) {
    ApiKey key;
    key.source = "none";
    if (store != nullptr) {
        key.value = store->getSecret(SecretStoreName(service));
        if (!key.value.empty()) {
            key.source = "secure-store";
            return key;
        }
    }
    if (const char* env = std::getenv(environmentVariable.c_str()); env != nullptr && *env != '\0') {
        key.value = env;
        key.source = "environment";
    }
    return key;
}

std::string RedactSecrets(const std::string& text, const std::vector<std::string>& keys) {
    std::string out = text;
    for (const std::string& k : keys) {
        if (k.size() < 6) {
            continue;
        }
        for (size_t at = out.find(k); at != std::string::npos; at = out.find(k, at)) {
            out.replace(at, k.size(), "***");
        }
    }
    static const std::regex patterns(R"((gsk_[A-Za-z0-9]{16,})|(AIza[0-9A-Za-z_\-]{20,})|(sk-[A-Za-z0-9_\-]{16,})|((Bearer|x-goog-api-key:?)\s+[^\s"',]+)|(key=[^&\s"']+))", std::regex::icase);
    return std::regex_replace(out, patterns, "***");
}

// -------------------------------------------------------------------- LRCLIB

namespace {

bool RecordFromJson(const json& j, LrclibRecord& r) {
    if (!j.is_object() || !j.contains("id")) {
        return false;
    }
    r.id = j["id"].is_number_integer() ? j["id"].get<long long>() : 0;
    r.trackName = StringField(j, "trackName");
    if (r.trackName.empty()) {
        r.trackName = StringField(j, "name");
    }
    r.artistName = StringField(j, "artistName");
    r.albumName = StringField(j, "albumName");
    r.durationS = std::max(0.0, NumberField(j, "duration", 0));
    r.instrumental = j.value("instrumental", false);
    r.hasWordSync = j.contains("hasWordSync") && j["hasWordSync"].is_boolean() && j["hasWordSync"].get<bool>();
    r.plainLyrics = StringField(j, "plainLyrics");
    r.syncedLyrics = StringField(j, "syncedLyrics");
    r.lyricsfile = StringField(j, "lyricsfile");
    return true;
}

} // namespace

bool ParseLrclibRecord(const std::string& body, LrclibRecord& record, std::string& error) {
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !RecordFromJson(j, record)) {
        error = "not an LRCLIB record";
        return false;
    }
    return true;
}

bool ParseLrclibSearch(const std::string& body, std::vector<LrclibRecord>& records, std::string& error) {
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_array()) {
        error = "not an LRCLIB search result";
        return false;
    }
    for (const json& item : j) {
        LrclibRecord r;
        if (RecordFromJson(item, r)) {
            records.push_back(std::move(r));
        }
    }
    return true;
}

LrclibMatch CheckLrclibMatch(const LrclibQuery& query, const LrclibRecord& record) {
    LrclibMatch m;
    m.record = record;
    m.titleMatches = SameName(query.track, record.trackName);
    m.artistMatches = SameArtist(query.artist, record.artistName);
    m.albumMatches = query.album.empty() || SameName(query.album, record.albumName);
    if (query.durationS > 0 && record.durationS > 0) {
        m.durationDiffS = std::abs(query.durationS - record.durationS);
        m.durationMatches = m.durationDiffS <= 2.0;
    } else {
        m.durationMatches = query.durationS <= 0;
    }
    m.score = (m.titleMatches ? 3 : 0) + (m.artistMatches ? 2 : 0) + (m.durationMatches ? 2 : 0) + (m.albumMatches && !query.album.empty() ? 1 : 0) + (!record.syncedLyrics.empty() ? 0.5 : 0) +
              (record.hasWordSync ? 0.25 : 0) - (m.durationDiffS > 0 ? std::min(1.0, m.durationDiffS * 0.1) : 0);
    return m;
}

LrclibResult LookupLrclib(IHttpClient& client, const LrclibQuery& query, const RetryPolicy& policy) {
    LrclibResult result;
    if (query.track.empty()) {
        result.failure.error = AssistError::BadResponse;
        result.failure.message = "a title is needed to search";
        return result;
    }
    const std::string base = "https://lrclib.net/api/";
    if (!query.artist.empty()) {
        HttpRequest get;
        get.url = base + "get?track_name=" + UrlEncode(query.track) + "&artist_name=" + UrlEncode(query.artist);
        if (!query.album.empty()) {
            get.url += "&album_name=" + UrlEncode(query.album);
        }
        if (query.durationS > 0) {
            get.url += "&duration=" + std::to_string(static_cast<int>(std::lround(query.durationS)));
        }
        get.timeoutS = 20;
        const HttpResponse r = SendWithRetry(client, get, policy, result.failure);
        LrclibRecord record;
        std::string error;
        if (result.failure.error == AssistError::None && ParseLrclibRecord(r.body, record, error)) {
            result.matches.push_back(CheckLrclibMatch(query, record));
            result.via = "get";
            if (result.matches.front().Plausible()) {
                return result;
            }
        } else if (result.failure.error != AssistError::NotFound && result.failure.error != AssistError::None) {
            return result;
        }
    }
    // File names put the artist on either side of the dash: both are searched.
    std::vector<std::pair<std::string, std::string>> searches = { { query.track, query.artist } };
    if (!query.artist.empty()) {
        searches.push_back({ query.artist, query.track });
    }
    std::set<long long> seen;
    for (const LrclibMatch& m : result.matches) {
        seen.insert(m.record.id);
    }
    for (const auto& [track, artist] : searches) {
        HttpRequest search;
        search.url = base + "search?track_name=" + UrlEncode(track) + (artist.empty() ? std::string() : "&artist_name=" + UrlEncode(artist));
        search.timeoutS = 20;
        AssistFailure failure;
        const HttpResponse r = SendWithRetry(client, search, policy, failure);
        std::vector<LrclibRecord> records;
        std::string error;
        if (failure.error != AssistError::None || !ParseLrclibSearch(r.body, records, error)) {
            result.failure = failure.error != AssistError::None ? failure : AssistFailure { AssistError::BadResponse, r.status, -1, failure.attempts, error };
            continue;
        }
        result.failure = AssistFailure();
        result.via = "search";
        LrclibQuery swapped = query;
        swapped.track = track;
        swapped.artist = artist;
        for (const LrclibRecord& record : records) {
            if (seen.insert(record.id).second) {
                result.matches.push_back(CheckLrclibMatch(swapped, record));
            }
        }
    }
    std::stable_sort(result.matches.begin(), result.matches.end(), [](const LrclibMatch& a, const LrclibMatch& b) { return a.score > b.score; });
    if (!result.matches.empty()) {
        result.failure = AssistFailure();
    } else if (result.failure.error == AssistError::None) {
        result.failure.error = AssistError::NotFound;
        result.failure.message = "no lyrics found";
    }
    return result;
}

LrclibQuery GuessLrclibQuery(const std::string& audioPath, double durationS) {
    LrclibQuery q;
    q.durationS = durationS;
    std::string stem = fs::path(audioPath).stem().string();
    stem = std::regex_replace(stem, std::regex(R"(\s*\[[^\]]*\]\s*$)"), "");          // "[mefO4CV957I]"
    stem = std::regex_replace(stem, std::regex(R"(\((official|video|audio|lyrics?|letra)[^)]*\))", std::regex::icase), "");
    stem = std::regex_replace(stem, std::regex(R"(_+)"), " ");
    const size_t dash = stem.find(" - ");
    if (dash == std::string::npos) {
        q.track = text::Trim(stem);
        return q;
    }
    q.artist = text::Trim(stem.substr(0, dash));
    q.track = text::Trim(stem.substr(dash + 3));
    return q;
}

// ------------------------------------------------------------------ transcripts

bool ParseWhisperVerboseJson(const std::string& body, Transcript& t, std::string& error) {
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "the transcription is not JSON";
        return false;
    }
    t.text = StringField(j, "text");
    t.language = StringField(j, "language");
    const double duration = NumberField(j, "duration", 0);
    t.durationMS = duration > 0 ? Seconds(duration) : 0;
    if (j.contains("segments") && j["segments"].is_array()) {
        for (const json& s : j["segments"]) {
            TranscriptSegment seg;
            seg.text = text::Trim(StringField(s, "text"));
            seg.startMS = Seconds(NumberField(s, "start", 0));
            seg.endMS = Seconds(NumberField(s, "end", 0));
            seg.avgLogprob = NumberField(s, "avg_logprob", 0);
            seg.noSpeechProb = NumberField(s, "no_speech_prob", 0);
            if (seg.endMS >= seg.startMS && seg.startMS >= 0) {
                t.segments.push_back(std::move(seg));
            }
        }
    }
    if (!j.contains("words") || !j["words"].is_array()) {
        error = "the transcription has no word times (timestamp_granularities[]=word)";
        return false;
    }
    for (const json& w : j["words"]) {
        TranscriptWord word;
        word.text = text::Trim(StringField(w, "word"));
        const double start = NumberField(w, "start");
        const double end = NumberField(w, "end");
        if (word.text.empty() || start < 0 || end < start) {
            continue;
        }
        word.startMS = Seconds(start);
        word.endMS = Seconds(end);
        word.confidence = NumberField(w, "probability", -1);
        t.words.push_back(std::move(word));
    }
    std::stable_sort(t.words.begin(), t.words.end(), [](const TranscriptWord& a, const TranscriptWord& b) { return a.startMS < b.startMS; });
    if (t.words.empty()) {
        error = "the transcription heard no words";
        return false;
    }
    return true;
}

HttpRequest BuildGroqRequest(const std::string& audioPath, const std::string& key, const TranscribeOptions& options) {
    HttpRequest r;
    r.method = "POST";
    r.url = options.endpoint;
    r.timeoutS = 300;
    r.headers.push_back({ "Authorization", "Bearer " + key });
    r.multipart.push_back({ "file", "", audioPath, AudioMimeType(audioPath) });
    r.multipart.push_back({ "model", options.model, "", "" });
    r.multipart.push_back({ "response_format", "verbose_json", "", "" });
    r.multipart.push_back({ "timestamp_granularities[]", "word", "", "" });
    r.multipart.push_back({ "timestamp_granularities[]", "segment", "", "" });
    r.multipart.push_back({ "temperature", "0", "", "" });
    if (!options.language.empty()) {
        r.multipart.push_back({ "language", options.language, "", "" });
    }
    return r;
}

bool TranscribeWithGroq(IHttpClient& client, const std::string& audioPath, const ApiKey& key, bool consent, const TranscribeOptions& options, const RetryPolicy& policy,
                        Transcript& transcript, AssistFailure& failure) {
    failure = AssistFailure();
    if (!consent) {
        failure = { AssistError::NoConsent, 0, -1, 0, "the song is only sent when the person allows it" };
        return false;
    }
    if (key.value.empty()) {
        failure = { AssistError::NoCredentials, 0, -1, 0, "no Groq key: set it in the window or GROQ_API_KEY" };
        return false;
    }
    static const std::set<std::string> FORMATS = { ".flac", ".mp3", ".mp4", ".mpeg", ".mpga", ".m4a", ".ogg", ".wav", ".webm" };
    if (!FORMATS.contains(Lower(fs::path(audioPath).extension().string()))) {
        failure = { AssistError::UnsupportedFile, 0, -1, 0, "Groq takes flac, mp3, mp4, mpeg, mpga, m4a, ogg, wav or webm" };
        return false;
    }
    const long long size = FileSize(audioPath);
    if (size < 0) {
        failure = { AssistError::UnsupportedFile, 0, -1, 0, "the song cannot be read" };
        return false;
    }
    if (size > options.maxBytes) {
        failure = { AssistError::FileTooLarge, 0, -1, 0, fmt::format("{:.1f} MB is more than the {:.0f} MB Groq accepts", size / 1048576.0, options.maxBytes / 1048576.0) };
        return false;
    }
    const HttpResponse r = SendWithRetry(client, BuildGroqRequest(audioPath, key.value, options), policy, failure);
    failure.message = RedactSecrets(failure.message, { key.value });
    if (failure.error != AssistError::None) {
        return false;
    }
    std::string error;
    transcript = Transcript();
    if (!ParseWhisperVerboseJson(r.body, transcript, error)) {
        failure = { AssistError::BadResponse, r.status, -1, failure.attempts, error };
        return false;
    }
    transcript.provider = "groq";
    transcript.model = options.model;
    return true;
}

std::string ToJson(const Transcript& t) {
    json words = json::array();
    for (const TranscriptWord& w : t.words) {
        words.push_back({ { "text", w.text }, { "startMS", w.startMS }, { "endMS", w.endMS }, { "confidence", w.confidence } });
    }
    json segments = json::array();
    for (const TranscriptSegment& s : t.segments) {
        segments.push_back({ { "text", s.text }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "avgLogprob", s.avgLogprob }, { "noSpeechProb", s.noSpeechProb } });
    }
    const json root = { { "kind", "composer-transcript" }, { "version", MUSIC_ASSIST_VERSION }, { "provider", t.provider }, { "model", t.model }, { "language", t.language },
                        { "durationMS", t.durationMS }, { "text", t.text }, { "cacheKey", t.cacheKey }, { "words", words }, { "segments", segments } };
    return root.dump(2);
}

bool FromJson(const std::string& text, Transcript& t, std::string& error) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || StringField(j, "kind") != "composer-transcript") {
        error = "not a Show Composer transcript";
        return false;
    }
    t = Transcript();
    t.provider = StringField(j, "provider");
    t.model = StringField(j, "model");
    t.language = StringField(j, "language");
    t.durationMS = j.value("durationMS", 0);
    t.text = StringField(j, "text");
    t.cacheKey = StringField(j, "cacheKey");
    for (const json& w : j.value("words", json::array())) {
        t.words.push_back({ StringField(w, "text"), w.value("startMS", 0), w.value("endMS", 0), w.value("confidence", -1.0) });
    }
    for (const json& s : j.value("segments", json::array())) {
        t.segments.push_back({ StringField(s, "text"), s.value("startMS", 0), s.value("endMS", 0), s.value("avgLogprob", 0.0), s.value("noSpeechProb", 0.0) });
    }
    return true;
}

// --------------------------------------------------------------- song character

const std::vector<std::string>& KnownThemes() {
    static const std::vector<std::string> THEMES = { "christmas", "halloween", "winter", "new-year", "celebration", "party", "romantic", "epic", "playful", "calm", "patriotic", "uncertain" };
    return THEMES;
}

std::string CharacterPrompt() {
    return "Listen to this song and describe it for someone designing a Christmas-light show for it. "
           "Answer only from what you hear, never from a title or from lyrics you may know. "
           "Use \"unknown\" for anything you cannot tell, and \"uncertain\" as the theme unless the music itself clearly suggests one; "
           "themeConfidence is how sure you are, from 0 to 1. "
           "Sections: the parts you hear (intro, verse, chorus, bridge, instrumental, outro, or your own words), with start and end in seconds and their energy.";
}

std::string CharacterSchemaJson() {
    const json energy = { { "type", "string" }, { "enum", { "low", "medium", "high", "varied", "unknown" } } };
    const json schema = {
        { "type", "object" },
        { "properties",
          { { "genre", { { "type", "string" } } },
            { "moods", { { "type", "array" }, { "items", { { "type", "string" } } } } },
            { "energy", energy },
            { "vocalPresence", { { "type", "string" }, { "enum", { "none", "some", "lead", "unknown" } } } },
            { "textures", { { "type", "array" }, { "items", { { "type", "string" } } } } },
            { "suggestedTheme", { { "type", "string" }, { "enum", KnownThemes() } } },
            { "themeConfidence", { { "type", "number" } } },
            { "sections",
              { { "type", "array" },
                { "items",
                  { { "type", "object" },
                    { "properties", { { "startSeconds", { { "type", "number" } } }, { "endSeconds", { { "type", "number" } } }, { "label", { { "type", "string" } } }, { "energy", energy } } },
                    { "required", { "startSeconds", "endSeconds", "label" } } } } } } } },
        { "required", { "genre", "moods", "energy", "vocalPresence", "suggestedTheme", "themeConfidence", "sections" } }
    };
    return schema.dump();
}

std::string AudioMimeType(const std::string& path) {
    static const std::map<std::string, std::string> TYPES = { { ".mp3", "audio/mp3" }, { ".wav", "audio/wav" }, { ".m4a", "audio/m4a" }, { ".aac", "audio/aac" },
                                                              { ".ogg", "audio/ogg" }, { ".flac", "audio/flac" }, { ".aiff", "audio/aiff" }, { ".aif", "audio/aiff" },
                                                              { ".webm", "audio/webm" }, { ".mp4", "audio/mp4" } };
    const auto it = TYPES.find(Lower(fs::path(path).extension().string()));
    return it != TYPES.end() ? it->second : std::string();
}

HttpRequest BuildGeminiRequest(const std::string& audioBytes, const std::string& mimeType, const std::string& key, const CharacterOptions& options) {
    HttpRequest r;
    r.method = "POST";
    r.url = options.endpoint + "/" + options.model + ":generateContent";
    r.timeoutS = 240;
    r.headers.push_back({ "x-goog-api-key", key });
    r.headers.push_back({ "Content-Type", "application/json" });
    const json body = { { "contents", json::array({ { { "role", "user" }, { "parts", json::array({ { { "text", CharacterPrompt() } }, { { "inline_data", { { "mime_type", mimeType }, { "data", Base64Encode(audioBytes) } } } } }) } } }) },
                        { "generationConfig", { { "temperature", 0 }, { "responseMimeType", "application/json" }, { "responseJsonSchema", json::parse(CharacterSchemaJson()) } } } };
    r.body = body.dump();
    return r;
}

namespace {

// generateContent puts the text in candidates[].content.parts[]; the Interactions API
// in output_text, outputs[] or steps[]. Whichever came back, its text.
std::string AnswerText(const json& j, std::string& error) {
    std::string out;
    if (j.contains("candidates") && j["candidates"].is_array() && !j["candidates"].empty()) {
        const json& c = j["candidates"][0];
        if (c.contains("content") && c["content"].contains("parts")) {
            for (const json& p : c["content"]["parts"]) {
                if (p.contains("text") && p["text"].is_string() && !p.value("thought", false)) {
                    out += p["text"].get<std::string>();
                }
            }
        }
        if (out.empty()) {
            error = "the answer has no text (" + StringField(c, "finishReason") + ")";
        }
        return out;
    }
    if (j.contains("promptFeedback") && j["promptFeedback"].contains("blockReason")) {
        error = "the request was blocked: " + StringField(j["promptFeedback"], "blockReason");
        return out;
    }
    if (j.contains("output_text") && j["output_text"].is_string()) {
        return j["output_text"].get<std::string>();
    }
    for (const char* list : { "outputs", "steps" }) {
        if (!j.contains(list) || !j[list].is_array()) {
            continue;
        }
        for (const json& item : j[list]) {
            if (item.contains("text") && item["text"].is_string()) {
                out += item["text"].get<std::string>();
            }
            if (item.contains("content") && item["content"].is_array()) {
                for (const json& c : item["content"]) {
                    if (c.contains("text") && c["text"].is_string()) {
                        out += c["text"].get<std::string>();
                    }
                }
            }
        }
    }
    if (out.empty()) {
        error = "the answer has no text";
    }
    return out;
}

std::string OneOf(const json& j, const char* key, std::initializer_list<const char*> allowed, std::vector<std::string>& warnings) {
    const std::string v = Lower(text::Trim(StringField(j, key)));
    for (const char* a : allowed) {
        if (v == a) {
            return v;
        }
    }
    if (!v.empty() && v != "unknown") {
        warnings.push_back(fmt::format("{}-not-understood", key));
    }
    return "unknown";
}

std::vector<std::string> Words(const json& j, const char* key, size_t most) {
    std::vector<std::string> out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) {
        return out;
    }
    for (const json& v : *it) {
        if (v.is_string()) {
            const std::string w = Lower(text::Trim(v.get<std::string>()));
            if (!w.empty() && w.size() <= 40 && w != "unknown" && out.size() < most) {
                out.push_back(w);
            }
        }
    }
    return out;
}

} // namespace

bool ParseGeminiCharacter(const std::string& body, SongCharacter& c, std::string& error) {
    const json answer = json::parse(body, nullptr, false);
    if (answer.is_discarded()) {
        error = "the answer is not JSON";
        return false;
    }
    std::string textAnswer = AnswerText(answer, error);
    if (textAnswer.empty()) {
        return false;
    }
    // A model sometimes wraps JSON in a ```json fence even when asked not to.
    const size_t open = textAnswer.find('{');
    const size_t close = textAnswer.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        error = "the answer has no JSON object";
        return false;
    }
    const json j = json::parse(textAnswer.substr(open, close - open + 1), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "the answer's JSON cannot be read";
        return false;
    }
    std::vector<std::string> warnings;
    const std::string genre = Lower(text::Trim(StringField(j, "genre")));
    c.genre = genre.empty() || genre.size() > 40 ? "unknown" : genre;
    c.moods = Words(j, "moods", 8);
    c.textures = Words(j, "textures", 8);
    c.energy = OneOf(j, "energy", { "low", "medium", "high", "varied" }, warnings);
    c.vocalPresence = OneOf(j, "vocalPresence", { "none", "some", "lead" }, warnings);
    double confidence = NumberField(j, "themeConfidence", 0);
    confidence = confidence >= 0 && confidence <= 1 ? confidence : 0;
    const std::string theme = Lower(text::Trim(StringField(j, "suggestedTheme")));
    const auto& themes = KnownThemes();
    c.suggestedTheme = "uncertain";
    c.themeConfidence = 0;
    if (std::find(themes.begin(), themes.end(), theme) == themes.end()) {
        if (!theme.empty()) {
            warnings.push_back("theme-not-known");
        }
    } else if (theme != "uncertain" && confidence < 0.5) {
        warnings.push_back("theme-low-confidence");
    } else if (theme != "uncertain") {
        c.suggestedTheme = theme;
        c.themeConfidence = std::round(confidence * 100) / 100;
    }
    int previousEnd = 0;
    for (const json& s : j.value("sections", json::array())) {
        CharacterSection section;
        const double start = NumberField(s, "startSeconds");
        const double end = NumberField(s, "endSeconds");
        section.label = Lower(text::Trim(StringField(s, "label")));
        section.energy = OneOf(s, "energy", { "low", "medium", "high", "varied" }, warnings);
        if (start < 0 || end <= start || section.label.empty() || Seconds(start) < previousEnd - 1000) {
            warnings.push_back("section-dropped");
            continue;
        }
        section.startMS = Seconds(start);
        section.endMS = Seconds(end);
        previousEnd = section.endMS;
        c.sections.push_back(std::move(section));
    }
    std::sort(warnings.begin(), warnings.end());
    warnings.erase(std::unique(warnings.begin(), warnings.end()), warnings.end());
    c.warnings = warnings;
    return true;
}

bool DescribeWithGemini(IHttpClient& client, const std::string& audioPath, const ApiKey& key, bool consent, const CharacterOptions& options, const RetryPolicy& policy,
                        SongCharacter& character, AssistFailure& failure) {
    failure = AssistFailure();
    if (!consent) {
        failure = { AssistError::NoConsent, 0, -1, 0, "the song is only sent when the person allows it" };
        return false;
    }
    if (key.value.empty()) {
        failure = { AssistError::NoCredentials, 0, -1, 0, "no Gemini key: set it in the window or GEMINI_API_KEY" };
        return false;
    }
    const std::string mime = AudioMimeType(audioPath);
    if (mime.empty()) {
        failure = { AssistError::UnsupportedFile, 0, -1, 0, "Gemini takes wav, mp3, aiff, aac, ogg, flac or m4a" };
        return false;
    }
    const long long size = FileSize(audioPath);
    if (size < 0 || size > options.maxBytes) {
        failure = { size < 0 ? AssistError::UnsupportedFile : AssistError::FileTooLarge, 0, -1, 0,
                    size < 0 ? std::string("the song cannot be read") : fmt::format("{:.1f} MB is more than the {:.0f} MB sent inline", size / 1048576.0, options.maxBytes / 1048576.0) };
        return false;
    }
    const HttpResponse r = SendWithRetry(client, BuildGeminiRequest(ReadBytes(audioPath), mime, key.value, options), policy, failure);
    failure.message = RedactSecrets(failure.message, { key.value });
    if (failure.error != AssistError::None) {
        return false;
    }
    std::string error;
    character = SongCharacter();
    if (!ParseGeminiCharacter(r.body, character, error)) {
        failure = { AssistError::BadResponse, r.status, -1, failure.attempts, RedactSecrets(error, { key.value }) };
        return false;
    }
    character.provider = "gemini";
    character.model = options.model;
    return true;
}

std::string ToJson(const SongCharacter& c) {
    json sections = json::array();
    for (const CharacterSection& s : c.sections) {
        sections.push_back({ { "startMS", s.startMS }, { "endMS", s.endMS }, { "label", s.label }, { "energy", s.energy } });
    }
    const json root = { { "kind", "composer-song-character" }, { "version", MUSIC_ASSIST_VERSION }, { "interpretation", true }, { "provider", c.provider }, { "model", c.model },
                        { "genre", c.genre }, { "moods", c.moods }, { "energy", c.energy }, { "vocalPresence", c.vocalPresence }, { "textures", c.textures },
                        { "suggestedTheme", c.suggestedTheme }, { "themeConfidence", c.themeConfidence }, { "sections", sections }, { "warnings", c.warnings }, { "cacheKey", c.cacheKey } };
    return root.dump(2);
}

bool FromJson(const std::string& text, SongCharacter& c, std::string& error) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || StringField(j, "kind") != "composer-song-character") {
        error = "not a Show Composer song description";
        return false;
    }
    c = SongCharacter();
    c.provider = StringField(j, "provider");
    c.model = StringField(j, "model");
    c.genre = j.value("genre", std::string("unknown"));
    c.moods = j.value("moods", std::vector<std::string>());
    c.energy = j.value("energy", std::string("unknown"));
    c.vocalPresence = j.value("vocalPresence", std::string("unknown"));
    c.textures = j.value("textures", std::vector<std::string>());
    c.suggestedTheme = j.value("suggestedTheme", std::string("uncertain"));
    c.themeConfidence = j.value("themeConfidence", 0.0);
    c.warnings = j.value("warnings", std::vector<std::string>());
    c.cacheKey = StringField(j, "cacheKey");
    for (const json& s : j.value("sections", json::array())) {
        c.sections.push_back({ s.value("startMS", -1), s.value("endMS", -1), StringField(s, "label"), s.value("energy", std::string("unknown")) });
    }
    return true;
}

// ------------------------------------------------------------------- the cache

std::string AssistCacheKey(const std::string& audioHash, const std::string& lyricsHash, const std::string& provider, const std::string& model, const std::string& params) {
    return HashText(fmt::format("{}|{}|{}|{}|{}|{}", audioHash, lyricsHash, provider, model, MUSIC_ASSIST_VERSION, params));
}

std::string AssistCachePath(const std::string& showDir, const std::string& key, const std::string& kind) {
    return (fs::path(showDir) / "ShowComposer" / "cache" / (key + "." + kind + ".json")).string();
}

// --------------------------------------------------------- transcript to lyrics

std::string MatchKey(const std::string& word) {
    std::string out;
    for (char32_t c : text::Decode(text::Fold(word))) {
        if (text::IsLetter(c) || text::IsDigit(c)) {
            out += text::Encode(c);
        }
    }
    return out;
}

TranscriptAlignment AlignTranscriptToLyrics(const LyricDocument& lyrics, const Transcript& transcript) {
    TranscriptAlignment out;
    struct LyricToken {
        std::string id;
        std::string key;
        size_t line;
    };
    std::vector<LyricToken> lyric;
    const std::vector<const LyricLine*> lines = lyrics.Lines();
    for (size_t l = 0; l < lines.size(); ++l) {
        const LyricLine& line = *lines[l];
        TranscriptLineMatch m;
        m.lineId = line.id;
        if (!line.instrumental) {
            for (size_t w = 0; w < line.words.size(); ++w) {
                const std::string key = MatchKey(line.words[w].text);
                if (!key.empty()) {
                    lyric.push_back({ fmt::format("{}/{}:{}", line.id, w, line.words[w].key), key, l });
                    ++m.words;
                }
            }
        }
        out.lines.push_back(m);
    }
    // Whisper sometimes hears "navidad," or " Navidad" or "tal-que": one key per word.
    struct Heard {
        std::string key;
        const TranscriptWord* word;
    };
    std::vector<Heard> heard;
    for (const TranscriptWord& w : transcript.words) {
        const std::string key = MatchKey(w.text);
        if (!key.empty()) {
            heard.push_back({ key, &w });
        }
    }
    out.lyricWords = static_cast<int>(lyric.size());
    out.transcriptWords = static_cast<int>(heard.size());
    if (lyric.empty() || heard.empty()) {
        out.warnings.push_back(lyric.empty() ? "no-lyric-words" : "no-words-heard");
        return out;
    }

    // In order, as a singer sings: a word skipped in the lyrics costs more than a word
    // heard that is not in them (fillers, "oh oh", a line sung twice).
    constexpr double MATCH = 2.0;
    constexpr double SKIP_LYRIC = -0.6;
    constexpr double SKIP_HEARD = -0.25;
    const size_t n = lyric.size();
    const size_t m = heard.size();
    std::vector<double> score((n + 1) * (m + 1), 0);
    std::vector<unsigned char> move((n + 1) * (m + 1), 0); // 1 match, 2 skip lyric, 3 skip heard
    const auto at = [m](size_t i, size_t j) { return i * (m + 1) + j; };
    for (size_t i = 1; i <= n; ++i) {
        score[at(i, 0)] = score[at(i - 1, 0)] + SKIP_LYRIC;
        move[at(i, 0)] = 2;
    }
    for (size_t j = 1; j <= m; ++j) {
        score[at(0, j)] = 0; // what is heard before the lyrics start is free
        move[at(0, j)] = 3;
    }
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            double best = score[at(i - 1, j)] + SKIP_LYRIC;
            unsigned char how = 2;
            const double heardSkip = score[at(i, j - 1)] + (i == n ? 0 : SKIP_HEARD); // and after they end
            if (heardSkip > best) {
                best = heardSkip;
                how = 3;
            }
            const double sim = Similarity(lyric[i - 1].key, heard[j - 1].key);
            if (sim > 0 && score[at(i - 1, j - 1)] + MATCH * sim > best) {
                best = score[at(i - 1, j - 1)] + MATCH * sim;
                how = 1;
            }
            score[at(i, j)] = best;
            move[at(i, j)] = how;
        }
    }
    struct Pair {
        size_t lyric;
        size_t heard;
        double sim;
    };
    std::vector<Pair> pairs;
    for (size_t i = n, j = m; i > 0 || j > 0;) {
        const unsigned char how = move[at(i, j)];
        if (how == 1) {
            pairs.push_back({ i - 1, j - 1, Similarity(lyric[i - 1].key, heard[j - 1].key) });
            --i;
            --j;
        } else if (how == 2) {
            --i;
        } else {
            --j;
        }
    }
    std::reverse(pairs.begin(), pairs.end());

    // A word matched far from the rest of its line is a coincidence ("la", "que"
    // are everywhere): it is dropped rather than pulling the line apart.
    std::map<size_t, std::vector<const Pair*>> byLine;
    for (const Pair& p : pairs) {
        byLine[lyric[p.lyric].line].push_back(&p);
    }
    for (auto& [line, matched] : byLine) {
        std::vector<int> starts;
        for (const Pair* p : matched) {
            starts.push_back(heard[p->heard].word->startMS);
        }
        std::nth_element(starts.begin(), starts.begin() + static_cast<std::ptrdiff_t>(starts.size() / 2), starts.end());
        const int median = starts[starts.size() / 2];
        const int reach = out.lines[line].words * 800 + 3000;
        const bool weak = matched.size() * 2 < static_cast<size_t>(out.lines[line].words);
        for (const Pair* p : matched) {
            const TranscriptWord& w = *heard[p->heard].word;
            const bool shortWord = text::Decode(lyric[p->lyric].key).size() <= 3;
            if (std::abs(w.startMS - median) > reach || (weak && shortWord && matched.size() == 1)) {
                out.warnings.push_back("isolated-match-dropped");
                continue;
            }
            // Whisper's word times are good to a tenth of a second or so, not better.
            double confidence = w.confidence >= 0 ? w.confidence : 0.8;
            for (const TranscriptSegment& s : transcript.segments) {
                if (w.startMS >= s.startMS && w.startMS < s.endMS) {
                    confidence = std::min(confidence, std::exp(s.avgLogprob));
                    break;
                }
            }
            confidence = std::clamp(confidence * p->sim, 0.0, 0.85);
            out.anchors.push_back({ lyric[p->lyric].id, w.startMS, std::max(w.endMS, w.startMS + 1), std::round(confidence * 1000) / 1000 });
            out.lines[line].matched++;
            out.lines[line].confidence += confidence;
        }
    }
    for (TranscriptLineMatch& l : out.lines) {
        l.confidence = l.words > 0 ? std::round(l.confidence / l.words * 1000) / 1000 : 0;
    }
    out.matchedWords = static_cast<int>(out.anchors.size());
    out.extraWords = out.transcriptWords - static_cast<int>(pairs.size());
    out.coverage = std::round(static_cast<double>(out.matchedWords) / out.lyricWords * 1000) / 1000;
    const int silentLines = static_cast<int>(std::count_if(out.lines.begin(), out.lines.end(), [](const TranscriptLineMatch& l) { return l.words > 0 && l.matched == 0; }));
    if (silentLines > 0) {
        out.warnings.push_back(fmt::format("lines-not-heard:{}", silentLines));
    }
    if (out.coverage < 0.5) {
        out.warnings.push_back("transcript-low-coverage");
    }
    std::sort(out.warnings.begin(), out.warnings.end());
    out.warnings.erase(std::unique(out.warnings.begin(), out.warnings.end()), out.warnings.end());
    return out;
}

// --------------------------------------------------------------------- LRC out

std::string ExportLrc(const std::vector<TimedLine>& lines, const std::string& title, const std::string& artist) {
    std::string out;
    if (!title.empty()) {
        out += "[ti:" + title + "]\n";
    }
    if (!artist.empty()) {
        out += "[ar:" + artist + "]\n";
    }
    out += "[by:xLights Show Composer]\n";
    std::vector<TimedLine> sorted;
    for (const TimedLine& l : lines) {
        if (l.startMS >= 0) {
            sorted.push_back(l);
        }
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const TimedLine& a, const TimedLine& b) { return a.startMS < b.startMS; });
    for (const TimedLine& l : sorted) {
        std::string text = l.text;
        std::replace(text.begin(), text.end(), '\n', ' ');
        out += LrcTime(l.startMS) + text + "\n";
    }
    return out;
}

std::string Base64Encode(const std::string& bytes) {
    static const char* const ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) | static_cast<unsigned char>(bytes[i + 2]);
        out += ALPHABET[(v >> 18) & 63];
        out += ALPHABET[(v >> 12) & 63];
        out += ALPHABET[(v >> 6) & 63];
        out += ALPHABET[v & 63];
    }
    if (i < bytes.size()) {
        unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) {
            v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        }
        out += ALPHABET[(v >> 18) & 63];
        out += ALPHABET[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? ALPHABET[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

} // namespace composer
