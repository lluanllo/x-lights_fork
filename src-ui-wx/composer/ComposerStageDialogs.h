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

#include <wx/dialog.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "composer/ComposerReview.h"
#include "composer/ProceduralField.h"
#include "composer/StageDirector.h"
#include "composer/StageSpace.h"

class FSEQFile;
class wxButton;
class wxCheckBox;
class wxChoice;
class wxListCtrl;
class wxSlider;
class wxStaticText;
class wxTextCtrl;
struct TimelineGesture;

// Plain words for the stage director's ideas, translated.
[[nodiscard]] wxString StageModeWords(composer::StageMode mode);
[[nodiscard]] wxString GestureWords(composer::GestureKind kind);
[[nodiscard]] wxString StageRoleWords(const std::string& role);
// The score's gestures for the song's timeline.
[[nodiscard]] std::vector<TimelineGesture> TimelineGestures(const composer::GlobalStageScore& score);

// The whole installation seen from the front: every light at its place in the layout, lit
// either as the score's light fields say or as xLights' own render drew it.
class StageMapView : public wxPanel {
public:
    explicit StageMapView(wxWindow* parent);
    ~StageMapView() override;

    void SetLights(std::vector<composer::StageFigureLights> lights);
    // The score's own effects (Stage Field events of a choreography), drawn as their fields say.
    void SetScoreEffects(const composer::Choreography& choreography);
    // A rendered show; "" forgets it.
    bool SetRender(const std::string& fseqPath);
    void UseRender(bool render);
    [[nodiscard]] bool UsingRender() const { return _useRender; }
    [[nodiscard]] bool HasRender() const { return _fseq != nullptr; }
    void SetTime(int ms);
    [[nodiscard]] int Time() const { return _timeMS; }
    void Mark(double x, double y); // a gesture's origin, stage units; x < 0 hides it
    [[nodiscard]] int LitLights() const { return _lit; } // lights drawn lit at the last paint

private:
    struct Field {
        composer::FieldSpec spec;
        std::string figure;
        int startMS = 0;
        int endMS = 0;
        std::vector<std::string> colors;
        std::vector<char> lit; // the lights it may light
    };
    void OnPaint(wxPaintEvent& event);
    void ReadFrame();

    std::vector<composer::StageFigureLights> _lights;
    std::vector<Field> _fields;
    std::unique_ptr<FSEQFile> _fseq;
    std::vector<uint8_t> _frame;
    uint32_t _channels = 0;
    int _frameMS = 25;
    int _frameRead = -1;
    bool _useRender = false;
    int _timeMS = 0;
    double _markX = -1, _markY = -1;
    int _lit = 0;
};

// "Coreografía de la instalación": the map, a time bar, play, and the gesture under way.
class StageMapDialog : public wxDialog {
public:
    StageMapDialog(wxWindow* parent, const composer::ReportFolder& report, const std::string& fseqPath, int startMS);

    void SetTime(int ms);
    void NextGesture();
    [[nodiscard]] StageMapView* Map() const { return _map; }
    [[nodiscard]] wxString Describe() const;

private:
    void ShowAt(int ms);

    composer::GlobalStageScore _score;
    StageMapView* _map = nullptr;
    wxSlider* _slider = nullptr;
    wxStaticText* _clock = nullptr;
    wxTextCtrl* _now = nullptr;
    wxChoice* _source = nullptr;
    wxButton* _play = nullptr;
    wxTimer _timer;
    int _durationMS = 0;
};

// "Capas de esta figura": what a figure has on top of what, at a moment, and whether it keeps
// its own colours. Changes are a person's edits of the score, applied when it is made again.
class FigureLayersDialog : public wxDialog {
public:
    FigureLayersDialog(wxWindow* parent, const composer::ReportFolder& report, const std::string& editsPath, const std::string& figure);

    void ShowFigure(const std::string& figure, int ms);
    std::string KeepColours(int mode); // 1 always, 0 as the director decides, -1 never; returns "" or an error
    [[nodiscard]] int Rows() const;
    [[nodiscard]] bool Changed() const { return _changed; }

private:
    const composer::ReportFolder& _report;
    composer::GlobalStageScore _score;
    std::string _editsPath;
    std::string _figure;
    wxChoice* _figures = nullptr;
    wxChoice* _scenes = nullptr;
    wxListCtrl* _list = nullptr;
    wxStaticText* _colours = nullptr;
    bool _changed = false;
};

// "Composiciones y gestos": the gestures and a figure's own compositions the director may
// use, a person's favourites and exclusions, and compositions of their own.
class StageLibraryDialog : public wxDialog {
public:
    StageLibraryDialog(wxWindow* parent, const std::string& showDir);

    bool Toggle(const std::string& id, bool favourite, bool value);
    bool AddOwn(const std::string& title, const std::string& function);
    bool Save();
    [[nodiscard]] const composer::StageLibrary& Library() const { return _library; }

private:
    void Fill();
    [[nodiscard]] std::string Selected() const;

    std::string _showDir;
    composer::StageLibrary _library;
    wxListCtrl* _list = nullptr;
    wxTextCtrl* _about = nullptr;
};

// Step 4, "Director de escena": the score scene by scene, who takes part and what moves
// across the house, and a person's changes, kept apart and applied when composed again.
class StageDirectorPanel : public wxPanel {
public:
    explicit StageDirectorPanel(wxWindow* parent);

    // The composition's report (its score) and where the person's changes are kept.
    // Returns whether the score shown changed.
    bool SetReport(const composer::ReportFolder* report, const std::string& showDir, const std::string& fseqPath);
    [[nodiscard]] const composer::GlobalStageScore& Score() const { return _score; }
    [[nodiscard]] bool DirectorOn() const;
    void SetDirectorOn(bool on);
    [[nodiscard]] std::string EditsText() const; // what is saved, for knowing when to compose again
    [[nodiscard]] int Scenes() const { return static_cast<int>(_score.scenes.size()); }
    [[nodiscard]] wxString Detail() const;

    // The calls the buttons make, for the UI test. They return "" or an error code.
    bool SelectScene(int index);
    std::string Edit(const composer::StageEdit& edit);
    std::string Undo();
    [[nodiscard]] int PendingEdits() const { return static_cast<int>(_edits.edits.size()); }

    std::function<void()> onRecompose;
    std::function<void()> onDirectorChanged;

private:
    void Fill();
    void ShowScene();
    bool LoadEdits();
    bool SaveEdits();
    [[nodiscard]] const composer::StageScene* Scene() const;
    void AskMode();
    void AskFigure(bool exclude);
    void AskGesture();
    void ChangeSpeed(double factor);
    void OpenMap();
    void OpenLayers();
    void OpenLibrary();

    const composer::ReportFolder* _report = nullptr;
    std::string _scoreText;
    composer::GlobalStageScore _score;
    std::string _showDir;
    std::string _fseqPath;
    std::string _editsPath;
    composer::StageEdits _edits;
    std::vector<std::string> _undo; // the edits as they were before each change
    int _scene = -1;
    wxCheckBox* _on = nullptr;
    wxListCtrl* _list = nullptr;
    wxTextCtrl* _detail = nullptr;
    wxStaticText* _state = nullptr;
};
