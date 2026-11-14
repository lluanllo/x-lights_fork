#include "composer/StageSpace.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

namespace {

// Least squares for out = a1*sx + a2*sy + a3*sz + b, with a tiny ridge so a flat model
// (every z the same) has an answer.
std::array<double, 4> Fit(const std::vector<std::array<double, 4>>& rows, const std::vector<double>& out) {
    double m[4][5] = {};
    for (size_t r = 0; r < rows.size(); ++r) {
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                m[i][j] += rows[r][i] * rows[r][j];
            }
            m[i][4] += rows[r][i] * out[r];
        }
    }
    double trace = 0;
    for (int i = 0; i < 4; ++i) {
        trace += m[i][i];
    }
    for (int i = 0; i < 4; ++i) {
        m[i][i] += 1e-12 * std::max(1.0, trace);
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) {
            pivot = std::fabs(m[r][c]) > std::fabs(m[pivot][c]) ? r : pivot;
        }
        for (int k = 0; k < 5; ++k) {
            std::swap(m[c][k], m[pivot][k]);
        }
        if (std::fabs(m[c][c]) < 1e-300) {
            continue;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == c) {
                continue;
            }
            const double f = m[r][c] / m[c][c];
            for (int k = c; k < 5; ++k) {
                m[r][k] -= f * m[c][k];
            }
        }
    }
    std::array<double, 4> a{};
    for (int i = 0; i < 4; ++i) {
        a[i] = std::fabs(m[i][i]) < 1e-300 ? 0 : m[i][4] / m[i][i];
    }
    return a;
}

double Round6(double v) {
    return std::round(v * 1e6) / 1e6;
}

} // namespace

const StageCalibration* StageCalibrations::Find(const std::string& figure) const {
    for (const StageCalibration& c : figures) {
        if (c.figure == figure) {
            return &c;
        }
    }
    return nullptr;
}

const StageFigure* StageSpace::Find(const std::string& name) const {
    for (const StageFigure& f : figures) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

std::pair<double, double> StageSpace::ToStage(double wx, double wy) const {
    return { (wx - originX) / size, (wy - originY) / size };
}

StageSpace BuildStageSpace(const FigureCatalog& geometry, const StageCalibrations* calibrations) {
    StageSpace space;
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (const FigureGeometry& g : geometry.figures) {
        if (g.kind != "model") {
            continue;
        }
        for (const FigureNode& n : g.nodeMap) {
            if (n.placed) {
                minX = std::min(minX, static_cast<double>(n.wx));
                maxX = std::max(maxX, static_cast<double>(n.wx));
                minY = std::min(minY, static_cast<double>(n.wy));
                maxY = std::max(maxY, static_cast<double>(n.wy));
            }
        }
    }
    if (maxX < minX) {
        space.limits.push_back("no-placed-lights: no figure has layout positions");
        return space;
    }
    // A square around the layout: the same unit across and up, centred.
    space.size = std::max({ maxX - minX, maxY - minY, 1e-6 });
    space.originX = minX - (space.size - (maxX - minX)) / 2;
    space.originY = minY - (space.size - (maxY - minY)) / 2;
    space.physical = calibrations != nullptr && calibrations->physical;
    space.metresPerUnit = space.physical ? calibrations->metresPerUnit : 0;
    if (!space.physical) {
        space.limits.push_back("schematic: layout positions, not measured distances");
    }
    for (const FigureGeometry& g : geometry.figures) {
        if (g.kind != "model") {
            continue;
        }
        StageFigure f;
        f.name = g.name;
        f.family = g.family;
        f.topology = g.topology;
        f.lights = g.nodes;
        f.noController = g.noController;
        std::vector<std::array<double, 4>> rows;
        std::vector<double> outX, outY;
        for (const FigureNode& n : g.nodeMap) {
            if (!n.placed) {
                continue;
            }
            rows.push_back({ n.sx, n.sy, n.sz, 1.0 });
            const auto [x, y] = space.ToStage(n.wx, n.wy);
            outX.push_back(x);
            outY.push_back(y);
        }
        if (rows.size() < 1) {
            f.precision = "unknown";
            space.figures.push_back(f);
            continue;
        }
        f.placed = true;
        std::array<double, 4> ax = Fit(rows, outX);
        std::array<double, 4> ay = Fit(rows, outY);
        if (rows.size() < 3) {
            // One or two lights: a place, not a shape.
            ax = { 0, 0, 0, outX.front() };
            ay = { 0, 0, 0, outY.front() };
        }
        f.minX = *std::min_element(outX.begin(), outX.end());
        f.maxX = *std::max_element(outX.begin(), outX.end());
        f.minY = *std::min_element(outY.begin(), outY.end());
        f.maxY = *std::max_element(outY.begin(), outY.end());
        f.cx = (f.minX + f.maxX) / 2;
        f.cy = (f.minY + f.maxY) / 2;
        // How well one transform places every light (a 2D tree's perspective is still
        // linear; a model edited by hand after this may not be).
        double worst = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            const double ex = ax[0] * rows[r][0] + ax[1] * rows[r][1] + ax[2] * rows[r][2] + ax[3];
            const double ey = ay[0] * rows[r][0] + ay[1] * rows[r][1] + ay[2] * rows[r][2] + ay[3];
            worst = std::max(worst, std::hypot(ex - outX[r], ey - outY[r]));
        }
        // A person's correction: scaled round the figure's centre, then moved.
        const StageCalibration* c = calibrations != nullptr ? calibrations->Find(g.name) : nullptr;
        if (c != nullptr) {
            const double k = c->scale > 0 ? c->scale : 1.0;
            for (double* a : { &ax[0], &ax[1], &ax[2], &ay[0], &ay[1], &ay[2] }) {
                *a *= k;
            }
            ax[3] = f.cx + k * (ax[3] - f.cx) + c->dx;
            ay[3] = f.cy + k * (ay[3] - f.cy) + c->dy;
            f.minX = f.cx + k * (f.minX - f.cx) + c->dx;
            f.maxX = f.cx + k * (f.maxX - f.cx) + c->dx;
            f.minY = f.cy + k * (f.minY - f.cy) + c->dy;
            f.maxY = f.cy + k * (f.maxY - f.cy) + c->dy;
            f.cx += c->dx;
            f.cy += c->dy;
            f.calibrated = true;
        }
        f.fitError = Round6(worst);
        f.precision = worst < 0.002 ? "exact" : "approximate";
        f.transform = { Round6(ax[0]), Round6(ax[1]), Round6(ax[2]), Round6(ax[3]), Round6(ay[0]), Round6(ay[1]), Round6(ay[2]), Round6(ay[3]) };
        if (f.precision != "exact") {
            space.limits.push_back(fmt::format("approximate-place:{}:{:.3f}", g.name, worst));
        }
        space.figures.push_back(std::move(f));
    }
    return space;
}

std::vector<std::pair<double, double>> LightsOnStage(const FigureGeometry& figure, const StageFigure& stage) {
    std::vector<std::pair<double, double>> out;
    out.reserve(figure.nodeMap.size());
    const std::vector<double>& m = stage.transform;
    for (const FigureNode& n : figure.nodeMap) {
        if (m.size() != 8) {
            out.push_back({ stage.cx, stage.cy });
            continue;
        }
        out.push_back({ m[0] * n.sx + m[1] * n.sy + m[2] * n.sz + m[3], m[4] * n.sx + m[5] * n.sy + m[6] * n.sz + m[7] });
    }
    return out;
}

StageAnchor ResolveAnchor(const std::string& id, const StageSpace& space, const FigureCatalog& geometry, const FigureSemantics* semantics) {
    StageAnchor a;
    a.id = id;
    const size_t colon = id.find(':');
    if (colon == std::string::npos) {
        a.problem = "malformed";
        return a;
    }
    const std::string kind = id.substr(0, colon);
    const std::string rest = id.substr(colon + 1);
    if (kind == "point") {
        const size_t comma = rest.find(',');
        if (comma == std::string::npos) {
            a.problem = "malformed";
            return a;
        }
        a.x = std::strtod(rest.substr(0, comma).c_str(), nullptr);
        a.y = std::strtod(rest.substr(comma + 1).c_str(), nullptr);
        a.ok = true;
        return a;
    }
    if (kind == "group") {
        const FigureGeometry* g = geometry.Find(rest);
        if (g == nullptr || g->kind != "group") {
            a.problem = "figure-missing";
            return a;
        }
        double sx = 0, sy = 0;
        int n = 0;
        for (const std::string& m : g->members) {
            if (const StageFigure* f = space.Find(m); f != nullptr && f->placed) {
                sx += f->cx;
                sy += f->cy;
                ++n;
            }
        }
        if (n == 0) {
            a.problem = "not-placed";
            return a;
        }
        a.x = sx / n;
        a.y = sy / n;
        a.ok = true;
        return a;
    }
    // node:<figure>:<n> and region:<figure>:<region>: the figure's name may hold colons
    // of its own, so the last part is taken from the end.
    std::string figure = rest;
    std::string part;
    if (kind == "node" || kind == "region") {
        const size_t last = rest.rfind(':');
        if (last == std::string::npos) {
            a.problem = "malformed";
            return a;
        }
        figure = rest.substr(0, last);
        part = rest.substr(last + 1);
    }
    const StageFigure* f = space.Find(figure);
    const FigureGeometry* g = geometry.Find(figure);
    if (f == nullptr || g == nullptr) {
        a.problem = "figure-missing";
        return a;
    }
    if (!f->placed) {
        a.problem = "not-placed";
        return a;
    }
    if (kind == "figure" || kind == "center") {
        a.x = f->cx;
        a.y = f->cy;
        a.ok = true;
        return a;
    }
    const std::vector<std::pair<double, double>> lights = LightsOnStage(*g, *f);
    if (kind == "node") {
        const long n = std::strtol(part.c_str(), nullptr, 10);
        if (n < 1 || n > static_cast<long>(lights.size())) {
            a.problem = "light-missing";
            return a;
        }
        a.x = lights[static_cast<size_t>(n - 1)].first;
        a.y = lights[static_cast<size_t>(n - 1)].second;
        a.ok = true;
        return a;
    }
    if (kind == "region") {
        const FigureProfile* p = semantics != nullptr ? semantics->Find(figure) : nullptr;
        const FigureRegion* r = p != nullptr ? p->Region(part) : nullptr;
        if (r == nullptr || r->nodes.empty()) {
            a.problem = "region-missing";
            return a;
        }
        double sx = 0, sy = 0;
        int n = 0;
        for (int node : r->nodes) {
            if (node >= 0 && node < static_cast<int>(lights.size())) {
                sx += lights[static_cast<size_t>(node)].first;
                sy += lights[static_cast<size_t>(node)].second;
                ++n;
            }
        }
        if (n == 0) {
            a.problem = "region-missing";
            return a;
        }
        a.x = sx / n;
        a.y = sy / n;
        a.ok = true;
        return a;
    }
    a.problem = "malformed";
    return a;
}

std::string StageCalibrationsPath(const std::string& showDir) {
    return (fs::path(showDir) / "ShowComposer" / "stage.json").string();
}

std::string ToJson(const StageCalibrations& c) {
    json figures = json::array();
    for (const StageCalibration& f : c.figures) {
        figures.push_back({ { "figure", f.figure }, { "dx", f.dx }, { "dy", f.dy }, { "scale", f.scale }, { "note", f.note } });
    }
    return json({ { "kind", "composer-stage-calibration" }, { "schemaVersion", c.schemaVersion }, { "physical", c.physical }, { "metresPerUnit", c.metresPerUnit }, { "figures", figures } }).dump(1);
}

bool FromJson(const std::string& text, StageCalibrations& out, std::string& error) {
    try {
        const json root = json::parse(text);
        if (root.value("schemaVersion", 0) < 1 || root.value("schemaVersion", 0) > STAGE_SPACE_SCHEMA_VERSION) {
            error = "stage: schema not read";
            return false;
        }
        StageCalibrations c;
        c.physical = root.value("physical", false);
        c.metresPerUnit = root.value("metresPerUnit", 0.0);
        for (const json& f : root.value("figures", json::array())) {
            c.figures.push_back({ f.value("figure", std::string()), f.value("dx", 0.0), f.value("dy", 0.0), f.value("scale", 1.0), f.value("note", std::string()) });
        }
        out = std::move(c);
    } catch (const json::exception& e) {
        error = std::string("stage: ") + e.what();
        return false;
    }
    return true;
}

std::string ToJson(const StageSpace& s) {
    json figures = json::array();
    for (const StageFigure& f : s.figures) {
        figures.push_back({ { "name", f.name }, { "family", f.family }, { "topology", f.topology }, { "lights", f.lights }, { "noController", f.noController }, { "placed", f.placed },
                            { "box", { Round6(f.minX), Round6(f.minY), Round6(f.maxX), Round6(f.maxY) } }, { "center", { Round6(f.cx), Round6(f.cy) } }, { "transform", f.transform },
                            { "fitError", f.fitError }, { "precision", f.precision }, { "calibrated", f.calibrated } });
    }
    return json({ { "kind", "composer-stage-space" }, { "schemaVersion", s.schemaVersion }, { "origin", { s.originX, s.originY } }, { "size", s.size }, { "physical", s.physical },
                  { "metresPerUnit", s.metresPerUnit }, { "figures", figures }, { "limits", s.limits } })
        .dump(1);
}

} // namespace composer
