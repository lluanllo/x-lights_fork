#pragma once

#include "composer/AudioUnderstanding.h"
#include "composer/FigureGeometry.h"
#include "composer/FigureSemantics.h"
#include "composer/PerformanceMapping.h"
#include "composer/VocalUnderstanding.h"

#include <functional>
#include <string>
#include <vector>

// What the render really did, set against what was meant. The sequence xLights rendered
// (its own .fseq, from xLights' own renderer) is read figure by figure and region by
// region, and compared with who was meant to perform when: a narrating tree that stays
// dark, a mouth that moves through an instrumental, a voice shown on another figure, a
// part of a figure that never lights. Nothing here approves anything: it points at the
// moments a person should look at, and says why.
namespace composer {

inline constexpr int RENDER_UNDERSTANDING_SCHEMA_VERSION = 1;

struct RegionTrace {
    std::string region;        // "" the whole figure
    std::vector<float> level;  // 0-1 mean brightness per step
    std::vector<float> motion; // 0-1 share of its lights that turned on or off since the step before
    std::vector<float> on;     // 0-1 share of its lights on
    std::vector<uint32_t> color; // mean colour per step, 0xRRGGBB
};

struct FigureTrace {
    std::string figure;
    int lights = 0;
    std::vector<RegionTrace> regions; // the whole figure first
    [[nodiscard]] const RegionTrace* Region(const std::string& region) const;
};

struct RenderTraces {
    int stepMS = 100;
    int steps = 0;
    std::vector<FigureTrace> figures;
    // Figures whose lights are not in the file (no controller: xLights writes only the
    // controllers' channels). They are not judged.
    std::vector<std::string> outside;
    [[nodiscard]] const FigureTrace* Figure(const std::string& figure) const;
};

struct RenderCheckOptions {
    int stepMS = 100;
    float litLevel = 0.04F;    // a region is lit above this mean brightness
    float movingShare = 0.04F; // a mouth moves above this share of lights changing per step
    int minWindowMS = 400;     // shorter windows are not judged
    int maxCaptures = 24;
    int captureSpacingMS = 1500;
};

struct RenderFinding {
    std::string id;     // "<kind>@<figure>@<startMS>": the same problem keeps its id between runs
    std::string kind;   // "expected-dark", "mouth-still", "mouth-moves-without-voice", "voice-on-other-figure",
                        // "region-never-lit", "dark-while-music", "figure-silent"
    std::string figure;
    std::string region;
    std::string source; // the assignment it is about
    int startMS = 0;
    int endMS = 0;
    std::string severity; // "check", "warning"
    double measured = 0;  // what was seen, 0-1
    std::string detail;
};

struct CaptureMoment {
    int timeMS = 0;
    std::string reason; // "voice-starts", "instrumental", "drop", "reentry", "finding:<id>"
    std::vector<std::string> figures;
};

struct IntentionCheck {
    std::string source;
    std::string figure;
    std::string region;
    int windows = 0;    // judged windows
    int honoured = 0;   // where the figure did what it was meant to
};

struct RenderUnderstanding {
    int schemaVersion = RENDER_UNDERSTANDING_SCHEMA_VERSION;
    std::string sequence;
    int stepMS = 100;
    int durationMS = 0;
    std::vector<IntentionCheck> checks;
    std::vector<RenderFinding> findings;
    std::vector<CaptureMoment> captures;
    std::vector<std::string> limits;   // what this reading cannot see, said plainly
    std::string review = "pending";    // only a person changes it
};

using RenderCheckCancel = std::function<bool()>;

// The figures' lights in the rendered sequence, per step. `figures` empty: every model
// with known channels. Regions come from the figure's semantics (real nodes only).
[[nodiscard]] bool ReadRenderTraces(const std::string& fseqPath, const FigureCatalog& geometry, const FigureSemantics& semantics, const std::vector<std::string>& figures,
                                    const RenderCheckOptions& options, RenderTraces& traces, std::string& error, const RenderCheckCancel& cancel = {});
// The same from frames already in memory (tests): frames[step][channel].
[[nodiscard]] RenderTraces TracesFromFrames(const std::vector<std::vector<uint8_t>>& frames, int frameMS, const FigureCatalog& geometry, const FigureSemantics& semantics,
                                            const std::vector<std::string>& figures, const RenderCheckOptions& options);

struct RenderIntention {
    const PerformanceMap* performers = nullptr;
    const AudioUnderstanding* audio = nullptr;
    const VocalUnderstanding* voices = nullptr;
    const FigureSemantics* semantics = nullptr;
};

[[nodiscard]] RenderUnderstanding CompareRender(const RenderTraces& traces, const RenderIntention& intention, const RenderCheckOptions& options);

[[nodiscard]] std::string ToJson(const RenderUnderstanding& r);
[[nodiscard]] bool FromJson(const std::string& text, RenderUnderstanding& r, std::string& error);
[[nodiscard]] std::string FindingPhrase(const std::string& kind);
// "12400,31000" (milliseconds) for --composer-snapshots --times, from the capture moments.
[[nodiscard]] std::string CaptureTimes(const RenderUnderstanding& r);

} // namespace composer
