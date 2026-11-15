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

#include <functional>
#include <string>
#include <vector>

#include "composer/ChromaticDirector.h"
#include "composer/ColorCheck.h"
#include "composer/ComposerReview.h"
#include "composer/MusicAssist.h"

class wxButton;
class wxCheckBox;
class wxImageList;
class wxListCtrl;
class wxStaticText;
class wxTextCtrl;
struct TimelineColors;

// A row of colours, drawn as equal bands.
class ColorSwatches : public wxPanel {
public:
    ColorSwatches(wxWindow* parent, const wxSize& size);
    void SetColors(const std::vector<std::string>& colors);
    [[nodiscard]] const std::vector<std::string>& Colors() const { return _colors; }

private:
    std::vector<std::string> _colors;
};

// The colours of each scene for the timeline (step 2), from a colour score.
[[nodiscard]] std::vector<TimelineColors> TimelineColorsOf(const composer::ChromaticScore& score);

// Step 4, "Director cromático": the song's family of colours, each scene's palette and
// why, which colour each figure carries, the gestures' colours and the changes between
// scenes. A person can change a colour, the accent, fix a palette, ask for more or less
// contrast or one family; the changes are kept apart and applied when composed again,
// and can be undone or all dropped to get the original plan back.
class ColorDirectorPanel : public wxPanel {
public:
    explicit ColorDirectorPanel(wxWindow* parent);

    bool SetReport(const composer::ReportFolder* report, const std::string& showDir);
    [[nodiscard]] const composer::ChromaticScore& Score() const { return _score; }
    [[nodiscard]] bool DirectorOn() const;
    void SetDirectorOn(bool on);
    [[nodiscard]] std::string EditsText() const;
    [[nodiscard]] int Scenes() const { return static_cast<int>(_score.scenes.size()); }
    [[nodiscard]] wxString Detail() const;
    [[nodiscard]] const composer::ChromaticEdits& Edits() const { return _edits; }
    [[nodiscard]] int SelectedScene() const { return _scene; }

    // The calls the buttons make, for the UI test. They return "" or an error code.
    bool SelectScene(int index);
    std::string Edit(const composer::ChromaticEdit& edit);
    std::string Undo();
    std::string Reset(); // every change dropped: the original plan
    [[nodiscard]] int PendingEdits() const { return static_cast<int>(_edits.edits.size()); }
    void OpenFigures();
    void OpenReview();

    std::function<void()> onRecompose;
    std::function<void()> onDirectorChanged;
    std::function<const composer::ReportFolder*()> resultReport; // step 5's report, for the review

private:
    void Fill();
    void ShowScene();
    bool LoadEdits();
    bool SaveEdits();
    [[nodiscard]] const composer::ChromaticScenePlan* Scene() const;
    void AskColor();

    const composer::ReportFolder* _report = nullptr;
    std::string _scoreText;
    composer::ChromaticScore _score;
    std::string _showDir;
    std::string _editsPath;
    composer::ChromaticEdits _edits;
    std::vector<std::string> _undo;
    int _scene = -1;
    wxCheckBox* _on = nullptr;
    wxStaticText* _state = nullptr;
    ColorSwatches* _song = nullptr;
    wxStaticText* _songText = nullptr;
    wxListCtrl* _list = nullptr;
    wxImageList* _images = nullptr;
    wxTextCtrl* _detail = nullptr;
};

// Step 3, "Colores de las figuras": the colour each figure carries in a scene and why;
// a figure can keep its own colours, take the palette, or get colours chosen by hand.
class FigureColorsDialog : public wxDialog {
public:
    FigureColorsDialog(wxWindow* parent, ColorDirectorPanel* panel);
    void ShowScene(int index);
    [[nodiscard]] int Rows() const;
    std::string KeepOwn(const std::string& figure, bool keep);
    std::string Choose(const std::string& figure, const std::vector<std::string>& colors);
    std::string Clear(const std::string& figure);

private:
    void Fill();
    [[nodiscard]] std::string Selected() const;

    ColorDirectorPanel* _panel = nullptr;
    int _scene = 0;
    wxListCtrl* _list = nullptr;
    wxImageList* _images = nullptr;
    wxStaticText* _sceneText = nullptr;
};

// Step 5, "Colores del espectáculo": what the render really showed against the plan,
// scene by scene and figure by figure, the gestures against what was lit, the stretches
// in one colour, and how the colour moved through the song. Measurements, never a verdict.
class ColorReviewDialog : public wxDialog {
public:
    ColorReviewDialog(wxWindow* parent, const composer::ReportFolder& report);
    [[nodiscard]] wxString Summary() const;
    [[nodiscard]] int SceneRows() const;
    [[nodiscard]] int FigureRows() const;
    [[nodiscard]] bool HasMeasure() const { return _check.ran; }

private:
    composer::ColorCheck _check;
    composer::ChromaticScore _score;
    wxTextCtrl* _summary = nullptr;
    wxListCtrl* _scenes = nullptr;
    wxListCtrl* _figures = nullptr;
};

// "Comprobar análisis artístico con Groq": where the colours' reading of the song comes
// from, in plain words. Never shows a key.
class ArtisticCheckDialog : public wxDialog {
public:
    ArtisticCheckDialog(wxWindow* parent, const composer::ArtisticCheck& check);
    [[nodiscard]] wxString Text() const;
    std::function<void()> onAskAgain; // "Ask Groq Again": the window asks for consent first

private:
    wxTextCtrl* _text = nullptr;
};

// Plain words, translated.
[[nodiscard]] wxString ArtisticStatusText(const composer::ArtisticCheck& check);
[[nodiscard]] wxString ColorFamilyWords(const std::string& family);
