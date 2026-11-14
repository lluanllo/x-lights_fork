#include "composer/ColorTest.h"

#include "composer/ColorCheck.h"
#include "composer/ColorSpace.h"
#include "composer/FigureGeometry.h"
#include "composer/ModelCapabilities.h"
#include "composer/StageDirector.h"
#include "composer/StageSpace.h"
#include "composer/UnderstandingStore.h"
#include "composer/WriterTest.h"
#include "render/HeadlessRenderContext.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace composer {

namespace {

bool WriteText(const fs::path& path, const std::string& text) {
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(path.wstring().c_str(), L"wb");
#else
    f = fopen(path.string().c_str(), "wb");
#endif
    if (f == nullptr) {
        return false;
    }
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return fclose(f) == 0 && ok;
}

// The scenes a colour score made in one family on purpose, read loosely so any version
// of the file can be measured.
std::set<std::string> IntendedScenes(const std::string& chromaPath) {
    std::set<std::string> out;
    std::string text;
    if (chromaPath.empty() || !ReadTextFile(chromaPath, text)) {
        return out;
    }
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (!root.is_object()) {
        return out;
    }
    for (const nlohmann::json& s : root.value("scenes", nlohmann::json::array())) {
        if (s.is_object() && s.value("monochrome", false)) {
            out.insert(s.value("id", std::string()));
        }
    }
    return out;
}

} // namespace

int RunColorMeasure(const std::string& showDir, const std::string& fseqPath, const std::string& scorePath, const std::string& chromaPath, const std::string& outDir) {
    std::string refused;
    if (!ControllersAreInactive(showDir, refused)) {
        printf("composer-color-measure: REFUSED: %s\n", refused.c_str());
        return 2;
    }
    HeadlessRenderContext ctx;
    if (!ctx.LoadShowFolder(showDir)) {
        printf("composer-color-measure: the show could not be loaded\n");
        return 1;
    }
    const ModelCapabilities caps = ExtractCapabilities(ctx, nullptr);
    const FigureCatalog figures = DescribeFigures(ctx, caps, true);
    const StageSpace space = BuildStageSpace(figures, nullptr);
    GlobalStageScore score;
    bool hasScore = false;
    if (!scorePath.empty()) {
        std::string text, error;
        hasScore = ReadTextFile(scorePath, text) && FromJson(text, score, error);
        if (!hasScore) {
            printf("composer-color-measure: score not read: %s\n", error.c_str());
            return 1;
        }
    }
    const std::set<std::string> intended = IntendedScenes(chromaPath);
    const ColorCheck check = CheckColors(fseqPath, figures, space, hasScore ? &score : nullptr, &intended);
    std::error_code ec;
    fs::create_directories(fs::path(outDir), ec);
    const bool ok = WriteText(fs::path(outDir) / "color-check.json", ToJson(check)) && WriteText(fs::path(outDir) / "color-check.txt", ColorCheckReport(check));
    printf("%s", ColorCheckReport(check).c_str());
    return check.ran && ok ? 0 : 1;
}

int RunColorTest(const std::string& workDir) {
    (void)workDir;
    printf("composer-color-test: not written yet\n");
    return 1;
}

} // namespace composer
