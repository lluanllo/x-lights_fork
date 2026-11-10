#include "composer/UnderstandingStore.h"

#include "composer/SongAnalysis.h"
#include "utils/ExternalHooks.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

namespace {

fs::path P(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string In(const std::string& folder, const char* name) {
    return (P(folder) / name).string();
}

bool StemsMatch(const AudioUnderstanding& u, const std::vector<StemInput>& stems) {
    if (u.stems.size() != stems.size()) {
        return false;
    }
    for (size_t i = 0; i < stems.size(); ++i) {
        const StemInfo& s = u.stems[i];
        if (s.role != stems[i].role || s.name != stems[i].name || s.hash != Sha256OfFile(stems[i].path)) {
            return false;
        }
    }
    return true;
}

bool ReadCorrections(const std::string& folder, std::vector<UnderstandingCorrection>& corrections, std::string& error) {
    std::string text;
    if (!ReadTextFile(In(folder, "corrections.json"), text)) {
        corrections.clear();
        return true;
    }
    AudioUnderstanding holder;
    if (!FromJson(text, holder, error)) {
        return false;
    }
    corrections = std::move(holder.corrections);
    return true;
}

bool ReadManual(const std::string& folder, std::vector<PerformerAssignment>& manual, std::string& error) {
    std::string text;
    if (!ReadTextFile(In(folder, "performers.json"), text)) {
        manual.clear();
        return true;
    }
    return FromJson(text, manual, error);
}

} // namespace

std::vector<StemInput> ParseStemList(const std::string& text) {
    std::vector<StemInput> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = std::min(text.find(';', at), text.size());
        const std::string item = text.substr(at, end - at);
        const size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0 && eq + 1 < item.size()) {
            StemInput stem;
            stem.role = item.substr(0, eq);
            stem.path = item.substr(eq + 1);
            stem.name = P(stem.path).stem().string();
            out.push_back(stem);
        }
        at = end + 1;
    }
    return out;
}

std::string StemListText(const std::vector<StemInput>& stems) {
    std::string out;
    for (const StemInput& s : stems) {
        out += (out.empty() ? "" : ";") + s.role + "=" + s.path;
    }
    return out;
}

std::string UnderstandingFolder(const std::string& showDir, const std::string& audioHash) {
    return (P(showDir) / "ShowComposer" / "understanding" / audioHash.substr(0, 16)).string();
}

bool ReadTextFile(const std::string& path, std::string& text) {
    std::error_code ec;
    if (!fs::is_regular_file(P(path), ec)) {
        return false;
    }
    std::ifstream in(P(path), std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return static_cast<bool>(in) || in.eof();
}

bool WriteTextFileAtomically(const std::string& path, const std::string& text, std::string& error) {
    std::error_code ec;
    fs::create_directories(P(path).parent_path(), ec);
    const fs::path tmp = P(path + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            error = "write:" + P(path).filename().string();
            return false;
        }
    }
    fs::rename(tmp, P(path), ec);
    if (ec) {
        error = "rename:" + P(path).filename().string();
        return false;
    }
    return true;
}

UnderstandOutcome UnderstandSong(const UnderstandRequest& r) {
    UnderstandOutcome out;
    if (r.showDir.empty() || r.audioPath.empty()) {
        out.errorCode = "request-incomplete";
        return out;
    }
    ObtainAccessToURL(r.audioPath, false);
    const std::string hash = Sha256OfFile(r.audioPath);
    if (hash.empty()) {
        out.errorCode = "audio-unreadable";
        out.errorDetail = P(r.audioPath).filename().string();
        return out;
    }
    SongUnderstanding& song = out.song;
    song.folder = UnderstandingFolder(r.showDir, hash);
    const std::string readingPath = In(song.folder, "reading.json");

    // The reading: reused while it is the same audio, the same reader and the same options.
    bool have = false;
    std::string text;
    if (r.reuse && ReadTextFile(readingPath, text)) {
        AudioUnderstanding cached;
        std::string error;
        if (!FromJson(text, cached, error)) {
            song.notes.push_back("stale-reading:unreadable");
        } else if (cached.audioHash != hash) {
            song.notes.push_back("stale-reading:other-audio");
        } else if (cached.version != AUDIO_UNDERSTANDING_VERSION || cached.optionsKey != UnderstandingOptionsKey(r.options)) {
            song.notes.push_back("stale-reading:reader-changed");
        } else if (!StemsMatch(cached, r.stems)) {
            song.notes.push_back("stale-reading:stems-changed");
        } else {
            cached.corrections.clear();
            song.audio = std::move(cached);
            song.fromCache = true;
            have = true;
            song.notes.push_back("reading-reused");
        }
    }
    if (!have) {
        UnderstandingResult heard = UnderstandAudio(r.audioPath, r.stems, r.options, r.progress, r.cancel);
        if (heard.cancelled) {
            out.cancelled = true;
            out.errorCode = "cancelled";
            return out;
        }
        if (!heard.ok) {
            out.errorCode = heard.errorCode;
            out.errorDetail = heard.errorDetail;
            return out;
        }
        song.audio = std::move(heard.understanding);
        song.audio.corrections.clear();
        std::string error;
        if (!WriteTextFileAtomically(readingPath, ToJson(song.audio), error)) {
            song.notes.push_back("reading-not-saved:" + error);
        } else {
            song.notes.push_back("reading-written");
        }
    }

    // The person's corrections and assignments, kept apart, applied over the reading.
    std::string error;
    if (!ReadCorrections(song.folder, song.audio.corrections, error)) {
        out.errorCode = "corrections-unreadable";
        out.errorDetail = error;
        return out;
    }
    if (!song.audio.corrections.empty()) {
        ApplyUnderstandingCorrections(song.audio);
        song.notes.push_back(fmt::format("corrections-applied:{}", song.audio.corrections.size()));
    }
    if (!ReadManual(song.folder, song.manual, error)) {
        out.errorCode = "performers-unreadable";
        out.errorDetail = error;
        return out;
    }
    RefreshVoices(song, r);
    if (!SaveDerived(song, error)) {
        song.notes.push_back("voices-not-saved:" + error);
    }
    out.ok = true;
    return out;
}

bool LoadSongUnderstanding(const std::string& folder, const UnderstandRequest& r, SongUnderstanding& song, std::string& error) {
    std::string text;
    if (!ReadTextFile(In(folder, "reading.json"), text)) {
        error = "no-reading";
        return false;
    }
    SongUnderstanding s;
    s.folder = folder;
    s.fromCache = true;
    if (!FromJson(text, s.audio, error)) {
        return false;
    }
    s.audio.corrections.clear();
    if (!ReadCorrections(folder, s.audio.corrections, error) || !ReadManual(folder, s.manual, error)) {
        return false;
    }
    ApplyUnderstandingCorrections(s.audio);
    RefreshVoices(s, r);
    song = std::move(s);
    return true;
}

void RefreshVoices(SongUnderstanding& song, const UnderstandRequest& r) {
    VocalInput in;
    in.audio = &song.audio;
    in.alignment = r.alignment;
    in.lyrics = r.lyrics;
    in.cast = r.cast;
    in.transcript = r.transcript;
    song.voices = UnderstandVoices(in);
}

bool SaveCorrections(const std::string& folder, const std::vector<UnderstandingCorrection>& corrections, std::string& error) {
    AudioUnderstanding holder;
    holder.corrections = corrections;
    json root = json::parse(ToJson(holder));
    const json kept = { { "kind", "composer-understanding-corrections" }, { "schemaVersion", root["schemaVersion"] }, { "corrections", root["corrections"] } };
    return WriteTextFileAtomically(In(folder, "corrections.json"), kept.dump(1), error);
}

bool SaveManualAssignments(const std::string& folder, const std::vector<PerformerAssignment>& manual, std::string& error) {
    return WriteTextFileAtomically(In(folder, "performers.json"), ToJson(manual), error);
}

bool SaveDerived(const SongUnderstanding& song, std::string& error) {
    return WriteTextFileAtomically(In(song.folder, "voices.json"), ToJson(song.voices), error);
}

bool RevertUnderstanding(const std::string& folder, bool restore, std::string& error) {
    std::error_code ec;
    for (const char* name : { "corrections", "performers" }) {
        const fs::path now = P(In(folder, (std::string(name) + ".json").c_str()));
        const fs::path before = P(In(folder, (std::string(name) + ".previous.json").c_str()));
        if (!restore) {
            if (fs::exists(now, ec)) {
                fs::remove(before, ec);
                fs::rename(now, before, ec);
            }
        } else if (fs::exists(before, ec)) {
            // A swap: what was made since the revert is kept as the previous one.
            const fs::path swap = P(In(folder, (std::string(name) + ".swap.json").c_str()));
            fs::remove(swap, ec);
            if (fs::exists(now, ec)) {
                fs::rename(now, swap, ec);
            }
            fs::rename(before, now, ec);
            if (!ec && fs::exists(swap, ec)) {
                fs::rename(swap, before, ec);
            }
        }
        if (ec) {
            error = "revert:" + std::string(name);
            return false;
        }
    }
    return true;
}

} // namespace composer
