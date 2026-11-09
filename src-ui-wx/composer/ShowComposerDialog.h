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
#include <wx/bitmap.h>
#include <wx/dialog.h>
#include <wx/timer.h>

#include <array>
#include <future>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "composer/ArtisticPreferences.h"
#include "composer/ComposerReview.h"
#include "composer/GenerationSidecar.h"
#include "composer/MediaAnalysis.h"
#include "composer/MediaDirection.h"
#include "composer/ModelCapabilities.h"
#include "composer/TemplateLibrary.h"

#include "ComposerJob.h"

class AudioManager;
class ComposerTimeline;
class MediaAnalysisRunner;
class ModelDownloadRunner;
class StoryboardView;
struct ReviewContext;
class wxButton;
class wxCheckBox;
class wxChoice;

class wxGauge;
class wxListBox;
class wxListCtrl;
class wxListEvent;
class wxPanel;
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
    // Files dropped on the first step: a song, lyrics (.txt, .lrc, .srt) or an .xtiming.
    // Each is used where it is, never copied or moved.
    void DropFiles(const std::vector<std::string>& paths);
    // While something is dragged over the first step: whether it would be taken.
    void ShowDropState(bool over);
    [[nodiscard]] bool AcceptsDrops() const { return !_job.Running() && _queue.empty(); }
    void SetLevel(composer::AutomationLevel level);
    // The music assistant, run apart like the generator. Nothing leaves this computer
    // unless `consent` is true, which only the person's yes in a dialog makes it.
    void StartAssist(const std::string& action, bool consent, const std::string& query = "");
    void SetTranscript(const std::string& path);
    void SetCharacter(const std::string& path);
    void SetProfile(const std::string& path);
    [[nodiscard]] bool AssistRunning() const { return _assistJob.Running(); }
    [[nodiscard]] int AssistExitCode() const { return _assistExit; }
    [[nodiscard]] const std::string& AssistMessage() const { return _assistMessage; }
    [[nodiscard]] const std::string& TranscriptPath() const { return _transcriptPath; }
    [[nodiscard]] const std::string& CharacterPath() const { return _characterPath; }
    [[nodiscard]] const std::vector<std::string>& ProfilePaths() const { return _profilePaths; }
    [[nodiscard]] const std::vector<std::string>& StyleColors() const { return _styleColors; }
    [[nodiscard]] const std::string& StyleTheme() const { return _styleTheme; }
    void SetTestConsent(bool yes) { _testConsent = yes; }
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
    [[nodiscard]] const std::vector<composer::LyricIssue>& LyricIssues() const { return _lyricIssues; }
    [[nodiscard]] bool AudioOk() const { return _audioOk; }
    [[nodiscard]] const std::string& AudioPath() const { return _audioPath; }
    [[nodiscard]] bool ShowOk() const { return _showOk; }
    [[nodiscard]] const composer::ModelCapabilities& Capabilities() const { return _capabilities; }
    [[nodiscard]] const std::string& ShowFolder() const { return _showDir; }
    [[nodiscard]] const std::vector<composer::FigureSummary>& Figures() const { return _figures; }
    void AddMusicBarAt(int ms, int beatsPerBar);
    // Pictures and videos, and voices: what the controls of steps 1, 2 and 3 change.
    void SetMediaFolders(const std::vector<std::string>& folders);
    [[nodiscard]] const std::vector<std::string>& MediaFolders() const { return _mediaFolders; }
    void SetMediaAutomatic(bool on);
    void SetCopyMedia(bool on);
    void ApplyVoiceEdits(const std::vector<composer::VoiceDef>& voices, const std::vector<composer::LyricCorrection>& corrections);
    void ApplyMediaCorrection(const composer::MediaCorrection& correction);
    struct MediaRow {
        std::string segmentId;
        int startMS = 0;
        int endMS = 0;
        std::string target;
        std::string mediaId;
        std::string name;
        std::string path;
        std::string state; // "automatic", "manual-edit", "locked"; empty for a cue without a file
        wxString why;
    };
    [[nodiscard]] const std::vector<MediaRow>& MediaRows() const { return _mediaRows; }
    // The visual montage: the library analysed on a thread of its own, the storyboard of
    // the level shown, and the changes a person makes to it.
    void StartMediaAnalysis();
    [[nodiscard]] bool MediaAnalysisRunning() const;
    [[nodiscard]] const composer::MediaAnalysisSummary& LastMediaAnalysis() const { return _lastMediaAnalysis; }
    [[nodiscard]] const composer::VisualMediaStoryboard& ShownStoryboard() const { return _storyboard; }
    void LockStoryboardScene(const std::string& sceneId);
    void LockStoryboardLane(const std::string& lane);
    // The screens and how words and pictures share them: kept with the show.
    void SetScreens(const composer::ScreenSettings& screens);
    [[nodiscard]] const composer::ScreenSettings& Screens() const { return _corrections.screens; }
    [[nodiscard]] composer::SurfacePlan ScreensPreview() const;
    void MoveShotToOtherScreen(int shot);
    void RebuildMontage();
    [[nodiscard]] int MontageVariant() const { return _mediaVariant; }
    [[nodiscard]] const std::vector<composer::StoryboardVersion>& StoryboardHistory() const { return _storyboardHistory; }
    void RestoreStoryboardVersion(size_t index);
    // The voices and phrases the voice editor starts from, from the last analysis.
    [[nodiscard]] std::vector<composer::VoiceDef> CurrentVoices() const;
    void OpenResult(); // what "Open the Sequence in xLights" does
    // The review of the result's pictures and videos, kept in its sidecar.
    [[nodiscard]] composer::VisualReview ResultReview() const;
    // Keeps a review: in the sidecar (and the old approval file once approved), what it
    // teaches in the show's preferences, and the files never to use in the library.
    bool SaveReview(const composer::VisualReview& review, const std::vector<std::string>& neverUse);
    // The review's corrections applied: the same show made again as a new version, with
    // the approved shots kept and the rejected ones chosen again.
    void ApplyReview(const composer::VisualReview& review);
    [[nodiscard]] composer::ArtisticPreferences ShowPreferences() const;
    bool SetShowPreferences(const composer::ArtisticPreferences& prefs);
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
    void EditMediaLibrary();
    void EditScreens();
    void EditReview();
    void EditPreferences();
    void EditRecipes();
    [[nodiscard]] std::set<std::string> HouseFamilies() const;

public:
    // The artistic director's settings, for the UI test.
    void SetDirector(int christmas, int variety, bool recipes);
    // The person's evaluation of the result, kept with its review; false when refused.
    bool SaveEvaluation(const composer::ShowEvaluation& evaluation);
    [[nodiscard]] std::map<std::string, double> ResultMeasures() const;
    [[nodiscard]] std::vector<std::string> GenerateArguments() const { return Arguments("", "report", "test.xsq", -1); }

private:
    [[nodiscard]] ReviewContext MakeReviewContext() const;
    void EditVoices();
    void ChangeMedia(int action);
    void GetVisualModel();
    void ChooseStoryboardVersion();
    void RefreshMediaAnalysisState();
    [[nodiscard]] std::string VisualModelDir() const;

    xLightsFrame* _frame = nullptr;
    std::vector<std::string> _mediaFolders;
    std::vector<MediaRow> _mediaRows;
    wxStaticText* _mediaInfo = nullptr;
    wxStaticText* _screensInfo = nullptr;
    wxCheckBox* _mediaAuto = nullptr;
    wxCheckBox* _copyMedia = nullptr;
    wxStaticText* _voicesInfo = nullptr;
    std::unique_ptr<MediaAnalysisRunner> _analysisRunner;
    std::unique_ptr<ModelDownloadRunner> _modelRunner;
    composer::MediaAnalysisSummary _lastMediaAnalysis;
    wxStaticText* _analysisState = nullptr;
    wxButton* _analyzeButton = nullptr;
    wxButton* _modelButton = nullptr;
    int _mediaVariant = 0;
    int _mediaStamp = 0; // changes when the library is analysed again
    StoryboardView* _storyboardView = nullptr;
    composer::VisualMediaStoryboard _storyboard;
    std::vector<composer::StoryboardVersion> _storyboardHistory;
    std::vector<int> _rowShot; // _mediaRows -> the storyboard shot, or -1
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
    std::vector<composer::LyricIssue> _lyricIssues;

    // Results of the generator
    ComposerJob _job;
    ComposerJob _assistJob;
    std::string _assistAction;
    std::string _assistResultPath;
    std::string _assistMessage;
    int _assistExit = -1;
    bool _testConsent = false;
    std::string _transcriptPath;
    std::string _characterPath;
    std::vector<std::string> _styleColors;
    std::string _styleTheme;
    std::vector<std::pair<int, std::vector<std::string>>> _sceneStrip; // scene start, colours
    int _sceneStripEnd = 0;
    void OnAssistJob(const ComposerJob::Update& u);
    void AssistFinished(int exitCode);
    bool AskConsent(const wxString& service, const wxString& where);
    void EditKeys();
    void RefreshStyle();
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
    mutable composer::VisualReview _reviewCache;
    // The library as last read, while its file does not change: the window refreshes
    // often, also while the generator works.
    [[nodiscard]] const composer::MediaLibrary* CachedLibrary() const;
    mutable composer::MediaLibrary _libraryCache;
    mutable std::string _libraryCacheKey;
    mutable bool _libraryCacheOk = false;
    std::map<std::string, wxBitmap> _thumbCache; // thumbnail path -> bitmap for the montage
    mutable std::string _reviewCacheKey;
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
    wxPanel* _dropZone = nullptr;
    wxStaticText* _dropLabel = nullptr;
    wxStaticText* _showName = nullptr;
    wxStaticText* _showInfo = nullptr;
    wxStaticText* _lyricsName = nullptr;
    wxStaticText* _timingName = nullptr;
    wxStaticText* _lyricsInfo = nullptr;
    wxStaticText* _assistInfo = nullptr;
    wxPanel* _swatch = nullptr;
    wxStaticText* _styleInfo = nullptr;
    wxStaticText* _alignmentInfo = nullptr;
    wxPanel* _sceneView = nullptr;
    wxTextCtrl* _directionText = nullptr;
    wxTextCtrl* _coherenceText = nullptr;
    wxChoice* _profile = nullptr;
    std::vector<std::string> _profilePaths;
    wxRadioBox* _level = nullptr;
    wxSpinCtrl* _seed = nullptr;
    std::string _briefPath; // colour ideas from the lyrics (Groq), when asked for
    int _recipesStamp = 0; // the recipes file changed: the composition is worked out again
    wxSlider* _christmas = nullptr;
    wxSlider* _variety = nullptr;
    wxCheckBox* _useRecipes = nullptr;
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
