#include "composer/MediaValidation.h"

#include "composer/MediaAnalysis.h"
#include "composer/VisualModel.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace composer {

namespace {

fs::path P(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string S(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

uint64_t Mix(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb93ca63fe53bULL;
    h ^= h >> 33;
    return h;
}

uint64_t Hash(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : s) {
        h = (h ^ c) * 1099511628211ULL;
    }
    return Mix(h);
}

std::string Now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char out[32];
    std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%S", &tm);
    return out;
}

bool Write(const fs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

double Ratio(double a, double b) {
    return b > 0 ? a / b : 0;
}

// The group of a file among its duplicates: the file it duplicates, or its own id.
std::string GroupOf(const MediaItem& m) {
    for (const std::string& i : m.analysis.issues) {
        if (i.rfind("duplicate-of:", 0) == 0) {
            return i.substr(std::string("duplicate-of:").size());
        }
    }
    return m.id;
}

} // namespace

MediaValidationSet BuildMediaValidationSet(const MediaLibrary& library, size_t size, uint64_t seed) {
    MediaValidationSet set;
    set.created = Now();
    // Grouped by folder and kind, each group in a fixed shuffled order, taken in turns:
    // every folder and the videos get their share.
    std::map<std::string, std::vector<const MediaItem*>> groups;
    for (const MediaItem& m : library.items) {
        if (!m.Usable() || !m.analysis.Done()) {
            continue;
        }
        const std::string folder = S(P(m.path).parent_path());
        groups[folder + "|" + MediaKindId(m.kind)].push_back(&m);
        if (set.model.empty() && !m.analysis.model.empty()) {
            set.model = m.analysis.model;
        }
    }
    for (auto& [k, list] : groups) {
        std::sort(list.begin(), list.end(), [&](const MediaItem* a, const MediaItem* b) { return Mix(Hash(a->id) ^ seed) < Mix(Hash(b->id) ^ seed); });
    }
    std::set<std::string> dhashes;
    for (size_t round = 0; set.entries.size() < size; ++round) {
        bool any = false;
        for (auto& [k, list] : groups) {
            if (round >= list.size() || set.entries.size() >= size) {
                continue;
            }
            any = true;
            const MediaItem& m = *list[round];
            // Exact duplicates teach nothing twice.
            if (m.analysis.dhash != 0 && !dhashes.insert(std::to_string(m.analysis.dhash)).second) {
                continue;
            }
            MediaValidationEntry e;
            e.mediaId = m.id;
            e.name = m.name;
            e.path = m.path;
            e.kind = MediaKindId(m.kind);
            for (size_t i = 0; i < m.analysis.concepts.size() && i < 6; ++i) {
                e.proposed.push_back(m.analysis.concepts[i]);
            }
            e.description = m.analysis.description;
            e.categories = m.analysis.categories;
            e.poiX = m.analysis.poiX;
            e.poiY = m.analysis.poiY;
            e.matrixFit = m.analysis.matrixFit;
            e.treeFit = m.analysis.treeFit;
            e.issues = m.analysis.issues;
            e.group = GroupOf(m);
            set.entries.push_back(std::move(e));
        }
        if (!any) {
            break;
        }
    }
    std::sort(set.entries.begin(), set.entries.end(), [](const MediaValidationEntry& a, const MediaValidationEntry& b) { return a.path < b.path; });
    return set;
}

void CarryMediaLabels(MediaValidationSet& into, const MediaValidationSet& labelled) {
    for (MediaValidationEntry& e : into.entries) {
        for (const MediaValidationEntry& l : labelled.entries) {
            if (l.mediaId == e.mediaId && l.Labelled()) {
                e.labelSource = l.labelSource;
                e.reviewer = l.reviewer;
                e.reviewedAt = l.reviewedAt;
                e.present = l.present;
                e.poiUseful = l.poiUseful;
                e.descriptionOk = l.descriptionOk;
                e.notes = l.notes;
            }
        }
    }
}

json MediaValidationSetJson(const MediaValidationSet& set) {
    json list = json::array();
    for (const MediaValidationEntry& e : set.entries) {
        json proposed = json::array();
        for (const MediaConcept& c : e.proposed) {
            proposed.push_back({ { "id", c.id }, { "score", c.score }, { "level", c.level }, { "source", c.source } });
        }
        list.push_back({ { "mediaId", e.mediaId },
                         { "name", e.name },
                         { "path", e.path },
                         { "kind", e.kind },
                         { "proposed", proposed },
                         { "description", e.description },
                         { "categories", e.categories },
                         { "poi", { e.poiX, e.poiY } },
                         { "matrixFit", e.matrixFit },
                         { "treeFit", e.treeFit },
                         { "issues", e.issues },
                         { "group", e.group },
                         { "labels",
                           { { "source", e.labelSource },
                             { "reviewer", e.reviewer },
                             { "reviewedAt", e.reviewedAt },
                             { "present", e.present },
                             { "poiUseful", e.poiUseful },
                             { "descriptionOk", e.descriptionOk },
                             { "notes", e.notes } } } });
    }
    return { { "version", set.version },
             { "model", set.model },
             { "created", set.created },
             { "howToLabel", "For each file, a person writes in labels.present the concept ids really in the picture (from visual-concepts.json), "
                             "poiUseful 1/0 and descriptionOk 1/0, their name in reviewer and \"person\" in source. Labels from anyone else must say "
                             "\"assistant-provisional\" and are never counted as a human measure." },
             { "entries", list } };
}

bool MediaValidationSetFrom(const json& j, MediaValidationSet& set, std::string& error) {
    set = MediaValidationSet();
    if (!j.is_object() || !j.contains("entries") || !j["entries"].is_array()) {
        error = "validation set: no entries";
        return false;
    }
    set.version = j.value("version", 1);
    set.model = j.value("model", std::string());
    set.created = j.value("created", std::string());
    for (const json& x : j["entries"]) {
        if (!x.is_object()) {
            continue;
        }
        MediaValidationEntry e;
        e.mediaId = x.value("mediaId", std::string());
        e.name = x.value("name", std::string());
        e.path = x.value("path", std::string());
        e.kind = x.value("kind", std::string());
        for (const json& c : x.value("proposed", json::array())) {
            MediaConcept mc;
            mc.id = c.value("id", std::string());
            mc.score = c.value("score", 0.0);
            mc.level = c.value("level", std::string());
            mc.source = c.value("source", std::string());
            e.proposed.push_back(mc);
        }
        e.description = x.value("description", std::vector<std::string>());
        e.categories = x.value("categories", std::vector<std::string>());
        const json poi = x.value("poi", json::array());
        if (poi.is_array() && poi.size() == 2 && poi[0].is_number() && poi[1].is_number()) {
            e.poiX = poi[0].get<double>();
            e.poiY = poi[1].get<double>();
        }
        e.matrixFit = x.value("matrixFit", 0.0);
        e.treeFit = x.value("treeFit", 0.0);
        e.issues = x.value("issues", std::vector<std::string>());
        e.group = x.value("group", std::string());
        const json l = x.value("labels", json::object());
        if (l.is_object()) {
            e.labelSource = l.value("source", std::string());
            if (!e.labelSource.empty() && e.labelSource != "person" && e.labelSource != "assistant-provisional") {
                error = "validation set: unknown label source '" + e.labelSource + "'";
                return false;
            }
            e.reviewer = l.value("reviewer", std::string());
            e.reviewedAt = l.value("reviewedAt", std::string());
            e.present = l.value("present", std::vector<std::string>());
            e.poiUseful = l.value("poiUseful", -1);
            e.descriptionOk = l.value("descriptionOk", -1);
            e.notes = l.value("notes", std::string());
        }
        set.entries.push_back(std::move(e));
    }
    return true;
}

MediaValidationSet MediaValidationSetFromLibrary(const MediaLibrary& library) {
    MediaValidationSet set;
    set.created = Now();
    for (const MediaItem& it : library.items) {
        if (!it.human.complete || !it.Usable()) {
            continue;
        }
        if (set.model.empty() && !it.analysis.model.empty()) {
            set.model = it.analysis.model;
        }
        MediaValidationEntry e;
        e.mediaId = it.id;
        e.name = it.name;
        e.path = it.path;
        e.kind = MediaKindId(it.kind);
        // The model's own words, as it said them: the labels never change them.
        e.proposed = it.analysis.concepts;
        e.description = it.analysis.description;
        e.categories = it.analysis.categories;
        e.poiX = it.analysis.poiX;
        e.poiY = it.analysis.poiY;
        e.issues = it.analysis.issues;
        e.labelSource = "person";
        e.reviewer = it.human.reviewer;
        e.reviewedAt = it.human.reviewedAt;
        e.present = it.human.confirmed;
        for (const std::string& a : it.human.added) {
            if (std::find(e.present.begin(), e.present.end(), a) == e.present.end()) {
                e.present.push_back(a);
            }
        }
        e.group = GroupOf(it);
        set.entries.push_back(std::move(e));
    }
    return set;
}

MediaValidationMetrics EvaluateMediaValidation(const MediaValidationSet& set, const std::string& source, size_t topK) {
    MediaValidationMetrics m;
    m.source = source;
    m.topK = topK;
    int hits = 0, poiYes = 0, descYes = 0;
    std::set<std::string> groups;
    for (const MediaValidationEntry& e : set.entries) {
        if (e.labelSource != source) {
            continue;
        }
        if (!e.group.empty() && !groups.insert(e.group).second) {
            ++m.duplicatesSkipped;
            continue;
        }
        ++m.files;
        const std::set<std::string> present(e.present.begin(), e.present.end());
        std::set<std::string> proposed;
        for (size_t i = 0; i < e.proposed.size() && i < topK; ++i) {
            proposed.insert(e.proposed[i].id);
            if (e.proposed[i].level == "strong" || e.proposed[i].level == "probable") {
                (present.contains(e.proposed[i].id) ? m.confidentRight : m.confidentWrong) += 1;
            }
        }
        bool hit = false;
        std::vector<std::string> wrong, missed;
        for (const std::string& id : proposed) {
            if (present.contains(id)) {
                ++m.truePositive;
                ++m.concepts[id].truePositive;
                hit = true;
            } else {
                ++m.falsePositive;
                ++m.concepts[id].falsePositive;
                wrong.push_back(id);
            }
        }
        for (const std::string& id : present) {
            if (!proposed.contains(id)) {
                ++m.falseNegative;
                ++m.concepts[id].falseNegative;
                missed.push_back(id);
            }
        }
        // A wrong concept stands for one that was there and not seen.
        for (const std::string& w : wrong) {
            for (const std::string& x : missed) {
                ++m.confusion[{ w, x }];
            }
        }
        hits += hit ? 1 : 0;
        if (e.poiUseful >= 0) {
            ++m.poiAnswered;
            poiYes += e.poiUseful;
        }
        if (e.descriptionOk >= 0) {
            ++m.descriptionAnswered;
            descYes += e.descriptionOk;
        }
    }
    m.precision = Ratio(m.truePositive, m.truePositive + m.falsePositive);
    m.recall = Ratio(m.truePositive, m.truePositive + m.falseNegative);
    m.confidentPrecision = Ratio(m.confidentRight, m.confidentRight + m.confidentWrong);
    m.f1 = Ratio(2 * m.precision * m.recall, m.precision + m.recall);
    m.reliable = m.files >= MIN_VALIDATION_FILES;
    m.filesWithAHit = Ratio(hits, m.files);
    m.poiUseful = Ratio(poiYes, m.poiAnswered);
    m.descriptionOk = Ratio(descYes, m.descriptionAnswered);
    return m;
}

std::string MediaValidationReport(const MediaValidationSet& set, const std::vector<MediaValidationMetrics>& metrics) {
    std::string out = fmt::format("Validation set: {} files, model {}\n", set.entries.size(), set.model.empty() ? std::string("none (colours, light and motion only)") : set.model);
    int person = 0, provisional = 0;
    for (const MediaValidationEntry& e : set.entries) {
        person += e.labelSource == "person" ? 1 : 0;
        provisional += e.labelSource == "assistant-provisional" ? 1 : 0;
    }
    out += fmt::format("Labelled by a person: {}; provisional labels (not a human measure): {}; not labelled: {}\n", person, provisional, static_cast<int>(set.entries.size()) - person - provisional);
    if (person == 0) {
        out += "No human labels yet: there is no measure of the model's accuracy on this library. Label the set and run again.\n";
    }
    for (const MediaValidationMetrics& m : metrics) {
        if (m.files == 0) {
            continue;
        }
        out += fmt::format("\n{} labels, {} files, top {} concepts{}:\n", m.source == "person" ? "Human" : "PROVISIONAL (assistant, not human)", m.files, m.topK,
                           m.duplicatesSkipped > 0 ? fmt::format(" ({} duplicates counted once)", m.duplicatesSkipped) : std::string());
        if (!m.reliable) {
            out += fmt::format("  TOO FEW FILES ({} of the {} needed): the numbers below only hint, they are not a measure\n", m.files, MIN_VALIDATION_FILES);
        }
        out += fmt::format("  F1 {:.2f}\n", m.f1);
        out += fmt::format("  precision {:.2f} ({} right, {} wrong), recall {:.2f} ({} missed), files with at least one right concept {:.0f}%\n", m.precision, m.truePositive, m.falsePositive, m.recall,
                           m.falseNegative, m.filesWithAHit * 100);
        out += fmt::format("  precision of the strong and probable concepts only: {:.2f} ({} right, {} wrong); weak ones weigh a quarter in the montage\n", m.confidentPrecision, m.confidentRight,
                           m.confidentWrong);
        if (m.poiAnswered > 0) {
            out += fmt::format("  point of interest useful: {:.0f}% of {}\n", m.poiUseful * 100, m.poiAnswered);
        }
        if (m.descriptionAnswered > 0) {
            out += fmt::format("  description good enough: {:.0f}% of {}\n", m.descriptionOk * 100, m.descriptionAnswered);
        }
        std::vector<std::pair<std::string, MediaConceptCount>> worst(m.concepts.begin(), m.concepts.end());
        std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) {
            const int ea = a.second.falsePositive + a.second.falseNegative, eb = b.second.falsePositive + b.second.falseNegative;
            return ea != eb ? ea > eb : a.first < b.first;
        });
        out += "  concepts (right / wrong / missed, precision, recall, F1):\n";
        for (const auto& [id, c] : worst) {
            const double p = Ratio(c.truePositive, c.truePositive + c.falsePositive), r = Ratio(c.truePositive, c.truePositive + c.falseNegative);
            const int support = c.truePositive + c.falseNegative;
            out += fmt::format("    {:<22} {:>3} / {:>3} / {:>3}  {:.2f} {:.2f} {:.2f}{}\n", id, c.truePositive, c.falsePositive, c.falseNegative, p, r, Ratio(2 * p * r, p + r),
                               support < MIN_CONCEPT_SUPPORT ? "  (few files)" : "");
        }
        std::vector<std::pair<std::pair<std::string, std::string>, int>> conf(m.confusion.begin(), m.confusion.end());
        std::sort(conf.begin(), conf.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        if (!conf.empty()) {
            out += "  most frequent confusions (proposed -> really there):\n";
            for (size_t i = 0; i < conf.size() && i < 12; ++i) {
                out += fmt::format("    {} -> {}: {}\n", conf[i].first.first, conf[i].first.second, conf[i].second);
            }
        }
    }
    return out;
}

int RunMediaEvaluation(const std::string& showDir, const std::vector<std::string>& folders, const std::string& modelDir, const std::string& outDir, const std::string& labelsFile, size_t sampleSize) {
    std::error_code ec;
    const fs::path out = P(outDir);
    fs::create_directories(out, ec);
    if (showDir.empty() || folders.empty() || ec) {
        printf("composer-media-eval: needs -s <show>, --media-folders and --out\n");
        return 2;
    }
    VisualConcepts concepts;
    std::string error;
    if (!ReadVisualConcepts(DefaultVisualConceptsPath(), concepts, error)) {
        printf("composer-media-eval: concepts: %s\n", error.c_str());
        return 1;
    }
    std::unique_ptr<VisualModel> model;
    ConceptIndex index;
    if (!modelDir.empty()) {
        model = OpenVisualModel(modelDir, error);
        if (model == nullptr || !BuildConceptIndex(*model, concepts, MediaCacheDir(showDir), index, error)) {
            printf("composer-media-eval: the model could not be used: %s\n", error.c_str());
            return 1;
        }
    }
    MediaLibrary previous;
    const bool hadLibrary = ReadMediaLibrary(MediaLibraryPath(showDir), previous, error);
    std::string report;
    MediaLibrary library;
    for (int pass = 0; pass < 2; ++pass) {
        MediaScanOptions so;
        so.showDir = showDir;
        so.folders = folders;
        const auto t0 = std::chrono::steady_clock::now();
        library = ScanMediaFolders(so, pass == 0 ? (hadLibrary ? &previous : nullptr) : &library);
        const auto scanMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        MediaAnalysisOptions o;
        o.showDir = showDir;
        o.concepts = &concepts;
        o.model = model.get();
        o.index = model != nullptr ? &index : nullptr;
        const MediaAnalysisSummary s = AnalyzeMediaLibrary(library, o);
        const std::string line = fmt::format("pass {}: {} files ({} pictures, {} animations, {} videos), {} usable, {} not supported, {} damaged; scan {} ms, analysis {} ms: {} looked at, {} from the cache, "
                                             "{} with the model, {} failed, {} duplicates, {} similar\n",
                                             pass + 1, s.total, s.images, s.animations, s.videos, s.usable, s.unsupported, s.damaged, scanMS, s.elapsedMS, s.analysed, s.cached, s.semantic, s.failed,
                                             s.duplicates, s.similar);
        printf("composer-media-eval: %s", line.c_str());
        report += line;
        if (pass == 0) {
            report += MediaAnalysisReport(library, s);
        }
        if (!SaveMediaLibrary(MediaLibraryPath(showDir), library, error)) {
            printf("composer-media-eval: the library could not be kept: %s\n", error.c_str());
            return 1;
        }
    }
    // The cache's size on disk.
    uintmax_t cacheBytes = 0;
    for (const auto& e : fs::recursive_directory_iterator(P(MediaCacheDir(showDir)), ec)) {
        cacheBytes += e.is_regular_file(ec) ? e.file_size(ec) : 0;
    }
    report += fmt::format("cache: {:.1f} MB in {}\n", cacheBytes / 1048576.0, MediaCacheDir(showDir));
    MediaValidationSet set = BuildMediaValidationSet(library, sampleSize, 2026);
    if (!labelsFile.empty()) {
        std::ifstream in(P(labelsFile), std::ios::binary);
        const json j = json::parse(in, nullptr, false);
        MediaValidationSet labelled;
        if (!MediaValidationSetFrom(j, labelled, error)) {
            printf("composer-media-eval: labels: %s\n", error.c_str());
            return 1;
        }
        CarryMediaLabels(set, labelled);
    }
    // Every file a person labelled completely in the library counts, with their labels.
    for (const MediaValidationEntry& lib : MediaValidationSetFromLibrary(library).entries) {
        auto it = std::find_if(set.entries.begin(), set.entries.end(), [&](const MediaValidationEntry& e) { return e.mediaId == lib.mediaId; });
        if (it != set.entries.end()) {
            *it = lib;
        } else {
            set.entries.push_back(lib);
        }
    }
    std::vector<MediaValidationMetrics> metrics;
    for (const char* source : { "person", "assistant-provisional" }) {
        metrics.push_back(EvaluateMediaValidation(set, source, 3));
    }
    const std::string text = MediaValidationReport(set, metrics);
    printf("%s", text.c_str());
    const bool ok = Write(out / "validation-set.json", MediaValidationSetJson(set).dump(2)) && Write(out / "validation-report.txt", text) && Write(out / "media-analysis-report.txt", report);
    return ok ? 0 : 1;
}

} // namespace composer
