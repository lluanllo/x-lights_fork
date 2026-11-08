#pragma once

#include "composer/MediaLibrary.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class xlImage;

// An optional image-text model that says what a picture shows, run on this computer.
// Nothing is sent anywhere: the model is a set of files a person downloads once into
// <show>/ai-models/, and without it the library still measures colours, light, motion
// and framing. The default is SigLIP base (Google, Apache-2.0) as ONNX, run by the ONNX
// Runtime xLights already ships for stem separation.
namespace composer {

inline constexpr const char* VISUAL_MODEL_NAME = "siglip-base-patch16-224";

// A file of the default model: where it is downloaded from (a fixed revision) and the
// SHA-256 it must have.
struct VisualModelFile {
    const char* name;
    const char* url;
    uint64_t bytes;
    const char* sha256;
};
[[nodiscard]] const std::vector<VisualModelFile>& DefaultVisualModelFiles();

struct VisualConceptDef {
    std::string id;
    std::string label;  // English; the window translates it
    std::string prompt; // what the model compares the picture with
    std::string group;  // "subject", "scene", "style", "mood"
    std::vector<std::string> categories;
    double energy = 0.5;
    double matrix = 0.5;
    double tree = 0.5;
};

struct VisualConcepts {
    int version = 1;
    std::string promptTemplate = "a photo of {}";
    std::vector<VisualConceptDef> concepts;
    std::string hash; // of what the model reads: ids, prompts and the template

    [[nodiscard]] const VisualConceptDef* Find(const std::string& id) const;
};

[[nodiscard]] std::string DefaultVisualConceptsPath();
bool ReadVisualConcepts(const std::string& path, VisualConcepts& concepts, std::string& error);

// A SentencePiece Unigram tokenizer read from a Hugging Face tokenizer.json: words split
// on spaces with the "▁" marker, the most likely split of each word (Viterbi), the end
// token, and padding to a fixed length.
class UnigramTokenizer {
public:
    bool Load(const std::string& path, std::string& error);
    [[nodiscard]] std::vector<int64_t> Encode(const std::string& text, int length, bool pad) const;
    [[nodiscard]] bool Loaded() const { return !_pieces.empty(); }
    // Lowercase, punctuation dropped, single spaces: how SigLIP's text was prepared.
    [[nodiscard]] static std::string Canonical(const std::string& text);

private:
    std::map<std::u32string, std::pair<int, double>> _pieces; // piece -> id, log probability
    size_t _longest = 0;
    int _unk = 2;
    int _eos = 1;
    int _pad = 1;
    double _unkScore = -20;

    void Word(const std::u32string& word, std::vector<int64_t>& out) const;
};

class VisualModel {
public:
    virtual ~VisualModel() = default;
    [[nodiscard]] virtual std::string Id() const = 0; // name@version, for the analysis cache
    // A unit-length embedding of the picture.
    virtual bool EmbedImage(const xlImage& image, std::vector<float>& out, std::string& error) = 0;
    // Unit-length embeddings of texts (already in the prompt's words).
    virtual bool EmbedTexts(const std::vector<std::string>& texts, std::vector<std::vector<float>>& out, std::string& error) = 0;
    // The model's calibrated probability that a text describes a picture, from their cosine.
    [[nodiscard]] virtual double Probability(double cosine) const = 0;
};

// True when this build can run a model at all (ONNX Runtime compiled in).
[[nodiscard]] bool VisualModelSupported();
// The first <root>/ai-models/<VISUAL_MODEL_NAME> folder holding a complete model, or "".
[[nodiscard]] std::string FindVisualModel(const std::vector<std::string>& roots);
// Whether `dir` holds every file of the model with the right size.
[[nodiscard]] bool VisualModelComplete(const std::string& dir);
// Opens the model in `dir`; null (with `error`) when it is incomplete or cannot run.
[[nodiscard]] std::unique_ptr<VisualModel> OpenVisualModel(const std::string& dir, std::string& error);

// Downloads the default model into <root>/ai-models/<VISUAL_MODEL_NAME>, only when a
// person asked for it. Each file is checked against its SHA-256 and kept only when it
// matches. `progress` gets 0..100 and returns false to stop.
bool InstallVisualModel(const std::string& root, const std::function<bool(int percent, const std::string& file)>& progress, std::string& dir, std::string& error);

// The concepts' text embeddings for one model, computed once and kept in `cacheDir`.
struct ConceptIndex {
    std::string modelId;
    std::string conceptsHash;
    std::vector<std::string> ids;
    std::vector<std::vector<float>> vectors;
};
bool BuildConceptIndex(VisualModel& model, const VisualConcepts& concepts, const std::string& cacheDir, ConceptIndex& index, std::string& error);

// What an image embedding seems to show, best first, each with how far it may be
// trusted. Only the most telling concepts are returned.
[[nodiscard]] std::vector<MediaConcept> ScoreConcepts(const VisualModel& model, const ConceptIndex& index, const std::vector<float>& image, size_t keep = 8);

} // namespace composer
