#include "composer/VisualModel.h"

#include "composer/ComposerText.h"
#include "composer/MediaAnalysis.h"
#include "composer/SongAnalysis.h"
#include "composer/TemplateLibrary.h"
#include "utils/CurlManager.h"
#include "utils/xlImage.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#if !defined(__APPLE__) && defined(HAVE_ORT)
#define COMPOSER_VISUAL_ORT 1
#include <onnxruntime_cxx_api.h>
#if defined(_MSC_VER) && !defined(XLIGHTS_CMAKE_BUILD)
#pragma comment(lib, "onnxruntime.lib")
#endif
#endif

using nlohmann::json;
namespace fs = std::filesystem;

namespace composer {

namespace {

fs::path P(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string S(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

uint64_t Fnv(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// SigLIP base patch16-224: the scale and bias of its sigmoid, read from the original
// model.safetensors (logit_scale is stored as a logarithm).
constexpr double SIGLIP_LOGIT_SCALE = 4.7649970054626465;
constexpr double SIGLIP_LOGIT_BIAS = -12.9324369430542;
constexpr int SIGLIP_SIZE = 224;
constexpr int SIGLIP_TOKENS = 64;
constexpr const char* SIGLIP_VERSION = "1";

constexpr const char* REVISION = "https://huggingface.co/Xenova/siglip-base-patch16-224/resolve/4649052661e53c7000355844105f8a1792088239/";

void Normalize(std::vector<float>& v) {
    double n = 0;
    for (float x : v) {
        n += static_cast<double>(x) * x;
    }
    n = std::sqrt(n);
    if (n > 0) {
        for (float& x : v) {
            x = static_cast<float>(x / n);
        }
    }
}

} // namespace

const std::vector<VisualModelFile>& DefaultVisualModelFiles() {
    static const std::vector<VisualModelFile> files = {
        { "vision_model_quantized.onnx", "onnx/vision_model_quantized.onnx", 99499129ull, "ef14a954f3d57e1806666432bd9785004c1dc27100aa260eee0cb0f10a5de058" },
        { "text_model_quantized.onnx", "onnx/text_model_quantized.onnx", 111475220ull, "ad0329b1f35acc66d8953ff2559ce358da8eb0a7011794cf951523d63a4dbce2" },
        { "tokenizer.json", "tokenizer.json", 2398744ull, "4a17c975210be5ab4c36b47d8dae4eefb866dbfb1e676e394aad85dc30a3ae08" },
    };
    return files;
}

const VisualConceptDef* VisualConcepts::Find(const std::string& id) const {
    const auto it = std::find_if(concepts.begin(), concepts.end(), [&](const VisualConceptDef& c) { return c.id == id; });
    return it != concepts.end() ? &*it : nullptr;
}

std::string DefaultVisualConceptsPath() {
    return S(P(DefaultTemplateDirectory()).parent_path() / "visual-concepts.json");
}

bool ReadVisualConcepts(const std::string& path, VisualConcepts& out, std::string& error) {
    out = VisualConcepts();
    std::ifstream in(P(path), std::ios::binary);
    if (!in) {
        error = "visual-concepts-missing";
        return false;
    }
    const json root = json::parse(in, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("concepts") || !root["concepts"].is_array()) {
        error = "visual-concepts-format";
        return false;
    }
    out.version = root.value("version", 1);
    out.promptTemplate = root.value("promptTemplate", std::string("a photo of {}"));
    if (out.promptTemplate.find("{}") == std::string::npos) {
        out.promptTemplate = "{}";
    }
    std::string fingerprint = out.promptTemplate;
    for (const json& c : root["concepts"]) {
        VisualConceptDef d;
        d.id = c.value("id", std::string());
        d.prompt = c.value("prompt", std::string());
        if (d.id.empty() || d.prompt.empty() || out.Find(d.id) != nullptr) {
            continue;
        }
        d.label = c.value("label", d.id);
        d.group = c.value("group", std::string("subject"));
        d.categories = c.value("categories", std::vector<std::string>());
        d.energy = std::clamp(c.value("energy", 0.5), 0.0, 1.0);
        d.matrix = std::clamp(c.value("matrix", 0.5), 0.0, 1.0);
        d.tree = std::clamp(c.value("tree", 0.5), 0.0, 1.0);
        fingerprint += "|" + d.id + "=" + d.prompt;
        out.concepts.push_back(std::move(d));
    }
    if (out.concepts.empty()) {
        error = "visual-concepts-empty";
        return false;
    }
    out.hash = fmt::format("{:016x}", Fnv(fingerprint));
    return true;
}

// ------------------------------------------------------------------ tokenizer

std::string UnigramTokenizer::Canonical(const std::string& textIn) {
    std::vector<char32_t> out;
    bool space = true;
    for (char32_t c : text::Decode(textIn)) {
        if (text::IsLetter(c) || text::IsDigit(c)) {
            out.push_back(text::ToLower(c));
            space = false;
        } else if (!space) {
            out.push_back(U' ');
            space = true;
        }
    }
    while (!out.empty() && out.back() == U' ') {
        out.pop_back();
    }
    return text::Encode(out);
}

bool UnigramTokenizer::Load(const std::string& path, std::string& error) {
    _pieces.clear();
    std::ifstream in(P(path), std::ios::binary);
    if (!in) {
        error = "tokenizer-missing";
        return false;
    }
    const json root = json::parse(in, nullptr, false);
    if (root.is_discarded() || !root.contains("model") || root["model"].value("type", std::string()) != "Unigram" || !root["model"].contains("vocab")) {
        error = "tokenizer-format";
        return false;
    }
    const json& model = root["model"];
    _unk = model.value("unk_id", 2);
    double lowest = 0;
    int id = 0;
    for (const json& v : model["vocab"]) {
        if (!v.is_array() || v.size() < 2 || !v[0].is_string()) {
            ++id;
            continue;
        }
        const std::string piece = v[0].get<std::string>();
        const double score = v[1].get<double>();
        const std::vector<char32_t> cps = text::Decode(piece);
        std::u32string key(cps.begin(), cps.end());
        _longest = std::max(_longest, key.size());
        _pieces.emplace(std::move(key), std::make_pair(id, score));
        lowest = std::min(lowest, score);
        ++id;
    }
    // As the tokenizers library scores a character no piece covers.
    _unkScore = lowest - 10.0;
    for (const json& t : root.value("added_tokens", json::array())) {
        if (t.value("content", std::string()) == "</s>") {
            _eos = t.value("id", 1);
        }
    }
    _pad = _eos; // SigLIP pads with the end token
    return !_pieces.empty();
}

void UnigramTokenizer::Word(const std::u32string& w, std::vector<int64_t>& out) const {
    const size_t n = w.size();
    struct Node {
        double score = -1e300;
        size_t from = 0;
        int id = -1;
    };
    std::vector<Node> best(n + 1);
    best[0].score = 0;
    for (size_t i = 0; i < n; ++i) {
        if (best[i].score <= -1e299) {
            continue;
        }
        bool single = false;
        for (size_t len = 1; len <= std::min(_longest, n - i); ++len) {
            const auto it = _pieces.find(w.substr(i, len));
            if (it == _pieces.end()) {
                continue;
            }
            single = single || len == 1;
            const double s = best[i].score + it->second.second;
            if (s > best[i + len].score) {
                best[i + len] = { s, i, it->second.first };
            }
        }
        if (!single) {
            const double s = best[i].score + _unkScore;
            if (s > best[i + 1].score) {
                best[i + 1] = { s, i, _unk };
            }
        }
    }
    std::vector<int64_t> ids;
    for (size_t at = n; at > 0;) {
        ids.push_back(best[at].id);
        at = best[at].from;
    }
    std::reverse(ids.begin(), ids.end());
    // Unknown characters next to each other become one unknown token.
    for (int64_t t : ids) {
        if (t == _unk && !out.empty() && out.back() == _unk) {
            continue;
        }
        out.push_back(t);
    }
}

std::vector<int64_t> UnigramTokenizer::Encode(const std::string& textIn, int length, bool pad) const {
    std::vector<int64_t> ids;
    std::u32string word;
    const auto flush = [&]() {
        if (!word.empty()) {
            Word(U"▁" + word, ids);
            word.clear();
        }
    };
    for (char32_t c : text::Decode(textIn)) {
        if (c == U' ') {
            flush();
        } else {
            word.push_back(c);
        }
    }
    flush();
    if (length > 0 && static_cast<int>(ids.size()) > length - 1) {
        ids.resize(static_cast<size_t>(length - 1));
    }
    ids.push_back(_eos);
    if (pad && length > 0) {
        while (static_cast<int>(ids.size()) < length) {
            ids.push_back(_pad);
        }
    }
    return ids;
}

// ------------------------------------------------------------------ model

bool VisualModelSupported() {
#ifdef COMPOSER_VISUAL_ORT
    return true;
#else
    return false;
#endif
}

bool VisualModelComplete(const std::string& dir) {
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    for (const VisualModelFile& f : DefaultVisualModelFiles()) {
        const fs::path p = P(dir) / f.name;
        if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) != f.bytes) {
            return false;
        }
    }
    return true;
}

std::string FindVisualModel(const std::vector<std::string>& roots) {
    for (const std::string& r : roots) {
        if (r.empty()) {
            continue;
        }
        const std::string dir = S(P(r) / "ai-models" / VISUAL_MODEL_NAME);
        if (VisualModelComplete(dir)) {
            return dir;
        }
    }
    return {};
}

#ifdef COMPOSER_VISUAL_ORT

namespace {

Ort::Env& Environment() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "xLights_composer_vision");
    return env;
}

class SiglipModel : public VisualModel {
public:
    bool Open(const std::string& dir, std::string& error) {
        if (!_tokenizer.Load(S(P(dir) / "tokenizer.json"), error)) {
            return false;
        }
        try {
            Ort::SessionOptions o;
            o.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            o.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
            o.SetIntraOpNumThreads(static_cast<int>(std::clamp(hw / 2, 1u, 8u)));
            const fs::path vision = P(dir) / "vision_model_quantized.onnx";
            const fs::path textModel = P(dir) / "text_model_quantized.onnx";
            _vision = std::make_unique<Ort::Session>(Environment(), vision.native().c_str(), o);
            _text = std::make_unique<Ort::Session>(Environment(), textModel.native().c_str(), o);
        } catch (const Ort::Exception& e) {
            error = std::string("model-open:") + e.what();
            return false;
        }
        return true;
    }

    std::string Id() const override {
        return fmt::format("{}@{}", VISUAL_MODEL_NAME, SIGLIP_VERSION);
    }

    double Probability(double cosine) const override {
        const double logit = std::exp(SIGLIP_LOGIT_SCALE) * cosine + SIGLIP_LOGIT_BIAS;
        return 1.0 / (1.0 + std::exp(-logit));
    }

    bool EmbedImage(const xlImage& image, std::vector<float>& out, std::string& error) override {
        if (!image.IsOk()) {
            error = "image-empty";
            return false;
        }
        // Squashed to 224 x 224 with no crop, as SigLIP's processor does; transparent
        // parts over black, as the lights show them.
        const xlImage squashed = ResampleImage(image, SIGLIP_SIZE, SIGLIP_SIZE);
        std::vector<float> pixels(static_cast<size_t>(3 * SIGLIP_SIZE * SIGLIP_SIZE));
        const uint8_t* d = squashed.GetData();
        const size_t plane = static_cast<size_t>(SIGLIP_SIZE) * SIGLIP_SIZE;
        for (size_t i = 0; i < plane; ++i) {
            const double a = d[i * 4 + 3] / 255.0;
            for (int c = 0; c < 3; ++c) {
                pixels[c * plane + i] = static_cast<float>((d[i * 4 + c] * a / 255.0 - 0.5) / 0.5);
            }
        }
        const std::array<int64_t, 4> shape = { 1, 3, SIGLIP_SIZE, SIGLIP_SIZE };
        std::lock_guard<std::mutex> lock(_run);
        try {
            const auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value input = Ort::Value::CreateTensor<float>(mem, pixels.data(), pixels.size(), shape.data(), shape.size());
            const char* inputs[] = { "pixel_values" };
            const char* outputs[] = { "pooler_output" };
            auto result = _vision->Run(Ort::RunOptions{ nullptr }, inputs, &input, 1, outputs, 1);
            const float* v = result[0].GetTensorData<float>();
            const size_t n = result[0].GetTensorTypeAndShapeInfo().GetElementCount();
            out.assign(v, v + n);
        } catch (const Ort::Exception& e) {
            error = std::string("model-run:") + e.what();
            return false;
        }
        Normalize(out);
        return true;
    }

    bool EmbedTexts(const std::vector<std::string>& texts, std::vector<std::vector<float>>& out, std::string& error) override {
        out.clear();
        std::lock_guard<std::mutex> lock(_run);
        constexpr size_t BATCH = 16;
        for (size_t at = 0; at < texts.size(); at += BATCH) {
            const size_t n = std::min(BATCH, texts.size() - at);
            std::vector<int64_t> ids;
            for (size_t i = 0; i < n; ++i) {
                const std::vector<int64_t> t = _tokenizer.Encode(UnigramTokenizer::Canonical(texts[at + i]), SIGLIP_TOKENS, true);
                ids.insert(ids.end(), t.begin(), t.end());
            }
            const std::array<int64_t, 2> shape = { static_cast<int64_t>(n), SIGLIP_TOKENS };
            try {
                const auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
                Ort::Value input = Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), shape.data(), shape.size());
                const char* inputs[] = { "input_ids" };
                const char* outputs[] = { "pooler_output" };
                auto result = _text->Run(Ort::RunOptions{ nullptr }, inputs, &input, 1, outputs, 1);
                const float* v = result[0].GetTensorData<float>();
                const size_t total = result[0].GetTensorTypeAndShapeInfo().GetElementCount();
                const size_t dim = total / n;
                for (size_t i = 0; i < n; ++i) {
                    std::vector<float> e(v + i * dim, v + (i + 1) * dim);
                    Normalize(e);
                    out.push_back(std::move(e));
                }
            } catch (const Ort::Exception& e) {
                error = std::string("model-run:") + e.what();
                return false;
            }
        }
        return true;
    }

private:
    UnigramTokenizer _tokenizer;
    std::unique_ptr<Ort::Session> _vision;
    std::unique_ptr<Ort::Session> _text;
    std::mutex _run;
};

} // namespace

#endif

std::unique_ptr<VisualModel> OpenVisualModel(const std::string& dir, std::string& error) {
    if (!VisualModelComplete(dir)) {
        error = "model-incomplete";
        return nullptr;
    }
#ifdef COMPOSER_VISUAL_ORT
    auto m = std::make_unique<SiglipModel>();
    if (!m->Open(dir, error)) {
        return nullptr;
    }
    return m;
#else
    error = "model-unsupported-build";
    return nullptr;
#endif
}

bool InstallVisualModel(const std::string& root, const std::function<bool(int, const std::string&)>& progress, std::string& dir, std::string& error) {
    const fs::path target = P(root) / "ai-models" / VISUAL_MODEL_NAME;
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) {
        error = "model-folder";
        return false;
    }
    dir = S(target);
    uint64_t total = 0;
    for (const VisualModelFile& f : DefaultVisualModelFiles()) {
        total += f.bytes;
    }
    uint64_t done = 0;
    for (const VisualModelFile& f : DefaultVisualModelFiles()) {
        const fs::path file = target / f.name;
        if (fs::is_regular_file(file, ec) && fs::file_size(file, ec) == f.bytes && Sha256OfFile(S(file)) == f.sha256) {
            done += f.bytes;
            continue;
        }
        const fs::path part = target / (std::string(f.name) + ".part");
        fs::remove(part, ec);
        bool stopped = false;
        const bool ok = CurlManager::HTTPSGetFile(std::string(REVISION) + f.url, S(part), "", "", 30, [&](int pct) {
            const int overall = static_cast<int>((done + f.bytes * static_cast<uint64_t>(std::clamp(pct, 0, 100)) / 100) * 100 / std::max<uint64_t>(1, total));
            if (progress && !progress(overall, f.name)) {
                stopped = true;
                return false;
            }
            return true;
        });
        if (stopped) {
            fs::remove(part, ec);
            error = "model-download-cancelled";
            return false;
        }
        if (!ok || !fs::is_regular_file(part, ec) || fs::file_size(part, ec) != f.bytes || Sha256OfFile(S(part)) != f.sha256) {
            fs::remove(part, ec);
            error = fmt::format("model-download-failed:{}", f.name);
            return false;
        }
        fs::rename(part, file, ec);
        if (ec) {
            fs::remove(part, ec);
            error = "model-folder";
            return false;
        }
        done += f.bytes;
    }
    if (progress) {
        progress(100, "");
    }
    return true;
}

bool BuildConceptIndex(VisualModel& model, const VisualConcepts& concepts, const std::string& cacheDir, ConceptIndex& index, std::string& error) {
    index = ConceptIndex();
    index.modelId = model.Id();
    index.conceptsHash = concepts.hash;
    std::string safe = index.modelId;
    for (char& c : safe) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') {
            c = '_';
        }
    }
    const fs::path file = P(cacheDir) / fmt::format("concepts-{}-{}.json", safe, concepts.hash);
    std::error_code ec;
    if (std::ifstream in(file, std::ios::binary); in) {
        const json root = json::parse(in, nullptr, false);
        if (!root.is_discarded() && root.value("model", std::string()) == index.modelId && root.value("concepts", std::string()) == concepts.hash) {
            for (const json& c : root.value("vectors", json::array())) {
                index.ids.push_back(c.value("id", std::string()));
                index.vectors.push_back(c.value("v", std::vector<float>()));
            }
            if (index.ids.size() == concepts.concepts.size()) {
                return true;
            }
            index.ids.clear();
            index.vectors.clear();
        }
    }
    std::vector<std::string> prompts;
    for (const VisualConceptDef& c : concepts.concepts) {
        std::string p = concepts.promptTemplate;
        p.replace(p.find("{}"), 2, c.prompt);
        prompts.push_back(p);
        index.ids.push_back(c.id);
    }
    if (!model.EmbedTexts(prompts, index.vectors, error) || index.vectors.size() != index.ids.size()) {
        if (error.empty()) {
            error = "concept-embeddings";
        }
        return false;
    }
    json vectors = json::array();
    for (size_t i = 0; i < index.ids.size(); ++i) {
        std::vector<float> rounded;
        for (float x : index.vectors[i]) {
            rounded.push_back(std::round(x * 1e6f) / 1e6f);
        }
        index.vectors[i] = rounded; // what a later run reads back, so both runs agree
        vectors.push_back({ { "id", index.ids[i] }, { "v", std::move(rounded) } });
    }
    fs::create_directories(P(cacheDir), ec);
    const fs::path tmp = P(S(file) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << json{ { "model", index.modelId }, { "concepts", concepts.hash }, { "vectors", std::move(vectors) } }.dump();
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
    }
    return true;
}

std::vector<MediaConcept> ScoreConcepts(const VisualModel& model, const ConceptIndex& index, const std::vector<float>& image, size_t keep) {
    struct Hit {
        size_t i;
        double cos;
    };
    std::vector<Hit> hits;
    for (size_t i = 0; i < index.vectors.size(); ++i) {
        const std::vector<float>& t = index.vectors[i];
        if (t.size() != image.size()) {
            continue;
        }
        double c = 0;
        for (size_t k = 0; k < t.size(); ++k) {
            c += static_cast<double>(t[k]) * image[k];
        }
        hits.push_back({ i, std::round(c * 10000.0) / 10000.0 });
    }
    std::sort(hits.begin(), hits.end(), [&](const Hit& a, const Hit& b) { return a.cos != b.cos ? a.cos > b.cos : index.ids[a.i] < index.ids[b.i]; });
    std::vector<MediaConcept> out;
    const double second = hits.size() > 1 ? hits[1].cos : 0;
    for (size_t r = 0; r < hits.size() && out.size() < keep; ++r) {
        const Hit& h = hits[r];
        const double p = model.Probability(h.cos);
        // SigLIP's probability is calibrated per concept; on drawings and small
        // pictures it stays low even when the ranking is right, so the rank and the
        // lead over the next concept count too. None of these is a certainty.
        std::string level;
        if (p >= 0.25 || (r == 0 && p >= 0.05 && h.cos - second >= 0.01)) {
            level = "strong";
        } else if (p >= 0.03 || (r == 0 && h.cos - second >= 0.012) || (r < 3 && h.cos >= 0.075)) {
            level = "probable";
        } else if (r < 3 && h.cos >= 0.03) {
            level = "weak";
        } else {
            continue;
        }
        out.push_back({ index.ids[h.i], std::round(p * 1000.0) / 1000.0, h.cos, level, "model" });
    }
    return out;
}

} // namespace composer
