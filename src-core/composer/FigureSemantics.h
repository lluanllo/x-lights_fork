#pragma once

#include "composer/FigureGeometry.h"
#include "composer/ModelCapabilities.h"

#include <map>
#include <string>
#include <vector>

class xLightsShowContext;

// What each figure can play and what its parts are. A region (a mouth, the eyes, a trunk,
// the foliage...) is only known when it is tied to real lights: a face definition's
// nodes, a submodel, or nodes a person chose. A name or a silhouette alone only suggests
// one, and says it is a suggestion. Each region keeps its own colour, apart from the
// song's palette (a tree's trunk stays brown). Descriptions, not drawing: making
// pictures from them is a later phase.
namespace composer {

inline constexpr int FIGURE_SEMANTICS_SCHEMA_VERSION = 1;

struct FigureRegion {
    std::string id;       // "mouth", "eyes", "face", "head", "body", "outline", "trunk", "foliage", "branches", "top", "background", "text", "image"
    std::string source;   // "face-definition:<face>", "face-images:<face>", "submodel:<name>", "nodes", "geometry", "name-hint:<submodel>", "manual"
    std::string subModel; // when it is one
    std::vector<int> nodes; // the figure's node numbers, 0-based; empty for "face-images"
    double confidence = 0;
    bool real = false;       // tied to real lights
    bool confirmed = false;  // a person said so
    std::string color;       // "#RRGGBB", the region's own colour; "" none
    std::string colorSource; // "default", "manual"
};

struct FigureProfile {
    std::string figure;
    std::string family;
    std::vector<std::string> roles; // "narrator", "character", "lead-singer", "secondary-singer", "chorus-member", "instrumental", "percussion", "decoration", "background", "accent"
    std::string mouth;              // "nodes", "images", "suggested", "none"
    std::vector<FigureRegion> regions;
    std::vector<std::string> notes; // what is missing, said plainly
    bool reviewed = false;

    [[nodiscard]] const FigureRegion* Region(const std::string& id) const;
    [[nodiscard]] bool CanSpeak() const { return mouth == "nodes" || mouth == "images"; }
};

// A person's choices, kept in <show>/ShowComposer/figures.json across analyses.
struct FigureChoice {
    std::string figure;
    std::vector<std::string> roles;    // empty: the suggested ones
    std::vector<FigureRegion> regions; // manual or confirmed regions
    std::vector<std::string> dropped;  // region ids the person removed
    std::map<std::string, std::string> colors; // region id -> "#RRGGBB"
    bool reviewed = false;
    std::string note;
};

struct FigureChoices {
    int schemaVersion = FIGURE_SEMANTICS_SCHEMA_VERSION;
    std::vector<FigureChoice> figures;
    [[nodiscard]] FigureChoice* Find(const std::string& figure);
    [[nodiscard]] const FigureChoice* Find(const std::string& figure) const;
};

struct FigureSemantics {
    std::vector<FigureProfile> profiles;
    [[nodiscard]] const FigureProfile* Find(const std::string& figure) const;
};

[[nodiscard]] FigureSemantics DescribeSemantics(const xLightsShowContext& ctx, const FigureCatalog& geometry, const ModelCapabilities& caps, const FigureChoices* choices);
// The person's choices over profiles found without them (read back from a file): what the
// window shows while a person edits, with no show loaded.
[[nodiscard]] FigureSemantics ApplyFigureChoices(const FigureSemantics& found, const FigureCatalog& geometry, const FigureChoices* choices);

// The colour a region keeps by default: foliage green, trunk brown, mouth red, eyes warm white.
[[nodiscard]] std::string DefaultRegionColor(const std::string& region, const std::string& family);

// "1-10,15,20-22" (1-based, as xLights writes node ranges) into 0-based node numbers.
[[nodiscard]] std::vector<int> ParseNodeRanges(const std::string& text, int nodeCount);
[[nodiscard]] std::string NodeRangesText(const std::vector<int>& nodes);

[[nodiscard]] std::string FigureChoicesPath(const std::string& showDir);
[[nodiscard]] std::string ToJson(const FigureChoices& choices);
[[nodiscard]] bool FromJson(const std::string& text, FigureChoices& choices, std::string& error);
[[nodiscard]] std::string ToJson(const FigureSemantics& semantics);
[[nodiscard]] bool FromJson(const std::string& text, FigureSemantics& semantics, std::string& error);

[[nodiscard]] std::string RegionPhrase(const std::string& region);
[[nodiscard]] std::string RolePhrase(const std::string& role);

} // namespace composer
