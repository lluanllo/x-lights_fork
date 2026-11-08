/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ComposerJob.h"

#include <wx/process.h>
#include <wx/stdpaths.h>
#include <wx/stream.h>
#include <wx/utils.h>

#include <filesystem>
#include <fstream>

#include "utils/ExternalHooks.h"

#include <spdlog/spdlog.h>

namespace {

constexpr const char* PREFIX = "composer-generate: ";

// Lets the job know when the child ends, or deletes itself if the job is gone first.
class JobProcess : public wxProcess {
public:
    explicit JobProcess(ComposerJob* job) :
        wxProcess(wxPROCESS_REDIRECT),
        _job(job) {}

    void Orphan() { _job = nullptr; }

    void OnTerminate(int pid, int status) override {
        --ComposerJob::LiveProcesses();
        if (_job != nullptr) {
            _job->ProcessEnded(status);
        }
        delete this;
    }

private:
    ComposerJob* _job;
};

} // namespace

int& ComposerJob::LiveProcesses() {
    static int live = 0;
    return live;
}

ComposerJob::ComposerJob(Listener listener) :
    _listener(std::move(listener)),
    _timer(this) {
    Bind(wxEVT_TIMER, &ComposerJob::OnTimer, this);
}

ComposerJob::~ComposerJob() {
    _timer.Stop();
    if (_process != nullptr) {
        // The window is going away while the generator still runs: it is asked to stop
        // and left to finish on its own, never killed in the middle of a write.
        Cancel();
        static_cast<JobProcess*>(_process)->Orphan();
        _process = nullptr;
    }
}

bool ComposerJob::Start(const std::vector<std::string>& arguments, const std::string& cancelFile, std::string& error) {
    if (_process != nullptr) {
        error = "a generation is already running";
        return false;
    }
    if (FileExists(cancelFile)) {
        error = "the cancel file already exists: " + cancelFile;
        return false;
    }
    _cancelFile = cancelFile;
    _cancelRequested = false;
    _partial.clear();
    _output.clear();

    std::vector<wxString> args;
    args.push_back(wxStandardPaths::Get().GetExecutablePath());
    for (const std::string& a : arguments) {
        args.push_back(wxString::FromUTF8(a));
    }
    args.push_back("--cancel-file");
    args.push_back(wxString::FromUTF8(cancelFile));
    std::vector<const wxChar*> argv;
    for (const wxString& a : args) {
        argv.push_back(a.wc_str());
    }
    argv.push_back(nullptr);

    auto* process = new JobProcess(this);
    _pid = wxExecute(argv.data(), wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE, process);
    if (_pid <= 0) {
        delete process;
        error = "the generator could not be started";
        return false;
    }
    ++LiveProcesses();
    spdlog::info("Show Composer: started the generator, pid {}", _pid);
    _process = process;
    _timer.Start(100);
    return true;
}

void ComposerJob::Cancel() {
    if (_process == nullptr || _cancelRequested) {
        return;
    }
    _cancelRequested = true;
    std::ofstream(std::filesystem::path(_cancelFile)) << "cancel\n";
    spdlog::info("Show Composer: cancel requested ({})", _cancelFile);
}

void ComposerJob::OnTimer(wxTimerEvent&) {
    ReadAvailable();
}

void ComposerJob::ReadAvailable() {
    if (_process == nullptr) {
        return;
    }
    char buffer[4096];
    wxInputStream* out = _process->GetInputStream();
    while (out != nullptr && _process->IsInputAvailable()) {
        out->Read(buffer, sizeof(buffer));
        if (out->LastRead() == 0) {
            break;
        }
        _partial.append(buffer, out->LastRead());
    }
    // The decoder's own messages go to stderr; they are read only so the pipe never fills.
    wxInputStream* err = _process->GetErrorStream();
    while (err != nullptr && _process->IsErrorAvailable()) {
        err->Read(buffer, sizeof(buffer));
        if (err->LastRead() == 0) {
            break;
        }
    }
    size_t end;
    while ((end = _partial.find('\n')) != std::string::npos) {
        std::string line = _partial.substr(0, end);
        _partial.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        Handle(line);
    }
}

void ComposerJob::Handle(const std::string& line) {
    _output.push_back(line);
    Update u;
    u.line = line;
    if (line.rfind(PREFIX, 0) == 0 && line.size() > std::char_traits<char>::length(PREFIX) + 2 && line[std::char_traits<char>::length(PREFIX)] == '[') {
        const size_t open = std::char_traits<char>::length(PREFIX);
        const size_t close = line.find(']', open);
        if (close != std::string::npos) {
            u.stage = line.substr(open + 1, close - open - 1);
            u.what = close + 2 <= line.size() ? line.substr(close + 2) : std::string();
            const bool state = u.what == "start" || u.what == "ok" || u.what == "warning" || u.what == "failed" || u.what == "cancelled";
            u.kind = state ? Update::Kind::Stage : Update::Kind::Detail;
        }
    }
    if (_listener) {
        _listener(u);
    }
}

void ComposerJob::ProcessEnded(int exitCode) {
    ReadAvailable();
    if (_process != nullptr) {
        // Whatever is left in the pipes after the child is gone.
        wxInputStream* stream = _process->GetInputStream();
        char buffer[4096];
        while (stream != nullptr && stream->CanRead()) {
            stream->Read(buffer, sizeof(buffer));
            if (stream->LastRead() == 0) {
                break;
            }
            _partial.append(buffer, stream->LastRead());
        }
    }
    _partial += "\n";
    _process = nullptr;
    _timer.Stop();
    size_t end;
    while ((end = _partial.find('\n')) != std::string::npos) {
        std::string line = _partial.substr(0, end);
        _partial.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            Handle(line);
        }
    }
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(_cancelFile), ec);
    spdlog::info("Show Composer: the generator ended with {}", exitCode);
    Update u;
    u.kind = Update::Kind::Finished;
    u.exitCode = exitCode;
    if (_listener) {
        _listener(u);
    }
}
