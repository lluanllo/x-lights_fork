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

#include <wx/bitmap.h>
#include <wx/panel.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "composer/MediaAnalysis.h"
#include "composer/MediaStoryboard.h"

// The library searched and looked at on a thread of its own, so the window stays usable:
// the folders are searched again, every file not analysed yet (or changed, or analysed
// with an older model) is measured, and the library is saved. With the image-text model
// the files are also described; without it the analysis is basic and says so.
class MediaAnalysisRunner {
public:
    ~MediaAnalysisRunner();
    void Start(const std::string& showDir, const std::vector<std::string>& folders, const std::string& modelDir);
    void Cancel() { _cancel = true; }
    [[nodiscard]] bool Running() const { return _running; }
    [[nodiscard]] bool Finished() const { return _finished; }
    [[nodiscard]] int Done() const { return _done; }
    [[nodiscard]] int Total() const { return _total; }
    [[nodiscard]] std::string Current() const;
    [[nodiscard]] composer::MediaAnalysisSummary Summary() const;
    [[nodiscard]] std::string Error() const;
    void Join();

private:
    std::thread _thread;
    std::atomic<bool> _running{ false };
    std::atomic<bool> _finished{ false };
    std::atomic<bool> _cancel{ false };
    std::atomic<int> _done{ 0 };
    std::atomic<int> _total{ 0 };
    mutable std::mutex _lock;
    std::string _current;
    std::string _error;
    composer::MediaAnalysisSummary _summary;
};

// The image-text model fetched once into <show>/ai-models, only after a person agreed.
class ModelDownloadRunner {
public:
    ~ModelDownloadRunner();
    void Start(const std::string& root);
    void Cancel() { _cancel = true; }
    [[nodiscard]] bool Running() const { return _running; }
    [[nodiscard]] bool Finished() const { return _finished; }
    [[nodiscard]] int Percent() const { return _percent; }
    [[nodiscard]] bool Ok() const { return _ok; }
    [[nodiscard]] std::string Error() const;
    void Join();

private:
    std::thread _thread;
    std::atomic<bool> _running{ false };
    std::atomic<bool> _finished{ false };
    std::atomic<bool> _cancel{ false };
    std::atomic<bool> _ok{ false };
    std::atomic<int> _percent{ 0 };
    mutable std::mutex _lock;
    std::string _error;
};

// The visual storyboard on a timeline: a lane for the matrix and one for the mega tree,
// each shot as its picture, the scenes behind them and the gaps left to the lights.
class StoryboardView : public wxPanel {
public:
    explicit StoryboardView(wxWindow* parent);
    void SetStoryboard(const composer::VisualMediaStoryboard* storyboard, int durationMS, std::map<std::string, wxBitmap> thumbnails);
    void Select(int shot);
    [[nodiscard]] int Selected() const { return _selected; }
    std::function<void(int shot)> onSelect;

private:
    void OnPaint(wxPaintEvent& event);
    void OnClick(wxMouseEvent& event);
    [[nodiscard]] wxRect ShotRect(int shot) const;

    const composer::VisualMediaStoryboard* _board = nullptr;
    int _durationMS = 0;
    std::map<std::string, wxBitmap> _thumbs;
    int _selected = -1;
};

// What a visual concept is called for a person, in their language.
[[nodiscard]] wxString VisualConceptName(const std::string& id);
// A shot's reasons in a short sentence: "winter theme, wide picture, the chorus' energy".
[[nodiscard]] wxString ShotWhy(const composer::StoryboardShot& shot);
// The framing, said simply: "fills the figure", "slow zoom", "round the tree"...
[[nodiscard]] wxString FramingName(const std::string& strategy);
