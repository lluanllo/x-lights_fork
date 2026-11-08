/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ComposerUITest.h"

#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/menu.h>
#include <wx/timer.h>

#ifdef __WXMSW__
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <sstream>

#include <spdlog/fmt/fmt.h>

#include "ComposerJob.h"
#include "ComposerMediaDialogs.h"
#include "ShowComposerDialog.h"
#include "composer/ComposerReview.h"
#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"
#include "composer/MediaLibrary.h"
#include "composer/MediaTest.h"
#include "composer/VisualTest.h"
#include "composer/WriterTest.h"
#include "utils/ExternalHooks.h"
#include "xLightsMain.h"

namespace fs = std::filesystem;
using namespace composer;

namespace {

class Driver : public wxEvtHandler {
public:
    Driver(xLightsFrame* frame, const ComposerUITestOptions& options) :
        _frame(frame),
        _o(options),
        _timer(this) {
        Bind(wxEVT_TIMER, &Driver::OnTick, this);
        fs::create_directories(fs::path(_o.outDir) / "capturas");
        _log.open(fs::path(_o.outDir) / "ui-test.log", std::ios::binary);
        BuildSteps();
        ShowComposerDialog::OpenedHook() = [this](ShowComposerDialog* d) {
            _dialog = d;
            d->SetTestMode(true);
            d->Bind(wxEVT_DESTROY, [this, d](wxWindowDestroyEvent& e) {
                if (e.GetWindow() == d) {
                    _dialog = nullptr;
                    ++_closed;
                }
                e.Skip();
            });
        };
        _last = std::chrono::steady_clock::now();
        _timer.Start(200);
    }

private:
    struct Step {
        std::string name;
        std::function<bool()> run; // true when done
        int timeoutS;
    };

    void Log(const std::string& text) {
        _log << text << "\n";
        _log.flush();
        printf("%s\n", text.c_str());
        fflush(stdout);
    }

    void Check(bool ok, const std::string& what) {
        Log(fmt::format("  {} {}", ok ? "OK    " : "FAILED", what));
        ok ? ++_passed : ++_failed;
    }

    static void PaintTree(wxWindow* window) {
        window->Refresh();
        window->Update();
        for (wxWindow* child : window->GetChildren()) {
            PaintTree(child);
        }
    }

    void Capture(wxWindow* window, const std::string& name) {
        if (window == nullptr) {
            return;
        }
        window->Raise();
        window->Layout();
        PaintTree(window);
        const wxSize size = window->GetSize();
        wxBitmap bitmap(size);
        {
            wxMemoryDC memory(bitmap);
#ifdef __WXMSW__
            // What the window draws, not what happens to be on the screen over it.
            ::PrintWindow(window->GetHWND(), memory.GetHDC(), 2 /* PW_RENDERFULLCONTENT */);
#else
            wxWindowDC dc(window);
            memory.Blit(0, 0, size.GetWidth(), size.GetHeight(), &dc, 0, 0);
#endif
        }
        const fs::path file = fs::path(_o.outDir) / "capturas" / fmt::format("{:02d}-{}.png", ++_shots, name);
        bitmap.SaveFile(wxString::FromUTF8(file.string()), wxBITMAP_TYPE_PNG);
        Log("  capture " + file.filename().string());
        // Forcing every window to paint is the test's own time, not the window's.
        _last = std::chrono::steady_clock::now();
    }

    bool Waited(double seconds) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - _stepStart).count() >= seconds;
    }

    bool JobDone() const {
        return _dialog != nullptr && !_dialog->Busy();
    }

    void OpenFromMenu() {
        const int id = _frame->ShowComposerMenuId();
        wxMenuItem* item = _frame->GetMenuBar()->FindItem(id);
        Check(item != nullptr && item->GetItemLabelText().Contains("Show Composer"), fmt::format("the Tools menu has \"{}\"", item != nullptr ? item->GetItemLabelText().ToStdString(wxConvUTF8) : std::string("-")));
        _frame->GetEventHandler()->QueueEvent(new wxCommandEvent(wxEVT_MENU, id));
    }

    std::string Show(const std::string& name) const {
        return (fs::path(_frame->GetShowDirectory()) / name).string();
    }

    void BuildSteps() {
        const fs::path out(_o.outDir);
        _steps.push_back({ "open from the Tools menu", [this]() {
                              if (!_opened) {
                                  _opened = true;
                                  OpenFromMenu();
                              }
                              return _dialog != nullptr;
                          },
                           20 });
        _steps.push_back({ "the window opens on the first step", [this]() {
                              if (!Waited(1.5)) {
                                  return false;
                              }
                              Check(_dialog->CurrentStep() == ShowComposerDialog::Step::Create, "it starts at \"Create the show\"");
                              Check(_dialog->ShowOk() && _dialog->ShowFolder() == _frame->GetShowDirectory() && _dialog->Capabilities().Count(TargetType::Model) > 0,
                                    fmt::format("the open show is the installation: {} models, {} groups, {} submodels", _dialog->Capabilities().Count(TargetType::Model), _dialog->Capabilities().Count(TargetType::Group),
                                                _dialog->Capabilities().Count(TargetType::SubModel)));
                              int faces = 0;
                              for (const TargetInfo& t : _dialog->Capabilities().targets) {
                                  faces += t.type == TargetType::Model && !t.faces.empty() && !t.Excluded() ? 1 : 0;
                              }
                              Check(faces > 0, fmt::format("{} figures with a face are shown", faces));
                              Check(!_dialog->CanGoTo(ShowComposerDialog::Step::Analysis), "nothing past the first step without a song");
                              Capture(_dialog, "crear-vacio");
                              return true;
                          },
                           20 });
        _steps.push_back({ "a file that is not audio is refused", [this, out]() {
                              const fs::path bad = out / "no-es-audio.mp3";
                              std::ofstream(bad, std::ios::binary) << "esto no es una cancion";
                              _dialog->SetAudio(bad.string());
                              Check(!_dialog->AudioOk() && !_dialog->CanGoTo(ShowComposerDialog::Step::Analysis), "an unreadable song: shown as such, and the next steps stay closed");
                              Capture(_dialog, "audio-no-valido");
                              return true;
                          },
                           10 });
        _steps.push_back({ "files dropped on the first step", [this, out]() {
                              const fs::path drops = out / "soltar";
                              std::error_code ec;
                              fs::create_directories(drops, ec);
                              const fs::path other = drops / "documento.docx";
                              std::ofstream(other, std::ios::binary) << "no";
                              const std::string before = _dialog->AudioPath();
                              _dialog->DropFiles({ other.string() });
                              Check(_dialog->ErrorShown() == std::string(wxGetTranslation("That is not a song xLights can play.").ToUTF8()) && _dialog->AudioPath() == before, "a file that is not a song: explained, and the song chosen stays");
                              // A name with accents, a dash and CJK letters, as Windows explorers hand them over.
                              const fs::path song = drops / fs::path(u8"Canción ñandú — 漢字.wav");
                              const fs::path second = drops / "otra.wav";
                              fs::copy_file(fs::path(_o.audio), song, fs::copy_options::overwrite_existing, ec);
                              fs::copy_file(fs::path(_o.audio), second, fs::copy_options::overwrite_existing, ec);
                              const std::u8string utf8 = song.u8string();
                              const std::string songPath(utf8.begin(), utf8.end());
                              const std::string hash = Sha256OfFile(songPath);
                              size_t files = 0;
                              for (const auto& e : fs::directory_iterator(drops, ec)) {
                                  files += e.is_regular_file() ? 1 : 0;
                              }
                              _dialog->ShowDropState(true);
                              Capture(_dialog, "soltar-aceptado");
                              _dialog->ShowDropState(false);
                              _dialog->DropFiles({ songPath, _o.lyrics });
                              Check(_dialog->AudioOk() && _dialog->AudioPath() == songPath && _dialog->LyricsState() == "lines", "a song with a Unicode name and its LRC dropped together: both used");
                              size_t after = 0;
                              for (const auto& e : fs::directory_iterator(drops, ec)) {
                                  after += e.is_regular_file() ? 1 : 0;
                              }
                              Check(after == files && Sha256OfFile(songPath) == hash && FileExists(_o.audio) && FileExists(_o.lyrics), "nothing was copied, moved or changed");
                              Capture(_dialog, "soltar-cancion-unicode");
                              _dialog->DropFiles({ second.string(), songPath });
                              Check(_dialog->AudioPath() == second.string(), "several songs: one is chosen (the first, with no one to ask in the test)");
                              _dialog->SetLyrics("");
                              return true;
                          },
                           20 });
        _steps.push_back({ "the music assistant", [this]() {
                              if (!_started) {
                                  _started = true;
                                  _dialog->SetAudio(_o.audio);
                                  // Never a yes in a test: with a key saved, a yes would send the song.
                                  _dialog->SetTestConsent(false);
                                  _dialog->StartAssist("transcribe", false);
                                  Check(_dialog->AssistExitCode() == 2 && !_dialog->AssistRunning() && !_dialog->AssistMessage().empty(),
                                        "without a yes nothing is started, and the window says so: " + _dialog->AssistMessage());
                                  // LRCLIB only reads; an invented title finds nothing, or there is no network.
                                  _dialog->StartAssist("lrclib", false, "Juajilandia Pruebas - Cancion inventada zqxw");
                                  Check(_dialog->AssistRunning(), "the LRCLIB search runs apart from the window");
                                  return false;
                              }
                              if (_dialog->AssistRunning() || !Waited(0.5)) {
                                  return false;
                              }
                              _started = false;
                              Check(_dialog->AssistExitCode() >= 0 && !_dialog->AssistMessage().empty(), fmt::format("and it ends with something to read ({}): {}", _dialog->AssistExitCode(), _dialog->AssistMessage()));
                              Capture(_dialog, "asistente-musical");
                              return true;
                          },
                           90 });
        _steps.push_back({ "song and lyrics", [this, out]() {
                              _dialog->SetAudio(_o.audio);
                              Check(_dialog->AudioOk(), "the demo song is readable");
                              std::string plain;
                              {
                                  std::ifstream in(fs::path(_o.lyrics), std::ios::binary);
                                  std::ostringstream text;
                                  text << in.rdbuf();
                                  plain = std::regex_replace(text.str(), std::regex("\\[[0-9:.]+\\]"), "");
                              }
                              const fs::path plainPath = out / "letra-sin-tiempos.txt";
                              std::ofstream(plainPath, std::ios::binary) << plain;
                              _dialog->SetLyrics(plainPath.string());
                              Check(_dialog->LyricsState() == "plain", "plain lyrics: shown as not synchronized");
                              Capture(_dialog, "letra-sin-sincronizar");
                              // A synchronized track, as another xLights sequence would export it.
                              std::ifstream in(fs::path(_o.lyrics), std::ios::binary);
                              std::ostringstream text;
                              text << in.rdbuf();
                              const LyricDocument doc = ParseLyrics(text.str());
                              AlignerInput input;
                              input.document = &doc;
                              const LyricAlignment alignment = AlignLyrics(input);
                              const fs::path xtiming = out / "letra-sincronizada.xtiming";
                              std::string error;
                              Check(WriteTimingTracks(xtiming.string(), { BuildLyricTrack(alignment, "Letra") }, error), "a synchronized lyrics track exported " + error);
                              _dialog->SetTiming(xtiming.string());
                              Check(_dialog->LyricsState() == "track", "a synchronization file: its lyrics track is recognized");
                              Capture(_dialog, "sincronizacion-cargada");
                              _dialog->SetTiming("");
                              _dialog->SetLyrics(_o.lyrics);
                              Check(_dialog->LyricsState() == "lines", "LRC lyrics: synchronized by line");
                              _dialog->SetLevel(AutomationLevel::Balanced);
                              std::string reference;
                              for (const std::string& path : _dialog->ProfilePaths()) {
                                  reference = path.find("reference-all.json") != std::string::npos ? path : reference;
                              }
                              Check(!reference.empty(), "the reference styles are offered with the show's own profiles");
                              _dialog->SetProfile(reference);
                              Check(!_dialog->StyleColors().empty() && !_dialog->StyleTheme().empty(), fmt::format("the reference style shows its theme {} and {} colours", _dialog->StyleTheme(), _dialog->StyleColors().size()));
                              Capture(_dialog, "estilo-referencias");
                              _dialog->SetSequenceName("demo-ui.xsq");
                              Check(_dialog->CanGoTo(ShowComposerDialog::Step::Analysis) && _dialog->CanGoTo(ShowComposerDialog::Step::Generate), "with a song, the next steps open");
                              Capture(_dialog, "crear-completo");
                              return true;
                          },
                           20 });
        _steps.push_back({ "pictures and videos", [this]() {
                              // Original files made by the test, in a folder of this inactive copy of the show.
                              const fs::path folder = fs::path(_frame->GetShowDirectory()) / "Medios de prueba Show Composer";
                              std::string error;
                              Check(WriteMediaFixtures(folder.string(), error), "original test pictures, an animation and a video in the show copy " + error);
                              auto* library = new MediaLibraryDialog(_dialog, _frame->GetShowDirectory(), {});
                              library->SetQuiet(true);
                              std::string refused;
                              Check(!library->AddFolder(fs::path(_frame->GetShowDirectory()).root_path().string(), refused) && refused == "root", "a whole disk is refused as a media folder");
                              Check(library->AddFolder(folder.string(), refused) && library->Scan(), "the folder is searched");
                              const MediaLibrary& found = library->Library();
                              int usable = 0;
                              std::string first;
                              for (const MediaItem& m : found.items) {
                                  usable += m.Usable() ? 1 : 0;
                                  first = first.empty() && m.kind == MediaKind::Image ? m.id : first;
                              }
                              Check(usable == 7, fmt::format("{} pictures, animations and videos found, each with a thumbnail", usable));
                              library->SelectItem(first);
                              library->SetItemCategories({ "christmas", "snow" });
                              // Several at once: the two first pictures get the same categories.
                              std::vector<std::string> two;
                              for (const MediaItem& m : found.items) {
                                  if (m.Usable() && m.kind == MediaKind::Image && two.size() < 2) {
                                      two.push_back(m.id);
                                  }
                              }
                              library->SelectItems(two);
                              library->SetItemCategories({ "christmas", "snow", "lights" });
                              const int given = library->ApplyCategoriesToSelected();
                              bool both = given == 2;
                              for (const std::string& id : two) {
                                  const MediaItem* m = library->Library().Find(id);
                                  both = both && m != nullptr && m->categories.size() == 3 && m->categorySource == "manual";
                              }
                              Check(both, "the categories ticked go to every file selected at once");
                              library->Show();
                              library->Update();
                              Capture(library, "biblioteca-medios");
                              Check(library->SaveLibrary() && FileExists(MediaLibraryPath(_frame->GetShowDirectory())), "the library is kept in the show's Show Composer folder");
                              // And the folder of Phase 11: many pictures and videos to make a montage of.
                              const fs::path more = fs::path(_frame->GetShowDirectory()) / "Medios Fase 11";
                              Check(WriteVisualFixtures(more.string(), error), "a second folder with many original pictures and videos " + error);
                              Check(library->AddFolder(more.string(), refused), "added to the library");
                              Check(library->SaveLibrary(), "and kept");
                              _dialog->SetMediaFolders(library->Folders());
                              library->Destroy();
                              Check(_dialog->MediaFolders().size() == 2, "the window uses both folders for the show");
                              _dialog->StartMediaAnalysis();
                              Check(_dialog->MediaAnalysisRunning(), "the library is analysed on its own thread, the window still usable");
                              Capture(_dialog, "crear-con-medios");
                              return true;
                          },
                           60 });
        _steps.push_back({ "the library analysed", [this]() {
                              if (_dialog->MediaAnalysisRunning() || !Waited(0.5)) {
                                  return false;
                              }
                              const MediaAnalysisSummary& a = _dialog->LastMediaAnalysis();
                              Check(a.total > 25 && a.analysed + a.cached == a.usable && a.damaged >= 1 && a.unsupported >= 0,
                                    fmt::format("{} files analysed in {} ms: {} pictures, {} videos, {} usable, {} damaged, {} duplicates, mode {}", a.total, a.elapsedMS, a.images + a.animations, a.videos, a.usable,
                                                a.damaged, a.duplicates, a.mode));
                              MediaLibrary library;
                              std::string error;
                              const bool read = ReadMediaLibrary(MediaLibraryPath(_frame->GetShowDirectory()), library, error);
                              const bool kept = read && std::all_of(library.items.begin(), library.items.end(), [](const MediaItem& m) { return !m.Usable() || m.analysis.Done(); });
                              Check(kept, "every usable file keeps its analysis in the library");
                              _dialog->StartMediaAnalysis();
                              return true;
                          },
                           300 });
        _steps.push_back({ "the library analysed again from the cache", [this]() {
                              if (_dialog->MediaAnalysisRunning() || !Waited(0.5)) {
                                  return false;
                              }
                              const MediaAnalysisSummary& a = _dialog->LastMediaAnalysis();
                              Check(a.analysed == 0 && a.cached == a.usable, fmt::format("a second analysis takes everything from the cache ({} files in {} ms)", a.cached, a.elapsedMS));
                              Capture(_dialog, "biblioteca-analizada");
                              return true;
                          },
                           120 });
        _steps.push_back({ "analysis and lyrics", [this]() {
                              if (!_started) {
                                  _started = true;
                                  _dialog->GoTo(ShowComposerDialog::Step::Analysis);
                                  Check(_dialog->JobRunning(), "going to Analysis starts the analysis in the generator");
                                  const std::string song = _dialog->AudioPath();
                                  _dialog->DropFiles({ song });
                                  Check(!_dialog->AcceptsDrops() && _dialog->ErrorShown() == std::string(wxGetTranslation("The generator is working.").ToUTF8()) && _dialog->AudioPath() == song && _dialog->JobRunning(),
                                        "a song dropped while the generator works is refused, and the work goes on");
                                  return false;
                              }
                              if (!JobDone() || !Waited(1.0)) {
                                  return false;
                              }
                              _started = false;
                              const ReportFolder& r = _dialog->AnalysisReport();
                              int synced = 0;
                              for (const AlignedPhrase& p : r.alignment.phrases) {
                                  synced += p.startMS >= 0 && !p.instrumental ? 1 : 0;
                              }
                              Check(r.hasAnalysis && _dialog->ShownAnalysis().durationMS > 0, fmt::format("the analysis is shown: {} ms, {} sections", _dialog->ShownAnalysis().durationMS, _dialog->ShownAnalysis().sections.size()));
                              Check(r.hasLyrics && synced > 0 && !r.alignment.words.empty() && !r.alignment.phonemes.empty(),
                                    fmt::format("the lyrics are shown on the timeline: {} phrases, {} words, {} mouth shapes", synced, r.alignment.words.size(), r.alignment.phonemes.size()));
                              Capture(_dialog, "analisis-y-letra");
                              return true;
                          },
                           900 });
        _steps.push_back({ "the bar marked by hand", [this]() {
                              int first = -1;
                              for (const MusicalEvent& e : _dialog->ShownAnalysis().events) {
                                  if (e.type == MusicalEventType::Beat && e.timeMS >= 8000) {
                                      first = e.timeMS;
                                      break;
                                  }
                              }
                              _dialog->AddMusicBarAt(first, 4);
                              const auto& events = _dialog->ShownAnalysis().events;
                              const auto downbeats = std::count_if(events.begin(), events.end(), [](const MusicalEvent& e) { return e.type == MusicalEventType::Downbeat; });
                              Check(first > 0 && downbeats > 0, fmt::format("a bar starting at {} ms gives {} strong beats on the timeline", first, downbeats));
                              Capture(_dialog, "compas-marcado-a-mano");
                              return true;
                          },
                           10 });
        _steps.push_back({ "the three levels", [this]() {
                              if (!_started) {
                                  _started = true;
                                  _dialog->GoTo(ShowComposerDialog::Step::Composition);
                                  return false;
                              }
                              if (!JobDone() || !_dialog->LevelReport(AutomationLevel::Creative).hasChoreography || !Waited(1.0)) {
                                  return false;
                              }
                              _started = false;
                              std::string numbers;
                              bool all = true;
                              for (AutomationLevel l : { AutomationLevel::Conservative, AutomationLevel::Balanced, AutomationLevel::Creative }) {
                                  const LevelSummary s = SummarizeLevel(_dialog->LevelReport(l));
                                  all = all && s.events > 0;
                                  numbers += fmt::format(" {}: {} effects, {} figures, {} accents, {} faces, {} text;", LevelId(l), s.events, s.figures, s.accents, s.faces, s.texts);
                              }
                              const LevelSummary b = SummarizeLevel(_dialog->LevelReport(AutomationLevel::Balanced));
                              Check(all, "the three levels compared from their own choreographies:" + numbers);
                              Check(b.faces > 0 && b.texts > 0 && b.accents > 0, "balanced: faces sing, a matrix shows the lyrics, and the hand-made bar gives accents");
                              {
                                  const auto& rows = _dialog->MediaRows();
                                  const bool chosen = std::any_of(rows.begin(), rows.end(), [](const ShowComposerDialog::MediaRow& m) { return !m.name.empty() && !m.why.empty(); });
                                  Check(chosen, fmt::format("pictures and videos chosen for the show, each saying why ({} rows)", rows.size()));
                              }
                              const ReportFolder& balanced = _dialog->LevelReport(AutomationLevel::Balanced);
                              Check(balanced.directionReport.find("Theme:") != std::string::npos && balanced.direction.find("\"scenes\"") != std::string::npos,
                                    "the art direction is shown: theme, colours and scenes");
                              Capture(_dialog, "composicion");
                              return true;
                          },
                           1800 });
        _steps.push_back({ "the visual montage", [this]() {
                              if (!_started) {
                                  const VisualMediaStoryboard& b = _dialog->ShownStoryboard();
                                  Check(!b.lanes.empty() && !b.shots.empty(), fmt::format("a visual storyboard for {} screens with {} shots is shown", b.lanes.size(), b.shots.size()));
                                  if (b.shots.empty()) {
                                      return true;
                                  }
                                  Capture(_dialog, "storyboard");
                                  _montageScene = b.shots.front().sceneId;
                                  _montageHash = b.hash;
                                  _montageBefore.clear();
                                  _montageAll.clear();
                                  for (const StoryboardShot& s : b.shots) {
                                      _montageAll.insert(fmt::format("{}|{}|{}|{}", s.lane, s.startMS, s.endMS, s.mediaId));
                                      if (s.sceneId == _montageScene) {
                                          _montageBefore.insert(fmt::format("{}|{}|{}|{}", s.lane, s.startMS, s.endMS, s.mediaId));
                                      }
                                  }
                                  _dialog->LockStoryboardScene(_montageScene);
                                  _dialog->RebuildMontage();
                                  Check(_dialog->MontageVariant() == 1 && _dialog->JobRunning(), "\"Rebuild the Montage\" plans it again, keeping the scene locked");
                                  _started = true;
                                  return false;
                              }
                              if (!JobDone() || !_dialog->LevelReport(AutomationLevel::Creative).hasChoreography || !Waited(1.0)) {
                                  return false;
                              }
                              _started = false;
                              const VisualMediaStoryboard& b = _dialog->ShownStoryboard();
                              std::set<std::string> after;
                              for (const StoryboardShot& s : b.shots) {
                                  if (s.sceneId == _montageScene) {
                                      after.insert(fmt::format("{}|{}|{}|{}", s.lane, s.startMS, s.endMS, s.mediaId));
                                  }
                              }
                              Check(after == _montageBefore, "the locked scene is exactly as it was");
                              Check(_dialog->StoryboardHistory().size() >= 2, fmt::format("{} montages to choose from in \"Earlier Versions\"", _dialog->StoryboardHistory().size()));
                              Capture(_dialog, "storyboard-reconstruido");
                              return true;
                          },
                           1800 });
        _steps.push_back({ "an earlier montage back", [this]() {
                              if (!_started) {
                                  const auto& history = _dialog->StoryboardHistory();
                                  size_t index = 0;
                                  for (size_t i = 0; i < history.size(); ++i) {
                                      index = history[i].hash == _montageHash ? i : index;
                                  }
                                  _dialog->RestoreStoryboardVersion(index);
                                  _started = true;
                                  return false;
                              }
                              if (!JobDone() || !_dialog->LevelReport(AutomationLevel::Creative).hasChoreography || !Waited(1.0)) {
                                  return false;
                              }
                              _started = false;
                              std::set<std::string> now;
                              for (const StoryboardShot& s : _dialog->ShownStoryboard().shots) {
                                  now.insert(fmt::format("{}|{}|{}|{}", s.lane, s.startMS, s.endMS, s.mediaId));
                              }
                              Check(now == _montageAll, fmt::format("the first montage is back, shot by shot ({} shots)", now.size()));
                              return true;
                          },
                           1800 });
        _steps.push_back({ "a lock and an exclusion", [this]() {
                              if (!_started) {
                                  std::string locked;
                                  std::string excluded;
                                  for (const FigureSummary& f : _dialog->Figures()) {
                                      if (f.type != TargetType::Model || f.events == 0 || f.receivesFace || f.receivesText || f.noController) {
                                          continue;
                                      }
                                      if (locked.empty()) {
                                          locked = f.name;
                                      } else if (excluded.empty()) {
                                          excluded = f.name;
                                      }
                                  }
                                  _locked = locked;
                                  _excluded = excluded;
                                  for (const ChoreographyEvent& e : _dialog->LevelReport(AutomationLevel::Balanced).choreography.events) {
                                      if (e.target == locked) {
                                          _lockedEvents.insert(fmt::format("{}|{}|{}", e.startMS, e.endMS, e.effect));
                                      }
                                  }
                                  _dialog->LockFigure(locked, true);
                                  _dialog->ExcludeFigure(excluded, true);
                                  _dialog->SetLevel(AutomationLevel::Creative);
                                  _dialog->StartComposition();
                                  _started = true;
                                  Log("  locked " + locked + ", excluded " + excluded);
                                  return false;
                              }
                              if (!JobDone() || !_dialog->LevelReport(AutomationLevel::Creative).hasChoreography || !Waited(1.0)) {
                                  return false;
                              }
                              _started = false;
                              const ReportFolder& r = _dialog->LevelReport(AutomationLevel::Creative);
                              std::set<std::string> kept;
                              int onExcluded = 0;
                              for (const ChoreographyEvent& e : r.choreography.events) {
                                  if (e.target == _locked && e.state == DecisionState::Locked) {
                                      kept.insert(fmt::format("{}|{}|{}", e.startMS, e.endMS, e.effect));
                                  }
                                  onExcluded += e.target == _excluded || r.capabilities.Overlap(e.target, _excluded) ? 1 : 0;
                              }
                              Check(!_lockedEvents.empty() && kept == _lockedEvents, fmt::format("on another level, the {} locked effects of {} are exactly as they were", _lockedEvents.size(), _locked));
                              Check(onExcluded == 0, fmt::format("{} is dark, and no group lights it", _excluded));
                              _dialog->SetLevel(AutomationLevel::Balanced);
                              Capture(_dialog, "correcciones");
                              return true;
                          },
                           1800 });
        _steps.push_back({ "voices", [this]() {
                              const ReportFolder& a = _dialog->AnalysisReport();
                              std::vector<VoicePhrase> phrases;
                              for (const AlignedPhrase& p : a.alignment.phrases) {
                                  if (!p.instrumental) {
                                      VoicePhrase v;
                                      v.id = p.id;
                                      v.text = p.text;
                                      v.startMS = p.startMS;
                                      v.endMS = p.endMS;
                                      v.voices = p.voices;
                                      v.words = 3;
                                      phrases.push_back(v);
                                  }
                              }
                              std::vector<std::string> faces;
                              for (const TargetInfo& t : _dialog->Capabilities().targets) {
                                  if (t.type == TargetType::Model && !t.faces.empty()) {
                                      faces.push_back(t.name);
                                  }
                              }
                              auto* voices = new VoicesDialog(_dialog, _dialog->CurrentVoices(), phrases, faces, {});
                              voices->AddVoice("Ana");
                              voices->AddVoice("Narrador");
                              voices->SetVoiceRole("v:ana", VoiceRole::Lead);
                              voices->SetVoiceRole("v:narrador", VoiceRole::Narrator);
                              voices->SetVoiceFacesOff("v:narrador", true);
                              for (int i = 0; i < static_cast<int>(phrases.size()); ++i) {
                                  voices->SetPhraseVoices(i, { i == 0 ? std::string("v:narrador") : std::string("v:ana") }, i == 0);
                              }
                              voices->Show();
                              voices->Update();
                              Capture(voices, "voces");
                              _dialog->ApplyVoiceEdits(voices->Voices(), voices->Corrections());
                              voices->Destroy();
                              const CorrectionSet& c = _dialog->Corrections();
                              const auto voiceMarks = std::count_if(c.lyrics.begin(), c.lyrics.end(), [](const LyricCorrection& l) { return l.level == LyricCorrection::Level::Voice; });
                              Check(c.voices.size() == 2 && voiceMarks == static_cast<long>(phrases.size()) && !phrases.empty(),
                                    fmt::format("two voices named by the person, {} phrases given their voice, the narrator with no face", voiceMarks));
                              _dialog->GoTo(ShowComposerDialog::Step::Analysis);
                              Capture(_dialog, "voces-en-analisis");
                              return true;
                          },
                           20 });
        _steps.push_back({ "an error is explained", [this]() {
                              _dialog->SetSequenceName("carpeta/nombre.xsq");
                              _dialog->StartGeneration();
                              Check(!_dialog->JobRunning() && !_dialog->ErrorShown().empty() && !_dialog->CanGoTo(ShowComposerDialog::Step::Generate),
                                    "a name with a folder: nothing starts, and the reason is given: " + _dialog->ErrorShown());
                              Capture(_dialog, "error-nombre");
                              _dialog->SetSequenceName("demo-ui-cancelada.xsq");
                              return true;
                          },
                           10 });
        _steps.push_back({ "cancel", [this]() {
                              if (!_started) {
                                  _started = true;
                                  _dialog->StartGeneration();
                                  Check(_dialog->JobRunning(), "the generation starts");
                                  return false;
                              }
                              if (!_cancelSent && Waited(3.0)) {
                                  _cancelSent = true;
                                  _dialog->CancelJob();
                                  Capture(_dialog, "cancelando");
                              }
                              if (!_cancelSent || !JobDone()) {
                                  return false;
                              }
                              _started = false;
                              Check(_dialog->LastExitCode() == 3 && !FileExists(Show("demo-ui-cancelada.xsq")) && !FileExists(Show("demo-ui-cancelada.composer.json")),
                                    fmt::format("cancelled (code {}): no sequence and no sidecar", _dialog->LastExitCode()));
                              Check(!fs::exists(fs::path(_frame->GetShowDirectory()) / "ShowComposer" / "tmp") || fs::is_empty(fs::path(_frame->GetShowDirectory()) / "ShowComposer" / "tmp"), "nothing left in the temporary folder");
                              Capture(_dialog, "cancelada");
                              return true;
                          },
                           900 });
        _steps.push_back({ "generate", [this]() {
                              if (!_started) {
                                  _started = true;
                                  _dialog->SetSequenceName("demo-ui.xsq");
                                  _dialog->StartGeneration();
                                  return false;
                              }
                              if (!_midShot && Waited(4.0)) {
                                  _midShot = true;
                                  Capture(_dialog, "generando");
                              }
                              if (!JobDone() || !Waited(1.0)) {
                                  return false;
                              }
                              _started = false;
                              const ReportFolder& r = _dialog->ResultReport();
                              Check(_dialog->LastExitCode() == 0 && FileExists(_dialog->ResultSequence()) && _dialog->CurrentStep() == ShowComposerDialog::Step::Result, "generated, saved, and the result step is shown");
                              Check(r.hasSummary && r.summary.find("\"faceTargets\": [") != std::string::npos && r.summary.find("\"faceTargets\": []") == std::string::npos && r.summary.find("\"textTargets\": []") == std::string::npos,
                                    "the result names the singing faces and the matrix with lyrics");
                              Check(r.summary.find("\"direction\"") != std::string::npos && r.summary.find("\"paletteCoherence\"") != std::string::npos, "the result compares the show with the references");
                              Check(r.summary.find("\"assignments\": [") != std::string::npos && r.summary.find("\"assignments\": []") == std::string::npos && r.summary.find("\"name\": \"Ana\"") != std::string::npos,
                                    "the result shows the pictures and videos used, and the voices the person named");
                              Capture(_dialog, "resultado");
                              return true;
                          },
                           1800 });
        _steps.push_back({ "the window never froze", [this]() {
                              Check(_maxGapMS < 1500, fmt::format("while the generator worked, the window answered every {} ms at worst (timer every 200 ms)", _maxGapMS));
                              return true;
                          },
                           5 });
        _steps.push_back({ "open the sequence in xLights", [this]() {
                              if (!_started) {
                                  _started = true;
                                  _sequence = _dialog->ResultSequence();
                                  _dialog->OpenResult();
                                  return false;
                              }
                              if (_dialog != nullptr || !Waited(2.0)) {
                                  return false;
                              }
                              if (!_frame->IsSequenceLoaded() && !Waited(60)) {
                                  return false;
                              }
                              _started = false;
                              Check(_frame->IsSequenceLoaded() && _frame->GetSeqData().NumFrames() > 0, "the sequence opened in this same xLights, with its layout");
                              Capture(_frame, "secuencia-abierta");
                              return true;
                          },
                           240 });
        _steps.push_back({ "a show made earlier comes back", [this]() {
                              if (!_started) {
                                  _started = true;
                                  _opened = false;
                                  OpenFromMenu();
                                  return false;
                              }
                              if (_dialog == nullptr || !Waited(2.0)) {
                                  return false;
                              }
                              _started = false;
                              std::string error;
                              const bool loaded = _dialog->LoadPreviousShow(SidecarPathFor(_sequence), error);
                              const CorrectionSet& c = _dialog->Corrections();
                              Check(loaded && !c.choreography.empty() && !c.plan.empty() && c.analysis.bar.firstDownbeatMS > 0 && _dialog->AudioOk(),
                                    fmt::format("from its sidecar: the song, {} locks, {} exclusions and the bar are back {}", c.choreography.size(), c.plan.size(), error));
                              Capture(_dialog, "espectaculo-recuperado");
                              return true;
                          },
                           60 });
        _steps.push_back({ "closing while it works", [this]() {
                              if (!_started) {
                                  _started = true;
                                  _closedBefore = _closed;
                                  _dialog->StartComposition();
                                  return false;
                              }
                              if (!_closeSent && Waited(2.0) && _dialog != nullptr && _dialog->JobRunning()) {
                                  _closeSent = true;
                                  _dialog->Close();
                                  Check(_dialog != nullptr, "the window waits for the generator instead of vanishing under it");
                              }
                              if (!_closeSent || _closed == _closedBefore) {
                                  return false;
                              }
                              if (ComposerJob::LiveProcesses() != 0 && !Waited(120)) {
                                  return false;
                              }
                              _started = false;
                              Check(ComposerJob::LiveProcesses() == 0, "closed only once the generator had stopped: no process left behind");
                              return true;
                          },
                           900 });
    }

    void OnTick(wxTimerEvent&) {
        const auto now = std::chrono::steady_clock::now();
        if (_dialog != nullptr && _dialog->JobRunning()) {
            _maxGapMS = std::max<long long>(_maxGapMS, std::chrono::duration_cast<std::chrono::milliseconds>(now - _last).count());
        }
        _last = now;
        if (_busy) {
            return;
        }
        if (_index >= _steps.size()) {
            Finish();
            return;
        }
        Step& step = _steps[_index];
        if (!_stepStarted) {
            _stepStarted = true;
            _stepStart = now;
            Log("=== " + step.name);
        }
        if (step.name != "open from the Tools menu" && step.name != "a show made earlier comes back" && step.name != "open the sequence in xLights" && step.name != "closing while it works" && _dialog == nullptr) {
            Check(false, step.name + ": the window is not open");
            Finish();
            return;
        }
        _busy = true;
        const bool done = step.run();
        _busy = false;
        if (done) {
            ++_index;
            _stepStarted = false;
        } else if (Waited(step.timeoutS)) {
            Check(false, step.name + ": timed out");
            Finish();
        }
    }

    void Finish() {
        _timer.Stop();
        if (_dialog != nullptr && _dialog->JobRunning()) {
            // A failed run must not leave its generator working on after the test.
            _dialog->CancelJob();
        }
        Log(fmt::format("composer-ui-test: {} checks passed, {} failed", _passed, _failed));
        Log("composer-ui-test: the screenshots show the window; nobody has judged how the show looks");
        _log.close();
        fflush(stdout);
        // Ends without the frame's close prompts: this is a test run on a show copy.
        std::_Exit(_failed == 0 ? 0 : 1);
    }

    xLightsFrame* _frame;
    ComposerUITestOptions _o;
    wxTimer _timer;
    std::ofstream _log;
    std::vector<Step> _steps;
    size_t _index = 0;
    bool _stepStarted = false;
    std::chrono::steady_clock::time_point _stepStart;
    std::chrono::steady_clock::time_point _last;
    long long _maxGapMS = 0;
    bool _busy = false;
    ShowComposerDialog* _dialog = nullptr;
    int _closed = 0;
    int _closedBefore = 0;
    bool _opened = false;
    bool _started = false;
    bool _cancelSent = false;
    bool _closeSent = false;
    bool _midShot = false;
    std::string _locked;
    std::string _montageScene;
    std::string _montageHash;
    std::set<std::string> _montageBefore;
    std::set<std::string> _montageAll;
    std::string _excluded;
    std::set<std::string> _lockedEvents;
    std::string _sequence;
    int _shots = 0;
    int _passed = 0;
    int _failed = 0;
};

} // namespace

void StartComposerUITest(xLightsFrame* frame, const ComposerUITestOptions& options) {
    std::string refused;
    if (!ControllersAreInactive(frame->GetShowDirectory(), refused)) {
        printf("composer-ui-test: REFUSED: %s\n", refused.c_str());
        fflush(stdout);
        std::_Exit(2);
    }
    new Driver(frame, options); // lives until the process ends
}
