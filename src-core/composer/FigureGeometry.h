#pragma once

#include "composer/ModelCapabilities.h"

#include <string>
#include <vector>

class xLightsShowContext;

// The real shape of every figure, read through xLights' own models: how many lights it
// has and where each one is, in which order, in rows, branches or segments when the
// figure has them, how dense and how big, its submodels, groups and controller. Lights,
// channels and effects are different things and are kept apart: a figure of 50 RGB
// lights is 150 channels and may carry any number of effects.
//
// Nothing assumes a rectangle: an irregular figure keeps its own node positions, and a
// shape the program does not know says so.
namespace composer {

inline constexpr int FIGURE_GEOMETRY_SCHEMA_VERSION = 1;

struct FigureNode {
    int index = 0;  // the model's node number, 0-based
    int bufX = 0;   // where xLights' buffer places it
    int bufY = 0;
    float x = 0;    // its position on the figure, 0-1 across and 0-1 up
    float y = 0;
    int strand = 0;
    int channel = -1; // its first channel, 0-based; -1 not known
};

struct FigureGeometry {
    std::string name;
    std::string family;   // as the visual direction groups figures
    std::string displayAs;
    std::string kind;     // "model", "submodel", "group"
    // "matrix", "tree", "tree-360", "arch", "line", "circle", "sphere", "star", "window",
    // "icicles", "candy-cane", "wreath", "spinner", "cube", "custom", "group", "submodel", "unknown"
    std::string topology;
    int nodes = 0;
    int channels = 0;
    int channelsPerNode = 0;
    int bufferWi = 0;
    int bufferHt = 0;
    int strands = 0;
    int nodesPerStrand = 0;
    int rows = 0;     // matrices: lines of lights
    int columns = 0;
    int branches = 0; // trees: strings round the tree
    int segments = 0; // arches, candy canes, icicles: separate pieces
    double width = 0; // in the layout's units
    double height = 0;
    double aspect = 0;
    std::string orientation; // "horizontal", "vertical", "square"
    double density = 0;      // lights per buffer cell, 0-1
    std::string resolution;  // "fine", "medium", "coarse" for pictures
    std::string parent;      // submodels
    std::vector<std::string> subModels;
    std::vector<std::string> groups;
    std::vector<std::string> members; // groups
    std::string controller;
    bool noController = false;
    std::string availability; // "installation", "preview", "layout"
    double confidence = 0;
    std::string source; // "model:<display type>", "group", "submodel", "generic"
    std::vector<FigureNode> nodeMap;
    std::vector<std::string> limits;
};

struct FigureCatalog {
    int schemaVersion = FIGURE_GEOMETRY_SCHEMA_VERSION;
    std::string layoutFingerprint;
    std::vector<FigureGeometry> figures;
    [[nodiscard]] const FigureGeometry* Find(const std::string& name) const;
};

// Every model, submodel and group of the loaded show. `withNodes` false skips the node
// map (the window's list does not need it).
[[nodiscard]] FigureCatalog DescribeFigures(const xLightsShowContext& ctx, const ModelCapabilities& caps, bool withNodes = true);
[[nodiscard]] std::string ToJson(const FigureCatalog& catalog, bool withNodes = false);
[[nodiscard]] bool FromJson(const std::string& text, FigureCatalog& catalog, std::string& error);
// The shape said simply: "matriz de 49 x 20 luces", in words a person reads.
[[nodiscard]] std::string TopologyPhrase(const std::string& topology);

} // namespace composer
