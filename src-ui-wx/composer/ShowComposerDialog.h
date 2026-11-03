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

#include <wx/collpane.h>
#include <wx/dialog.h>
#include <wx/timer.h>

#include <array>
#include <future>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "composer/ComposerReview.h"
#include "composer/GenerationSidecar.h"
#include "composer/ModelCapabilities.h"
#include "composer/TemplateLibrary.h"

#include "ComposerJob.h"

class AudioManager;
class ComposerTimeline;
class wxButton;
class wxCheckBox;
class wxChoice;

class wxGauge;
class wxListBox;
class wxListCtrl;
class wxListEvent;
class wxRadioBox;
class wxSimplebook;
class wxSlider;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;
class xLightsFrame;

// Show Composer: from a song, its lyrics and this installation to a finished sequence, in
// five steps a show's author can follow without knowing how xLights stores anything.
//
// All the work happens in the Composer's own generator (see ComposerJob), which this
// window starts and reads back; nothing here composes, places or renders an effect.
class ShowComposerDialog : public wxDialog {
public:
    enum class Step {
        Create,
        Analysis,
        Composition,
        Generate,
        Result
    };
    static constexpr int STEP_COUNT = 5;

    explicit ShowComposerDialog(xLightsFrame* frame);
    ~ShowComposerDialog() override;

    // What the person asked to open when the dialog ended with wxID_OK.
    [[nodiscard]] const std::string& SequenceToOpen() const { return _openSequence; }
    [[nodiscard]] const std::string& ShowFolderToOpen() const { return _openShowDir; }

    // For the UI test: the same calls the controls make.
    void SetAudio(const std::string& path);
    void SetShowFolder(const std::string& path);
    void SetLyrics(const std::string& path);
    void SetTiming(const std::string& path);
    void SetLevel(composer::AutomationLevel level);
    void SetSequenceName(const std::string& name);
    void GoTo(Step step);
    void StartAnalysis();
    void StartComposition();
    void StartGeneration();
    void CancelJob();
    void ExcludeFigure(const std::string& name, bool exclude);
    void LockFigure(const std::string& name, bool lock);
    [[nodiscard]] bool LoadPreviousShow(const std::string& sidecarPath, std::string& error);
    [[nodiscard]] bool JobRunning() const { return _job.Running(); }
    // A generator running, queued, or its results still being read.
    [[nodiscard]] bool Busy() const { return _job.Running() || !_queue.empty() || !_loads.empty(); }
    [[nodiscard]] int LastExitCode() const { return _lastExitCode; }
    [[nodiscard]] Step CurrentStep() const { return _step; }
    [[nodiscard]] bool CanGoTo(Step step) const;
    [[nodiscard]] const composer::ReportFolder& AnalysisReport() const { return _analysisReport; }
    [[nodiscard]] const composer::ReportFolder& LevelReport(composer::AutomationLevel level) const { return _levels[static_cast<int>(level)]; }
    [[nodiscard]] const composer::ReportFolder& ResultReport() const { return _result; }
    [[nodiscard]] const composer::CorrectionSet& Corrections() const { return _corrections; }
    [[nodiscard]] const std::string& ResultSequence() const { return _resultSequence; }
    [[nodiscard]] const std::string& ErrorShown() const { return _lastError; }
    [[nodiscard]] wxWindow* Page(Step step) const;
    void SetTestMode(bool on) { _testMode = on; }
    [[nodiscard]] const composer::SongAnalysis& ShownAnalysis() const { return _shownAnalysis; }
    [[nodiscard]] const std::string& LyricsState() const { return _lyricsState; }
    [[nodiscard]] bool AudioOk() const { return _audioOk; }
    [[nodiscard]] bool ShowOk() const { return _showOk; }
    [[nodiscard]] const composer::ModelCapabilities& Capabilities() const { return _capabilities; }
    [[nodiscard]] const std::string& ShowFolder() const { return _showDir; }
    [[nodiscard]] const std::vector<composer::FigureSummary>& Figures() const { return _figures; }
    void AddMusicBarAt(int ms, int beatsPerBar);
    void OpenResult(); // what "Open the Sequence in xLights" does
    // Lets a UI test drive the window once it exists; called at the end of the constructor.
    static std::function<void(ShowComposerDialog*)>& OpenedHook();

private:
    enum class Purpose {
        None,
        Layout,
        Analysis,
        Composition,
        Generation
    };

    void BuildControls();
    wxWindow* BuildCreatePage(wxWindow* parent);
    wxWindow* BuildAnalysisPage(wxWindow* parent);
    wxWindow* BuildCompositionPage(wxWindow* parent);
    wxWindow* BuildGeneratePage(wxWindow* parent);
    wxWindow* BuildResultPage(wxWindow* parent);

    void RefreshAll();
    void RefreshCreate();
    void RefreshAnalysis();
    void RefreshComposition();
    void RefreshFigureDetails();
    void RefreshGenerate();
    void RefreshResult();
    void RefreshSteps();

    void LoadAudio();
    void LoadShowInfo();
    void LoadLyricsInfo();
    void LoadLibrary();
    [[nodiscard]] std::vector<std::string> Arguments(const std::string& stopAfter, const std::string& reportDir, const std::string& name, int level) const;
    [[nodiscard]] std::string NewWorkFolder(const std::string& what);
    [[nodiscard]] std::string InputsKey() const;
    [[nodiscard]] std::string SequencePath() const;
    [[nodiscard]] bool SequenceNameFree(wxString& why) const;
    [[nodiscard]] composer::AutomationLevel Level() const;
    void RunNext();
    void OnJob(const ComposerJob::Update& update);
    void JobFinished(int exitCode);
    void ShowError(const wxString& what, const wxString& how);
    void ApplyMusicCorrectionsLocally();
    void ApplyReport(Purpose purpose, int level, const std::string& sequenceName, composer::ReportFolder report);
    void CollectLoads();

    void OnStepSelected(wxCommandEvent& event);
    void OnBack(wxCommandEvent& event);
    void OnNext(wxCommandEvent& event);
    void OnCloseButton(wxCommandEvent& event);
    void OnClose(wxCloseEvent& event);
    void OnTimer(wxTimerEvent& event);
    void OnFigureSelected(wxListEvent& event);

    xLightsFrame* _frame = nullptr;
    Step _step = Step::Create;
    bool _testMode = false;

    // Inputs
    std::string _audioPath;
    std::string _showDir;
    std::string _lyricsPath;
    std::string _timingPath;
    std::string _sessionPath;
    std::string _profilePath;
    std::string _templatesDir;
    std::string _rolesPath;
    std::string _sequenceName;
    bool _renderCheck = true;
    composer::CorrectionSet _corrections;
    std::string _workDir;
    int _workCounter = 0;

    // What was found about them
    std::unique_ptr<AudioManager> _audio;
    int _durationMS = 0;
    bool _audioOk = false;
    bool _peaksPending = false;
    std::string _audioHash;
    bool _cacheValid = false;
    composer::ModelCapabilities _capabilities;
    bool _showOk = false;
    std::string _showError;
    composer::TemplateLibrary _library;
    std::string _lyricsState;
    int _lyricLines = 0;
    bool _lyricsTimed = false;

    // Results of the generator
    ComposerJob _job;
    Purpose _purpose = Purpose::None;
    std::vector<std::pair<Purpose, std::vector<std::string>>> _queue;
    std::vector<std::string> _queueDirs;
    std::vector<int> _queueLevels;
    std::string _jobDir;
    int _jobLevel = -1;
    int _lastExitCode = -1;
    std::string _lastError;
    composer::ReportFolder _analysisReport;
    composer::SongAnalysis _shownAnalysis; // the analysis with the corrections made here
    std::string _analysisKey;
    std::array<composer::ReportFolder, 3> _levels;
    std::string _levelsKey;
    std::vector<composer::FigureSummary> _figures;
    composer::ReportFolder _result;
    std::string _resultSequence;
    std::string _resultReportDir;
    std::string _resultSummary;
    bool _closeWhenDone = false;
    // Reports are read off the UI thread: a large layout's files take seconds to parse.
    struct PendingLoad {
        Purpose purpose = Purpose::None;
        int level = -1;
        std::string sequenceName;
        std::shared_ptr<composer::ReportFolder> report;
        std::future<std::string> done; // the error, empty when it read
    };
    std::vector<PendingLoad> _loads;
    std::string _openSequence;
    std::string _openShowDir;

    // Controls
    wxListBox* _steps = nullptr;
    wxSimplebook* _book = nullptr;
    wxButton* _back = nullptr;
    wxButton* _next = nullptr;
    wxButton* _close = nullptr;
    wxStaticText* _status = nullptr;
    // Create
    wxStaticText* _audioName = nullptr;
    wxStaticText* _audioInfo = nullptr;
    wxStaticText* _showName = nullptr;
    wxStaticText* _showInfo = nullptr;
    wxStaticText* _lyricsName = nullptr;
    wxStaticText* _timingName = nullptr;
    wxStaticText* _lyricsInfo = nullptr;
    wxChoice* _profile = nullptr;
    std::vector<std::string> _profilePaths;
    wxRadioBox* _level = nullptr;
    wxSpinCtrl* _seed = nullptr;
    wxTextCtrl* _name = nullptr;
    wxStaticText* _nameInfo = nullptr;
    wxCollapsiblePane* _advanced = nullptr;
    wxStaticText* _templatesName = nullptr;
    wxStaticText* _rolesName = nullptr;
    wxCheckBox* _render = nullptr;
    // Analysis
    ComposerTimeline* _timeline = nullptr;
    wxSlider* _zoom = nullptr;
    wxStaticText* _analysisInfo = nullptr;
    wxStaticText* _lyricsSync = nullptr;
    wxStaticText* _cursorInfo = nullptr;
    wxChoice* _beatsPerBar = nullptr;
    wxChoice* _sectionKind = nullptr;
    wxStaticText* _musicFixes = nullptr;
    // Composition
    wxListCtrl* _levelList = nullptr;
    wxListCtrl* _figureList = nullptr;
    wxChoice* _figureFilter = nullptr;
    wxTextCtrl* _figureDetails = nullptr;
    wxListCtrl* _cueList = nullptr;
    wxStaticText* _compositionInfo = nullptr;
    wxStaticText* _correctionInfo = nullptr;
    // Generate
    wxListCtrl* _stageList = nullptr;
    wxGauge* _gauge = nullptr;
    wxStaticText* _stageInfo = nullptr;
    wxStaticText* _generateSummary = nullptr;
    wxButton* _generate = nullptr;
    wxButton* _cancel = nullptr;
    // Result
    wxStaticText* _resultTitle = nullptr;
    wxStaticText* _resultFacts = nullptr;
    wxStaticText* _reviewState = nullptr;
    wxListCtrl* _quietList = nullptr;
    wxListCtrl* _warningList = nullptr;
    wxListCtrl* _resultCues = nullptr;

    wxTimer _timer;
};
