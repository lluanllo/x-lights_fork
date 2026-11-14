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

#include <functional>
#include <string>
#include <vector>

#include "composer/FigureSemantics.h"
#include "composer/LyricDocument.h"
#include "composer/MultimodalTimeline.h"
#include "composer/PerformanceMapping.h"
#include "composer/RenderUnderstanding.h"
#include "composer/SongAnalysis.h"
#include "composer/UnderstandingStore.h"

class wxButton;
class wxCheckListBox;
class wxChoice;
class wxListCtrl;
class wxSlider;
class wxStaticText;
class wxTextCtrl;
class LanesView;
class NodesView;

// Plain words for what the recording says, for a person: "Habla", "Entra el coro"...
[[nodiscard]] wxString UnderstandingWords(const std::string& coreText);
[[nodiscard]] wxString ClockText(int ms);
// A piece of the timeline in plain words: "Voz A: Canta".
[[nodiscard]] wxString ItemWords(const composer::TimelineItem& item);
// One line about a song's understanding: "Se oye: habla 3 veces, canta 5, el coro 2...".
[[nodiscard]] wxString UnderstandingSummary(const composer::SongUnderstanding& song);

// "Qué está pasando en la grabación": the lanes of the recording, and the corrections a
// person makes on them, kept apart from the analysis and applied after every new one.
class RecordingDialog : public wxDialog {
public:
    RecordingDialog(wxWindow* parent, composer::SongUnderstanding song, composer::UnderstandRequest request, const composer::SongAnalysis* analysis,
                    const composer::FigureCatalog* geometry, const composer::FigureSemantics* semantics);

    [[nodiscard]] const composer::SongUnderstanding& Song() const { return _song; }
    [[nodiscard]] bool Changed() const { return _changed; }
    [[nodiscard]] const composer::MultimodalTimeline& Timeline() const { return _timeline; }

    // The same calls the controls make, for the UI test. They return the error code, "" ok.
    bool Select(const std::string& itemId);
    [[nodiscard]] const std::string& Selected() const { return _selected; }
    std::string Edit(const composer::TimelineEdit& edit);
    std::string Revert(bool restore);
    void SetCursor(int ms);
    [[nodiscard]] wxString Detail() const;

private:
    void Rebuild();
    void ShowSelection();
    void AskMove();
    void AskKind();
    void AskVoice();
    void AskFigures();

    composer::SongUnderstanding _song;
    composer::UnderstandRequest _request;
    const composer::SongAnalysis* _analysis = nullptr;
    const composer::FigureCatalog* _geometry = nullptr;
    const composer::FigureSemantics* _semantics = nullptr;
    composer::PerformanceMap _performers;
    composer::MultimodalTimeline _timeline;
    std::string _selected;
    int _cursorMS = 0;
    bool _changed = false;

    LanesView* _lanes = nullptr;
    wxSlider* _zoom = nullptr;
    wxStaticText* _summary = nullptr;
    wxStaticText* _detail = nullptr;
    wxStaticText* _status = nullptr;
    wxTextCtrl* _technical = nullptr;
};

// "Conocer mis figuras": each figure as xLights draws it, its parts tied to real lights,
// their colours, what it can play; a person confirms, removes, adds or recolours parts.
class FiguresDialog : public wxDialog {
public:
    FiguresDialog(wxWindow* parent, composer::FigureCatalog geometry, composer::FigureSemantics found, composer::FigureChoices choices, std::string showDir);

    [[nodiscard]] const composer::FigureChoices& Choices() const { return _choices; }
    [[nodiscard]] const composer::FigureSemantics& Semantics() const { return _semantics; }
    [[nodiscard]] bool Changed() const { return _changed; }

    // For the UI test.
    bool SelectFigure(const std::string& figure);
    bool SelectRegion(const std::string& region);
    bool ConfirmRegion();
    bool DropRegion();
    bool AddRegion(const std::string& region, const std::string& ranges, std::string& error);
    bool SetRegionColor(const std::string& hex);
    bool SetRoles(const std::vector<std::string>& roles);
    bool MarkReviewed(bool reviewed);
    bool Save(std::string& error);
    [[nodiscard]] wxString FigureText() const;

private:
    void Recompute();
    void FillFigures();
    void FillRegions();
    composer::FigureChoice& ChoiceFor(const std::string& figure);
    [[nodiscard]] const composer::FigureProfile* Profile() const;

    composer::FigureCatalog _geometry;
    composer::FigureSemantics _found;
    composer::FigureSemantics _semantics;
    composer::FigureChoices _choices;
    std::string _showDir;
    std::vector<std::string> _figureRows;
    std::string _figure;
    std::string _region;
    bool _changed = false;

    wxListCtrl* _figures = nullptr;
    wxListCtrl* _regions = nullptr;
    NodesView* _nodes = nullptr;
    wxStaticText* _about = nullptr;
    wxCheckListBox* _roles = nullptr;
    wxStaticText* _status = nullptr;
};

// "Asignar voces y personajes": who performs each voice and each moment of the music,
// automatic or chosen by the person, and every problem in plain words.
class PerformersDialog : public wxDialog {
public:
    PerformersDialog(wxWindow* parent, composer::SongUnderstanding song, composer::FigureCatalog geometry, composer::FigureSemantics semantics);

    [[nodiscard]] const std::vector<composer::PerformerAssignment>& Manual() const { return _song.manual; }
    [[nodiscard]] const composer::PerformanceMap& Map() const { return _map; }
    [[nodiscard]] bool Changed() const { return _changed; }

    // For the UI test.
    bool Assign(const std::string& source, const std::vector<std::string>& figures);
    bool Automatic(const std::string& source);
    bool Save(std::string& error);
    [[nodiscard]] wxString ProblemsText() const;

private:
    void Recompute();
    void Fill();
    [[nodiscard]] std::string Selected() const;
    void AskFigures();

    composer::SongUnderstanding _song;
    composer::FigureCatalog _geometry;
    composer::FigureSemantics _semantics;
    composer::PerformanceMap _map;
    std::vector<std::string> _rows;
    bool _changed = false;

    wxListCtrl* _list = nullptr;
    wxTextCtrl* _problems = nullptr;
};

// What the figures did in the rendered show against what was meant: moments to look at,
// never an approval.
class RenderCheckDialog : public wxDialog {
public:
    // `takePictures`: makes pictures of the render at these moments of these figures.
    RenderCheckDialog(wxWindow* parent, composer::RenderUnderstanding check, std::function<void(const std::vector<int>&, const std::vector<std::string>&)> takePictures);
    [[nodiscard]] int Rows() const;

private:
    composer::RenderUnderstanding _check;
    std::function<void(const std::vector<int>&, const std::vector<std::string>&)> _takePictures;
    wxListCtrl* _list = nullptr;
};
