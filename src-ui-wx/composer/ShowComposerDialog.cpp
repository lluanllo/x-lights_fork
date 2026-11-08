/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ShowComposerDialog.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/collpane.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/choicdlg.h>
#include <wx/datetime.h>
#include <wx/dnd.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/dcclient.h>
#include <wx/utils.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "ComposerMediaDialogs.h"
#include "ComposerReviewDialogs.h"
#include "ComposerScreens.h"
#include "ComposerStoryboard.h"
#include "composer/VisualModel.h"
#include "ComposerTimeline.h"
#include "ai/WxServiceSettingsStore.h"
#include "composer/LyricDocument.h"
#include "composer/MusicAssist.h"
#include "composer/VisualDirection.h"
#include "composer/SequenceWriter.h"
#include "composer/ShowGenerationPipeline.h"
#include "media/AudioManager.h"
#include "sequencer/LyricSyncDialog.h"
#include "utils/ExternalHooks.h"
#include "xLightsMain.h"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace composer;

// Marks a msgid for ci_scripts/localization/build_catalog.py where _() cannot be used.
#define COMPOSER_TEXT(text) text

namespace {

enum {
    ID_STEPS = wxID_HIGHEST + 1,
    ID_BACK,
    ID_NEXT,
    ID_AUDIO,
    ID_SHOW,
    ID_SHOW_CURRENT,
    ID_LYRICS,
    ID_LYRICS_CLEAR,
    ID_TIMING,
    ID_TIMING_CLEAR,
    ID_MARK_LYRICS,
    ID_PREVIOUS,
    ID_SEED_NEW,
    ID_TEMPLATES,
    ID_ROLES,
    ID_ANALYZE,
    ID_PLAY,
    ID_PLAY_SELECTION,
    ID_FIRST_DOWNBEAT,
    ID_ADD_DOWNBEAT,
    ID_REMOVE_MARK,
    ID_SET_SECTION,
    ID_CLEAR_MUSIC,
    ID_RESYNC,
    ID_COMPOSE,
    ID_EXCLUDE,
    ID_INCLUDE,
    ID_LOCK,
    ID_UNLOCK,
    ID_GENERATE,
    ID_CANCEL,
    ID_OPEN_SEQUENCE,
    ID_OPEN_FOLDER,
    ID_OPEN_REPORT,
    ID_REVIEW_AGAIN,
    ID_ANOTHER,
    ID_APPROVE,
    ID_LRCLIB,
    ID_TRANSCRIBE,
    ID_DESCRIBE,
    ID_AI_KEYS,
    ID_MEDIA_LIBRARY,
    ID_VOICES,
    ID_MEDIA_ASSIGN,
    ID_MEDIA_REMOVE,
    ID_MEDIA_LOCK,
    ID_MEDIA_ANALYZE,
    ID_MEDIA_MODEL,
    ID_SCENE_LOCK,
    ID_SHOT_MOVE,
    ID_MONTAGE_REBUILD,
    ID_MONTAGE_VERSIONS,
    ID_SCREENS,
    ID_LANE_LOCK,
    ID_REVIEW_CLIPS,
    ID_LEARNED,
};

// Logs a handler of the window that kept it from answering for long: the window must
// stay responsive while the generator works.
struct SlowLog {
    const char* what;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit SlowLog(const char* w) : what(w) {}
    ~SlowLog() {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (ms > 300) {
            spdlog::info("Show Composer: {} took {} ms", what, ms);
        }
    }
};

constexpr int FRAME_MS = 25;

// The generation's stages as a person reads them, and the generator stage each follows.
struct StageRow {
    const char* label;
    const char* stage;
};
const StageRow STAGE_ROWS[] = {
    { COMPOSER_TEXT("Checking the inputs"), "inputs" },
    { COMPOSER_TEXT("Loading the installation"), "layout" },
    { COMPOSER_TEXT("Analyzing the song"), "analysis" },
    { COMPOSER_TEXT("Preparing the lyrics"), "lyrics" },
    { COMPOSER_TEXT("Synchronizing"), "lyrics" },
    { COMPOSER_TEXT("Building the composition"), "plan" },
    { COMPOSER_TEXT("Choreographing"), "choreography" },
    { COMPOSER_TEXT("Writing the sequence"), "write" },
    { COMPOSER_TEXT("Validating"), "write" },
    { COMPOSER_TEXT("Rendering"), "write" },
    { COMPOSER_TEXT("Publishing"), "publish" },
};
constexpr int STAGE_ROW_COUNT = static_cast<int>(std::size(STAGE_ROWS));

wxString Time(int ms) {
    if (ms < 0) {
        return "-";
    }
    return wxString::Format("%d:%02d.%d", ms / 60000, (ms / 1000) % 60, (ms % 1000) / 100);
}

bool HasExtension(const std::string& path, std::initializer_list<const char*> extensions) {
    std::string e = fs::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::any_of(extensions.begin(), extensions.end(), [&](const char* x) { return e == x; });
}

bool IsAudioFile(const std::string& path) {
    return HasExtension(path, { ".mp3", ".wav", ".m4a", ".ogg", ".flac", ".aac", ".wma", ".aiff", ".aif" });
}

wxString U(const std::string& s) {
    return wxString::FromUTF8(s);
}

std::string S(const wxString& s) {
    return s.ToStdString(wxConvUTF8);
}

class ComposerDropTarget : public wxFileDropTarget {
public:
    explicit ComposerDropTarget(ShowComposerDialog* dialog) :
        _dialog(dialog) {}

    wxDragResult OnEnter(wxCoord x, wxCoord y, wxDragResult def) override {
        _dialog->ShowDropState(true);
        return OnDragOver(x, y, def);
    }
    wxDragResult OnDragOver(wxCoord, wxCoord, wxDragResult def) override {
        return _dialog->AcceptsDrops() ? (def == wxDragMove ? wxDragCopy : def) : wxDragNone;
    }
    void OnLeave() override {
        _dialog->ShowDropState(false);
    }
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& names) override {
        _dialog->ShowDropState(false);
        std::vector<std::string> paths;
        for (const wxString& n : names) {
            paths.push_back(S(n));
        }
        // The program the files came from waits until this returns: loading the song
        // happens after, so neither of them freezes.
        _dialog->CallAfter([d = _dialog, paths]() { d->DropFiles(paths); });
        return true;
    }

private:
    ShowComposerDialog* _dialog;
};

wxString RoleText(Role role) {
    switch (role) {
    case Role::Matrix:
        return _("Matrix");
    case Role::MegaTree:
        return _("Mega tree");
    case Role::SmallTree:
        return _("Small tree");
    case Role::Window:
        return _("Window");
    case Role::Arch:
        return _("Arch");
    case Role::Line:
        return _("Line");
    case Role::Star:
        return _("Star");
    case Role::Face:
        return _("Singing face");
    case Role::Figure:
        return _("Figure");
    case Role::Group:
        return _("Group");
    case Role::Excluded:
        return _("Excluded");
    default:
        return _("Prop");
    }
}

wxString LevelText(AutomationLevel level) {
    switch (level) {
    case AutomationLevel::Conservative:
        return _("Conservative");
    case AutomationLevel::Creative:
        return _("Creative");
    default:
        return _("Balanced");
    }
}

// Why the choreography left a figure dark, for the person who has to decide what to do.
wxString NoteText(const std::string& code) {
    static const std::map<std::string, const char*> texts = {
        { "no-template", COMPOSER_TEXT("There is no technique yet for this kind of figure in this role.") },
        { "accents-without-downbeats", COMPOSER_TEXT("Its accents follow the strong beats, and none were found in this stretch: mark the bar in Analysis.") },
        { "accents-off-at-level", COMPOSER_TEXT("This automation level adds no accents.") },
        { "accents-do-not-fit", COMPOSER_TEXT("The strong beats here are too close for its accents.") },
        { "text-without-lyrics", COMPOSER_TEXT("It would show the lyrics, and there are none.") },
        { "text-without-phrase-in-segment", COMPOSER_TEXT("No synchronized phrase is sung in its stretch.") },
        { "face-without-synchronized-track", COMPOSER_TEXT("Its face would sing, and the lyrics are not synchronized.") },
        { "face-without-definition", COMPOSER_TEXT("It has no face definition to draw.") },
        { "face-without-phonemes-in-segment", COMPOSER_TEXT("Nothing is sung in its stretch.") },
        { "media-cue", COMPOSER_TEXT("A picture or video could go here; none is assigned yet.") },
    };
    const auto found = texts.find(code);
    return found != texts.end() ? wxGetTranslation(found->second) : wxString::Format(_("Reason: %s"), U(code));
}

wxString ConflictText(const std::string& reason) {
    if (reason.rfind("template-incompatible", 0) == 0) {
        return _("The technique chosen by hand does not suit this figure; the previous decision was kept.");
    }
    if (reason == "target-excluded") {
        return _("The figure is excluded, so a decision on it cannot be applied.");
    }
    if (reason.rfind("overlaps-manual", 0) == 0) {
        return _("Two decisions made by hand overlap on this figure; the first one was kept.");
    }
    if (reason == "target-missing") {
        return _("The figure no longer exists in this installation.");
    }
    if (reason == "no-event-in-range") {
        return _("There was nothing to change at that time.");
    }
    return wxString::Format(_("A decision could not be applied (%s)."), U(reason));
}

// The names of the themes that ship, so they read in the person's language; a theme
// added or renamed in themes.json shows its name as written.
const char* const THEME_NAMES[] = { COMPOSER_TEXT("Christmas, red and green"), COMPOSER_TEXT("Candy cane, red and white"), COMPOSER_TEXT("Winter night, blue and white"),
                                    COMPOSER_TEXT("Sunset and savanna, gold and green"), COMPOSER_TEXT("Party, magenta, lime and violet"), COMPOSER_TEXT("Magic, red and white with a rainbow"),
                                    COMPOSER_TEXT("Halloween, orange and violet"), COMPOSER_TEXT("New year, gold and white"), COMPOSER_TEXT("Calm, warm white and soft blue") };

wxString ThemeName(const std::string& name) {
    for (const char* known : THEME_NAMES) {
        if (name == known) {
            return wxGetTranslation(known);
        }
    }
    return U(name);
}

wxString ThemeSource(const std::string& source) {
    return source == "song-character" ? _("from the song's description") : source == "lyrics" ? _("from the words of the lyrics") : _("the profile's, as nothing else pointed to one");
}

// The visual direction in words, from visual-direction.json.
wxString DirectionText(const nlohmann::json& d) {
    const nlohmann::json& theme = d.value("interpretation", nlohmann::json::object());
    const std::string source = theme.value("source", std::string());
    wxString text = wxString::Format(_("Theme: %s, %s (confidence %.2f). It is an interpretation; the analysis is untouched."), ThemeName(theme.value("themeName", std::string())), ThemeSource(source), theme.value("confidence", 0.0));
    const std::vector<std::string> evidence = theme.value("evidence", std::vector<std::string>());
    if (source == "lyrics" && !evidence.empty()) {
        text += " " + wxString::Format(_("Words found: %s."), U(std::accumulate(evidence.begin(), evidence.end(), std::string(), [](const std::string& a, const std::string& b) { return a.empty() ? b : a + ", " + b; })));
    }
    static const std::map<std::string, const char*> FAMILIES = { { "outline", COMPOSER_TEXT("outline") }, { "windows", COMPOSER_TEXT("windows") }, { "arches", COMPOSER_TEXT("arches") },
                                                                 { "snowflakes", COMPOSER_TEXT("snowflakes") }, { "trees", COMPOSER_TEXT("small trees") }, { "stars", COMPOSER_TEXT("stars") },
                                                                 { "mega-tree", COMPOSER_TEXT("mega tree") }, { "props", COMPOSER_TEXT("props") }, { "figures", COMPOSER_TEXT("figures") },
                                                                 { "matrix", COMPOSER_TEXT("matrix") }, { "faces", COMPOSER_TEXT("faces") } };
    wxString families;
    for (const nlohmann::json& f : d.value("families", nlohmann::json::array())) {
        const auto name = FAMILIES.find(f.value("id", std::string()));
        const nlohmann::json targets = f.value("targets", nlohmann::json::array());
        const bool group = targets.size() == 1 && targets[0].value("type", std::string()) == "group";
        families += (families.IsEmpty() ? "" : ", ") + (name != FAMILIES.end() ? wxGetTranslation(name->second) : U(f.value("id", std::string()))) +
                    (group ? wxString::Format(" (%s)", U(targets[0].value("target", std::string()))) : wxString());
    }
    text += "\n" + wxString::Format(_("Families that move together: %s."), families);
    static const std::map<std::string, const char*> BANDS = { { "rest", COMPOSER_TEXT("pause") }, { "low", COMPOSER_TEXT("calm") }, { "mid", COMPOSER_TEXT("moving") }, { "high", COMPOSER_TEXT("intense") }, { "climax", COMPOSER_TEXT("climax") } };
    static const std::map<std::string, const char*> MOTIFS = { { "rest", COMPOSER_TEXT("dark") }, { "glow", COMPOSER_TEXT("soft glow") }, { "flow", COMPOSER_TEXT("flowing") }, { "drive", COMPOSER_TEXT("driving") }, { "burst", COMPOSER_TEXT("burst") } };
    static const std::map<std::string, const char*> KINDS = { { "home", COMPOSER_TEXT("home colours") }, { "contrast", COMPOSER_TEXT("contrast colours") }, { "climax", COMPOSER_TEXT("home colours and the accent") } };
    const auto word = [](const std::map<std::string, const char*>& m, const std::string& k) { const auto it = m.find(k); return it != m.end() ? wxGetTranslation(it->second) : U(k); };
    for (const nlohmann::json& s : d.value("scenes", nlohmann::json::array())) {
        const std::vector<std::string> colors = s.value("colors", std::vector<std::string>());
        text += "\n" + wxString::Format("%s-%s  %s, %s%s%s", Time(s.value("startMS", 0)), Time(s.value("endMS", 0)), word(BANDS, s.value("band", std::string())), word(MOTIFS, s.value("motif", std::string())),
                                        colors.empty() ? wxString() : ", " + word(KINDS, s.value("paletteKind", std::string())) + " ",
                                        U(std::accumulate(colors.begin(), colors.end(), std::string(), [](const std::string& a, const std::string& b) { return a.empty() ? b : a + " " + b; })));
    }
    return text;
}

// What went wrong asking a service, for the person who asked.
wxString AssistErrorText(const std::string& id, int retryAfterS) {
    if (id == "no-consent") {
        return _("Nothing was sent: the song is only sent when you agree.");
    }
    if (id == "no-credentials") {
        return _("There is no key for that service. Add one with \"AI Keys...\" or set GROQ_API_KEY / GEMINI_API_KEY.");
    }
    if (id == "file-too-large" || id == "too-large") {
        return _("The song is too large for that service. A shorter or more compressed file would fit.");
    }
    if (id == "unsupported-file") {
        return _("That service does not take this kind of audio file.");
    }
    if (id == "unauthorized") {
        return _("The service did not accept the key: it may be wrong or expired.");
    }
    if (id == "forbidden") {
        return _("The key cannot use that service or model.");
    }
    if (id == "rate-limited") {
        return retryAfterS > 0 ? wxString::Format(_("The service asks to wait %d seconds before trying again."), retryAfterS) : _("The service is busy: try again in a while.");
    }
    if (id == "server-error") {
        return _("The service failed on its side: try again later.");
    }
    if (id == "network") {
        return _("There is no connection to the service. Everything else keeps working without it.");
    }
    if (id == "not-found") {
        return _("LRCLIB has no lyrics for that song.");
    }
    return wxString::Format(_("The service gave an answer that could not be used (%s)."), U(id));
}

std::string SafeFileName(std::string s) {
    for (char& ch : s) {
        if (std::string("\\/:*?\"<>|").find(ch) != std::string::npos) {
            ch = '_';
        }
    }
    return s.size() > 60 ? s.substr(0, 60) : s;
}

wxString LyricIssueText(const LyricIssue& issue) {
    const wxString line = issue.line > 0 ? wxString::Format(_("line %d"), issue.line) : wxString();
    const wxString text = U(issue.text);
    if (issue.code == "malformed-time") {
        return wxString::Format(_("%s has a time that is not minutes:seconds (%s)."), line, text);
    }
    if (issue.code == "negative-time") {
        return wxString::Format(_("%s would start before the song (%s)."), line, text);
    }
    if (issue.code == "time-past-song-end") {
        return wxString::Format(_("\"%s\" starts at %s, after the end of the song: is it the right song?"), text, Time(issue.timeMS));
    }
    if (issue.code == "empty-file") {
        return _("the file is empty.");
    }
    if (issue.code == "no-timestamps") {
        return _("it is an LRC file without a single time.");
    }
    if (issue.code == "out-of-order") {
        return wxString::Format(_("%s was out of order and was moved to its time."), line);
    }
    if (issue.code == "duplicate-time") {
        return wxString::Format(_("%s had the same time as the line before and was joined to it."), line);
    }
    if (issue.code == "offset-applied") {
        return wxString::Format(_("the file's [offset:] of %s ms was applied."), text);
    }
    return U(DescribeLyricIssue(issue));
}

// What went wrong in the generator, worded for whoever has to fix it.
wxString ProblemText(const std::string& line) {
    static const std::vector<std::pair<const char*, const char*>> texts = {
        { "audio-missing", COMPOSER_TEXT("The song file does not exist. Choose it again.") },
        { "audio-unreadable", COMPOSER_TEXT("The song could not be read. Check that it is an audio file and that it is not damaged.") },
        { "audio-too-short", COMPOSER_TEXT("The song is too short to make a show from it.") },
        { "show-missing", COMPOSER_TEXT("The installation folder has no layout. Choose the show folder that contains your models.") },
        { "layout-unreadable", COMPOSER_TEXT("The installation could not be loaded. Open it in xLights and check it.") },
        { "output-exists", COMPOSER_TEXT("A sequence with that name already exists, and it is never replaced. Choose another name.") },
        { "sequence-name-invalid", COMPOSER_TEXT("The sequence name must be a file name ending in .xsq, without folders.") },
        { "lyrics-unreadable", COMPOSER_TEXT("The lyrics file could not be read.") },
        { "lyrics-invalid", COMPOSER_TEXT("The lyrics times are not valid. Check the marks.") },
        { "lyrics-times-invalid", COMPOSER_TEXT("Some times in the lyrics file cannot be used: a time that is not minutes:seconds, one before the song, or a line after its end. The first step says which.") },
        { "lyrics-empty-file", COMPOSER_TEXT("The lyrics file is empty.") },
        { "timing-invalid", COMPOSER_TEXT("The synchronization file is not a valid xLights timing file.") },
        { "timing-no-lyrics-track", COMPOSER_TEXT("The synchronization file has no lyrics track with phrases, words and phonemes.") },
        { "lyric-session-invalid", COMPOSER_TEXT("The saved lyrics marks could not be read.") },
        { "profile-invalid", COMPOSER_TEXT("The style profile could not be read.") },
        { "roles-invalid", COMPOSER_TEXT("The figure roles file could not be read.") },
        { "templates-empty", COMPOSER_TEXT("No technique could be loaded.") },
        { "plan-invalid", COMPOSER_TEXT("The composition did not pass its checks. See the technical report.") },
        { "choreography-invalid", COMPOSER_TEXT("The choreography did not pass its checks, often because of a decision made by hand. See the technical report.") },
        { "spec-invalid", COMPOSER_TEXT("The sequence would not look as planned, so it was not written. See the technical report.") },
        { "not-published", COMPOSER_TEXT("The sequence did not pass the checks after writing and rendering it, so nothing was saved. See the technical report.") },
        { "published-invalid", COMPOSER_TEXT("The saved sequence did not pass the checks when opened again. See the technical report.") },
        { "round-trip-changed", COMPOSER_TEXT("Saving the sequence again changed it. See the technical report.") },
        { "previous-sidecar", COMPOSER_TEXT("The earlier show's data could not be used.") },
        { "layout-modified", COMPOSER_TEXT("The installation files changed during the generation. Nothing of the Composer writes them: check whether another program did.") },
        { "cache-unwritable", COMPOSER_TEXT("The analysis could not be saved in the show folder.") },
        { "report-dir", COMPOSER_TEXT("The report folder could not be prepared.") },
    };
    for (const auto& [code, text] : texts) {
        if (line.find(code) != std::string::npos) {
            return wxGetTranslation(text);
        }
    }
    return wxString::Format(_("The generation stopped: %s"), U(line));
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

wxStaticText* Bold(wxWindow* parent, const wxString& text) {
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    wxFont font = label->GetFont();
    font.MakeBold();
    label->SetFont(font);
    return label;
}

// Long explanations wrap instead of setting the page's width.
constexpr int WRAP = 680;

wxStaticText* Muted(wxWindow* parent, const wxString& text) {
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    label->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    if (!text.IsEmpty()) {
        label->Wrap(WRAP);
    }
    return label;
}

void SetWrapped(wxStaticText* label, const wxString& text) {
    label->SetLabel(text);
    label->Wrap(WRAP);
}

} // namespace

ShowComposerDialog::ShowComposerDialog(xLightsFrame* frame) :
    wxDialog(frame, wxID_ANY, _("Show Composer"), wxDefaultPosition, wxSize(1180, 780), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX),
    _frame(frame),
    _job([this](const ComposerJob::Update& u) { OnJob(u); }),
    _assistJob([this](const ComposerJob::Update& u) { OnAssistJob(u); }),
    _timer(this) {
    _showDir = _frame->GetShowDirectory();
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    _workDir = (fs::path(S(wxFileName::GetTempDir())) / "xLights-ShowComposer" / std::to_string(stamp)).string();
    BuildControls();
    LoadShowInfo();
    RefreshStyle();
    LoadLibrary();
    Bind(wxEVT_TIMER, &ShowComposerDialog::OnTimer, this);
    Bind(wxEVT_CLOSE_WINDOW, &ShowComposerDialog::OnClose, this);
    _timer.Start(100);
    RefreshAll();
    CentreOnParent();
    if (OpenedHook()) {
        OpenedHook()(this);
    }
}

std::function<void(ShowComposerDialog*)>& ShowComposerDialog::OpenedHook() {
    static std::function<void(ShowComposerDialog*)> hook;
    return hook;
}

void ShowComposerDialog::OpenResult() {
    if (_resultSequence.empty() || _job.Running()) {
        return;
    }
    _openSequence = _resultSequence;
    _openShowDir = _showDir;
    EndModal(wxID_OK);
}

void ShowComposerDialog::AddMusicBarAt(int ms, int beatsPerBar) {
    _corrections.analysis.bar.firstDownbeatMS = ms;
    _corrections.analysis.bar.beatsPerBar = beatsPerBar;
    _corrections.analysis.bar.locked = true;
    ApplyMusicCorrectionsLocally();
}

ShowComposerDialog::~ShowComposerDialog() {
    _timer.Stop();
    if (_audio != nullptr && _audio->GetPlayingState() != MEDIAPLAYINGSTATE::STOPPED) {
        _audio->Stop();
    }
}

// ------------------------------------------------------------------ layout

void ShowComposerDialog::BuildControls() {
    auto* main = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxBoxSizer(wxVERTICAL);
    wxStaticText* heading = Bold(this, _("Show Composer"));
    wxFont big = heading->GetFont();
    big.SetPointSize(big.GetPointSize() + 5);
    heading->SetFont(big);
    title->Add(heading, 0);
    title->Add(Muted(this, _("Make a show from a song, its lyrics and your installation, review it and open it in xLights.")), 0, wxTOP, 2);
    main->Add(title, 0, wxALL, 12);

    auto* body = new wxBoxSizer(wxHORIZONTAL);
    _steps = new wxListBox(this, ID_STEPS, wxDefaultPosition, wxSize(230, -1));
    for (const wxString& s : { _("1. Create the show"), _("2. Analysis and synchronization"), _("3. Review the composition"), _("4. Generate the show"), _("5. Review the result") }) {
        _steps->Append(s);
    }
    wxFont stepFont = _steps->GetFont();
    stepFont.SetPointSize(stepFont.GetPointSize() + 1);
    _steps->SetFont(stepFont);
    body->Add(_steps, 0, wxEXPAND | wxLEFT | wxBOTTOM, 12);

    _book = new wxSimplebook(this, wxID_ANY);
    _book->AddPage(BuildCreatePage(_book), "");
    _book->AddPage(BuildAnalysisPage(_book), "");
    _book->AddPage(BuildCompositionPage(_book), "");
    _book->AddPage(BuildGeneratePage(_book), "");
    _book->AddPage(BuildResultPage(_book), "");
    body->Add(_book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    main->Add(body, 1, wxEXPAND);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    _status = new wxStaticText(this, wxID_ANY, "");
    buttons->Add(_status, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    _back = new wxButton(this, ID_BACK, _("< Back"));
    _next = new wxButton(this, ID_NEXT, _("Next >"));
    _close = new wxButton(this, wxID_CLOSE, _("Close"));
    buttons->Add(_back, 0, wxRIGHT, 6);
    buttons->Add(_next, 0, wxRIGHT, 18);
    buttons->Add(_close, 0, wxRIGHT, 12);
    main->Add(buttons, 0, wxEXPAND | wxBOTTOM, 12);
    SetSizer(main);
    SetMinSize(wxSize(980, 640));

    Bind(wxEVT_LISTBOX, &ShowComposerDialog::OnStepSelected, this, ID_STEPS);
    Bind(wxEVT_BUTTON, &ShowComposerDialog::OnBack, this, ID_BACK);
    Bind(wxEVT_BUTTON, &ShowComposerDialog::OnNext, this, ID_NEXT);
    Bind(wxEVT_BUTTON, &ShowComposerDialog::OnCloseButton, this, wxID_CLOSE);
}

wxWindow* ShowComposerDialog::BuildCreatePage(wxWindow* parent) {
    auto* page = new wxScrolledWindow(parent);
    page->SetScrollRate(0, 10);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* intro = new wxBoxSizer(wxHORIZONTAL);
    intro->Add(Bold(page, _("Create the show")), 1, wxALIGN_CENTER_VERTICAL);
    intro->Add(new wxButton(page, ID_PREVIOUS, _("Open a Show Made Earlier...")), 0);
    sizer->Add(intro, 0, wxEXPAND | wxBOTTOM, 8);

    const auto box = [&](const wxString& label) {
        auto* b = new wxStaticBoxSizer(wxVERTICAL, page, label);
        sizer->Add(b, 0, wxEXPAND | wxBOTTOM, 10);
        return b;
    };
    const auto row = [&](wxStaticBoxSizer* b, wxStaticText*& value, int button, const wxString& buttonLabel, int second = 0, const wxString& secondLabel = "") {
        auto* r = new wxBoxSizer(wxHORIZONTAL);
        value = new wxStaticText(b->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, wxSize(120, -1), wxST_ELLIPSIZE_MIDDLE);
        r->Add(value, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        r->Add(new wxButton(b->GetStaticBox(), button, buttonLabel), 0, wxRIGHT, 4);
        if (second != 0) {
            r->Add(new wxButton(b->GetStaticBox(), second, secondLabel), 0);
        }
        b->Add(r, 0, wxEXPAND | wxALL, 4);
    };

    wxStaticBoxSizer* song = box(_("Song"));
    row(song, _audioName, ID_AUDIO, _("Choose Song..."));
    _audioInfo = Muted(song->GetStaticBox(), "");
    song->Add(_audioInfo, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
    _dropZone = new wxPanel(song->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    auto* dropSizer = new wxBoxSizer(wxVERTICAL);
    _dropLabel = new wxStaticText(_dropZone, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL | wxST_NO_AUTORESIZE);
    dropSizer->Add(_dropLabel, 0, wxEXPAND | wxALL, 14);
    _dropZone->SetSizer(dropSizer);
    song->Add(_dropZone, 0, wxEXPAND | wxALL, 4);
    ShowDropState(false);
    page->SetDropTarget(new ComposerDropTarget(this));

    wxStaticBoxSizer* show = box(_("Installation"));
    row(show, _showName, ID_SHOW, _("Choose Another Show Folder..."), ID_SHOW_CURRENT, _("Use the Open Show"));
    _showInfo = Muted(show->GetStaticBox(), "");
    show->Add(_showInfo, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

    wxStaticBoxSizer* lyrics = box(_("Lyrics"));
    row(lyrics, _lyricsName, ID_LYRICS, _("Choose Lyrics..."), ID_LYRICS_CLEAR, _("No Lyrics"));
    row(lyrics, _timingName, ID_TIMING, _("Choose Synchronization..."), ID_TIMING_CLEAR, _("No Synchronization"));
    auto* mark = new wxBoxSizer(wxHORIZONTAL);
    mark->Add(new wxButton(lyrics->GetStaticBox(), ID_MARK_LYRICS, _("Mark the Lyrics by Ear...")), 0, wxRIGHT, 8);
    mark->Add(Muted(lyrics->GetStaticBox(), _("Plain text, LRC or SRT lyrics; an xLights timing file (.xtiming) with a lyrics track wins over any other time.")), 1, wxALIGN_CENTER_VERTICAL);
    lyrics->Add(mark, 0, wxEXPAND | wxALL, 4);
    _lyricsInfo = new wxStaticText(lyrics->GetStaticBox(), wxID_ANY, "");
    lyrics->Add(_lyricsInfo, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
    auto* assist = new wxBoxSizer(wxHORIZONTAL);
    assist->Add(new wxButton(lyrics->GetStaticBox(), ID_LRCLIB, _("Find the Lyrics Online (LRCLIB)...")), 0, wxRIGHT, 4);
    assist->Add(new wxButton(lyrics->GetStaticBox(), ID_TRANSCRIBE, _("Time the Words with AI (Groq)...")), 0, wxRIGHT, 4);
    assist->Add(new wxButton(lyrics->GetStaticBox(), ID_AI_KEYS, _("AI Keys...")), 0);
    lyrics->Add(assist, 0, wxALL, 4);
    _assistInfo = Muted(lyrics->GetStaticBox(), _("Optional: lyrics from LRCLIB are only read, never published; a song is only sent to an AI service after you say yes, and its answer is kept with the show so it works offline afterwards."));
    lyrics->Add(_assistInfo, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

    wxStaticBoxSizer* style = box(_("Style and automation"));
    auto* styleRow = new wxBoxSizer(wxHORIZONTAL);
    styleRow->Add(new wxStaticText(style->GetStaticBox(), wxID_ANY, _("Style profile:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _profile = new wxChoice(style->GetStaticBox(), wxID_ANY);
    styleRow->Add(_profile, 1, wxRIGHT, 16);
    styleRow->Add(new wxStaticText(style->GetStaticBox(), wxID_ANY, _("Seed:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _seed = new wxSpinCtrl(style->GetStaticBox(), wxID_ANY, "1", wxDefaultPosition, wxSize(110, -1), wxSP_ARROW_KEYS, 1, 999999, 1);
    styleRow->Add(_seed, 0, wxRIGHT, 4);
    styleRow->Add(new wxButton(style->GetStaticBox(), ID_SEED_NEW, _("Another")), 0);
    style->Add(styleRow, 0, wxEXPAND | wxALL, 4);
    const wxString levels[] = { _("Conservative: few figures, calm changes"), _("Balanced"), _("Creative: more figures, more variety and accents") };
    _level = new wxRadioBox(style->GetStaticBox(), wxID_ANY, _("Automation level"), wxDefaultPosition, wxDefaultSize, 3, levels, 1, wxRA_SPECIFY_COLS);
    _level->SetSelection(1);
    style->Add(_level, 0, wxEXPAND | wxALL, 4);
    style->Add(Muted(style->GetStaticBox(), _("The same song, installation, lyrics, level and seed always give the same show.")), 0, wxLEFT | wxBOTTOM, 6);
    auto* look = new wxBoxSizer(wxHORIZONTAL);
    _swatch = new wxPanel(style->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxSize(220, 26), wxBORDER_SIMPLE);
    _swatch->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(_swatch);
        const wxSize size = _swatch->GetClientSize();
        if (_styleColors.empty()) {
            dc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, size.x, size.y);
            return;
        }
        const int w = std::max(1, size.x / static_cast<int>(_styleColors.size()));
        for (size_t i = 0; i < _styleColors.size(); ++i) {
            const wxColour colour(U(_styleColors[i]));
            dc.SetBrush(wxBrush(colour));
            dc.SetPen(wxPen(colour));
            dc.DrawRectangle(static_cast<int>(i) * w, 0, i + 1 == _styleColors.size() ? size.x - static_cast<int>(i) * w : w, size.y);
        }
    });
    look->Add(_swatch, 0, wxRIGHT, 8);
    _styleInfo = new wxStaticText(style->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, wxSize(120, -1));
    look->Add(_styleInfo, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    look->Add(new wxButton(style->GetStaticBox(), ID_DESCRIBE, _("Describe the Song with AI (Gemini)...")), 0, wxALIGN_CENTER_VERTICAL);
    style->Add(look, 0, wxEXPAND | wxALL, 4);

    wxStaticBoxSizer* mediaBox = box(_("Pictures and videos"));
    auto* mediaRow = new wxBoxSizer(wxHORIZONTAL);
    _mediaInfo = new wxStaticText(mediaBox->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, wxSize(120, -1), wxST_ELLIPSIZE_END);
    mediaRow->Add(_mediaInfo, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    mediaRow->Add(new wxButton(mediaBox->GetStaticBox(), ID_MEDIA_LIBRARY, _("Pictures and Videos...")), 0);
    mediaBox->Add(mediaRow, 0, wxEXPAND | wxALL, 4);
    auto* screensRow = new wxBoxSizer(wxHORIZONTAL);
    _screensInfo = new wxStaticText(mediaBox->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, wxSize(120, -1), wxST_ELLIPSIZE_END);
    screensRow->Add(_screensInfo, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    screensRow->Add(new wxButton(mediaBox->GetStaticBox(), ID_SCREENS, _("Screens...")), 0);
    mediaBox->Add(screensRow, 0, wxEXPAND | wxALL, 4);
    _mediaAuto = new wxCheckBox(mediaBox->GetStaticBox(), wxID_ANY, _("Choose pictures and videos for the show by themselves (otherwise only the ones I assign)"));
    _mediaAuto->SetValue(true);
    mediaBox->Add(_mediaAuto, 0, wxALL, 4);
    _copyMedia = new wxCheckBox(mediaBox->GetStaticBox(), wxID_ANY, _("Copy the files that are outside the show folder into it (Images, Videos), so the show can be moved"));
    mediaBox->Add(_copyMedia, 0, wxALL, 4);
    auto* analysisRow = new wxBoxSizer(wxHORIZONTAL);
    _analyzeButton = new wxButton(mediaBox->GetStaticBox(), ID_MEDIA_ANALYZE, _("Analyze the Library"));
    analysisRow->Add(_analyzeButton, 0, wxRIGHT, 4);
    _modelButton = new wxButton(mediaBox->GetStaticBox(), ID_MEDIA_MODEL, _("Get the Image Model..."));
    analysisRow->Add(_modelButton, 0, wxRIGHT, 8);
    _analysisState = new wxStaticText(mediaBox->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, wxSize(120, -1), wxST_ELLIPSIZE_END);
    analysisRow->Add(_analysisState, 1, wxALIGN_CENTER_VERTICAL);
    mediaBox->Add(analysisRow, 0, wxEXPAND | wxALL, 4);
    mediaBox->Add(Muted(mediaBox->GetStaticBox(), _("Only your own files are used, on the screens of the show (\"Screens...\"); a picture never covers a sung phrase, and the montage is planned for you. Originals are never moved or changed, and nothing is sent anywhere.")), 0, wxLEFT | wxBOTTOM, 6);

    wxStaticBoxSizer* output = box(_("Sequence"));
    auto* nameRow = new wxBoxSizer(wxHORIZONTAL);
    nameRow->Add(new wxStaticText(output->GetStaticBox(), wxID_ANY, _("Name:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _name = new wxTextCtrl(output->GetStaticBox(), wxID_ANY);
    nameRow->Add(_name, 1);
    output->Add(nameRow, 0, wxEXPAND | wxALL, 4);
    _nameInfo = Muted(output->GetStaticBox(), "");
    output->Add(_nameInfo, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

    _advanced = new wxCollapsiblePane(page, wxID_ANY, _("Advanced options"));
    wxWindow* pane = _advanced->GetPane();
    auto* adv = new wxBoxSizer(wxVERTICAL);
    const auto advRow = [&](wxStaticText*& value, int button, const wxString& label) {
        auto* r = new wxBoxSizer(wxHORIZONTAL);
        value = new wxStaticText(pane, wxID_ANY, "", wxDefaultPosition, wxSize(120, -1), wxST_ELLIPSIZE_MIDDLE);
        r->Add(value, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        r->Add(new wxButton(pane, button, label), 0);
        adv->Add(r, 0, wxEXPAND | wxALL, 4);
    };
    advRow(_templatesName, ID_TEMPLATES, _("Techniques Folder..."));
    advRow(_rolesName, ID_ROLES, _("Figure Roles File..."));
    _render = new wxCheckBox(pane, wxID_ANY, _("Render and check the sequence before saving it (recommended)"));
    _render->SetValue(true);
    adv->Add(_render, 0, wxALL, 4);
    pane->SetSizer(adv);
    sizer->Add(_advanced, 0, wxEXPAND | wxBOTTOM, 10);
    page->SetSizer(sizer);

    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dialog(this, _("Choose the Song"), U(_audioPath.empty() ? _showDir : fs::path(_audioPath).parent_path().string()), "",
                            _("Audio files (*.mp3;*.wav;*.m4a;*.ogg;*.flac;*.aac;*.wma;*.aiff)|*.mp3;*.wav;*.m4a;*.ogg;*.flac;*.aac;*.wma;*.aiff;*.aif|All files (*.*)|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            SetAudio(S(dialog.GetPath()));
        }
    }, ID_AUDIO);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxDirDialog dialog(this, _("Choose the Show Folder"), U(_showDir), wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            SetShowFolder(S(dialog.GetPath()));
        }
    }, ID_SHOW);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SetShowFolder(_frame->GetShowDirectory()); }, ID_SHOW_CURRENT);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dialog(this, _("Choose the Lyrics"), U(_showDir), "", _("Lyrics (*.txt;*.lrc;*.srt)|*.txt;*.lrc;*.srt|All files (*.*)|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            SetLyrics(S(dialog.GetPath()));
        }
    }, ID_LYRICS);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SetLyrics(""); }, ID_LYRICS_CLEAR);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!_audioOk) {
            ShowError(_("Choose a song that can be played first."), _("The search uses its name and its length."));
            return;
        }
        const LrclibQuery guess = GuessLrclibQuery(_audioPath, _durationMS / 1000.0);
        wxString query = U(guess.artist.empty() ? guess.track : guess.artist + " - " + guess.track);
        if (!_testMode) {
            wxTextEntryDialog ask(this, _("Artist and title, as \"Artist - Title\". Both orders are searched. Nothing is published to LRCLIB."), _("Find the Lyrics Online"), query);
            if (ask.ShowModal() != wxID_OK) {
                return;
            }
            query = ask.GetValue();
        }
        StartAssist("lrclib", false, S(query));
    }, ID_LRCLIB);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!_audioOk) {
            ShowError(_("Choose a song that can be played first."), wxString());
            return;
        }
        StartAssist("transcribe", AskConsent("Groq (api.groq.com)", _("to hear which word is sung when; the lyrics you gave stay the text")));
    }, ID_TRANSCRIBE);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!_audioOk) {
            ShowError(_("Choose a song that can be played first."), wxString());
            return;
        }
        StartAssist("describe", AskConsent("Google Gemini (generativelanguage.googleapis.com)", _("to describe its genre, mood and energy; it is only a suggestion and never changes the analysis")));
    }, ID_DESCRIBE);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditKeys(); }, ID_AI_KEYS);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditScreens(); }, ID_SCREENS);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditMediaLibrary(); }, ID_MEDIA_LIBRARY);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (MediaAnalysisRunning()) {
            _analysisRunner->Cancel();
        } else {
            StartMediaAnalysis();
        }
    }, ID_MEDIA_ANALYZE);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { GetVisualModel(); }, ID_MEDIA_MODEL);
    _mediaAuto->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { RefreshAll(); });
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dialog(this, _("Choose the Synchronization"), U(_showDir), "", _("xLights timing (*.xtiming)|*.xtiming"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            SetTiming(S(dialog.GetPath()));
        }
    }, ID_TIMING);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SetTiming(""); }, ID_TIMING_CLEAR);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!_audioOk) {
            ShowError(_("Choose a song that can be played first."), _("The phrases are marked while the song plays."));
            return;
        }
        std::string lyrics;
        if (!_lyricsPath.empty()) {
            lyrics = ReadFile(fs::path(_lyricsPath));
        }
        if (_sessionPath.empty()) {
            _sessionPath = NewWorkFolder("marks") + "/lyric-session.json";
        }
        LyricSyncDialog dialog(this, _audio.get(), _durationMS, FRAME_MS, _sessionPath, lyrics, "Composer Lyrics");
        dialog.ShowModal();
        LoadLyricsInfo();
        RefreshAll();
    }, ID_MARK_LYRICS);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dialog(this, _("Open a Show Made Earlier"), U(_showDir), "", _("Show Composer data (*.composer.json)|*.composer.json"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }
        std::string error;
        if (!LoadPreviousShow(S(dialog.GetPath()), error)) {
            ShowError(_("That show could not be opened."), U(error));
        }
    }, ID_PREVIOUS);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        _seed->SetValue(static_cast<int>(wxGetUTCTimeMillis().GetValue() % 999999) + 1);
        RefreshAll();
    }, ID_SEED_NEW);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxDirDialog dialog(this, _("Choose the Techniques Folder"), U(_templatesDir.empty() ? DefaultTemplateDirectory() : _templatesDir), wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            _templatesDir = S(dialog.GetPath());
            LoadLibrary();
            RefreshAll();
        }
    }, ID_TEMPLATES);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dialog(this, _("Choose the Figure Roles File"), U(_showDir), "roles.json", _("Roles (*.json)|*.json"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            _rolesPath = S(dialog.GetPath());
            RefreshAll();
        }
    }, ID_ROLES);
    _name->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        _sequenceName = S(_name->GetValue());
        RefreshCreate();
        RefreshSteps();
    });
    _level->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent&) { RefreshAll(); });
    _seed->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { RefreshAll(); });
    _profile->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int i = _profile->GetSelection();
        _profilePath = i > 0 && i < static_cast<int>(_profilePaths.size()) ? _profilePaths[i] : "";
        RefreshStyle();
        RefreshAll();
    });
    _render->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { _renderCheck = _render->GetValue(); });
    _advanced->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [page](wxCollapsiblePaneEvent&) { page->FitInside(); page->Layout(); });
    return page;
}

wxWindow* ShowComposerDialog::BuildAnalysisPage(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    head->Add(Bold(page, _("Analysis and synchronization")), 1, wxALIGN_CENTER_VERTICAL);
    head->Add(new wxButton(page, ID_ANALYZE, _("Analyze the Song")), 0, wxRIGHT, 4);
    head->Add(new wxButton(page, ID_RESYNC, _("Synchronize Again")), 0);
    sizer->Add(head, 0, wxEXPAND | wxBOTTOM, 6);
    _analysisInfo = new wxStaticText(page, wxID_ANY, "");
    sizer->Add(_analysisInfo, 0, wxEXPAND | wxBOTTOM, 4);
    _lyricsSync = new wxStaticText(page, wxID_ANY, "");
    sizer->Add(_lyricsSync, 0, wxEXPAND | wxBOTTOM, 2);
    _alignmentInfo = new wxStaticText(page, wxID_ANY, "");
    sizer->Add(_alignmentInfo, 0, wxEXPAND | wxBOTTOM, 4);
    auto* voicesRow = new wxBoxSizer(wxHORIZONTAL);
    voicesRow->Add(new wxButton(page, ID_VOICES, _("Voices...")), 0, wxRIGHT, 8);
    _voicesInfo = new wxStaticText(page, wxID_ANY, "");
    voicesRow->Add(_voicesInfo, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(voicesRow, 0, wxEXPAND | wxBOTTOM, 6);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditVoices(); }, ID_VOICES);

    auto* play = new wxBoxSizer(wxHORIZONTAL);
    play->Add(new wxButton(page, ID_PLAY, _("Play / Pause")), 0, wxRIGHT, 4);
    play->Add(new wxButton(page, ID_PLAY_SELECTION, _("Play the Selection")), 0, wxRIGHT, 12);
    play->Add(new wxStaticText(page, wxID_ANY, _("Zoom:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    _zoom = new wxSlider(page, wxID_ANY, 40, 4, 400, wxDefaultPosition, wxSize(180, -1));
    play->Add(_zoom, 0, wxRIGHT, 12);
    _cursorInfo = new wxStaticText(page, wxID_ANY, "");
    play->Add(_cursorInfo, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(play, 0, wxEXPAND | wxBOTTOM, 6);

    _timeline = new ComposerTimeline(page);
    sizer->Add(_timeline, 0, wxEXPAND | wxBOTTOM, 4);
    sizer->Add(Muted(page, _("From the top: time, sections, waveform with silences and climax, beats (strong beats in red, marked by hand in green), onsets, and the lyrics' phrases, words and mouth shapes. Click to place the cursor, drag to select, Ctrl+wheel to zoom.")),
               0, wxEXPAND | wxBOTTOM, 10);

    auto* fix = new wxStaticBoxSizer(wxVERTICAL, page, _("Correct the music"));
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    bar->Add(new wxButton(fix->GetStaticBox(), ID_FIRST_DOWNBEAT, _("The Bar Starts at the Cursor")), 0, wxRIGHT, 4);
    bar->Add(new wxStaticText(fix->GetStaticBox(), wxID_ANY, _("Beats per bar:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    _beatsPerBar = new wxChoice(fix->GetStaticBox(), wxID_ANY);
    for (const char* n : { "2", "3", "4", "6" }) {
        _beatsPerBar->Append(n);
    }
    _beatsPerBar->SetSelection(2);
    bar->Add(_beatsPerBar, 0, wxRIGHT, 12);
    bar->Add(new wxButton(fix->GetStaticBox(), ID_ADD_DOWNBEAT, _("Add a Strong Beat at the Cursor")), 0, wxRIGHT, 4);
    bar->Add(new wxButton(fix->GetStaticBox(), ID_REMOVE_MARK, _("Remove the Nearest Beat")), 0);
    fix->Add(bar, 0, wxALL, 4);
    auto* section = new wxBoxSizer(wxHORIZONTAL);
    section->Add(new wxStaticText(fix->GetStaticBox(), wxID_ANY, _("The section at the cursor is:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    _sectionKind = new wxChoice(fix->GetStaticBox(), wxID_ANY);
    for (const wxString& k : { _("Intro"), _("Verse"), _("Chorus"), _("Bridge"), _("Instrumental"), _("Outro") }) {
        _sectionKind->Append(k);
    }
    _sectionKind->SetSelection(2);
    section->Add(_sectionKind, 0, wxRIGHT, 4);
    section->Add(new wxButton(fix->GetStaticBox(), ID_SET_SECTION, _("Change It")), 0, wxRIGHT, 12);
    section->Add(new wxButton(fix->GetStaticBox(), ID_CLEAR_MUSIC, _("Undo All Music Corrections")), 0);
    fix->Add(section, 0, wxALL, 4);
    _musicFixes = Muted(fix->GetStaticBox(), "");
    fix->Add(_musicFixes, 0, wxALL, 4);
    sizer->Add(fix, 0, wxEXPAND | wxBOTTOM, 8);
    auto* lyricFix = new wxBoxSizer(wxHORIZONTAL);
    lyricFix->Add(new wxButton(page, ID_MARK_LYRICS, _("Correct the Phrases by Ear...")), 0, wxRIGHT, 8);
    lyricFix->Add(Muted(page, _("Marks made by hand win over any computed time and are kept for every new version.")), 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(lyricFix, 0, wxEXPAND);
    page->SetSizer(sizer);

    _timeline->SetCursorListener([this](int) { RefreshAnalysis(); });
    _zoom->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { _timeline->SetPixelsPerSecond(_zoom->GetValue()); });
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartAnalysis(); }, ID_ANALYZE);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartAnalysis(); }, ID_RESYNC);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (_audio == nullptr || !_audioOk) {
            return;
        }
        if (_audio->GetPlayingState() == MEDIAPLAYINGSTATE::PLAYING) {
            _audio->Pause();
        } else {
            _audio->Seek(_timeline->Cursor());
            _audio->Play();
        }
    }, ID_PLAY);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (_audio == nullptr || !_audioOk) {
            return;
        }
        const int start = _timeline->HasSelection() ? _timeline->SelectionStart() : _timeline->Cursor();
        const int length = _timeline->HasSelection() ? _timeline->SelectionEnd() - start : 5000;
        _audio->Stop();
        _audio->Play(start, length);
    }, ID_PLAY_SELECTION);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        _corrections.analysis.bar.firstDownbeatMS = _timeline->Cursor();
        _corrections.analysis.bar.beatsPerBar = static_cast<int>(std::strtol(S(_beatsPerBar->GetStringSelection()).c_str(), nullptr, 10));
        _corrections.analysis.bar.locked = true;
        ApplyMusicCorrectionsLocally();
    }, ID_FIRST_DOWNBEAT);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        EventCorrection add;
        add.action = EventCorrection::Action::Add;
        add.type = MusicalEventType::Downbeat;
        add.timeMS = _timeline->Cursor() - _timeline->Cursor() % FRAME_MS;
        _corrections.analysis.events.push_back(add);
        ApplyMusicCorrectionsLocally();
    }, ID_ADD_DOWNBEAT);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const MusicalEvent* nearest = nullptr;
        for (const MusicalEvent& e : _shownAnalysis.events) {
            if ((e.type == MusicalEventType::Beat || e.type == MusicalEventType::Downbeat) && (nearest == nullptr || std::abs(e.timeMS - _timeline->Cursor()) < std::abs(nearest->timeMS - _timeline->Cursor()))) {
                nearest = &e;
            }
        }
        if (nearest == nullptr) {
            return;
        }
        if (nearest->provenance != Provenance::Detected) {
            // One added here is simply taken back.
            auto& events = _corrections.analysis.events;
            events.erase(std::remove_if(events.begin(), events.end(), [&](const EventCorrection& c) { return c.action == EventCorrection::Action::Add && c.timeMS == nearest->timeMS; }), events.end());
        } else {
            EventCorrection remove;
            remove.action = EventCorrection::Action::Remove;
            remove.type = nearest->type;
            remove.originalMS = nearest->timeMS;
            _corrections.analysis.events.push_back(remove);
        }
        ApplyMusicCorrectionsLocally();
    }, ID_REMOVE_MARK);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        for (const SongSection& s : _shownAnalysis.sections) {
            if (_timeline->Cursor() >= s.startMS && _timeline->Cursor() < s.endMS) {
                const SectionKind kinds[] = { SectionKind::Intro, SectionKind::Verse, SectionKind::Chorus, SectionKind::Bridge, SectionKind::Instrumental, SectionKind::Outro };
                _corrections.analysis.sections.push_back({ s.startMS, s.endMS, kinds[std::max(0, _sectionKind->GetSelection())], true });
                ApplyMusicCorrectionsLocally();
                return;
            }
        }
    }, ID_SET_SECTION);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        _corrections.analysis = AnalysisCorrections();
        ApplyMusicCorrectionsLocally();
    }, ID_CLEAR_MUSIC);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        // The same marking dialog as on the first page.
        wxCommandEvent forward(wxEVT_BUTTON, ID_MARK_LYRICS);
        _book->GetPage(0)->GetEventHandler()->ProcessEvent(forward);
        e.Skip(false);
    }, ID_MARK_LYRICS);
    return page;
}

wxWindow* ShowComposerDialog::BuildCompositionPage(wxWindow* parent) {
    // It scrolls: the montage has a row per screen, however many the show has.
    auto* page = new wxScrolledWindow(parent);
    page->SetScrollRate(0, 10);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    head->Add(Bold(page, _("Review the composition")), 1, wxALIGN_CENTER_VERTICAL);
    head->Add(new wxButton(page, ID_COMPOSE, _("Work Out the Composition")), 0);
    sizer->Add(head, 0, wxEXPAND | wxBOTTOM, 6);
    _compositionInfo = new wxStaticText(page, wxID_ANY, "");
    sizer->Add(_compositionInfo, 0, wxEXPAND | wxBOTTOM, 6);

    auto* art = new wxStaticBoxSizer(wxVERTICAL, page, _("Art direction: theme, colours and scenes"));
    _sceneView = new wxPanel(art->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxSize(-1, 30), wxBORDER_SIMPLE);
    _sceneView->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(_sceneView);
        const wxSize size = _sceneView->GetClientSize();
        dc.SetBrush(wxBrush(wxColour(16, 16, 20)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, size.x, size.y);
        if (_sceneStripEnd <= 0) {
            return;
        }
        for (size_t i = 0; i < _sceneStrip.size(); ++i) {
            const int from = _sceneStrip[i].first;
            const int to = i + 1 < _sceneStrip.size() ? _sceneStrip[i + 1].first : _sceneStripEnd;
            const int x0 = static_cast<int>(static_cast<long long>(from) * size.x / _sceneStripEnd);
            const int x1 = static_cast<int>(static_cast<long long>(to) * size.x / _sceneStripEnd);
            const auto& colors = _sceneStrip[i].second;
            for (size_t k = 0; k < colors.size(); ++k) {
                const wxColour colour(U(colors[k]));
                dc.SetBrush(wxBrush(colour));
                const int y0 = static_cast<int>(size.y * k / colors.size());
                const int y1 = static_cast<int>(size.y * (k + 1) / colors.size());
                dc.DrawRectangle(x0, y0, std::max(1, x1 - x0 - 1), y1 - y0);
            }
        }
    });
    art->Add(_sceneView, 0, wxEXPAND | wxALL, 4);
    _directionText = new wxTextCtrl(art->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, wxSize(-1, 90), wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    art->Add(_directionText, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    sizer->Add(art, 0, wxEXPAND | wxBOTTOM, 8);

    _levelList = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxSize(-1, 96), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("Level"), 120 }, { _("Effects"), 65 }, { _("Figures lit"), 125 }, { _("In the plan"), 85 }, { _("Accents"), 70 }, { _("Techniques"), 80 }, { _("Faces singing"), 105 }, { _("Text"), 55 }, { _("Mean intensity"), 115 }, { _("To review"), 90 } }) {
        _levelList->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    sizer->Add(_levelList, 0, wxEXPAND | wxBOTTOM, 8);

    auto* figures = new wxBoxSizer(wxHORIZONTAL);
    auto* left = new wxBoxSizer(wxVERTICAL);
    auto* filter = new wxBoxSizer(wxHORIZONTAL);
    filter->Add(new wxStaticText(page, wxID_ANY, _("Show:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    _figureFilter = new wxChoice(page, wxID_ANY);
    for (const wxString& f : { _("Every figure in the show"), _("Leading figures"), _("Accompanying figures"), _("Accent figures"), _("Groups"), _("Single models"), _("Singing faces"), _("Matrices showing lyrics"), _("Excluded figures"), _("Planned but left dark") }) {
        _figureFilter->Append(f);
    }
    _figureFilter->SetSelection(0);
    filter->Add(_figureFilter, 1);
    left->Add(filter, 0, wxEXPAND | wxBOTTOM, 4);
    _figureList = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxSize(560, 240), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("Figure"), 190 }, { _("Kind"), 95 }, { _("Part in the plan"), 140 }, { _("Effects"), 55 }, { _("State"), 110 } }) {
        _figureList->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    left->Add(_figureList, 1, wxEXPAND);
    figures->Add(left, 3, wxEXPAND | wxRIGHT, 8);
    auto* right = new wxBoxSizer(wxVERTICAL);
    _figureDetails = new wxTextCtrl(page, wxID_ANY, "", wxDefaultPosition, wxSize(320, 200), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    right->Add(_figureDetails, 1, wxEXPAND | wxBOTTOM, 4);
    auto* actions = new wxGridSizer(2, 4, 4);
    actions->Add(new wxButton(page, ID_EXCLUDE, _("Exclude This Figure")), 0, wxEXPAND);
    actions->Add(new wxButton(page, ID_INCLUDE, _("Include It Again")), 0, wxEXPAND);
    actions->Add(new wxButton(page, ID_LOCK, _("Lock Its Effects")), 0, wxEXPAND);
    actions->Add(new wxButton(page, ID_UNLOCK, _("Unlock")), 0, wxEXPAND);
    right->Add(actions, 0, wxEXPAND);
    figures->Add(right, 2, wxEXPAND);
    sizer->Add(figures, 1, wxEXPAND | wxBOTTOM, 6);
    _correctionInfo = Muted(page, "");
    sizer->Add(_correctionInfo, 0, wxEXPAND | wxBOTTOM, 6);

    sizer->Add(new wxStaticText(page, wxID_ANY, _("Visual montage, one row per screen: each shot, its picture, how it comes in, and where the lights carry on")), 0, wxBOTTOM, 2);
    _storyboardView = new StoryboardView(page);
    sizer->Add(_storyboardView, 0, wxEXPAND | wxBOTTOM, 4);
    _storyboardView->onSelect = [this](int shot) {
        for (size_t i = 0; i < _rowShot.size(); ++i) {
            if (_rowShot[i] == shot) {
                _cueList->SetItemState(static_cast<long>(i), wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
                _cueList->EnsureVisible(static_cast<long>(i));
            }
        }
    };
    _cueList = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxSize(-1, 110), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("From"), 70 }, { _("To"), 70 }, { _("Figure"), 160 }, { _("File"), 200 }, { _("State"), 110 }, { _("Why"), 320 } }) {
        _cueList->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    _cueList->SetMinSize(wxSize(-1, 110));
    sizer->Add(_cueList, 0, wxEXPAND | wxBOTTOM, 4);
    auto* mediaActions = new wxBoxSizer(wxHORIZONTAL);
    mediaActions->Add(new wxButton(page, ID_MEDIA_ASSIGN, _("Choose a File...")), 0, wxRIGHT, 4);
    mediaActions->Add(new wxButton(page, ID_MEDIA_REMOVE, _("No Picture Here")), 0, wxRIGHT, 4);
    mediaActions->Add(new wxButton(page, ID_MEDIA_LOCK, _("Keep This One")), 0, wxRIGHT, 4);
    mediaActions->Add(new wxButton(page, ID_SCENE_LOCK, _("Keep the Whole Scene")), 0, wxRIGHT, 4);
    mediaActions->Add(new wxButton(page, ID_LANE_LOCK, _("Keep This Screen")), 0, wxRIGHT, 4);
    mediaActions->Add(new wxButton(page, ID_SHOT_MOVE, _("On the Other Screen")), 0, wxRIGHT, 12);
    mediaActions->Add(new wxButton(page, ID_MONTAGE_REBUILD, _("Rebuild the Montage")), 0, wxRIGHT, 4);
    mediaActions->Add(new wxButton(page, ID_MONTAGE_VERSIONS, _("Earlier Versions...")), 0, wxRIGHT, 12);
    mediaActions->Add(new wxButton(page, ID_MEDIA_LIBRARY, _("Pictures and Videos...")), 0, wxRIGHT, 4);
    mediaActions->Add(new wxButton(page, ID_SCREENS, _("Screens...")), 0);
    sizer->Add(mediaActions, 0);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const long selected = _cueList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (selected < 0 || selected >= static_cast<long>(_mediaRows.size()) || _mediaRows[selected].segmentId.empty()) {
            wxMessageBox(_("Choose a row of the list first."), _("Pictures and Videos"), wxOK | wxICON_INFORMATION, this);
            return;
        }
        LockStoryboardScene(_mediaRows[selected].segmentId);
    }, ID_SCENE_LOCK);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const long selected = _cueList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (selected < 0 || selected >= static_cast<long>(_rowShot.size()) || _rowShot[selected] < 0) {
            wxMessageBox(_("Choose a shot of the montage first."), _("Pictures and Videos"), wxOK | wxICON_INFORMATION, this);
            return;
        }
        MoveShotToOtherScreen(_rowShot[selected]);
    }, ID_SHOT_MOVE);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const long selected = _cueList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        const int shot = selected >= 0 && selected < static_cast<long>(_rowShot.size()) ? _rowShot[selected] : -1;
        const std::string lane = shot >= 0 ? _storyboard.shots[static_cast<size_t>(shot)].lane : _storyboardView->SelectedLane();
        if (lane.empty()) {
            wxMessageBox(_("Choose a shot or a screen of the montage first."), _("Pictures and Videos"), wxOK | wxICON_INFORMATION, this);
            return;
        }
        LockStoryboardLane(lane);
    }, ID_LANE_LOCK);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditScreens(); }, ID_SCREENS);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RebuildMontage(); }, ID_MONTAGE_REBUILD);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ChooseStoryboardVersion(); }, ID_MONTAGE_VERSIONS);
    _cueList->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) {
        const long i = e.GetIndex();
        if (_storyboardView != nullptr && i >= 0 && i < static_cast<long>(_rowShot.size())) {
            _storyboardView->Select(_rowShot[i]);
        }
    });
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ChangeMedia(0); }, ID_MEDIA_ASSIGN);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ChangeMedia(1); }, ID_MEDIA_REMOVE);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ChangeMedia(2); }, ID_MEDIA_LOCK);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditMediaLibrary(); }, ID_MEDIA_LIBRARY);
    page->SetSizer(sizer);

    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartComposition(); }, ID_COMPOSE);
    _figureFilter->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { RefreshComposition(); });
    _figureList->Bind(wxEVT_LIST_ITEM_SELECTED, &ShowComposerDialog::OnFigureSelected, this);
    _levelList->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) {
        if (_level->GetSelection() != e.GetIndex()) {
            _level->SetSelection(static_cast<int>(e.GetIndex()));
            RefreshAll();
        }
    });
    const auto selected = [this]() -> std::string {
        const long i = _figureList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        return i >= 0 ? S(_figureList->GetItemText(i)) : std::string();
    };
    page->Bind(wxEVT_BUTTON, [this, selected](wxCommandEvent&) { ExcludeFigure(selected(), true); }, ID_EXCLUDE);
    page->Bind(wxEVT_BUTTON, [this, selected](wxCommandEvent&) { ExcludeFigure(selected(), false); }, ID_INCLUDE);
    page->Bind(wxEVT_BUTTON, [this, selected](wxCommandEvent&) { LockFigure(selected(), true); }, ID_LOCK);
    page->Bind(wxEVT_BUTTON, [this, selected](wxCommandEvent&) { LockFigure(selected(), false); }, ID_UNLOCK);
    return page;
}

wxWindow* ShowComposerDialog::BuildGeneratePage(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(Bold(page, _("Generate the show")), 0, wxBOTTOM, 6);
    _generateSummary = new wxStaticText(page, wxID_ANY, "");
    sizer->Add(_generateSummary, 0, wxEXPAND | wxBOTTOM, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    _generate = new wxButton(page, ID_GENERATE, _("Generate the Show"));
    wxFont font = _generate->GetFont();
    font.MakeBold();
    _generate->SetFont(font);
    buttons->Add(_generate, 0, wxRIGHT, 8);
    _cancel = new wxButton(page, ID_CANCEL, _("Cancel"));
    buttons->Add(_cancel, 0);
    sizer->Add(buttons, 0, wxBOTTOM, 8);
    _stageList = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxSize(-1, 290), wxLC_REPORT | wxLC_NO_HEADER | wxLC_SINGLE_SEL);
    _stageList->AppendColumn("", wxLIST_FORMAT_LEFT, 40);
    _stageList->AppendColumn("", wxLIST_FORMAT_LEFT, 280);
    _stageList->AppendColumn("", wxLIST_FORMAT_LEFT, 200);
    for (int i = 0; i < STAGE_ROW_COUNT; ++i) {
        _stageList->InsertItem(i, "");
        _stageList->SetItem(i, 1, wxGetTranslation(STAGE_ROWS[i].label));
    }
    sizer->Add(_stageList, 0, wxEXPAND | wxBOTTOM, 6);
    _gauge = new wxGauge(page, wxID_ANY, 100);
    sizer->Add(_gauge, 0, wxEXPAND | wxBOTTOM, 4);
    _stageInfo = new wxStaticText(page, wxID_ANY, "");
    sizer->Add(_stageInfo, 0, wxEXPAND | wxBOTTOM, 6);
    sizer->Add(Muted(page, _("The generation runs apart from this window, which stays usable. Loading the installation, saving and rendering cannot stop halfway: a cancel takes effect as soon as the current one ends, and nothing is saved.")), 0, wxEXPAND);
    page->SetSizer(sizer);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartGeneration(); }, ID_GENERATE);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CancelJob(); }, ID_CANCEL);
    return page;
}

wxWindow* ShowComposerDialog::BuildResultPage(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    _resultTitle = Bold(page, _("Review the result"));
    sizer->Add(_resultTitle, 0, wxBOTTOM, 6);
    _resultFacts = new wxStaticText(page, wxID_ANY, "");
    sizer->Add(_resultFacts, 0, wxEXPAND | wxBOTTOM, 6);
    _reviewState = new wxStaticText(page, wxID_ANY, "");
    wxFont font = _reviewState->GetFont();
    font.MakeBold();
    _reviewState->SetFont(font);
    sizer->Add(_reviewState, 0, wxEXPAND | wxBOTTOM, 8);
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    actions->Add(new wxButton(page, ID_OPEN_SEQUENCE, _("Open the Sequence in xLights")), 0, wxRIGHT, 4);
    actions->Add(new wxButton(page, ID_OPEN_FOLDER, _("Open the Folder")), 0, wxRIGHT, 4);
    actions->Add(new wxButton(page, ID_OPEN_REPORT, _("Technical Report")), 0, wxRIGHT, 4);
    actions->Add(new wxButton(page, ID_REVIEW_AGAIN, _("Review the Composition Again")), 0, wxRIGHT, 4);
    actions->Add(new wxButton(page, ID_ANOTHER, _("Make Another Version")), 0, wxRIGHT, 4);
    actions->Add(new wxButton(page, ID_APPROVE, _("Mark as Reviewed and Approved...")), 0);
    sizer->Add(actions, 0, wxBOTTOM, 4);
    auto* review = new wxBoxSizer(wxHORIZONTAL);
    review->Add(new wxButton(page, ID_REVIEW_CLIPS, _("Review the Pictures and Videos...")), 0, wxRIGHT, 4);
    review->Add(new wxButton(page, ID_LEARNED, _("What Has Been Learned...")), 0);
    sizer->Add(review, 0, wxBOTTOM, 8);
    const auto list = [&](const wxString& label, wxListCtrl*& ctrl, const std::vector<std::pair<wxString, int>>& columns, int height) {
        sizer->Add(new wxStaticText(page, wxID_ANY, label), 0, wxBOTTOM, 2);
        ctrl = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxSize(-1, height), wxLC_REPORT | wxLC_SINGLE_SEL);
        for (const auto& [text, width] : columns) {
            ctrl->AppendColumn(text, wxLIST_FORMAT_LEFT, width);
        }
        sizer->Add(ctrl, height > 100 ? 1 : 0, wxEXPAND | wxBOTTOM, 6);
    };
    sizer->Add(new wxStaticText(page, wxID_ANY, _("How it compares with the reference shows (measures, not a judgement)")), 0, wxBOTTOM, 2);
    _coherenceText = new wxTextCtrl(page, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 88), wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    sizer->Add(_coherenceText, 0, wxEXPAND | wxBOTTOM, 6);
    list(_("Points that need your attention"), _warningList, { { _("What"), 760 } }, 110);
    list(_("Figures in the plan that stayed dark"), _quietList, { { _("Figure"), 220 }, { _("Why"), 560 } }, 120);
    list(_("Suggested pictures and videos"), _resultCues, { { _("From"), 80 }, { _("To"), 80 }, { _("Figure"), 220 }, { _("Kind"), 90 }, { _("State"), 110 } }, 80);
    page->SetSizer(sizer);

    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenResult(); }, ID_OPEN_SEQUENCE);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!_resultSequence.empty()) {
            wxLaunchDefaultApplication(U(fs::path(_resultSequence).parent_path().string()));
        }
    }, ID_OPEN_FOLDER);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const fs::path report = fs::path(_resultReportDir) / "generation-report.txt";
        if (FileExists(report.string())) {
            wxLaunchDefaultApplication(U(report.string()));
        }
    }, ID_OPEN_REPORT);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { GoTo(Step::Composition); }, ID_REVIEW_AGAIN);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        _seed->SetValue(_seed->GetValue() % 999999 + 1);
        SetSequenceName("");
        GoTo(Step::Create);
    }, ID_ANOTHER);
    // Approving goes through the review: what was checked in xLights, then the approval.
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditReview(); }, ID_APPROVE);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditReview(); }, ID_REVIEW_CLIPS);
    page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditPreferences(); }, ID_LEARNED);
    return page;
}

// ------------------------------------------------------------------ inputs

void ShowComposerDialog::SetAudio(const std::string& path) {
    _audioPath = path;
    if (_audio != nullptr && _audio->GetPlayingState() != MEDIAPLAYINGSTATE::STOPPED) {
        _audio->Stop();
    }
    _analysisReport = ReportFolder();
    _shownAnalysis = SongAnalysis();
    if (_sequenceName.empty() || _name->GetValue().IsEmpty()) {
        SetSequenceName("");
    }
    LoadAudio();
    LoadLyricsInfo(); // a line past the end depends on the song
    RefreshAll();
}

void ShowComposerDialog::ShowDropState(bool over) {
    if (_dropZone == nullptr) {
        return;
    }
    const bool accept = AcceptsDrops();
    if (over && accept) {
        _dropZone->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT));
        _dropLabel->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT));
        _dropLabel->SetLabel(_("Drop it to use it: a song, lyrics or an .xtiming. The file stays where it is."));
    } else {
        _dropZone->SetBackgroundColour(wxNullColour);
        _dropLabel->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        _dropLabel->SetLabel(!accept ? _("Wait until the generator finishes to change the song.")
                                     : _("Or drag the song here from the file explorer. Lyrics (.txt, .lrc, .srt) and .xtiming files can be dropped too."));
    }
    _dropZone->Refresh();
    _dropZone->Layout();
}

void ShowComposerDialog::DropFiles(const std::vector<std::string>& paths) {
    if (!AcceptsDrops()) {
        ShowError(_("The generator is working."), _("Wait until it finishes, or cancel it, to change the song."));
        return;
    }
    std::vector<std::string> songs;
    std::string lyrics;
    std::string timing;
    std::vector<std::string> unknown;
    for (const std::string& p : paths) {
        std::error_code ec;
        if (fs::is_directory(fs::path(p), ec)) {
            unknown.push_back(p);
        } else if (IsAudioFile(p)) {
            songs.push_back(p);
        } else if (HasExtension(p, { ".txt", ".lrc", ".srt" })) {
            lyrics = lyrics.empty() ? p : lyrics;
        } else if (HasExtension(p, { ".xtiming" })) {
            timing = timing.empty() ? p : timing;
        } else {
            unknown.push_back(p);
        }
    }
    std::string song;
    if (songs.size() == 1) {
        song = songs[0];
    } else if (songs.size() > 1) {
        wxArrayString names;
        for (const std::string& s : songs) {
            names.Add(U(fs::path(s).filename().string()));
        }
        int chosen = 0;
        if (!_testMode) {
            wxSingleChoiceDialog choose(this, _("Several songs were dropped. Which one is the show for?"), _("Show Composer"), names);
            chosen = choose.ShowModal() == wxID_OK ? choose.GetSelection() : -1;
        }
        if (chosen < 0) {
            return;
        }
        song = songs[chosen];
    }
    for (const std::string& p : { song, lyrics, timing }) {
        if (!p.empty()) {
            ObtainAccessToURL(p, false);
        }
    }
    if (!song.empty()) {
        SetAudio(song);
    }
    if (!lyrics.empty()) {
        SetLyrics(lyrics);
    }
    if (!timing.empty()) {
        SetTiming(timing);
    }
    if (song.empty() && lyrics.empty() && timing.empty()) {
        wxString names;
        for (const std::string& u : unknown) {
            names += "\n" + U(fs::path(u).filename().string());
        }
        ShowError(_("That is not a song xLights can play."), _("Drop an audio file (mp3, wav, m4a, ogg, flac, aac, wma or aiff), lyrics (.txt, .lrc, .srt) or an .xtiming file.") + names);
    } else if (!song.empty() && !_audioOk) {
        ShowError(_("This file cannot be played by xLights: choose another one."), U(fs::path(song).filename().string()));
    }
}

bool ShowComposerDialog::AskConsent(const wxString& service, const wxString& why) {
    if (_testMode) {
        return _testConsent;
    }
    std::error_code ec;
    const auto bytes = fs::file_size(fs::path(_audioPath), ec);
    const wxString message = wxString::Format(_("The song \"%s\" (%.1f MB) will be sent to %s %s.\n\nOnly this song, only now. Its answer is kept with this show, so it is not sent again. Send it?"),
                                              U(fs::path(_audioPath).filename().string()), ec ? 0.0 : bytes / 1048576.0, service, why);
    return wxMessageBox(message, _("Send the song?"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES;
}

void ShowComposerDialog::StartAssist(const std::string& action, bool consent, const std::string& query) {
    if (_assistJob.Running()) {
        ShowError(_("The assistant is still working on the last request."), wxString());
        return;
    }
    if (!consent && action != "lrclib") {
        _assistExit = 2;
        _assistMessage = S(AssistErrorText("no-consent", -1));
        _assistInfo->SetLabel(U(_assistMessage));
        return;
    }
    _assistAction = action;
    const std::string dir = NewWorkFolder("assist");
    _assistResultPath = (fs::path(dir) / (action + ".json")).string();
    std::vector<std::string> args = { "--composer-assist", action, "--audio", _audioPath, "-s", _showDir, "--out", _assistResultPath };
    if (consent) {
        args.push_back("--allow-upload");
    }
    if (!query.empty()) {
        args.push_back("--query");
        args.push_back(query);
    }
    if (_durationMS > 0) {
        args.push_back("--duration");
        args.push_back(fmt::format("{:.1f}", _durationMS / 1000.0));
    }
    std::string error;
    _assistExit = -1;
    if (!_assistJob.Start(args, (fs::path(dir) / "cancel").string(), error)) {
        ShowError(_("The assistant could not be started."), U(error));
        return;
    }
    _assistInfo->SetLabel(action == "lrclib" ? _("Searching LRCLIB...") : action == "transcribe" ? _("Sending the song to Groq and waiting for the words...") : _("Sending the song to Gemini and waiting for its description..."));
}

void ShowComposerDialog::OnAssistJob(const ComposerJob::Update& u) {
    if (u.kind == ComposerJob::Update::Kind::Finished) {
        AssistFinished(u.exitCode);
    }
}

void ShowComposerDialog::AssistFinished(int exitCode) {
    _assistExit = exitCode;
    const nlohmann::json answer = nlohmann::json::parse(ReadFile(fs::path(_assistResultPath)), nullptr, false);
    if (answer.is_discarded()) {
        _assistMessage = S(_("The assistant gave no answer."));
        _assistInfo->SetLabel(U(_assistMessage));
        return;
    }
    if (answer.value("ok", true) == false) {
        _assistMessage = S(AssistErrorText(answer.value("error", std::string()), answer.value("retryAfterS", -1)));
        SetWrapped(_assistInfo, U(_assistMessage));
        return;
    }
    if (_assistAction == "lrclib") {
        const nlohmann::json matches = answer.value("matches", nlohmann::json::array());
        wxArrayString choices;
        int first = -1;
        for (size_t i = 0; i < matches.size(); ++i) {
            const nlohmann::json& m = matches[i];
            const bool plausible = m.value("plausible", false);
            first = first < 0 && plausible ? static_cast<int>(i) : first;
            const double length = m.value("durationS", 0.0);
            const wxString kind = !m.value("syncedLyrics", std::string()).empty() ? _("with times") : m.value("instrumental", false) ? _("instrumental") : _("text only");
            choices.Add(wxString::Format("%s  %s - %s  (%s)  %d:%02d  %s", plausible ? wxString::FromUTF8("\xE2\x9C\x94") : wxString::FromUTF8("\xE2\x80\x93"), U(m.value("artistName", std::string())),
                                         U(m.value("trackName", std::string())), U(m.value("albumName", std::string())), static_cast<int>(length) / 60, static_cast<int>(length) % 60, kind));
        }
        int pick = first;
        if (!_testMode && !matches.empty()) {
            wxSingleChoiceDialog choose(this, _("Pick the right song. A tick means title, artist and length (within 2 seconds) agree."), _("Lyrics from LRCLIB"), choices);
            choose.SetSelection(std::max(0, first));
            pick = choose.ShowModal() == wxID_OK ? choose.GetSelection() : -1;
        }
        if (pick < 0) {
            _assistMessage = S(matches.empty() ? AssistErrorText("not-found", -1) : _("No lyrics chosen."));
            SetWrapped(_assistInfo, U(_assistMessage));
            return;
        }
        const nlohmann::json& m = matches[static_cast<size_t>(pick)];
        const std::string synced = m.value("syncedLyrics", std::string());
        const std::string plain = m.value("plainLyrics", std::string());
        if (synced.empty() && plain.empty()) {
            _assistMessage = S(_("That song is instrumental on LRCLIB: no lyrics."));
            SetWrapped(_assistInfo, U(_assistMessage));
            return;
        }
        // Saved in the show's own Show Composer folder, under a name of its own.
        const fs::path folder = fs::path(_showDir) / "ShowComposer" / "lyrics";
        std::error_code ec;
        fs::create_directories(folder, ec);
        const std::string stem = SafeFileName(m.value("artistName", std::string()) + " - " + m.value("trackName", std::string())) + fmt::format(" (LRCLIB {})", m.value("id", 0LL));
        const fs::path file = folder / fs::u8path(stem + (synced.empty() ? ".txt" : ".lrc"));
        if (!FileExists(file.string())) {
            std::ofstream(file, std::ios::binary) << (synced.empty() ? plain : synced);
        }
        const std::u8string utf8 = file.u8string();
        SetLyrics(std::string(utf8.begin(), utf8.end()));
        _assistMessage = S(wxString::Format(_("Lyrics from LRCLIB (%s), saved with the show. Check them: the lyrics stay yours to correct."), synced.empty() ? _("text only") : _("with line times")));
        SetWrapped(_assistInfo, U(_assistMessage));
        return;
    }
    if (_assistAction == "transcribe") {
        Transcript t;
        std::string error;
        if (!FromJson(ReadFile(fs::path(_assistResultPath)), t, error)) {
            _assistMessage = S(_("The transcription could not be read."));
        } else if (_lyricsPath.empty()) {
            // No lyrics of its own: what was heard becomes the lyrics, kept with the show
            // for a person to read, correct and keep.
            SetTranscript(_assistResultPath);
            const TranscriptLrc lrc = TranscriptToLrc(t, _durationMS);
            int doubtful = 0;
            for (const TranscriptLrcLine& l : lrc.lines) {
                doubtful += l.doubtful ? 1 : 0;
            }
            if (lrc.lines.empty()) {
                _assistMessage = S(_("Groq heard no sung words in this song: no lyrics were made."));
            } else {
                const fs::path folder = fs::path(_showDir) / "ShowComposer" / "lyrics";
                std::error_code ec;
                fs::create_directories(folder, ec);
                const std::u8string stem = fs::u8path(_audioPath).stem().u8string();
                const std::string name = stem.empty() ? std::string("song") : std::string(stem.begin(), stem.end());
                const fs::path file = folder / fs::u8path(SafeFileName(name) + " (Groq).lrc");
                std::ofstream(file, std::ios::binary) << lrc.lrc;
                const std::u8string utf8 = file.u8string();
                SetLyrics(std::string(utf8.begin(), utf8.end()));
                _assistMessage = S(wxString::Format(_("No lyrics were given, so the lyrics are what Groq heard: %d phrases, %d of them doubtful, %d left out (not sung). Read them and correct them before generating: nothing was filled in from memory."),
                                                    static_cast<int>(lrc.lines.size()), doubtful, static_cast<int>(lrc.dropped.size())));
            }
        } else {
            SetTranscript(_assistResultPath);
            _assistMessage = S(wxString::Format(_("%d words heard by %s. They give the words their times; your lyrics stay the text, and words not heard keep no invented time."), static_cast<int>(t.words.size()), U(t.model)));
        }
        SetWrapped(_assistInfo, U(_assistMessage));
        return;
    }
    SongCharacter ch;
    std::string error;
    if (!FromJson(ReadFile(fs::path(_assistResultPath)), ch, error)) {
        _assistMessage = S(_("The description could not be read."));
    } else {
        SetCharacter(_assistResultPath);
        _assistMessage = S(wxString::Format(_("Gemini hears %s, energy %s, voice %s; theme %s (%.2f). An interpretation: it only helps choose colours."), U(ch.genre), U(ch.energy), U(ch.vocalPresence),
                                            U(ch.suggestedTheme), ch.themeConfidence));
    }
    SetWrapped(_styleInfo, U(_assistMessage));
}

void ShowComposerDialog::SetTranscript(const std::string& path) {
    _transcriptPath = path;
    RefreshAll();
}

void ShowComposerDialog::SetCharacter(const std::string& path) {
    _characterPath = path;
    RefreshStyle();
    RefreshAll();
}

void ShowComposerDialog::SetProfile(const std::string& path) {
    const auto at = std::find(_profilePaths.begin(), _profilePaths.end(), path);
    const int index = at == _profilePaths.end() ? 0 : static_cast<int>(at - _profilePaths.begin());
    _profile->SetSelection(index);
    _profilePath = index > 0 ? _profilePaths[index] : "";
    RefreshStyle();
    RefreshAll();
}

void ShowComposerDialog::EditKeys() {
    WxServiceSettingsStore store;
    for (const auto& [service, env] : { std::pair<const char*, const char*>{ "Groq", "GROQ_API_KEY" }, { "Gemini", "GEMINI_API_KEY" } }) {
        const ApiKey current = ResolveApiKey(&store, service, env);
        const wxString state = current.source == "secure-store" ? _("one is saved in the system's secure store") : current.source == "environment" ? wxString::Format(_("one comes from %s"), env) : _("none yet");
        wxPasswordEntryDialog ask(this, wxString::Format(_("%s key (%s). It is kept in the system's secure store, never in the show or in a log. Leave it empty to keep the current one."), service, state),
                                  _("AI Keys"));
        if (ask.ShowModal() != wxID_OK) {
            return;
        }
        const std::string key = S(ask.GetValue());
        if (!key.empty()) {
            store.setSecret(SecretStoreName(service), key);
        }
    }
}

void ShowComposerDialog::RefreshStyle() {
    if (_styleInfo == nullptr) {
        return;
    }
    StyleProfile p = NeutralStarterProfile();
    std::string error;
    if (!_profilePath.empty()) {
        (void)FromJson(ReadFile(fs::path(_profilePath)), p, error);
    }
    _styleColors.clear();
    _styleTheme.clear();
    if (!p.ensemble.enabled) {
        SetWrapped(_styleInfo, _("Figures take turns as leads and support; no colour theme for the whole house."));
        _swatch->Refresh();
        return;
    }
    ThemeCatalog themes;
    (void)LoadThemeCatalog(DefaultThemeCatalogPath(), themes, error);
    std::string themeId = p.ensemble.defaultTheme;
    wxString source = _("the profile's");
    if (!_characterPath.empty()) {
        SongCharacter ch;
        if (FromJson(ReadFile(fs::path(_characterPath)), ch, error) && ch.suggestedTheme != "uncertain" && ch.themeConfidence >= 0.5) {
            for (const ThemeDef& t : themes.themes) {
                if (std::find(t.characterThemes.begin(), t.characterThemes.end(), ch.suggestedTheme) != t.characterThemes.end()) {
                    themeId = t.id;
                    source = _("from the song's description");
                }
            }
        }
    }
    if (const ThemeDef* t = themes.Find(themeId)) {
        _styleTheme = t->id;
        for (size_t i = 0; i < t->home.size() && i < 3; ++i) {
            for (const std::string& col : t->home[i]) {
                if (std::find(_styleColors.begin(), _styleColors.end(), col) == _styleColors.end()) {
                    _styleColors.push_back(col);
                }
            }
        }
        _styleColors.push_back(t->accent);
        SetWrapped(_styleInfo, wxString::Format(_("The whole house moves together in the colours of \"%s\" (%s; the lyrics may point to another). Measured from: %s."), ThemeName(t->name), source,
                                                U(std::accumulate(p.ensemble.sources.begin(), p.ensemble.sources.end(), std::string(), [](const std::string& a, const std::string& b) { return a.empty() ? b : a + ", " + b; }))));
    }
    _swatch->Refresh();
}

void ShowComposerDialog::SetShowFolder(const std::string& path) {
    _showDir = path;
    _profilePath.clear();
    // The show's media folders are the ones its library was last searched in.
    _mediaFolders.clear();
    MediaLibrary library;
    std::string error;
    if (!_showDir.empty() && ReadMediaLibrary(MediaLibraryPath(_showDir), library, error)) {
        for (const MediaFolder& f : library.folders) {
            _mediaFolders.push_back(f.path);
        }
    }
    LoadShowInfo();
    SetSequenceName(_sequenceName);
    RefreshAll();
}

void ShowComposerDialog::SetLyrics(const std::string& path) {
    _lyricsPath = path;
    LoadLyricsInfo();
    RefreshAll();
}

void ShowComposerDialog::SetTiming(const std::string& path) {
    _timingPath = path;
    LoadLyricsInfo();
    RefreshAll();
}

void ShowComposerDialog::SetLevel(AutomationLevel level) {
    _level->SetSelection(static_cast<int>(level));
    RefreshAll();
}

void ShowComposerDialog::SetSequenceName(const std::string& name) {
    std::string chosen = name;
    if (chosen.empty() && !_audioPath.empty()) {
        // A free name from the song's, so nothing existing is ever proposed.
        std::string stem = fs::path(_audioPath).stem().string();
        for (char& c : stem) {
            if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) {
                c = '_';
            }
        }
        if (stem.size() > 40) {
            stem.resize(40);
        }
        for (int n = 1; n < 100; ++n) {
            chosen = n == 1 ? stem + " - Composer.xsq" : fmt::format("{} - Composer {}.xsq", stem, n);
            if (!FileExists((fs::path(_showDir) / chosen).string())) {
                break;
            }
        }
    }
    _sequenceName = chosen;
    if (_name != nullptr && S(_name->GetValue()) != chosen) {
        _name->ChangeValue(U(chosen));
    }
    RefreshCreate();
}

void ShowComposerDialog::LoadAudio() {
    _audio.reset();
    _audioOk = false;
    _durationMS = 0;
    _cacheValid = false;
    _audioHash.clear();
    if (_audioPath.empty() || !FileExists(_audioPath)) {
        return;
    }
    ObtainAccessToURL(_audioPath, false);
    _audio = std::make_unique<AudioManager>(_audioPath, FRAME_MS);
    _audioOk = _audio->IsOk();
    _durationMS = _audioOk ? static_cast<int>(_audio->LengthMS()) : 0;
    _peaksPending = _audioOk;
    _audioHash = Sha256OfFile(_audioPath);
    const fs::path cache = fs::path(_showDir) / "ShowComposer" / "cache" / (_audioHash.substr(0, std::min<size_t>(16, _audioHash.size())) + ".analysis.json");
    SongAnalysis cached;
    std::string error;
    _cacheValid = !_audioHash.empty() && FileExists(cache.string()) && FromJson(ReadFile(cache), cached, error) && cached.audioHash == _audioHash && cached.analyzerVersion == ANALYZER_VERSION &&
                  cached.optionsKey == OptionsKey(AnalysisOptions());
}

void ShowComposerDialog::LoadShowInfo() {
    _showOk = false;
    _showError.clear();
    _capabilities = ModelCapabilities();
    if (_showDir.empty() || !FileExists((fs::path(_showDir) / "xlights_rgbeffects.xml").string())) {
        _showError = S(_("This folder has no xLights layout."));
        return;
    }
    const fs::path roles = !_rolesPath.empty() ? fs::path(_rolesPath) : fs::path(_showDir) / "ShowComposer" / "roles.json";
    RoleOverrides overrides;
    std::string error;
    const bool haveRoles = FileExists(roles.string()) && FromJson(ReadFile(roles), overrides, error);
    if (fs::weakly_canonical(fs::path(_showDir)) == fs::weakly_canonical(fs::path(_frame->GetShowDirectory()))) {
        // The installation is already loaded in xLights: read it from there.
        _capabilities = ExtractCapabilities(*_frame, haveRoles ? &overrides : nullptr);
        _showOk = true;
    } else {
        // Another installation is read by the generator, which loads it on its own.
        const std::string dir = NewWorkFolder("layout");
        _queue.push_back({ Purpose::Layout, Arguments("layout", dir, "composer-layout-check.xsq", -1) });
        _queueDirs.push_back(dir);
        _queueLevels.push_back(-1);
        RunNext();
    }
    _profilePaths = { "" };
    if (_profile != nullptr) {
        _profile->Clear();
        _profile->Append(_("Neutral starting profile (not your style yet)"));
        // The styles measured from reference shows, as they ship.
        const fs::path builtIn = fs::path(DefaultTemplateDirectory()).parent_path() / "profiles";
        std::error_code ec;
        std::vector<fs::path> shipped;
        for (const auto& entry : fs::directory_iterator(builtIn, ec)) {
            if (entry.path().extension() == ".json") {
                shipped.push_back(entry.path());
            }
        }
        std::sort(shipped.begin(), shipped.end());
        for (const fs::path& file : shipped) {
            StyleProfile p;
            std::string error;
            if (FromJson(ReadFile(file), p, error)) {
                _profilePaths.push_back(file.string());
                _profile->Append(U(p.name));
            }
        }
        const fs::path profiles = fs::path(_showDir) / "ShowComposer" / "profiles";
        if (fs::is_directory(profiles, ec)) {
            for (const auto& entry : fs::directory_iterator(profiles, ec)) {
                if (entry.path().extension() == ".json") {
                    _profilePaths.push_back(entry.path().string());
                    _profile->Append(U(entry.path().stem().string()));
                }
            }
        }
        _profile->SetSelection(0);
    }
}

void ShowComposerDialog::LoadLyricsInfo() {
    _lyricLines = 0;
    _lyricsTimed = false;
    _lyricsState = "none";
    _lyricIssues.clear();
    std::string error;
    if (!_timingPath.empty()) {
        std::vector<TimingTrackSpec> tracks;
        if (ImportTimingFile(_timingPath, FRAME_MS, tracks, error)) {
            for (const TimingTrackSpec& t : tracks) {
                if (t.layers.size() >= 3) {
                    _lyricsState = "track";
                    _lyricLines = static_cast<int>(t.layers[0].size());
                    _lyricsTimed = true;
                }
            }
            if (_lyricsState != "track") {
                _lyricsState = "track-unusable";
            }
        } else {
            _lyricsState = "track-unreadable";
        }
        return;
    }
    int marks = 0;
    LyricSession session;
    if (!_sessionPath.empty() && FileExists(_sessionPath) && LoadLyricSession(_sessionPath, session, error)) {
        for (const LyricCorrection& c : session.corrections) {
            marks += c.level == LyricCorrection::Level::Phrase && c.startMS >= 0 && c.endMS > c.startMS ? 1 : 0;
        }
    }
    std::string text;
    if (!_lyricsPath.empty()) {
        text = ReadFile(fs::path(_lyricsPath));
    } else if (!session.lyrics.empty()) {
        text = session.lyrics;
    }
    if (text.empty()) {
        return;
    }
    // Read as the generator will read it, so a wrong time is seen here and not after waiting.
    LyricDocument doc = _lyricsPath.empty() ? ParseLyrics(text) : ParseLyricsFile(_lyricsPath, text);
    CheckLyricsAgainstSong(doc, _audioOk ? _durationMS : 0);
    _lyricIssues = doc.issues;
    _lyricLines = static_cast<int>(doc.Lines().size());
    if (doc.ErrorCount() > 0 && marks == 0) {
        _lyricsState = "invalid";
    } else if (marks > 0) {
        _lyricsState = "marked";
        _lyricsTimed = true;
        _lyricLines = marks;
    } else if (doc.HasTimes()) {
        _lyricsState = "lines";
        _lyricsTimed = true;
    } else {
        _lyricsState = "plain";
    }
}

void ShowComposerDialog::LoadLibrary() {
    _library = LoadTemplateLibrary(_templatesDir.empty() ? DefaultTemplateDirectory() : _templatesDir, MetadataFrom(_frame->GetEffectManager()));
}

bool ShowComposerDialog::LoadPreviousShow(const std::string& sidecarPath, std::string& error) {
    GenerationSidecar sidecar;
    const SidecarStatus status = ReadSidecar(sidecarPath, sidecar, error);
    if (status == SidecarStatus::Newer) {
        error = S(_("It was made by a newer version of Show Composer."));
        return false;
    }
    if (status != SidecarStatus::Ok) {
        error = S(_("Its data is damaged or incomplete.")) + " " + error;
        return false;
    }
    std::string audio = sidecar.audioPath;
    if (audio.empty() || !FileExists(audio)) {
        const fs::path beside = fs::path(sidecarPath).parent_path() / sidecar.audioFile;
        audio = FileExists(beside.string()) ? beside.string() : std::string();
    }
    if (audio.empty()) {
        error = S(wxString::Format(_("The song %s was not found; choose it again."), U(sidecar.audioFile)));
        return false;
    }
    if (Sha256OfFile(audio) != sidecar.audioHash) {
        error = S(_("The song file has changed since that show was made."));
        return false;
    }
    _corrections = CorrectionSet();
    _corrections.plan = sidecar.planCorrections;
    _corrections.choreography = sidecar.choreographyCorrections;
    _corrections.analysis = sidecar.analysisCorrections;
    _corrections.media = sidecar.media.corrections;
    _corrections.screens = sidecar.media.screens;
    if (!sidecar.media.folders.empty()) {
        _mediaFolders.clear();
        for (const std::string& f : sidecar.media.folders) {
            _mediaFolders.push_back(MediaAbsolutePath(f, _showDir));
        }
    }
    if (_mediaAuto != nullptr) {
        _mediaAuto->SetValue(sidecar.media.automatic);
    }
    _lyricsPath.clear();
    _timingPath.clear();
    _sessionPath.clear();
    if (!sidecar.lyrics.lyrics.empty() || !sidecar.lyrics.corrections.empty()) {
        _sessionPath = NewWorkFolder("marks") + "/lyric-session.json";
        if (!SaveLyricSession(_sessionPath, sidecar.lyrics, error)) {
            return false;
        }
    }
    _showDir = fs::path(sidecarPath).parent_path().string();
    LoadShowInfo();
    _seed->SetValue(static_cast<int>(std::clamp<uint64_t>(sidecar.seed, 1, 999999)));
    _level->SetSelection(static_cast<int>(sidecar.level));
    _audioPath = audio;
    LoadAudio();
    LoadLyricsInfo();
    SetSequenceName("");
    RefreshAll();
    return true;
}

// ------------------------------------------------------------------ jobs

std::string ShowComposerDialog::NewWorkFolder(const std::string& what) {
    const fs::path dir = fs::path(_workDir) / fmt::format("{:03d}-{}", ++_workCounter, what);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

AutomationLevel ShowComposerDialog::Level() const {
    return static_cast<AutomationLevel>(std::clamp(_level->GetSelection(), 0, 2));
}

std::vector<std::string> ShowComposerDialog::Arguments(const std::string& stopAfter, const std::string& reportDir, const std::string& name, int level) const {
    std::vector<std::string> a = { "--composer-generate", _audioPath, "-s", _showDir, "--name", name, "--out", reportDir, "--level", LevelId(level >= 0 ? static_cast<AutomationLevel>(level) : Level()),
                                   "--seed", std::to_string(_seed->GetValue()) };
    if (stopAfter == "layout") {
        // Only the installation is read; any song name will do for the check.
        a[1] = _audioPath.empty() ? (fs::path(_showDir) / "xlights_rgbeffects.xml").string() : _audioPath;
    }
    const auto add = [&](const char* option, const std::string& value) {
        if (!value.empty()) {
            a.push_back(option);
            a.push_back(value);
        }
    };
    add("--lyrics", _timingPath.empty() ? _lyricsPath : std::string());
    add("--timing", _timingPath);
    add("--lyric-session", _timingPath.empty() ? _sessionPath : std::string());
    add("--profile", _profilePath);
    add("--templates", _templatesDir);
    add("--roles", _rolesPath);
    add("--transcript", _transcriptPath);
    add("--character", _characterPath);
    add("--stop-after", stopAfter);
    if (!_corrections.Empty()) {
        const fs::path file = fs::path(reportDir).parent_path() / (fs::path(reportDir).filename().string() + ".corrections.json");
        std::ofstream(file, std::ios::binary) << ToJson(_corrections);
        add("--corrections", file.string());
    }
    if (!_renderCheck && stopAfter.empty()) {
        a.push_back("--no-render");
    }
    if (!_mediaFolders.empty()) {
        add("--media-folders", std::accumulate(std::next(_mediaFolders.begin()), _mediaFolders.end(), _mediaFolders.front(), [](std::string x, const std::string& y) { return x + ";" + y; }));
    }
    if (_mediaAuto != nullptr && !_mediaAuto->GetValue()) {
        a.push_back("--media-manual-only");
    }
    if (_mediaVariant != 0) {
        add("--media-variant", std::to_string(_mediaVariant));
    }
    // A copy is made only by the generation the person asked for, never by a preview.
    if (_copyMedia != nullptr && _copyMedia->GetValue() && stopAfter.empty()) {
        a.push_back("--copy-media");
    }
    return a;
}

std::string ShowComposerDialog::InputsKey() const {
    return fmt::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", _audioPath, _showDir, _lyricsPath, _timingPath, _sessionPath.empty() ? std::string() : ReadFile(fs::path(_sessionPath)), _profilePath, _templatesDir, _rolesPath,
                       _seed->GetValue(), ToJson(_corrections), _transcriptPath, _characterPath) +
           fmt::format("|{}|{}", std::accumulate(_mediaFolders.begin(), _mediaFolders.end(), std::string(), [](std::string x, const std::string& y) { return x + ";" + y; }),
                       _mediaAuto != nullptr && _mediaAuto->GetValue() ? 1 : 0) +
           fmt::format("|{}|{}", _mediaVariant, _mediaStamp);
}

void ShowComposerDialog::RunNext() {
    const SlowLog slow("RunNext");
    if (_job.Running() || _queue.empty()) {
        return;
    }
    auto [purpose, arguments] = _queue.front();
    _queue.erase(_queue.begin());
    _jobDir = _queueDirs.front();
    _queueDirs.erase(_queueDirs.begin());
    _jobLevel = _queueLevels.front();
    _queueLevels.erase(_queueLevels.begin());
    _purpose = purpose;
    std::string error;
    if (!_job.Start(arguments, _jobDir + ".cancel", error)) {
        _purpose = Purpose::None;
        ShowError(_("The Show Composer generator could not be started."), U(error));
        return;
    }
    if (purpose == Purpose::Generation) {
        for (int i = 0; i < STAGE_ROW_COUNT; ++i) {
            _stageList->SetItem(i, 0, "");
            _stageList->SetItem(i, 2, _("Waiting"));
        }
        _gauge->SetValue(0);
    }
    RefreshAll();
}

void ShowComposerDialog::StartAnalysis() {
    if (_audioPath.empty() || !_showOk) {
        ShowError(_("Choose a song and an installation first."), _("Both are on the first step."));
        return;
    }
    if (_job.Running()) {
        return;
    }
    const std::string dir = NewWorkFolder("analysis");
    _analysisKey = InputsKey();
    _queue.push_back({ Purpose::Analysis, Arguments("lyrics", dir, "composer-preview.xsq", -1) });
    _queueDirs.push_back(dir);
    _queueLevels.push_back(-1);
    RunNext();
}

void ShowComposerDialog::StartComposition() {
    if (_audioPath.empty() || !_showOk) {
        ShowError(_("Choose a song and an installation first."), _("Both are on the first step."));
        return;
    }
    if (_job.Running()) {
        return;
    }
    _levelsKey = InputsKey();
    for (int level = 0; level < 3; ++level) {
        _levels[level] = ReportFolder();
        const std::string dir = NewWorkFolder(std::string("composition-") + LevelId(static_cast<AutomationLevel>(level)));
        _queue.push_back({ Purpose::Composition, Arguments("choreography", dir, "composer-preview.xsq", level) });
        _queueDirs.push_back(dir);
        _queueLevels.push_back(level);
    }
    RunNext();
}

void ShowComposerDialog::StartGeneration() {
    wxString why;
    if (_audioPath.empty() || !_showOk || !SequenceNameFree(why)) {
        ShowError(_("The show cannot be generated yet."), why.IsEmpty() ? _("Choose a song and an installation on the first step.") : why);
        return;
    }
    if (_job.Running()) {
        return;
    }
    const fs::path stem = fs::path(_sequenceName).stem();
    fs::path dir = fs::path(_showDir) / "ShowComposer" / "reports" / stem;
    for (int n = 2; fs::exists(dir); ++n) {
        dir = fs::path(_showDir) / "ShowComposer" / "reports" / (stem.string() + "-" + std::to_string(n));
    }
    std::error_code ec;
    fs::create_directories(dir.parent_path(), ec);
    _resultReportDir = dir.string();
    _result = ReportFolder();
    _resultSequence.clear();
    _queue.push_back({ Purpose::Generation, Arguments("", _resultReportDir, _sequenceName, -1) });
    _queueDirs.push_back(_resultReportDir);
    _queueLevels.push_back(-1);
    GoTo(Step::Generate);
    RunNext();
}

void ShowComposerDialog::CancelJob() {
    _queue.clear();
    _queueDirs.clear();
    _queueLevels.clear();
    if (_job.Running()) {
        _job.Cancel();
        _stageInfo->SetLabel(_("Cancelling: the generator stops as soon as the current step ends. Nothing is saved."));
    }
    RefreshAll();
}

void ShowComposerDialog::OnJob(const ComposerJob::Update& u) {
    const SlowLog slow("OnJob");
    if (u.kind == ComposerJob::Update::Kind::Finished) {
        JobFinished(u.exitCode);
        return;
    }
    if (_purpose != Purpose::Generation) {
        if (u.kind == ComposerJob::Update::Kind::Stage && u.what == "start") {
            _status->SetLabel(wxString::Format(_("Working: %s"), wxGetTranslation(STAGE_ROWS[0].label)));
            for (const StageRow& r : STAGE_ROWS) {
                if (u.stage == r.stage) {
                    _status->SetLabel(wxString::Format(_("Working: %s"), wxGetTranslation(r.label)));
                    break;
                }
            }
        }
        return;
    }
    const auto set = [&](int row, const wxString& glyph, const wxString& state) {
        _stageList->SetItem(row, 0, glyph);
        _stageList->SetItem(row, 2, state);
    };
    const wxString running = wxString::FromUTF8("\xE2\x80\xA6");
    const wxString done = wxString::FromUTF8("\xE2\x9C\x94");
    const wxString failed = wxString::FromUTF8("\xE2\x9C\x96");
    if (u.kind == ComposerJob::Update::Kind::Detail) {
        if (u.stage == "analysis" && !u.what.empty() && u.what.back() == '%') {
            const size_t space = u.what.rfind(' ');
            const int percent = static_cast<int>(std::strtol(u.what.substr(space == std::string::npos ? 0 : space + 1).c_str(), nullptr, 10));
            _gauge->SetValue(std::clamp(percent, 0, 100));
        } else if (u.stage == "write" && u.what == "render") {
            set(7, done, _("Done"));
            set(8, done, _("Done"));
            set(9, running, _("In progress"));
            _stageInfo->SetLabel(_("Rendering the whole song without a window, to check every effect lights its figure."));
        } else if (u.stage == "write" && u.what == "check render") {
            _stageInfo->SetLabel(_("Checking the render: figures, faces, silences and timing."));
        }
        return;
    }
    if (u.kind != ComposerJob::Update::Kind::Stage) {
        return;
    }
    for (int i = 0; i < STAGE_ROW_COUNT; ++i) {
        if (u.stage != STAGE_ROWS[i].stage) {
            continue;
        }
        if (u.what == "start") {
            if (i == 9) {
                continue; // the render starts inside the write
            }
            set(i, running, _("In progress"));
            _stageInfo->SetLabel(wxString::Format(_("Now: %s"), wxGetTranslation(STAGE_ROWS[i].label)));
            if (u.stage == "analysis") {
                _gauge->SetValue(0);
            } else {
                _gauge->Pulse();
            }
        } else if (u.what == "ok" || u.what == "warning") {
            set(i, done, u.what == "ok" ? _("Done") : _("Done, with notes"));
        } else if (u.what == "failed") {
            set(i, failed, _("Failed"));
        } else if (u.what == "cancelled") {
            set(i, failed, _("Cancelled"));
        }
    }
    if (u.stage == "spec" && u.what != "start") {
        // Building the sequence is part of the choreography row.
        set(6, u.what == "failed" ? failed : done, u.what == "failed" ? _("Failed") : _("Done"));
    }
}

void ShowComposerDialog::JobFinished(int exitCode) {
    const SlowLog slow("JobFinished");
    const Purpose purpose = _purpose;
    _purpose = Purpose::None;
    _lastExitCode = exitCode;
    _status->SetLabel("");
    std::string problem;
    for (const std::string& line : _job.Output()) {
        if (line.find("PROBLEM ") != std::string::npos && problem.empty()) {
            problem = line.substr(line.find("PROBLEM ") + 8);
        }
    }
    const bool ok = exitCode == 0;
    if (exitCode == 3 || _job.CancelRequested()) {
        _stageInfo->SetLabel(_("Cancelled. Nothing was saved in the show folder."));
        _gauge->SetValue(0);
        _queue.clear();
        _queueDirs.clear();
        _queueLevels.clear();
    } else if (!ok) {
        _queue.clear();
        _queueDirs.clear();
        _queueLevels.clear();
        if (purpose == Purpose::Generation) {
            _stageInfo->SetLabel(_("The show was not saved. See below what happened."));
        }
        ShowError(ProblemText(problem.empty() ? std::string("unknown") : problem), purpose == Purpose::Generation ? _("Nothing was saved in the show folder. The technical report has every detail.") : wxString());
    }
    if (ok) {
        PendingLoad load;
        load.purpose = purpose;
        load.level = _jobLevel;
        load.sequenceName = _sequenceName;
        load.report = std::make_shared<ReportFolder>();
        load.done = std::async(std::launch::async, [dir = _jobDir, report = load.report]() {
            std::string error;
            return LoadReportFolder(dir, *report, error) ? std::string() : (error.empty() ? std::string("unreadable") : error);
        });
        _loads.push_back(std::move(load));
    }
    if (_closeWhenDone && !Busy()) {
        EndModal(wxID_CANCEL);
        return;
    }
    RunNext();
    RefreshAll();
}

void ShowComposerDialog::CollectLoads() {
    const SlowLog slow("CollectLoads");
    for (auto it = _loads.begin(); it != _loads.end();) {
        if (it->done.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        const std::string error = it->done.get();
        const Purpose purpose = it->purpose;
        const int level = it->level;
        const std::string name = it->sequenceName;
        ReportFolder report = std::move(*it->report);
        it = _loads.erase(it);
        if (!error.empty()) {
            ShowError(_("The generator's results could not be read."), U(error));
            continue;
        }
        if (!_closeWhenDone) {
            ApplyReport(purpose, level, name, std::move(report));
        }
    }
    if (_closeWhenDone && !Busy()) {
        EndModal(wxID_CANCEL);
    }
}

void ShowComposerDialog::ApplyReport(Purpose purpose, int level, const std::string& sequenceName, ReportFolder report) {
    switch (purpose) {
    case Purpose::Layout:
        _capabilities = report.capabilities;
        _showOk = report.hasCapabilities;
        if (!_showOk) {
            _showError = S(_("The installation could not be read."));
        }
        break;
    case Purpose::Analysis:
        _analysisReport = std::move(report);
        ApplyMusicCorrectionsLocally();
        break;
    case Purpose::Composition:
        if (level >= 0 && level < 3) {
            _levels[level] = std::move(report);
        }
        break;
    case Purpose::Generation:
        _result = std::move(report);
        _resultSequence = (fs::path(_showDir) / sequenceName).string();
        _resultSummary = _result.summary;
        _stageInfo->SetLabel(_("Done: the show is saved. Review it in the next step."));
        _gauge->SetValue(100);
        LoadLyricsInfo();
        GoTo(Step::Result);
        break;
    default:
        break;
    }
    RefreshAll();
}

void ShowComposerDialog::ShowError(const wxString& what, const wxString& how) {
    _lastError = S(what);
    spdlog::warn("Show Composer: {} {}", S(what), S(how));
    if (_testMode) {
        return;
    }
    wxMessageBox(how.IsEmpty() ? what : what + "\n\n" + how, _("Show Composer"), wxOK | wxICON_WARNING, this);
}

void ShowComposerDialog::ApplyMusicCorrectionsLocally() {
    // The corrections go on the analysis as it was detected, exactly as the generator
    // applies them, so what is shown is what the show will follow.
    _shownAnalysis = _analysisReport.analysis;
    const fs::path cache = fs::path(_showDir) / "ShowComposer" / "cache" / (_audioHash.substr(0, std::min<size_t>(16, _audioHash.size())) + ".analysis.json");
    SongAnalysis detected;
    std::string error;
    if (FileExists(cache.string()) && FromJson(ReadFile(cache), detected, error) && detected.audioHash == _audioHash) {
        _shownAnalysis = detected;
        MergeAnalysisCorrections(_shownAnalysis.corrections, _corrections.analysis);
        ApplyCorrections(_shownAnalysis);
    }
    _timeline->SetSong(_shownAnalysis.durationMS > 0 ? &_shownAnalysis : nullptr, _audioOk && !_peaksPending ? _audio.get() : nullptr);
    _timeline->SetLyrics(_analysisReport.hasLyrics ? &_analysisReport.alignment : nullptr);
    RefreshAll();
}

// ------------------------------------------------------------------ figures

void ShowComposerDialog::ExcludeFigure(const std::string& name, bool exclude) {
    if (name.empty()) {
        return;
    }
    RemoveCorrections(_corrections, name, false, true);
    if (exclude) {
        PlanCorrection c;
        c.action = PlanCorrection::Action::Exclude;
        c.target = name;
        _corrections.plan.push_back(c);
    }
    RefreshAll();
}

void ShowComposerDialog::LockFigure(const std::string& name, bool lock) {
    if (name.empty()) {
        return;
    }
    RemoveCorrections(_corrections, name, true, false);
    if (lock) {
        const ReportFolder& level = _levels[static_cast<int>(Level())];
        for (ChoreographyCorrection& c : composer::LockFigure(level.choreography, name)) {
            _corrections.choreography.push_back(std::move(c));
        }
    }
    RefreshAll();
}

void ShowComposerDialog::OnFigureSelected(wxListEvent&) {
    RefreshFigureDetails();
}

// ------------------------------------------------------------------ navigation

bool ShowComposerDialog::CanGoTo(Step step) const {
    switch (step) {
    case Step::Create:
        return true;
    case Step::Analysis:
    case Step::Composition:
        return !_audioPath.empty() && _audioOk && _showOk;
    case Step::Generate: {
        wxString why;
        return !_audioPath.empty() && _audioOk && _showOk && SequenceNameFree(why);
    }
    case Step::Result:
        return !_resultSequence.empty();
    }
    return false;
}

wxWindow* ShowComposerDialog::Page(Step step) const {
    return _book->GetPage(static_cast<size_t>(step));
}

void ShowComposerDialog::GoTo(Step step) {
    if (!CanGoTo(step)) {
        RefreshSteps();
        return;
    }
    _step = step;
    _book->ChangeSelection(static_cast<size_t>(step));
    if (step == Step::Analysis && _analysisReport.folder.empty() && !_job.Running()) {
        StartAnalysis();
    }
    if (step == Step::Composition && (_levelsKey != InputsKey() || _levels[0].folder.empty()) && !_job.Running()) {
        StartComposition();
    }
    RefreshAll();
}

void ShowComposerDialog::OnStepSelected(wxCommandEvent&) {
    const int i = _steps->GetSelection();
    if (i >= 0) {
        GoTo(static_cast<Step>(i));
    }
}

void ShowComposerDialog::OnBack(wxCommandEvent&) {
    if (static_cast<int>(_step) > 0) {
        GoTo(static_cast<Step>(static_cast<int>(_step) - 1));
    }
}

void ShowComposerDialog::OnNext(wxCommandEvent&) {
    if (static_cast<int>(_step) + 1 < STEP_COUNT) {
        GoTo(static_cast<Step>(static_cast<int>(_step) + 1));
    }
}

void ShowComposerDialog::OnCloseButton(wxCommandEvent&) {
    Close();
}

void ShowComposerDialog::OnClose(wxCloseEvent& event) {
    if (_job.Running()) {
        if (!_testMode && wxMessageBox(_("The generator is still working. Cancel it and close? Nothing will be saved."), _("Show Composer"), wxYES_NO | wxICON_QUESTION, this) != wxYES) {
            event.Veto();
            return;
        }
        // The window closes once the generator has really stopped.
        _closeWhenDone = true;
        CancelJob();
        event.Veto();
        return;
    }
    EndModal(wxID_CANCEL);
}

void ShowComposerDialog::OnTimer(wxTimerEvent&) {
    const SlowLog slow("OnTimer");
    if (!_loads.empty()) {
        CollectLoads();
    }
    if (_peaksPending && _audio != nullptr && _audio->IsDataLoaded()) {
        _peaksPending = false;
        _timeline->SetSong(_shownAnalysis.durationMS > 0 ? &_shownAnalysis : nullptr, _audio.get());
    }
    if (_audio != nullptr && _audioOk) {
        const bool playing = _audio->GetPlayingState() == MEDIAPLAYINGSTATE::PLAYING;
        _timeline->SetPlayhead(playing ? static_cast<int>(_audio->Tell()) : -1);
    }
    if (_job.Running() && _purpose == Purpose::Generation) {
        _gauge->Pulse();
    }
    if (_analysisRunner != nullptr && (_analysisRunner->Running() || _analysisRunner->Finished())) {
        if (_analysisRunner->Finished()) {
            _analysisRunner->Join();
            _lastMediaAnalysis = _analysisRunner->Summary();
            _analysisRunner.reset();
            ++_mediaStamp;
            RefreshAll();
        } else {
            RefreshMediaAnalysisState();
        }
    }
    if (_modelRunner != nullptr) {
        if (_modelRunner->Finished()) {
            _modelRunner->Join();
            const bool ok = _modelRunner->Ok();
            const std::string error = _modelRunner->Error();
            _modelRunner.reset();
            if (!ok && !_testMode) {
                wxMessageBox(wxString::Format(_("The image model could not be downloaded: %s. The library is still analysed without it."), U(error)), _("Pictures and Videos"), wxOK | wxICON_WARNING, this);
            }
            RefreshAll();
        } else if (_analysisState != nullptr) {
            _analysisState->SetLabel(wxString::Format(_("Downloading the image model: %d%%"), _modelRunner->Percent()));
        }
    }
}

// ------------------------------------------------------------------ refresh

std::string ShowComposerDialog::SequencePath() const {
    return (fs::path(_showDir) / _sequenceName).string();
}

bool ShowComposerDialog::SequenceNameFree(wxString& why) const {
    const fs::path name(_sequenceName);
    if (_sequenceName.empty() || name.has_parent_path() || name.extension() != ".xsq" || name.stem().empty()) {
        why = _("The name must end in .xsq and have no folders in it.");
        return false;
    }
    fs::path backup = SequencePath();
    backup.replace_extension(".xbkp");
    if (FileExists(SequencePath()) || FileExists(backup.string()) || FileExists(SidecarPathFor(SequencePath()))) {
        why = _("A sequence with this name already exists, and it is never replaced. Choose another name.");
        return false;
    }
    return true;
}

void ShowComposerDialog::RefreshAll() {
    // The window must keep answering while the generator works: a slow part is logged.
    const auto timed = [](const char* what, const std::function<void()>& f) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 300) {
            spdlog::info("Show Composer: {} took {} ms", what, ms);
        }
    };
    RefreshSteps();
    timed("RefreshCreate", [this] { RefreshCreate(); });
    timed("RefreshAnalysis", [this] { RefreshAnalysis(); });
    timed("RefreshComposition", [this] { RefreshComposition(); });
    timed("RefreshGenerate", [this] { RefreshGenerate(); });
    timed("RefreshResult", [this] { RefreshResult(); });
    // Texts change length with what they say; the pages follow them.
    for (size_t i = 0; i < _book->GetPageCount(); ++i) {
        wxWindow* page = _book->GetPage(i);
        page->Layout();
        if (auto* scrolled = dynamic_cast<wxScrolledWindow*>(page)) {
            scrolled->FitInside();
        }
    }
}

void ShowComposerDialog::RefreshSteps() {
    _steps->SetSelection(static_cast<int>(_step));
    _back->Enable(static_cast<int>(_step) > 0 && !(_job.Running() && _purpose == Purpose::Generation));
    const bool nextAllowed = static_cast<int>(_step) + 1 < STEP_COUNT && CanGoTo(static_cast<Step>(static_cast<int>(_step) + 1));
    _next->Enable(nextAllowed && !(_job.Running() && _purpose == Purpose::Generation));
}

void ShowComposerDialog::RefreshCreate() {
    if (_audioName == nullptr) {
        return;
    }
    if (_mediaInfo != nullptr) {
        if (_mediaFolders.empty()) {
            _mediaInfo->SetLabel(_("No folder of pictures or videos: the show uses none."));
        } else {
            int usable = 0;
            if (const MediaLibrary* library = CachedLibrary(); library != nullptr) {
                for (const MediaItem& m : library->items) {
                    usable += m.Usable() ? 1 : 0;
                }
            }
            _mediaInfo->SetLabel(wxString::Format(_("%d folders, %d files that can be used."), static_cast<int>(_mediaFolders.size()), usable));
        }
    }
    if (_screensInfo != nullptr) {
        wxString text;
        if (_showOk) {
            const SurfacePlan sp = ScreensPreview();
            for (const Surface& s : sp.surfaces) {
                if (s.role != SurfaceRole::Support && s.role != SurfaceRole::Off) {
                    text += (text.empty() ? "" : ", ") + U(s.target) + ": " + ScreenRoleName(s.role);
                }
            }
            text = text.empty() ? wxString(_("No screen for pictures in this show.")) : wxString::Format(_("Screens: %s. Words and pictures: %s."), text, LyricsModeName(_corrections.screens.mode));
        }
        _screensInfo->SetLabel(text);
        _screensInfo->SetToolTip(text);
    }
    RefreshMediaAnalysisState();
    if (_audioPath.empty()) {
        _audioName->SetLabel(_("No song chosen"));
        SetWrapped(_audioInfo, _("Choose the song the show is for."));
    } else {
        _audioName->SetLabel(U(fs::path(_audioPath).filename().string()));
        if (!_audioOk) {
            SetWrapped(_audioInfo, _("This file cannot be played by xLights: choose another one."));
        } else {
            SetWrapped(_audioInfo, wxString::Format(_("Length %s. It can be played. %s"), Time(_durationMS),
                                                  _cacheValid ? _("Its analysis is already saved for this installation.") : _("It will be analyzed in the next step.")));
        }
    }
    _showName->SetLabel(U(_showDir));
    if (!_showError.empty()) {
        SetWrapped(_showInfo, U(_showError));
    } else if (!_showOk) {
        SetWrapped(_showInfo, _("Reading the installation..."));
    } else {
        int faces = 0;
        int texts = 0;
        for (const TargetInfo& t : _capabilities.targets) {
            if (t.type != TargetType::Model || t.Excluded()) {
                continue;
            }
            faces += !t.faces.empty() && !t.noController ? 1 : 0;
            texts += t.Can(Capability::Text).support == Support::Yes && !t.noController ? 1 : 0;
        }
        const bool current = fs::weakly_canonical(fs::path(_showDir)) == fs::weakly_canonical(fs::path(_frame->GetShowDirectory()));
        SetWrapped(_showInfo, wxString::Format(_("%d models, %d groups, %d submodels. Figures with a face: %d. Matrices for lyrics: %d. %s"), _capabilities.Count(TargetType::Model), _capabilities.Count(TargetType::Group),
                                             _capabilities.Count(TargetType::SubModel), faces, texts, current ? _("It is the show open in xLights.") : _("It is not the show open in xLights; it will be opened with it.")));
    }
    _lyricsName->SetLabel(_lyricsPath.empty() ? (_sessionPath.empty() ? _("No lyrics") : _("Lyrics marked by ear")) : U(fs::path(_lyricsPath).filename().string()));
    _timingName->SetLabel(_timingPath.empty() ? _("No synchronization file") : U(fs::path(_timingPath).filename().string()));
    int faceFigures = 0;
    int hiddenFaces = 0;
    for (const TargetInfo& t : _capabilities.targets) {
        if (t.type == TargetType::Model && !t.faces.empty() && !t.Excluded()) {
            (t.noController ? hiddenFaces : faceFigures)++;
        }
    }
    wxString lyrics;
    if (_lyricsState == "none") {
        lyrics = _("No lyrics: no face will sing and no lyrics will be shown.");
    } else if (_lyricsState == "plain") {
        lyrics = wxString::Format(_("%d lines of lyrics, not synchronized. No face will sing until the phrases are marked: use \"Mark the Lyrics by Ear\"."), _lyricLines);
    } else if (_lyricsState == "lines") {
        lyrics = wxString::Format(_("%d lines with their times (LRC or SRT). Words and mouth shapes are estimated by syllables inside each line."), _lyricLines);
    } else if (_lyricsState == "marked") {
        lyrics = wxString::Format(_("%d phrases marked by ear. Words and mouth shapes are estimated by syllables inside each phrase."), _lyricLines);
    } else if (_lyricsState == "track") {
        lyrics = wxString::Format(_("A synchronized lyrics track with %d phrases: its times win over any other."), _lyricLines);
    } else if (_lyricsState == "invalid") {
        const auto first = std::find_if(_lyricIssues.begin(), _lyricIssues.end(), [](const LyricIssue& i) { return i.error; });
        const int errors = static_cast<int>(std::count_if(_lyricIssues.begin(), _lyricIssues.end(), [](const LyricIssue& i) { return i.error; }));
        lyrics = wxString::Format(_("The lyrics file has %d times that cannot be used, so no show can be made from it until it is fixed. The first: %s"), errors, LyricIssueText(*first));
    } else if (_lyricsState == "track-unusable") {
        lyrics = _("The synchronization file has no lyrics track with phrases, words and phonemes.");
    } else {
        lyrics = _("The synchronization file could not be read.");
    }
    const int repaired = static_cast<int>(std::count_if(_lyricIssues.begin(), _lyricIssues.end(), [](const LyricIssue& i) { return i.repaired; }));
    if (repaired > 0 && _lyricsState != "invalid") {
        const auto first = std::find_if(_lyricIssues.begin(), _lyricIssues.end(), [](const LyricIssue& i) { return i.repaired; });
        lyrics += " " + wxString::Format(_("%d things in the file were put right: %s"), repaired, LyricIssueText(*first));
    }
    if (_lyricsTimed) {
        lyrics += " " + (faceFigures > 0 ? wxString::Format(_("Faces can sing: %d figures have a face."), faceFigures) : _("No figure has a face, so no face will sing."));
        if (hiddenFaces > 0) {
            lyrics += " " + wxString::Format(_("Another %d have a face but no controller, so the render cannot show them."), hiddenFaces);
        }
    }
    SetWrapped(_lyricsInfo, lyrics);
    _templatesName->SetLabel(_templatesDir.empty() ? _("Starting techniques of Show Composer") : U(_templatesDir));
    _rolesName->SetLabel(_rolesPath.empty() ? _("Roles saved with the show, if any") : U(_rolesPath));
    wxString why;
    if (SequenceNameFree(why)) {
        SetWrapped(_nameInfo, wxString::Format(_("It will be saved in the show folder as %s, next to its other sequences. Nothing existing is replaced."), U(_sequenceName)));
    } else {
        SetWrapped(_nameInfo, why);
    }
}

void ShowComposerDialog::RefreshAnalysis() {
    if (_analysisInfo == nullptr) {
        return;
    }
    if (_voicesInfo != nullptr) {
        const std::vector<VoiceDef> voices = CurrentVoices();
        wxString names;
        for (const VoiceDef& v : voices) {
            names += (names.empty() ? "" : ", ") + U(v.name);
        }
        _voicesInfo->SetLabel(voices.empty() ? _("One voice: every face sings the whole lyrics. Tag the lyrics ([Voz: Ana], [Coro], [Narrador]) to have several.")
                                             : wxString::Format(_("%d voices: %s. Changes are used the next time the show is composed."), static_cast<int>(voices.size()), names));
    }
    const SongAnalysis& a = _shownAnalysis;
    if (a.durationMS <= 0) {
        _analysisInfo->SetLabel(_job.Running() && _purpose == Purpose::Analysis ? _("Analyzing the song...") : _("The song has not been analyzed yet."));
        _lyricsSync->SetLabel("");
    } else {
        int beats = 0;
        int downbeats = 0;
        int silences = 0;
        for (const MusicalEvent& e : a.events) {
            beats += e.type == MusicalEventType::Beat ? 1 : 0;
            downbeats += e.type == MusicalEventType::Downbeat ? 1 : 0;
            silences += e.type == MusicalEventType::Silence ? 1 : 0;
        }
        wxString info = wxString::Format(_("%s long, about %.0f beats a minute. %d beats, %d strong beats, %d sections, %d silences."), Time(a.durationMS), a.globalBpm, beats, downbeats, static_cast<int>(a.sections.size()), silences);
        if (downbeats == 0) {
            info += " " + _("No strong beat was found: accents need them. Place the cursor on the first beat of a bar and press \"The Bar Starts at the Cursor\".");
        }
        SetWrapped(_analysisInfo, info);
        const LyricAlignment& l = _analysisReport.alignment;
        if (!_analysisReport.hasLyrics) {
            _lyricsSync->SetLabel(_("No lyrics: no face will sing."));
        } else {
            int synced = 0;
            for (const AlignedPhrase& p : l.phrases) {
                synced += p.startMS >= 0 && !p.instrumental ? 1 : 0;
            }
            _lyricsSync->SetLabel(synced == 0 ? _("The lyrics are loaded but not synchronized: no face will sing and no lyrics will be shown until the phrases are marked.")
                                              : wxString::Format(_("Lyrics synchronized: %d phrases, %d words and %d mouth shapes."), synced, static_cast<int>(l.words.size()), static_cast<int>(l.phonemes.size())));
        }
    }
    // How sure each mark is, and which ones to listen to first.
    wxString sure;
    const LyricAlignment& al = _analysisReport.alignment;
    if (_analysisReport.hasLyrics && !al.words.empty()) {
        double sum = 0;
        int timed = 0;
        for (const AlignedPhrase& p : al.phrases) {
            if (p.startMS >= 0 && !p.instrumental) {
                sum += p.confidence;
                ++timed;
            }
        }
        std::vector<const AlignedWord*> doubtful;
        for (const AlignedWord& w : al.words) {
            if (w.confidence < 0.5 && w.state == Provenance::Detected) {
                doubtful.push_back(&w);
            }
        }
        sure = wxString::Format(_("Confidence of the phrases %.2f on average (%s). %d of %d words are doubtful (below 0.5): listen to them first."), timed > 0 ? sum / timed : 0.0, U(al.quality),
                                static_cast<int>(doubtful.size()), static_cast<int>(al.words.size()));
        wxString list;
        int shown = 0;
        int lastPhraseStart = -1;
        for (const AlignedWord* w : doubtful) {
            const auto phrase = std::find_if(al.phrases.begin(), al.phrases.end(), [&](const AlignedPhrase& p) { return p.id == w->phraseId; });
            if (phrase == al.phrases.end() || phrase->startMS == lastPhraseStart || shown >= 4) {
                continue;
            }
            lastPhraseStart = phrase->startMS;
            list += wxString::Format(" %s \u00AB%s\u00BB;", Time(phrase->startMS), U(phrase->text.substr(0, 32)));
            ++shown;
        }
        if (!list.IsEmpty()) {
            sure += " " + _("First doubtful phrases:") + list;
        }
        const nlohmann::json ta = nlohmann::json::parse(_analysisReport.transcriptAlignment.empty() ? std::string("{}") : _analysisReport.transcriptAlignment, nullptr, false);
        if (!ta.is_discarded() && ta.contains("coverage")) {
            sure += " " + wxString::Format(_("The transcription heard %d of %d lyric words (%.0f%%); the rest keep estimated times."), ta.value("matchedWords", 0), ta.value("lyricWords", 0), ta.value("coverage", 0.0) * 100);
        }
    } else if (!_transcriptPath.empty()) {
        sure = _("A transcription is ready: it gives the words their times when the song is analyzed.");
    }
    SetWrapped(_alignmentInfo, sure);
    const int cursor = _timeline->Cursor();
    wxString at;
    for (const SongSection& s : a.sections) {
        if (cursor >= s.startMS && cursor < s.endMS) {
            at = wxString::Format(_("in the section %s-%s"), Time(s.startMS), Time(s.endMS));
        }
    }
    for (const AlignedPhrase& p : _analysisReport.alignment.phrases) {
        if (cursor >= p.startMS && cursor < p.endMS && !p.instrumental) {
            at += wxString::Format(_(", phrase \"%s\""), U(p.text));
        }
    }
    _cursorInfo->SetLabel(wxString::Format(_("Cursor %s %s"), Time(cursor), at));
    const AnalysisCorrections& c = _corrections.analysis;
    const int fixes = static_cast<int>(c.events.size() + c.sections.size()) + (c.bar.firstDownbeatMS >= 0 ? 1 : 0);
    _musicFixes->SetLabel(fixes == 0 ? _("No music corrections. They are kept with the show and win over the analysis in every version.")
                                     : wxString::Format(_("%d music corrections, kept with the show for every version."), fixes));
}

void ShowComposerDialog::RefreshComposition() {
    if (_levelList == nullptr) {
        return;
    }
    const ReportFolder& shown = _levels[static_cast<int>(Level())];
    _sceneStrip.clear();
    _sceneStripEnd = 0;
    const nlohmann::json dir = nlohmann::json::parse(shown.direction.empty() ? std::string("{}") : shown.direction, nullptr, false);
    if (!dir.is_discarded() && dir.value("enabled", false)) {
        for (const nlohmann::json& scene : dir.value("scenes", nlohmann::json::array())) {
            std::vector<std::string> colors = scene.value("colors", std::vector<std::string>());
            _sceneStrip.push_back({ scene.value("startMS", 0), colors });
            _sceneStripEnd = std::max(_sceneStripEnd, scene.value("endMS", 0));
        }
        wxString text = DirectionText(dir);
        const nlohmann::json m = nlohmann::json::parse(shown.directionMetrics.empty() ? std::string("{}") : shown.directionMetrics, nullptr, false);
        if (!m.is_discarded() && m.contains("coverageMean")) {
            text = wxString::Format(_("Display lit %.0f%% of the time with music, %.0f events a minute, %d hits on the beat; colours always the scene's: %.0f%%."), m.value("coverageMean", 0.0) * 100,
                                    m.value("eventsPerMinute", 0.0), m.value("hits", 0), m.value("paletteCoherence", 0.0) * 100) +
                   "\n" + text;
        }
        _directionText->ChangeValue(text);
    } else {
        _directionText->ChangeValue(shown.folder.empty() ? wxString(_("The art direction appears once the composition is worked out."))
                                                         : wxString(_("This profile does not direct the whole house: figures take turns. Choose a \"Reference\" style on the first step for themes, scenes and the ensemble.")));
    }
    _sceneView->Refresh();
    const bool stale = !_levelsKey.empty() && _levelsKey != InputsKey();
    if (_job.Running() && _purpose == Purpose::Composition) {
        _compositionInfo->SetLabel(wxString::Format(_("Working out level %d of 3..."), _jobLevel + 1));
    } else if (_levels[0].folder.empty()) {
        _compositionInfo->SetLabel(_("The composition has not been worked out yet."));
    } else if (stale) {
        _compositionInfo->SetLabel(_("Something changed since this composition was worked out: press \"Work Out the Composition\" to see its effect."));
    } else {
        _compositionInfo->SetLabel(wxString::Format(_("Level %s, seed %d. Choose a row to compare the levels; the numbers come from the choreography itself."), LevelText(Level()), _seed->GetValue()));
    }
    _levelList->DeleteAllItems();
    for (int i = 0; i < 3; ++i) {
        const ReportFolder& r = _levels[i];
        _levelList->InsertItem(i, LevelText(static_cast<AutomationLevel>(i)));
        if (!r.hasChoreography) {
            continue;
        }
        const LevelSummary s = SummarizeLevel(r);
        int col = 1;
        for (const wxString& v : { wxString::Format("%d", s.events), wxString::Format("%d", s.figures), wxString::Format("%d", s.planned), wxString::Format("%d", s.accents), wxString::Format("%d", s.templates),
                                   wxString::Format("%d", s.faces), wxString::Format("%d", s.texts), wxString::Format("%.0f%%", s.meanIntensity * 100), wxString::Format("%d", s.needsReview) }) {
            _levelList->SetItem(i, col++, v);
        }
    }
    _levelList->SetItemState(static_cast<long>(Level()), wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);

    const ReportFolder& r = _levels[static_cast<int>(Level())];
    _figures = SummarizeFigures(r, &_library, _corrections);
    const long selected = _figureList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    const std::string keep = selected >= 0 ? S(_figureList->GetItemText(selected)) : std::string();
    _figureList->DeleteAllItems();
    const int filter = _figureFilter->GetSelection();
    long row = 0;
    for (const FigureSummary& f : _figures) {
        const bool planned = f.leadSegments + f.supportSegments + f.accentSegments > 0;
        const bool show = filter == 0 || (filter == 1 && f.leadSegments > 0) || (filter == 2 && f.supportSegments > 0) || (filter == 3 && f.accentSegments > 0) || (filter == 4 && f.type == TargetType::Group) ||
                          (filter == 5 && f.type == TargetType::Model) || (filter == 6 && f.receivesFace) || (filter == 7 && f.receivesText) || (filter == 8 && (f.excluded || f.excludedByPerson)) ||
                          (filter == 9 && planned && f.events == 0);
        if (!show) {
            continue;
        }
        wxString part;
        if (f.leadSegments > 0) {
            part += wxString::Format(_("leads %d"), f.leadSegments);
        }
        if (f.supportSegments > 0) {
            part += (part.IsEmpty() ? "" : ", ") + wxString::Format(_("accompanies %d"), f.supportSegments);
        }
        if (f.accentSegments > 0) {
            part += (part.IsEmpty() ? "" : ", ") + wxString::Format(_("accents %d"), f.accentSegments);
        }
        wxString state = f.excludedByPerson ? _("Excluded by you") : f.excluded ? _("Excluded") : f.locked ? _("Locked") : !f.conflicts.empty() ? _("Conflict") : (planned && f.events == 0) ? _("Left dark") : f.receivesFace ? _("Sings") : f.receivesText ? _("Shows lyrics") : wxString();
        _figureList->InsertItem(row, U(f.name));
        _figureList->SetItem(row, 1, f.type == TargetType::Group ? _("Group") : RoleText(f.role));
        _figureList->SetItem(row, 2, part);
        _figureList->SetItem(row, 3, wxString::Format("%d", f.events));
        _figureList->SetItem(row, 4, state);
        if (S(U(f.name)) == keep) {
            _figureList->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
        ++row;
    }
    int locks = 0;
    int exclusions = 0;
    for (const ChoreographyCorrection& c : _corrections.choreography) {
        locks += c.action == ChoreographyCorrection::Action::Lock ? 1 : 0;
    }
    for (const PlanCorrection& c : _corrections.plan) {
        exclusions += c.action == PlanCorrection::Action::Exclude ? 1 : 0;
    }
    const int conflicts = r.hasChoreography ? static_cast<int>(r.choreography.conflicts.size() + r.plan.conflicts.size()) : 0;
    _correctionInfo->SetLabel(wxString::Format(_("Your decisions: %d locked effects, %d excluded figures. %d could not be applied (see the figure). They are kept for every version, whatever the level or seed."), locks, exclusions, conflicts));
    _cueList->DeleteAllItems();
    _mediaRows.clear();
    _rowShot.clear();
    _storyboard = VisualMediaStoryboard();
    std::string boardError;
    const json board = r.storyboard.empty() ? json() : json::parse(r.storyboard, nullptr, false);
    const bool hasBoard = board.is_object() && StoryboardFrom(board, _storyboard, boardError);
    if (hasBoard && !_storyboard.shots.empty() && (_storyboardHistory.empty() || _storyboardHistory.back().hash != _storyboard.hash)) {
        // Each montage seen here can be brought back; the last five are kept.
        const wxString now = wxDateTime::Now().Format("%H:%M:%S");
        _storyboardHistory.push_back(StoryboardVersionOf(_storyboard, _storyboardHistory.empty() ? 1 : _storyboardHistory.back().number + 1, S(now)));
        while (_storyboardHistory.size() > STORYBOARD_HISTORY + 1) {
            _storyboardHistory.erase(_storyboardHistory.begin());
        }
    }
    if (_storyboardView != nullptr) {
        std::map<std::string, wxBitmap> thumbs;
        const MediaLibrary* cached = hasBoard ? CachedLibrary() : nullptr;
        for (size_t k = 0; cached != nullptr && k < _storyboard.shots.size(); ++k) {
            const StoryboardShot& s = _storyboard.shots[k];
            const MediaItem* m = cached->Find(s.mediaId);
            if (m != nullptr && !m->thumbnail.empty() && _thumbCache.contains(m->thumbnail)) {
                thumbs[s.mediaId] = _thumbCache[m->thumbnail];
            }
        }
        if (cached != nullptr) {
            const MediaLibrary& library = *cached;
            for (const StoryboardShot& s : _storyboard.shots) {
                const MediaItem* m = library.Find(s.mediaId);
                if (thumbs.contains(s.mediaId)) {
                    continue;
                }
                wxImage image;
                if (m != nullptr && !m->thumbnail.empty() && !thumbs.contains(s.mediaId) && image.LoadFile(U(MediaAbsolutePath(m->thumbnail, _showDir)), wxBITMAP_TYPE_PNG) && image.IsOk()) {
                    const double k = 54.0 / std::max(1, image.GetHeight());
                    image.Rescale(std::max(1, static_cast<int>(image.GetWidth() * k)), 54, wxIMAGE_QUALITY_HIGH);
                    thumbs[s.mediaId] = wxBitmap(image);
                    _thumbCache[m->thumbnail] = thumbs[s.mediaId];
                }
            }
        }
        _storyboardView->SetStoryboard(hasBoard ? &_storyboard : nullptr, r.hasPlan ? r.plan.durationMS : 0, std::move(thumbs));
    }
    const json media = hasBoard ? json() : r.media.empty() ? json() : json::parse(r.media, nullptr, false);
    for (size_t i = 0; hasBoard && i < _storyboard.shots.size(); ++i) {
        const StoryboardShot& s = _storyboard.shots[i];
        MediaRow row;
        row.segmentId = s.sceneId;
        row.startMS = s.startMS;
        row.endMS = s.endMS;
        row.target = s.lane;
        row.mediaId = s.mediaId;
        row.name = s.name;
        row.path = s.source;
        row.state = DecisionStateId(s.state);
        row.why = ShotWhy(s);
        _mediaRows.push_back(std::move(row));
        _rowShot.push_back(static_cast<int>(i));
    }
    if (media.is_object()) {
        for (const json& a : media.value("assignments", json::array())) {
            MediaRow row;
            row.segmentId = a.value("segment", std::string());
            row.startMS = a.value("startMS", 0);
            row.endMS = a.value("endMS", 0);
            row.target = a.value("target", std::string());
            row.mediaId = a.value("media", std::string());
            row.name = a.value("name", std::string());
            row.path = a.value("path", std::string());
            row.state = a.value("state", std::string("automatic"));
            for (const json& why : a.value("reasons", json::array())) {
                const std::string code = why.value("code", std::string());
                const wxString detail = U(why.value("detail", std::string()));
                const wxString part = code == "theme-category" ? wxString::Format(_("theme: %s"), detail) : code == "aspect" ? wxString::Format(_("shape %s"), detail.BeforeFirst('(').Strip(wxString::both))
                                      : code == "energy"           ? wxString::Format(_("energy %s"), detail)
                                      : code == "repeat-keeps-media" ? _("same picture as the repeated section")
                                      : code == "instrumental"     ? _("no singing")
                                      : code == "chosen-by-person" || code == "locked-by-person" ? _("your choice")
                                      : code == "copied-into-show" ? _("copied into the show")
                                                                   : wxString();
                if (!part.empty()) {
                    row.why += (row.why.empty() ? "" : "; ") + part;
                }
            }
            _mediaRows.push_back(std::move(row));
            _rowShot.push_back(-1);
        }
    }
    for (const MediaCue& m : r.choreography.mediaCues) {
        if (std::any_of(_mediaRows.begin(), _mediaRows.end(), [&](const MediaRow& x) { return x.target == m.target && x.startMS < m.endMS && x.endMS > m.startMS; })) {
            continue;
        }
        MediaRow row;
        row.startMS = m.startMS;
        row.endMS = m.endMS;
        row.target = m.target;
        row.why = m.kind == "video" ? _("a video would fit here") : _("a picture would fit here");
        _mediaRows.push_back(std::move(row));
        _rowShot.push_back(-1);
    }
    long cue = 0;
    for (const MediaRow& m : _mediaRows) {
        _cueList->InsertItem(cue, Time(m.startMS));
        _cueList->SetItem(cue, 1, Time(m.endMS));
        _cueList->SetItem(cue, 2, U(m.target));
        _cueList->SetItem(cue, 3, m.name.empty() ? _("no file") : U(m.name));
        _cueList->SetItem(cue, 4, m.state.empty() ? _("Pending") : m.state == "locked" ? _("Kept") : m.state == "manual-edit" ? _("Your choice") : _("Automatic"));
        _cueList->SetItem(cue, 5, m.why);
        ++cue;
    }
    RefreshFigureDetails();
}

void ShowComposerDialog::RefreshFigureDetails() {
    if (_figureDetails == nullptr) {
        return;
    }
    const long selected = _figureList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (selected < 0) {
        _figureDetails->SetValue(_("Choose a figure to see what it does in this show."));
        return;
    }
    const std::string name = S(_figureList->GetItemText(selected));
    const auto f = std::find_if(_figures.begin(), _figures.end(), [&](const FigureSummary& x) { return x.name == name; });
    if (f == _figures.end()) {
        return;
    }
    wxString t = U(f->name) + "\n";
    t += wxString::Format(_("Kind: %s%s"), f->type == TargetType::Group ? _("Group") : RoleText(f->role), f->roleStatus == RoleStatus::Confirmed ? _(" (set by you)") : _(" (suggested from the layout)")) + "\n";
    if (f->noController) {
        t += _("It has no controller: it takes part, but the render cannot show it.") + "\n";
    }
    t += (f->hasFace ? _("It has a face definition.") : _("It has no face.")) + " " + (f->canShowText ? _("It can show text.") : _("It cannot show readable text.")) + "\n";
    if (f->excluded || f->excludedByPerson) {
        t += _("It is excluded: it stays dark, and no group lights it.") + "\n";
    }
    t += wxString::Format(_("In the plan: leads %d, accompanies %d, accents %d stretches. %d effects."), f->leadSegments, f->supportSegments, f->accentSegments, f->events) + "\n";
    if (!f->effects.empty()) {
        wxString effects;
        for (const std::string& e : f->effects) {
            effects += (effects.IsEmpty() ? "" : ", ") + wxGetTranslation(U(e));
        }
        t += wxString::Format(_("Effects: %s"), effects) + "\n";
    }
    if (f->receivesFace) {
        t += _("Its face sings the synchronized lyrics.") + "\n";
    }
    if (f->receivesText) {
        t += _("It shows the lyrics' phrases.") + "\n";
    }
    if (f->locked) {
        t += _("Its effects are locked: they stay as they are in every version.") + "\n";
    }
    for (const std::string& n : f->notes) {
        t += "- " + NoteText(n) + "\n";
    }
    for (const std::string& c : f->conflicts) {
        t += "! " + ConflictText(c) + "\n";
    }
    if (!f->templates.empty()) {
        wxString list;
        for (const auto& [id, compat] : f->templates) {
            list += (list.IsEmpty() ? "" : ", ") + U(id) + (compat == Compatibility::Compatible ? wxString() : compat == Compatibility::Limited ? _(" (limited)") : _(" (to check)"));
        }
        t += "\n" + wxString::Format(_("Techniques it can take: %s"), list);
    }
    _figureDetails->SetValue(t);
}

void ShowComposerDialog::RefreshGenerate() {
    if (_generateSummary == nullptr) {
        return;
    }
    wxString why;
    const bool ready = !_audioPath.empty() && _audioOk && _showOk && SequenceNameFree(why);
    const int fixes = static_cast<int>(_corrections.plan.size() + _corrections.choreography.size() + _corrections.analysis.events.size() + _corrections.analysis.sections.size()) + (_corrections.analysis.bar.firstDownbeatMS >= 0 ? 1 : 0);
    _generateSummary->SetLabel(ready ? wxString::Format(_("Song: %s\nInstallation: %s\nLevel: %s, seed %d. %d decisions of yours.\nIt will be saved as %s."), U(fs::path(_audioPath).filename().string()), U(_showDir), LevelText(Level()),
                                                        _seed->GetValue(), fixes, U(SequencePath()))
                                     : (why.IsEmpty() ? _("Choose a song and an installation on the first step.") : why));
    _generate->Enable(ready && !_job.Running());
    _cancel->Enable(_job.Running() && !_job.CancelRequested());
}

void ShowComposerDialog::RefreshResult() {
    if (_resultFacts == nullptr) {
        return;
    }
    _warningList->DeleteAllItems();
    _quietList->DeleteAllItems();
    _resultCues->DeleteAllItems();
    if (_resultSequence.empty() || _resultSummary.empty()) {
        _resultTitle->SetLabel(_("Review the result"));
        _resultFacts->SetLabel(_("No show has been generated in this window yet."));
        _reviewState->SetLabel("");
        return;
    }
    const json s = json::parse(_resultSummary, nullptr, false);
    if (!s.is_discarded() && s.contains("direction")) {
        const json& d = s["direction"];
        // The references' ranges, from the reference study, beside this show's numbers.
        _coherenceText->ChangeValue(wxString::Format(_("Theme: %s (%s, %.2f)\n"
                                                       "Display under an effect: %.0f%%   (references 87-99%%)\n"
                                                       "Events a minute: %.0f   (references 344-1974)\n"
                                                       "Starts shared by three or more figures: %.0f%%   (references 62-74%%)\n"
                                                       "Colour time in the two main hues: %.0f%%   (references 47-90%%); in the scene's colours: %.0f%%\n"
                                                       "Repeated sections that kept their look: %.0f%%; distinct looks over sections heard once: %.0f%%"),
                                                     ThemeName(d.value("themeName", std::string())), ThemeSource(d.value("source", std::string())), d.value("confidence", 0.0), d.value("coverage", 0.0) * 100,
                                                     d.value("eventsPerMinute", 0.0), d.value("startsTogether", 0.0) * 100, d.value("hueConcentration", 0.0) * 100, d.value("paletteCoherence", 0.0) * 100,
                                                     d.value("repeatConsistency", 0.0) * 100, d.value("lookDiversity", 0.0) * 100));
    } else {
        _coherenceText->ChangeValue(_("This show was made without a visual direction (the neutral profile), so there is nothing to compare with the references."));
    }
    if (s.is_discarded()) {
        return;
    }
    const json render = s.value("render", json::object());
    const json validation = s.value("validation", json::object());
    const int errors = validation.value("planErrors", 0) + validation.value("choreographyErrors", 0) + validation.value("sequenceErrors", 0) + validation.value("renderErrors", 0);
    const std::vector<std::string> faces = s.value("faceTargets", std::vector<std::string>());
    const std::vector<std::string> texts = s.value("textTargets", std::vector<std::string>());
    const std::string lyrics = s.value("lyricsQuality", std::string("none"));
    _resultTitle->SetLabel(wxString::Format(_("Done: %s"), U(fs::path(_resultSequence).filename().string())));
    wxString faceNames;
    for (size_t i = 0; i < faces.size() && i < 4; ++i) {
        faceNames += (faceNames.IsEmpty() ? "" : ", ") + U(faces[i]);
    }
    if (faces.size() > 4) {
        faceNames += wxString::Format(_(" and %d more"), static_cast<int>(faces.size() - 4));
    }
    wxString textNames;
    for (const std::string& t : texts) {
        textNames += (textNames.IsEmpty() ? "" : ", ") + U(t);
    }
    const wxString lyricText = lyrics == "none" ? _("no lyrics") : lyrics == "unsynchronized" ? _("lyrics not synchronized") : _("synchronized lyrics");
    const wxString facts = (wxString::Format(_("%s long, level %s, seed %d. %d effects on %d figures (%d in the plan).\nChecks: %s. Render: %s.\nLyrics: %s. Faces singing: %s. Lyrics shown on: %s. %d pictures or videos suggested."),
                                            Time(s.value("durationMS", 0)), LevelText(Level()), static_cast<int>(s.value("seed", 0)), s.value("effectsWritten", 0), s.value("targetsLit", 0), s.value("participants", 0),
                                            errors == 0 ? _("passed") : _("failed"), render.value("ran", false) ? wxString::Format(_("%d of %d frames, every effect lights its figure"), render.value("frames", 0), render.value("expectedFrames", 0)) : _("not run"),
                                            lyricText, faceNames.IsEmpty() ? _("none") : faceNames, textNames.IsEmpty() ? _("none") : textNames, static_cast<int>(s.value("mediaCues", 0))));
    wxString extra;
    wxString voiceNames;
    for (const json& v : s.value("voices", json::array())) {
        VoiceRole role = VoiceRole::Unknown;
        (void)VoiceRoleFromId(v.value("role", std::string()), role);
        const std::vector<std::string> vf = v.value("faces", std::vector<std::string>());
        voiceNames += (voiceNames.empty() ? "" : "; ") + U(v.value("name", std::string())) + " (" + VoiceRoleName(role) + ", " +
                      (v.value("facesOff", false) || vf.empty() ? _("no face") : U(std::accumulate(std::next(vf.begin()), vf.end(), vf.front(), [](std::string x, const std::string& y) { return x + ", " + y; }))) + ")";
    }
    if (!voiceNames.empty()) {
        extra += "\n" + wxString::Format(_("Voices: %s."), voiceNames);
    }
    const json mediaSummary = s.value("media", json::object());
    const json assigned = mediaSummary.value("assignments", json::array());
    if (!assigned.empty()) {
        extra += "\n" + wxString::Format(_("Pictures and videos: %d, %.0f%% of the song, from your files."), static_cast<int>(assigned.size()), mediaSummary.value("share", 0.0) * 100);
    }
    const json montage = s.value("storyboard", json());
    if (montage.is_object()) {
        wxString lanes;
        for (const json& l : montage.value("lanes", json::array())) {
            lanes += wxString::Format(_("%s %d shots, %.0f%% of its free time; "), U(l.value("lane", std::string())), l.value("shots", 0), l.value("ofFree", 0.0) * 100);
        }
        const std::string mode = montage.value("analysis", std::string());
        extra += "\n" + wxString::Format(_("Visual montage: %s%d files (%d pictures, %d videos), %d prepared pictures, %d repeats (%d where the music repeats), %d stretches left to the lights. Analysis: %s."), lanes,
                                         montage.value("files", 0), montage.value("images", 0), montage.value("videos", 0), montage.value("derived", 0), montage.value("repeats", 0), montage.value("repeatsByMusic", 0),
                                         montage.value("gaps", 0), mode == "semantic" ? _("what each file shows, with the image model") : mode == "mixed" ? _("partly with the image model") : _("colours, light and motion only"));
        wxString screens;
        for (const json& l : montage.value("screens", json::array())) {
            wxString cover;
            for (const json& x : montage.value("lanes", json::array())) {
                if (x.value("lane", std::string()) == l.value("target", std::string())) {
                    cover = wxString::Format(_(", %.0f%% of the song"), x.value("coverage", 0.0) * 100);
                }
            }
            screens += (screens.empty() ? "" : "; ") + U(l.value("target", std::string())) + " (" + ScreenRoleNameOf(l.value("screen", std::string())) +
                       (l.value("gapsOnly", false) ? wxString(_(", between phrases")) : wxString()) + cover + ")";
        }
        if (!screens.empty()) {
            LyricsMode lm = LyricsMode::Smart;
            (void)LyricsModeFromId(montage.value("lyricsMode", std::string("smart")), lm);
            wxString moves;
            const json transitions = montage.value("transitions", json::object());
            for (const auto& [k, v] : transitions.items()) {
                moves += (moves.empty() ? "" : ", ") + wxString::Format("%s %d", TransitionName(k), v.is_number() ? v.get<int>() : 0);
            }
            extra += "\n" + wxString::Format(_("Screens: %s. Words and pictures: %s."), screens, LyricsModeName(lm));
            extra += "\n" + wxString::Format(_("Shots of %.1f s on average; transitions: %s; %d pictures between phrases; %d moving clips. Related shots one after another: %.0f%%; between the screens at the same time: %.0f%%."),
                                             montage.value("meanShotMS", 0) / 1000.0, moves.empty() ? wxString(_("none")) : moves, montage.value("overlays", 0), montage.value("motionClips", 0),
                                             montage.value("coherence", 0.0) * 100, montage.value("crossScreen", 0.0) * 100);
        }
    }
    SetWrapped(_resultFacts, facts + extra);
    const VisualReview shown = ResultReview();
    if (shown.state == "approved") {
        _reviewState->SetLabel(wxString::Format(_("Reviewed and approved by a person: %s, %s."), U(shown.reviewer), U(shown.approvedAt)));
    } else if (shown.state == "in-progress") {
        const ReviewCounts n = CountReview(shown);
        _reviewState->SetLabel(wxString::Format(_("Review in progress: %d of %d shots decided. Pending visual review: watch it in xLights before showing it."), n.clips - n.pending, n.clips));
    } else {
        _reviewState->SetLabel(render.value("ran", false) ? _("Rendered correctly. Pending visual review: watch it in xLights before showing it.") : _("Pending visual review: watch it in xLights before showing it."));
    }
    long w = 0;
    const auto warn = [&](const wxString& text) { _warningList->InsertItem(w++, text); };
    if (lyrics == "unsynchronized") {
        warn(_("The lyrics are not synchronized, so no face sings. Mark the phrases and make another version."));
    }
    {
        const json screens = _result.screens.empty() ? json() : json::parse(_result.screens, nullptr, false);
        if (screens.is_object()) {
            SurfacePlan sp;
            for (const json& x : screens.value("warnings", json::array())) {
                sp.warnings.push_back(x.get<std::string>());
            }
            for (const wxString& line : ScreenWarnings(sp)) {
                warn(line);
            }
        }
    }
    if (faces.empty() && lyrics != "none" && lyrics != "unsynchronized") {
        warn(_("No face sings: no figure with a face took part."));
    }
    const ReportFolder& r = _result;
    if (r.hasChoreography && r.choreography.metrics.needsReview > 0) {
        warn(wxString::Format(_("%d effects are best checked by eye, for example lyrics that scroll on a small matrix."), r.choreography.metrics.needsReview));
    }
    if (r.hasChoreography && (!r.choreography.conflicts.empty() || !r.plan.conflicts.empty())) {
        warn(wxString::Format(_("%d of your decisions could not be applied; the earlier ones were kept. See the composition."), static_cast<int>(r.choreography.conflicts.size() + r.plan.conflicts.size())));
    }
    if (validation.value("warnings", 0) > 0) {
        warn(wxString::Format(_("%d minor notices from the checks; the technical report lists them."), validation.value("warnings", 0)));
    }
    if (r.hasAnalysis && std::none_of(r.analysis.events.begin(), r.analysis.events.end(), [](const MusicalEvent& e) { return e.type == MusicalEventType::Downbeat; })) {
        warn(_("No strong beat was found, so there are no accents. Mark the bar in Analysis and make another version."));
    }
    warn(_("Show Composer's techniques are starting values that nobody has approved by eye yet."));
    long q = 0;
    for (const json& c : s.value("coverage", json::array())) {
        if (c.value("planned", 0) <= 0 || c.value("events", 0) > 0) {
            continue;
        }
        wxString why;
        std::string notes = c.value("note", std::string());
        std::stringstream parts(notes);
        std::string part;
        while (std::getline(parts, part, ',')) {
            part.erase(0, part.find_first_not_of(' '));
            why += (why.IsEmpty() ? "" : " ") + NoteText(part.substr(0, part.find(':')));
        }
        _quietList->InsertItem(q, U(c.value("target", std::string())));
        _quietList->SetItem(q, 1, why.IsEmpty() ? _("No reason was recorded.") : why);
        ++q;
    }
    long m = 0;
    for (const json& c : s.value("media", json::object()).value("assignments", json::array())) {
        _resultCues->InsertItem(m, Time(c.value("startMS", 0)));
        _resultCues->SetItem(m, 1, Time(c.value("endMS", 0)));
        _resultCues->SetItem(m, 2, U(c.value("target", std::string())));
        _resultCues->SetItem(m, 3, U(c.value("name", std::string())));
        _resultCues->SetItem(m, 4, c.value("state", std::string()) == "automatic" ? _("Automatic") : _("Your choice"));
        ++m;
    }
    for (const json& c : s.value("mediaCueList", json::array())) {
        _resultCues->InsertItem(m, Time(c.value("startMS", 0)));
        _resultCues->SetItem(m, 1, Time(c.value("endMS", 0)));
        _resultCues->SetItem(m, 2, U(c.value("target", std::string())));
        _resultCues->SetItem(m, 3, c.value("kind", std::string()) == "video" ? _("Video") : _("Picture"));
        _resultCues->SetItem(m, 4, _("Pending"));
        ++m;
    }
}

// ------------------------------------------------------------------ pictures, videos and voices

void ShowComposerDialog::SetMediaFolders(const std::vector<std::string>& folders) {
    _mediaFolders = folders;
    RefreshAll();
}

void ShowComposerDialog::SetMediaAutomatic(bool on) {
    _mediaAuto->SetValue(on);
    RefreshAll();
}

void ShowComposerDialog::SetCopyMedia(bool on) {
    _copyMedia->SetValue(on);
}

void ShowComposerDialog::EditMediaLibrary() {
    if (_showDir.empty() || !_showOk) {
        return;
    }
    MediaLibraryDialog dialog(this, _showDir, _mediaFolders);
    if (dialog.ShowModal() == wxID_OK) {
        _mediaFolders = dialog.Folders();
        RefreshAll();
    }
}

std::vector<VoiceDef> ShowComposerDialog::CurrentVoices() const {
    std::vector<VoiceDef> cast;
    const json s = _analysisReport.summary.empty() ? json() : json::parse(_analysisReport.summary, nullptr, false);
    if (s.is_object()) {
        for (const json& v : s.value("voices", json::array())) {
            VoiceDef d;
            d.id = v.value("id", std::string());
            d.name = v.value("name", d.id);
            (void)VoiceRoleFromId(v.value("role", std::string("unknown")), d.role);
            d.delivery = v.value("delivery", std::string()) == "spoken" ? Delivery::Spoken : Delivery::Sung;
            d.faces = v.value("faces", std::vector<std::string>());
            d.facesOff = v.value("facesOff", false);
            d.source = "lyrics";
            cast.push_back(std::move(d));
        }
    }
    for (const VoiceDef& e : _corrections.voices) {
        std::erase_if(cast, [&](const VoiceDef& v) { return v.id == e.id; });
        cast.push_back(e);
    }
    return cast;
}

void ShowComposerDialog::EditVoices() {
    if (!_analysisReport.hasLyrics) {
        wxMessageBox(_("Analyze the song with its lyrics first: the voices come from the lyrics."), _("Voices"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    std::vector<VoicePhrase> phrases;
    for (const AlignedPhrase& p : _analysisReport.alignment.phrases) {
        if (p.instrumental) {
            continue;
        }
        VoicePhrase v;
        v.id = p.id;
        v.text = p.text;
        v.startMS = p.startMS;
        v.endMS = p.endMS;
        v.voices = p.voices;
        v.spoken = p.delivery == "spoken";
        v.words = static_cast<int>(std::count_if(_analysisReport.alignment.words.begin(), _analysisReport.alignment.words.end(), [&](const AlignedWord& w) { return w.phraseId == p.id; }));
        phrases.push_back(std::move(v));
    }
    std::vector<std::string> faces;
    for (const TargetInfo& t : _capabilities.targets) {
        if (t.type == TargetType::Model && !t.faces.empty() && !t.Excluded()) {
            faces.push_back(t.name);
        }
    }
    VoicesDialog dialog(this, CurrentVoices(), phrases, faces, [this](int start, int length) {
        if (_audio != nullptr && _audioOk) {
            _audio->Stop();
            _audio->Play(start, length);
        }
    });
    if (dialog.ShowModal() == wxID_OK) {
        ApplyVoiceEdits(dialog.Voices(), dialog.Corrections());
    }
}

void ShowComposerDialog::ApplyVoiceEdits(const std::vector<VoiceDef>& voices, const std::vector<LyricCorrection>& corrections) {
    CorrectionSet extra;
    extra.voices = voices;
    extra.lyrics = corrections;
    MergeCorrections(_corrections, extra);
    RefreshAll();
}

void ShowComposerDialog::ApplyMediaCorrection(const MediaCorrection& correction) {
    UpsertMediaCorrection(_corrections.media, correction);
    RefreshAll();
}

void ShowComposerDialog::ChangeMedia(int action) {
    const ReportFolder& level = _levels[static_cast<int>(Level())];
    const long selected = _cueList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    MediaRow row;
    if (selected >= 0 && selected < static_cast<long>(_mediaRows.size())) {
        row = _mediaRows[selected];
    } else if (action != 0 || !level.hasPlan) {
        wxMessageBox(_("Choose a row of the list first."), _("Pictures and Videos"), wxOK | wxICON_INFORMATION, this);
        return;
    } else {
        // No row: the person picks the section.
        wxArrayString names;
        std::vector<const PlanSegment*> segments;
        for (const PlanSegment& s : level.plan.segments) {
            if (s.endMS - s.startMS >= 3000) {
                names.Add(wxString::Format("%s - %s  %s", Time(s.startMS), Time(s.endMS), U(s.sectionKind)));
                segments.push_back(&s);
            }
        }
        wxSingleChoiceDialog pick(this, _("Where should the picture or video play?"), _("Pictures and Videos"), names);
        if (segments.empty() || pick.ShowModal() != wxID_OK) {
            return;
        }
        row.segmentId = segments[pick.GetSelection()]->id;
        row.startMS = segments[pick.GetSelection()]->startMS;
    }
    if (row.segmentId.empty() && level.hasPlan) {
        for (const PlanSegment& s : level.plan.segments) {
            if (row.startMS >= s.startMS && row.startMS < s.endMS) {
                row.segmentId = s.id;
                row.startMS = s.startMS;
            }
        }
    }
    if (row.segmentId.empty()) {
        return;
    }
    MediaCorrection c;
    c.segmentId = row.segmentId;
    c.startMS = row.startMS;
    c.target = row.target;
    if (action == 1) {
        c.action = MediaCorrection::Action::Remove;
    } else if (action == 2) {
        if (row.mediaId.empty()) {
            return;
        }
        c.action = MediaCorrection::Action::Lock;
        c.mediaId = row.mediaId;
        c.path = row.path;
    } else {
        MediaLibrary library;
        std::string error;
        if (!ReadMediaLibrary(MediaLibraryPath(_showDir), library, error) || library.items.empty()) {
            wxMessageBox(_("There are no pictures or videos yet: add a folder in \"Pictures and Videos...\"."), _("Pictures and Videos"), wxOK | wxICON_INFORMATION, this);
            return;
        }
        MediaChooserDialog chooser(this, _showDir, library);
        if (chooser.ShowModal() != wxID_OK || chooser.Chosen().empty()) {
            return;
        }
        const MediaItem* m = library.Find(chooser.Chosen());
        c.action = MediaCorrection::Action::Assign;
        c.mediaId = m->id;
        c.path = m->relative.empty() ? m->path : m->relative;
    }
    ApplyMediaCorrection(c);
}

// ------------------------------------------------------------------ visual montage

std::string ShowComposerDialog::VisualModelDir() const {
    if (!VisualModelSupported() || _showDir.empty()) {
        return {};
    }
    std::vector<std::string> roots = { _showDir };
    roots.insert(roots.end(), _mediaFolders.begin(), _mediaFolders.end());
    return FindVisualModel(roots);
}

bool ShowComposerDialog::MediaAnalysisRunning() const {
    // Until the timer has collected its result, so the summary read next is this run's.
    return _analysisRunner != nullptr;
}

void ShowComposerDialog::StartMediaAnalysis() {
    if (MediaAnalysisRunning() || _mediaFolders.empty() || _showDir.empty()) {
        return;
    }
    _analysisRunner = std::make_unique<MediaAnalysisRunner>();
    _analysisRunner->Start(_showDir, _mediaFolders, VisualModelDir());
    RefreshMediaAnalysisState();
}

void ShowComposerDialog::RefreshMediaAnalysisState() {
    if (_analysisState == nullptr) {
        return;
    }
    const bool running = MediaAnalysisRunning();
    _analyzeButton->SetLabel(running ? _("Stop the Analysis") : _("Analyze the Library"));
    _analyzeButton->Enable(running || (!_mediaFolders.empty() && _showOk && _modelRunner == nullptr));
    const bool offerModel = VisualModelSupported() && !_showDir.empty() && VisualModelDir().empty();
    _modelButton->Enable(offerModel && _modelRunner == nullptr && !running);
    if (running) {
        const int total = _analysisRunner->Total();
        _analysisState->SetLabel(total > 0 ? wxString::Format(_("Analysing %d of %d: %s"), _analysisRunner->Done(), total, U(_analysisRunner->Current()))
                                           : wxString::Format(_("Searching the folders: %d files read"), _analysisRunner->Done()));
        return;
    }
    if (_modelRunner != nullptr) {
        return;
    }
    const MediaLibrary* cached = _mediaFolders.empty() ? nullptr : CachedLibrary();
    if (cached == nullptr) {
        _analysisState->SetLabel(_mediaFolders.empty() ? wxString() : _("Not analysed yet."));
        return;
    }
    const MediaLibrary& library = *cached;
    int images = 0, videos = 0, usable = 0, unsupported = 0, damaged = 0, analysed = 0, semantic = 0;
    for (const MediaItem& m : library.items) {
        images += m.kind == MediaKind::Image || m.kind == MediaKind::Animation ? 1 : 0;
        videos += m.kind == MediaKind::Video ? 1 : 0;
        usable += m.Usable() ? 1 : 0;
        unsupported += m.status == MediaStatus::Unsupported ? 1 : 0;
        damaged += m.status == MediaStatus::Unreadable ? 1 : 0;
        analysed += m.Usable() && m.analysis.Done() ? 1 : 0;
        semantic += m.analysis.mode == "semantic" ? 1 : 0;
    }
    wxString text = wxString::Format(_("%d files: %d pictures, %d videos; %d usable, %d not supported, %d damaged. Analysed: %d of %d"), static_cast<int>(library.items.size()), images, videos, usable,
                                     unsupported, damaged, analysed, usable);
    text += semantic > 0 ? wxString::Format(_(", %d with the image model"), semantic) : wxString(_(", colours, light and motion only (no image model)"));
    if (_lastMediaAnalysis.total > 0) {
        text += wxString::Format(_("; last run: %d looked at, %d from the cache%s"), _lastMediaAnalysis.analysed, _lastMediaAnalysis.cached, _lastMediaAnalysis.cancelled ? wxString(_(", stopped")) : wxString());
    }
    _analysisState->SetLabel(text + ".");
    _analysisState->SetToolTip(text);
}

void ShowComposerDialog::GetVisualModel() {
    if (!VisualModelSupported()) {
        wxMessageBox(_("This build of xLights cannot run the image model. The library is analysed by its colours, light and motion."), _("Pictures and Videos"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (_showDir.empty() || _modelRunner != nullptr) {
        return;
    }
    const bool yes = _testMode ? _testConsent
                               : wxMessageBox(_("Download the image model SigLIP (Google, Apache 2.0 licence), about 210 MB, from Hugging Face into the show's ai-models folder?\n\n"
                                                "It is downloaded once and runs on this computer: your pictures and videos are never sent anywhere."),
                                              _("Image Model"), wxYES_NO | wxICON_QUESTION, this) == wxYES;
    if (!yes) {
        return;
    }
    _modelRunner = std::make_unique<ModelDownloadRunner>();
    _modelRunner->Start(_showDir);
    RefreshMediaAnalysisState();
}

void ShowComposerDialog::LockStoryboardScene(const std::string& sceneId) {
    for (const MediaCorrection& c : composer::LockStoryboardScene(_storyboard, sceneId)) {
        UpsertMediaCorrection(_corrections.media, c);
    }
    RefreshAll();
}

void ShowComposerDialog::LockStoryboardLane(const std::string& lane) {
    for (const MediaCorrection& c : composer::LockStoryboardLane(_storyboard, lane)) {
        UpsertMediaCorrection(_corrections.media, c);
    }
    RefreshAll();
}

void ShowComposerDialog::SetScreens(const ScreenSettings& screens) {
    _corrections.screens = screens;
    RefreshAll();
}

SurfacePlan ShowComposerDialog::ScreensPreview() const {
    const ReportFolder& level = _levels[static_cast<int>(Level())];
    return PlanSurfaces(_capabilities, level.hasPlan ? &level.plan : nullptr, _corrections.screens);
}

void ShowComposerDialog::EditScreens() {
    if (!_showOk) {
        return;
    }
    const ReportFolder& level = _levels[static_cast<int>(Level())];
    ScreensDialog dialog(this, _capabilities, level.hasPlan ? &level.plan : nullptr, _corrections.screens);
    if (dialog.ShowModal() == wxID_OK) {
        SetScreens(dialog.Settings());
    }
}

const MediaLibrary* ShowComposerDialog::CachedLibrary() const {
    if (_showDir.empty()) {
        return nullptr;
    }
    const std::string path = MediaLibraryPath(_showDir);
    std::error_code ec;
    const auto stamp = fs::last_write_time(fs::path(path), ec);
    const std::string key = ec ? std::string() : path + "|" + std::to_string(stamp.time_since_epoch().count());
    if (key.empty()) {
        _libraryCacheKey.clear();
        _libraryCacheOk = false;
        return nullptr;
    }
    if (key != _libraryCacheKey) {
        std::string error;
        _libraryCacheOk = ReadMediaLibrary(path, _libraryCache, error);
        _libraryCacheKey = key;
    }
    return _libraryCacheOk ? &_libraryCache : nullptr;
}

VisualReview ShowComposerDialog::ResultReview() const {
    VisualReview r;
    if (_resultSequence.empty()) {
        return r;
    }
    // Read once and kept while the sidecar does not change: a whole sidecar is slow to
    // read on every refresh of the window.
    const std::string path = SidecarPathFor(_resultSequence);
    std::error_code ec;
    const auto stamp = fs::last_write_time(fs::path(path), ec);
    const std::string key = path + "|" + std::to_string(ec ? 0 : stamp.time_since_epoch().count());
    if (key != _reviewCacheKey) {
        GenerationSidecar sidecar;
        std::string error;
        _reviewCache = ReadSidecar(path, sidecar, error) == SidecarStatus::Ok ? sidecar.review : VisualReview();
        _reviewCacheKey = key;
    }
    r = _reviewCache;
    // A show approved with the old window: its record still counts.
    fs::path legacy = _resultSequence;
    legacy.replace_extension(".composer-review.json");
    if (r.state != "approved" && FileExists(legacy.string())) {
        VisualReview old;
        if (LegacyReviewFrom(json::parse(ReadFile(legacy), nullptr, false), old)) {
            r.state = old.state;
            r.reviewer = old.reviewer;
            r.approvedAt = old.approvedAt;
            r.legacy = true;
        }
    }
    return r;
}

ArtisticPreferences ShowComposerDialog::ShowPreferences() const {
    ArtisticPreferences p;
    std::string error;
    if (!_showDir.empty()) {
        (void)ReadPreferences(PreferencesPath(_showDir), p, error);
    }
    return p;
}

bool ShowComposerDialog::SetShowPreferences(const ArtisticPreferences& prefs) {
    std::string error;
    return !_showDir.empty() && SavePreferences(PreferencesPath(_showDir), prefs, error);
}

bool ShowComposerDialog::SaveReview(const VisualReview& review, const std::vector<std::string>& neverUse) {
    if (_resultSequence.empty()) {
        return false;
    }
    const std::string path = SidecarPathFor(_resultSequence);
    GenerationSidecar sidecar;
    std::string error;
    if (ReadSidecar(path, sidecar, error) != SidecarStatus::Ok) {
        ShowError(_("The review could not be kept."), U(error));
        return false;
    }
    sidecar.review = review;
    sidecar.visualReview = review.state == "approved" ? "approved" : "not-reviewed";
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(fs::path(tmp), std::ios::binary | std::ios::trunc);
        out << ToJson(sidecar);
        if (!out) {
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        return false;
    }
    if (review.state == "approved") {
        // The record older versions of the window read.
        fs::path legacy = _resultSequence;
        legacy.replace_extension(".composer-review.json");
        const json record = { { "schemaVersion", 1 }, { "sequence", fs::path(_resultSequence).filename().string() }, { "reviewedBy", review.reviewer }, { "reviewedAt", review.approvedAt }, { "state", "approved" } };
        std::ofstream(legacy, std::ios::binary) << record.dump(1);
    }
    MediaLibrary library;
    const bool haveLibrary = ReadMediaLibrary(MediaLibraryPath(_showDir), library, error);
    if (haveLibrary && !neverUse.empty()) {
        for (const std::string& id : neverUse) {
            if (MediaItem* m = library.Find(id); m != nullptr) {
                m->disabled = true; // a person's permanent choice, not something learned
            }
        }
        (void)SaveMediaLibrary(MediaLibraryPath(_showDir), library, error);
    }
    ArtisticPreferences prefs = ShowPreferences();
    if (LearnFromReview(prefs, review, haveLibrary ? &library : nullptr, _result.plan.profileId) > 0) {
        (void)SetShowPreferences(prefs);
    }
    RefreshResult();
    return true;
}

void ShowComposerDialog::ApplyReview(const VisualReview& review) {
    VisualMediaStoryboard board;
    std::string error;
    const json j = _result.storyboard.empty() ? json() : json::parse(_result.storyboard, nullptr, false);
    if (!j.is_object() || !StoryboardFrom(j, board, error)) {
        return;
    }
    for (const MediaCorrection& c : ReviewCorrections(review, board)) {
        UpsertMediaCorrection(_corrections.media, c);
    }
    _corrections.review = review;
    // A new version beside this one: generation never replaces a file.
    const fs::path stem = fs::path(_resultSequence).stem();
    std::string base = stem.string();
    if (const size_t v = base.rfind("-v"); v != std::string::npos && v + 2 < base.size() && std::all_of(base.begin() + static_cast<long>(v) + 2, base.end(), ::isdigit)) {
        base = base.substr(0, v);
    }
    for (int n = 2; n < 1000; ++n) {
        const std::string name = fmt::format("{}-v{}.xsq", base, n);
        if (!FileExists((fs::path(_showDir) / name).string())) {
            SetSequenceName(name);
            break;
        }
    }
    StartGeneration();
}

ReviewContext ShowComposerDialog::MakeReviewContext() const {
    ReviewContext c;
    c.sequence = fs::path(_resultSequence).filename().string();
    c.song = fs::path(_audioPath).filename().string();
    c.durationMS = _result.hasPlan ? _result.plan.durationMS : 0;
    VisualMediaStoryboard board;
    std::string error;
    const json j = _result.storyboard.empty() ? json() : json::parse(_result.storyboard, nullptr, false);
    if (j.is_object() && StoryboardFrom(j, board, error)) {
        for (const StoryboardLane& l : board.lanes) {
            c.screens.push_back(l.target + " (" + S(ScreenRoleNameOf(l.screen)) + ")");
        }
        c.transitions = board.stats.transitions;
        int doubtful = 0;
        for (const StoryboardShot& s : board.shots) {
            doubtful += s.level == "doubtful" ? 1 : 0;
        }
        if (doubtful > 0) {
            c.warnings.push_back(wxString::Format(_("%d shots the program was not sure about."), doubtful));
        }
    }
    const json screens = _result.screens.empty() ? json() : json::parse(_result.screens, nullptr, false);
    if (screens.is_object()) {
        SurfacePlan sp;
        for (const json& x : screens.value("warnings", json::array())) {
            sp.warnings.push_back(x.get<std::string>());
        }
        for (const wxString& line : ScreenWarnings(sp)) {
            c.warnings.push_back(line);
        }
    }
    return c;
}

void ShowComposerDialog::EditReview() {
    if (_resultSequence.empty()) {
        return;
    }
    const VisualReview current = ResultReview();
    if (current.clips.empty() && current.state != "approved") {
        wxMessageBox(_("This show has no pictures or videos to review. Watch it in xLights before showing it."), _("Review the Pictures and Videos"), wxOK | wxICON_INFORMATION, this);
    }
    MediaLibrary library;
    std::string error;
    (void)ReadMediaLibrary(MediaLibraryPath(_showDir), library, error);
    ReviewDialog dialog(this, current, MakeReviewContext(), library, _showDir);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }
    if (SaveReview(dialog.Review(), dialog.NeverUse()) && dialog.ApplyAsked()) {
        ApplyReview(dialog.Review());
    }
}

void ShowComposerDialog::EditPreferences() {
    if (_showDir.empty()) {
        return;
    }
    MediaLibrary library;
    std::string error;
    const bool have = ReadMediaLibrary(MediaLibraryPath(_showDir), library, error);
    PreferencesDialog dialog(this, ShowPreferences(), have ? &library : nullptr);
    if (dialog.ShowModal() == wxID_OK) {
        (void)SetShowPreferences(dialog.Preferences());
    }
}

void ShowComposerDialog::MoveShotToOtherScreen(int shot) {
    if (shot < 0 || shot >= static_cast<int>(_storyboard.shots.size())) {
        return;
    }
    const StoryboardShot s = _storyboard.shots[static_cast<size_t>(shot)];
    const auto other = std::find_if(_storyboard.lanes.begin(), _storyboard.lanes.end(), [&](const StoryboardLane& l) { return l.target != s.lane; });
    const StoryboardScene* scene = _storyboard.Scene(s.sceneId);
    if (other == _storyboard.lanes.end() || scene == nullptr) {
        return;
    }
    MediaCorrection off;
    off.action = MediaCorrection::Action::Remove;
    off.segmentId = s.sceneId;
    off.startMS = scene->startMS;
    off.target = s.lane;
    UpsertMediaCorrection(_corrections.media, off);
    MediaCorrection on;
    on.action = MediaCorrection::Action::Lock;
    on.segmentId = s.sceneId;
    on.startMS = scene->startMS;
    on.target = other->target;
    on.mediaId = s.mediaId;
    on.path = s.source;
    UpsertMediaCorrection(_corrections.media, on);
    RefreshAll();
}

void ShowComposerDialog::RebuildMontage() {
    ++_mediaVariant;
    RefreshAll();
    if (!_job.Running()) {
        StartComposition();
    }
}

void ShowComposerDialog::RestoreStoryboardVersion(size_t index) {
    if (index >= _storyboardHistory.size()) {
        return;
    }
    for (const MediaCorrection& c : RestoreStoryboard(_storyboardHistory[index], _storyboard)) {
        UpsertMediaCorrection(_corrections.media, c);
    }
    RefreshAll();
    if (!_job.Running()) {
        StartComposition();
    }
}

void ShowComposerDialog::ChooseStoryboardVersion() {
    if (_storyboardHistory.size() < 2) {
        wxMessageBox(_("There is no earlier montage yet: rebuild it or change it first."), _("Pictures and Videos"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    wxArrayString names;
    for (size_t i = 0; i + 1 < _storyboardHistory.size(); ++i) {
        const StoryboardVersion& v = _storyboardHistory[i];
        names.Add(wxString::Format(_("Version %d, made at %s: %d shots"), v.number, U(v.created), static_cast<int>(v.shots.size())));
    }
    wxSingleChoiceDialog pick(this, _("Which montage should come back? Every shot is kept as it was."), _("Earlier Versions"), names);
    if (pick.ShowModal() == wxID_OK) {
        RestoreStoryboardVersion(static_cast<size_t>(pick.GetSelection()));
    }
}
