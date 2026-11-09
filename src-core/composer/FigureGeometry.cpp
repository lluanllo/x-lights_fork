#include "composer/FigureGeometry.h"

#include "composer/VisualDirection.h"
#include "models/DisplayAsType.h"
#include "models/Model.h"
#include "models/ModelGroup.h"
#include "models/Node.h"
#include "models/SubModel.h"
#include "render/xLightsShowContext.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <map>

#define COMPOSER_TEXT(text) text

using json = nlohmann::json;

namespace composer {

namespace {

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

std::string TopologyOf(const Model& m, const TargetInfo* t) {
    switch (m.GetDisplayAs()) {
    case DisplayAsType::Matrix:
        return "matrix";
    case DisplayAsType::Tree:
        return t != nullptr && t->degrees >= 300 ? "tree-360" : "tree";
    case DisplayAsType::Arches:
        return "arch";
    case DisplayAsType::SingleLine:
    case DisplayAsType::PolyLine:
    case DisplayAsType::MultiPoint:
        return "line";
    case DisplayAsType::Circle:
        return "circle";
    case DisplayAsType::Sphere:
        return "sphere";
    case DisplayAsType::Star:
        return "star";
    case DisplayAsType::WindowFrame:
        return "window";
    case DisplayAsType::Icicles:
        return "icicles";
    case DisplayAsType::CandyCanes:
        return "candy-cane";
    case DisplayAsType::Wreath:
        return "wreath";
    case DisplayAsType::Spinner:
        return "spinner";
    case DisplayAsType::Cube:
        return "cube";
    case DisplayAsType::Custom:
    case DisplayAsType::Image:
        return "custom";
    case DisplayAsType::ModelGroup:
        return "group";
    case DisplayAsType::SubModel:
        return "submodel";
    default:
        return "unknown";
    }
}

void NodesOf(Model& m, FigureGeometry& g, const std::map<int, int>* parentIndex) {
    std::vector<std::tuple<float, float, float>> pts;
    float minX = 1e30F, maxX = -1e30F, minY = 1e30F, maxY = -1e30F;
    const uint32_t count = m.GetNodeCount();
    g.nodeMap.reserve(count);
    for (uint32_t n = 0; n < count; ++n) {
        const NodeBaseClass* node = m.GetNode(n);
        if (node == nullptr) {
            continue;
        }
        FigureNode f;
        f.index = parentIndex != nullptr && parentIndex->contains(static_cast<int>(n)) ? parentIndex->at(static_cast<int>(n)) : static_cast<int>(n);
        if (!node->Coords.empty()) {
            f.bufX = node->Coords[0].bufX;
            f.bufY = node->Coords[0].bufY;
        }
        pts.clear();
        m.GetNode3DScreenCoords(static_cast<int>(n), pts);
        if (!pts.empty()) {
            f.x = std::get<0>(pts.front());
            f.y = std::get<1>(pts.front());
        } else {
            f.x = static_cast<float>(f.bufX);
            f.y = static_cast<float>(f.bufY);
        }
        f.strand = m.GetNodeStringNumber(n);
        f.channel = static_cast<int>(m.NodeStartChannel(n));
        minX = std::min(minX, f.x);
        maxX = std::max(maxX, f.x);
        minY = std::min(minY, f.y);
        maxY = std::max(maxY, f.y);
        g.nodeMap.push_back(f);
    }
    const float w = std::max(1e-6F, maxX - minX);
    const float h = std::max(1e-6F, maxY - minY);
    for (FigureNode& f : g.nodeMap) {
        f.x = static_cast<float>(Round3((f.x - minX) / w));
        f.y = static_cast<float>(Round3((f.y - minY) / h));
    }
}

void Shape(FigureGeometry& g, const TargetInfo* t) {
    g.aspect = Round3(t != nullptr && t->width > 0 && t->height > 0 ? t->width / t->height : static_cast<double>(std::max(1, g.bufferWi)) / std::max(1, g.bufferHt));
    g.orientation = g.aspect > 1.2 ? "horizontal" : g.aspect < 0.83 ? "vertical" : "square";
    g.density = Round3(g.bufferWi > 0 && g.bufferHt > 0 ? std::min(1.0, static_cast<double>(g.nodes) / (static_cast<double>(g.bufferWi) * g.bufferHt)) : 0);
    g.resolution = g.nodes >= 1500 && g.density >= 0.6 ? "fine" : g.nodes >= 300 && g.density >= 0.3 ? "medium" : "coarse";
    if (g.topology == "matrix") {
        g.rows = g.bufferHt;
        g.columns = g.bufferWi;
    } else if (g.topology == "tree" || g.topology == "tree-360") {
        g.branches = g.strands;
    } else if (g.topology == "arch" || g.topology == "candy-cane" || g.topology == "icicles") {
        g.segments = g.strands;
    }
    if (g.topology == "unknown") {
        g.limits.push_back("shape-unknown: kept as a set of points");
        g.confidence = 0.3;
    } else if (g.topology == "custom") {
        g.limits.push_back("irregular-shape: positions only, no rows or branches");
        g.confidence = 0.6;
    } else {
        g.confidence = 0.9;
    }
    if (g.nodes > 0 && g.channelsPerNode < 3) {
        g.limits.push_back("single-colour: no pictures or mixed colours");
    }
}

} // namespace

const FigureGeometry* FigureCatalog::Find(const std::string& name) const {
    for (const FigureGeometry& g : figures) {
        if (g.name == name) {
            return &g;
        }
    }
    return nullptr;
}

FigureCatalog DescribeFigures(const xLightsShowContext& ctx, const ModelCapabilities& caps, bool withNodes) {
    FigureCatalog catalog;
    catalog.layoutFingerprint = caps.layoutFingerprint;
    for (const auto& [name, model] : ctx.AllModels) {
        if (model == nullptr) {
            continue;
        }
        const TargetInfo* t = caps.Find(name);
        FigureGeometry g;
        g.name = name;
        g.displayAs = model->GetDisplayAsString();
        g.topology = TopologyOf(*model, t);
        if (const auto* group = dynamic_cast<const ModelGroup*>(model)) {
            g.kind = "group";
            g.source = "group";
            g.members = group->ModelNames();
            for (const std::string& m : t != nullptr ? t->flatModels : std::vector<std::string>()) {
                if (const TargetInfo* mt = caps.Find(m)) {
                    g.nodes += mt->nodes;
                }
            }
            g.bufferWi = t != nullptr ? t->bufferWi : 0;
            g.bufferHt = t != nullptr ? t->bufferHt : 0;
            g.family = t != nullptr ? FamilyIdOf(*t) : "";
            g.confidence = 0.8;
            g.availability = "installation";
            catalog.figures.push_back(std::move(g));
            continue;
        }
        g.kind = "model";
        g.source = "model:" + g.displayAs;
        g.nodes = static_cast<int>(model->GetNodeCount());
        g.channelsPerNode = model->GetChanCountPerNode();
        g.channels = g.nodes * g.channelsPerNode;
        g.bufferWi = model->GetDefaultBufferWi();
        g.bufferHt = model->GetDefaultBufferHt();
        g.strands = model->GetNumStrands();
        g.nodesPerStrand = g.strands > 0 ? g.nodes / g.strands : g.nodes;
        g.width = t != nullptr ? Round3(t->width) : 0;
        g.height = t != nullptr ? Round3(t->height) : 0;
        g.family = t != nullptr ? FamilyIdOf(*t) : "";
        g.groups = t != nullptr ? t->groups : std::vector<std::string>();
        g.controller = model->GetControllerName();
        g.noController = g.controller == "No Controller";
        g.availability = g.noController ? "preview" : "installation";
        for (const Model* sub : model->GetSubModels()) {
            g.subModels.push_back(sub->GetName());
        }
        Shape(g, t);
        if (withNodes) {
            NodesOf(*model, g, nullptr);
        }
        const std::string parentName = name;
        const std::string parentFamily = g.family;
        const std::string parentController = g.controller;
        const bool parentFree = g.noController;
        catalog.figures.push_back(std::move(g));

        for (Model* subModel : model->GetSubModels()) {
            const auto* sub = dynamic_cast<const SubModel*>(subModel);
            FigureGeometry s;
            s.name = parentName + "/" + subModel->GetName();
            s.kind = "submodel";
            s.parent = parentName;
            s.displayAs = "SubModel";
            s.topology = "submodel";
            s.source = "submodel";
            s.family = parentFamily;
            s.nodes = static_cast<int>(subModel->GetNodeCount());
            s.channelsPerNode = subModel->GetChanCountPerNode();
            s.channels = s.nodes * s.channelsPerNode;
            s.bufferWi = subModel->GetDefaultBufferWi();
            s.bufferHt = subModel->GetDefaultBufferHt();
            s.strands = subModel->GetNumStrands();
            s.controller = parentController;
            s.noController = parentFree;
            s.availability = parentFree ? "preview" : "installation";
            const TargetInfo* st = caps.Find(s.name);
            Shape(s, st);
            s.confidence = 0.8;
            if (withNodes && sub != nullptr) {
                // Its lights by the parent's numbers, so a region can name them.
                std::map<int, int> parentOf;
                for (const auto& [parentIndex, own] : sub->GetNodeIndexMap()) {
                    parentOf[own] = parentIndex;
                }
                NodesOf(*subModel, s, &parentOf);
            }
            catalog.figures.push_back(std::move(s));
        }
    }
    std::sort(catalog.figures.begin(), catalog.figures.end(), [](const FigureGeometry& a, const FigureGeometry& b) { return a.name < b.name; });
    return catalog;
}

std::string TopologyPhrase(const std::string& t) {
    static const std::map<std::string, const char*> P = {
        { "matrix", COMPOSER_TEXT("a matrix (rows and columns of lights)") }, { "tree", COMPOSER_TEXT("a tree (strings from the top down)") },
        { "tree-360", COMPOSER_TEXT("a round tree (strings all the way round)") }, { "arch", COMPOSER_TEXT("arches") }, { "line", COMPOSER_TEXT("a line of lights") },
        { "circle", COMPOSER_TEXT("a circle") }, { "sphere", COMPOSER_TEXT("a sphere") }, { "star", COMPOSER_TEXT("a star") }, { "window", COMPOSER_TEXT("a window frame") },
        { "icicles", COMPOSER_TEXT("icicles") }, { "candy-cane", COMPOSER_TEXT("candy canes") }, { "wreath", COMPOSER_TEXT("a wreath") }, { "spinner", COMPOSER_TEXT("a spinner") },
        { "cube", COMPOSER_TEXT("a cube") }, { "custom", COMPOSER_TEXT("a figure of its own shape") }, { "group", COMPOSER_TEXT("a group of figures") },
        { "submodel", COMPOSER_TEXT("a part of a figure") }, { "unknown", COMPOSER_TEXT("a shape the program does not know") }
    };
    const auto it = P.find(t);
    return it != P.end() ? it->second : t;
}

std::string ToJson(const FigureCatalog& c, bool withNodes) {
    json figures = json::array();
    for (const FigureGeometry& g : c.figures) {
        json j = { { "name", g.name }, { "family", g.family }, { "displayAs", g.displayAs }, { "kind", g.kind }, { "topology", g.topology }, { "nodes", g.nodes },
                   { "channels", g.channels }, { "channelsPerNode", g.channelsPerNode }, { "buffer", { g.bufferWi, g.bufferHt } }, { "strands", g.strands },
                   { "nodesPerStrand", g.nodesPerStrand }, { "rows", g.rows }, { "columns", g.columns }, { "branches", g.branches }, { "segments", g.segments },
                   { "size", { g.width, g.height } }, { "aspect", g.aspect }, { "orientation", g.orientation }, { "density", g.density }, { "resolution", g.resolution },
                   { "parent", g.parent }, { "subModels", g.subModels }, { "groups", g.groups }, { "members", g.members }, { "controller", g.controller },
                   { "noController", g.noController }, { "availability", g.availability }, { "confidence", g.confidence }, { "source", g.source }, { "limits", g.limits } };
        if (withNodes) {
            json nodes = json::array();
            for (const FigureNode& n : g.nodeMap) {
                nodes.push_back(json::array({ n.index, n.bufX, n.bufY, n.x, n.y, n.strand, n.channel }));
            }
            j["nodeFields"] = { "index", "bufX", "bufY", "x", "y", "strand", "channel" };
            j["nodeMap"] = nodes;
        }
        figures.push_back(std::move(j));
    }
    return json({ { "kind", "composer-figures" }, { "schemaVersion", c.schemaVersion }, { "layoutFingerprint", c.layoutFingerprint }, { "figures", figures } }).dump(1);
}

bool FromJson(const std::string& text, FigureCatalog& out, std::string& error) {
    try {
        const json root = json::parse(text);
        if (root.value("schemaVersion", 0) < 1 || root.value("schemaVersion", 0) > FIGURE_GEOMETRY_SCHEMA_VERSION) {
            error = "figures: schema not read";
            return false;
        }
        FigureCatalog c;
        c.layoutFingerprint = root.value("layoutFingerprint", std::string());
        for (const json& j : root.value("figures", json::array())) {
            FigureGeometry g;
            g.name = j.value("name", std::string());
            g.family = j.value("family", std::string());
            g.displayAs = j.value("displayAs", std::string());
            g.kind = j.value("kind", std::string());
            g.topology = j.value("topology", std::string());
            g.nodes = j.value("nodes", 0);
            g.channels = j.value("channels", 0);
            g.channelsPerNode = j.value("channelsPerNode", 0);
            const json buffer = j.value("buffer", json::array({ 0, 0 }));
            g.bufferWi = buffer[0].get<int>();
            g.bufferHt = buffer[1].get<int>();
            g.strands = j.value("strands", 0);
            g.nodesPerStrand = j.value("nodesPerStrand", 0);
            g.rows = j.value("rows", 0);
            g.columns = j.value("columns", 0);
            g.branches = j.value("branches", 0);
            g.segments = j.value("segments", 0);
            const json size = j.value("size", json::array({ 0.0, 0.0 }));
            g.width = size[0].get<double>();
            g.height = size[1].get<double>();
            g.aspect = j.value("aspect", 0.0);
            g.orientation = j.value("orientation", std::string());
            g.density = j.value("density", 0.0);
            g.resolution = j.value("resolution", std::string());
            g.parent = j.value("parent", std::string());
            g.subModels = j.value("subModels", std::vector<std::string>());
            g.groups = j.value("groups", std::vector<std::string>());
            g.members = j.value("members", std::vector<std::string>());
            g.controller = j.value("controller", std::string());
            g.noController = j.value("noController", false);
            g.availability = j.value("availability", std::string());
            g.confidence = j.value("confidence", 0.0);
            g.source = j.value("source", std::string());
            g.limits = j.value("limits", std::vector<std::string>());
            for (const json& n : j.value("nodeMap", json::array())) {
                if (n.is_array() && n.size() >= 6) {
                    g.nodeMap.push_back({ n[0].get<int>(), n[1].get<int>(), n[2].get<int>(), n[3].get<float>(), n[4].get<float>(), n[5].get<int>(), n.size() >= 7 ? n[6].get<int>() : -1 });
                }
            }
            c.figures.push_back(std::move(g));
        }
        out = std::move(c);
    } catch (const json::exception& e) {
        error = std::string("figures: ") + e.what();
        return false;
    }
    return true;
}

} // namespace composer
