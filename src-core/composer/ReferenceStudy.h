#pragma once

#include <string>
#include <vector>

// How a finished show is made, read from the show itself: what effects go where,
// for how long, in which colours and layers, how the figures start and stop
// together, and what the render actually lights. Everything here is measured; the
// aesthetic reading of it is written by a person (or a report) from these numbers
// and from the pictures, never stored as if it were measured.
namespace composer {

struct ReferenceStudyOptions {
    std::string showDir;      // a copy with every controller inactive
    std::string sequencePath; // inside that copy; opened, never saved
    std::string outDir;
    std::string id;           // a short name for the reports
    std::vector<int> timesMS; // empty: chosen from the show's own changes
    bool render = true;
    int imageWidth = 1280;
};

// Writes stats.json, summary.txt, timeline.csv, frames/house_<ms>.png and
// contact_sheet.png under outDir. 0 written, 1 failed, 2 refused.
[[nodiscard]] int RunReferenceStudy(const ReferenceStudyOptions& options);

} // namespace composer
