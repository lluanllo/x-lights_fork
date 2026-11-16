#pragma once

#include "composer/ProductionCheck.h"
#include "composer/StageDirector.h"

#include <wx/dialog.h>

#include <functional>
#include <string>

class wxListCtrl;
class wxTextCtrl;
class wxStaticText;

// Step 5, "Preparación y revisión": what the check of the show found, worded for the person
// who will switch it on, and the production folder. Nothing here approves anything.
class ProductionDialog : public wxDialog {
public:
    ProductionDialog(wxWindow* parent, const composer::ProductionReport& report, const std::string& showDir);

    // The production folder, never over an earlier one. Returns "" or what went wrong.
    std::string MakePackage();
    [[nodiscard]] const composer::ProductionPackage& Package() const { return _package; }
    [[nodiscard]] wxString Text() const;

    std::function<void()> openInXLights;

private:
    composer::ProductionReport _report;
    std::string _showDir;
    composer::ProductionPackage _package;
    wxTextCtrl* _text = nullptr;
    wxStaticText* _packageState = nullptr;
};

// The song's visual ideas: where each appears, what each scene does with it, who carries it,
// and keeping one as a composition of your own.
class MotifsDialog : public wxDialog {
public:
    MotifsDialog(wxWindow* parent, const composer::GlobalStageScore& score, const std::string& showDir, const std::string& song);

    bool SelectMotif(int index);
    // Keeps the motif shown as a composition of the person's own. Returns "" or an error code.
    std::string KeepAsComposition();
    [[nodiscard]] wxString Detail() const;
    [[nodiscard]] int Motifs() const { return static_cast<int>(_score.motifs.size()); }

private:
    void ShowMotif(int index);

    const composer::GlobalStageScore& _score;
    std::string _showDir;
    std::string _song;
    int _shown = -1;
    wxListCtrl* _motifs = nullptr;
    wxListCtrl* _uses = nullptr;
    wxTextCtrl* _about = nullptr;
};

// The status of a show, before it goes out, in one line per thing a person relies on.
[[nodiscard]] wxString ProductionStatusText(const composer::ProductionReport& report);
// A composition kept in the show's own library (ShowComposer/stage/library.json).
bool KeepComposition(const std::string& showDir, const composer::StageLibraryEntry& entry, std::string& error);
