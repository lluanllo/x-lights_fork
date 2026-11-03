#pragma once

/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include <string>

class xLightsFrame;

struct ComposerUITestOptions {
    std::string outDir;  // results, screenshots and generated files' reports
    std::string audio;   // a song the generator can read
    std::string lyrics;  // LRC lyrics for it
};

// Drives Show Composer through the real window, from the Tools menu to opening the
// sequence it made, taking screenshots on the way, then ends the process with 0 when
// every check passed. Only for a show folder copy whose controllers are all inactive.
void StartComposerUITest(xLightsFrame* frame, const ComposerUITestOptions& options);
