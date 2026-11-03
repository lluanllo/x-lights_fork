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

#include <wx/event.h>
#include <wx/timer.h>

#include <functional>
#include <string>
#include <vector>

class wxProcess;

// One Show Composer generation run by this same executable with --composer-generate,
// in a process of its own. The window stays responsive, the render engine and audio of
// the open sequence are never shared with it, and cancelling uses the generator's own
// cancel file, so a cancel stops between steps rather than in the middle of a write.
//
// Everything here runs on the UI thread: the child's output is read on a timer.
class ComposerJob : public wxEvtHandler {
public:
    struct Update {
        enum class Kind {
            Stage,   // `stage` changed its state to `what` ("start", "ok", "warning", "failed", "cancelled")
            Detail,  // something inside `stage`: "analysis 40%", "write", "render", "check render"
            Line,    // any other output line
            Finished // `exitCode` holds the generator's code: 0 published, 1 failed, 2 refused, 3 cancelled
        };
        Kind kind = Kind::Line;
        std::string stage;
        std::string what;
        std::string line;
        int exitCode = -1;
    };
    using Listener = std::function<void(const Update&)>;

    explicit ComposerJob(Listener listener);
    ~ComposerJob() override;

    // `arguments` come after the executable; `cancelFile` must not exist yet.
    [[nodiscard]] bool Start(const std::vector<std::string>& arguments, const std::string& cancelFile, std::string& error);
    // Asks the generator to stop at its next check; the Finished update says when it did.
    void Cancel();
    [[nodiscard]] bool Running() const { return _process != nullptr; }
    [[nodiscard]] bool CancelRequested() const { return _cancelRequested; }
    [[nodiscard]] const std::vector<std::string>& Output() const { return _output; }

    // Called by the process object when the child ends.
    void ProcessEnded(int exitCode);

    // Generators started by any job that have not ended yet, orphaned ones included.
    static int& LiveProcesses();

private:
    void OnTimer(wxTimerEvent& event);
    void ReadAvailable();
    void Handle(const std::string& line);

    Listener _listener;
    wxProcess* _process = nullptr;
    long _pid = 0;
    wxTimer _timer;
    std::string _cancelFile;
    bool _cancelRequested = false;
    std::string _partial;
    std::vector<std::string> _output;
};
