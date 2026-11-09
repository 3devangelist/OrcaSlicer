#pragma once

// [multi-printer] Printer, process and filaments per plate (fork 3devangelist/OrcaSlicer, see fork/DESIGN.md).
//
// OrcaSlicer keeps one PresetBundle: one selected printer, process and filament list. A plate may now
// own a "preset set" (a snapshot of those selections and their unsaved edits). Plates without their own
// set share the project's set. Selecting a plate swaps its set into the PresetBundle, so the sidebar,
// the settings tabs and slicing (which reads the bundle) all work on that plate's set, with no changes
// to them. Only the public PresetBundle / PresetCollection API is used.

#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

class wxMenu;
class wxWindow;

namespace Slic3r {

class ModelObject;
class PresetBundle;

namespace GUI {

class PartPlate;
class Plater;

namespace MultiPrinterGui {

// What a plate's set holds: the selected presets with their unsaved edits, and the project data that
// belongs to the filament list (colours, flush volumes, ...).
struct PresetSnapshot
{
    struct Collection
    {
        std::string        selected;
        DynamicPrintConfig edited;
    };
    Collection                      printers, prints, filaments;
    std::vector<std::string>        filament_presets;
    DynamicPrintConfig              project_config;
    std::vector<std::map<int, int>> extruder_ams_counts;
};

PresetSnapshot capture_presets(const PresetBundle& bundle);

// Selects the snapshot's presets and restores their edits. Returns false when a preset no longer exists
// (that collection then keeps its current selection).
bool restore_presets(PresetBundle& bundle, const PresetSnapshot& snapshot);

bool plate_has_own_presets(Plater& plater, const PartPlate* plate);

// Gives the plate its own set (a copy of the set it uses now) or drops it, so it uses the project's set.
void set_plate_own_presets(Plater& plater, PartPlate* plate, bool own);

// Called right after a plate is selected and before its Print is applied: swaps in the plate's set.
void on_plate_selected(Plater& plater, PartPlate* plate);

// True while a set is being swapped in; code reacting to preset changes must not touch other plates then.
bool is_swapping_presets();

// True when objects must move with their plate when the bed size changes (a plate's printer has another
// bed), so that they stay on the plate they were on. Upstream leaves them where they are.
bool objects_follow_plates();

// True when the feature is on: plates may have their own beds, so objects are reassigned to plates after
// every bed change.
bool plate_layout_enabled();

// The printer settings a plate uses and their preset name: the PresetBundle's when the plate's set is the
// one loaded in it (or `plate` is nullptr: a plate about to be added from the current one), otherwise the
// plate's stored set.
const DynamicPrintConfig& plate_printer_config(const PartPlate* plate, std::string& preset_name);

// Called by ProjectSaveScope: lays the plates out the upstream way while a project is written (`true`)
// and back (`false`). Defined in PlateLayout.cpp.
void suspend_plate_layout(Plater& plater, bool suspend);

// Adds (or refreshes) the plate menu items: give the selected plate its own printer, process and
// filaments (or take them back), and copy / paste plate settings. Does nothing when the feature is off.
void append_plate_presets_menu_item(wxMenu* menu, size_t pos, wxWindow* parent);

// A new plate takes the settings of the plate it was added from: its plate options and process
// overrides, and its own preset set if it has one. Called before the new plate is selected.
void on_plate_added(Plater& plater, PartPlate* source, PartPlate* added);

// Copy / paste everything a plate has: plate options and process overrides, and which printer,
// process and filaments it uses (its own set, or the project's).
void copy_plate_settings(Plater& plater, PartPlate* plate);
bool can_paste_plate_settings();
void paste_plate_settings(Plater& plater, PartPlate* plate);

// While a project file is written: the PresetBundle holds the project's set (so project_settings.config
// is what plates without their own set use, and what upstream OrcaSlicer opens), and
// Model::multi_printer_plates holds the plates' own sets as JSON. The plate's set is back afterwards.
// Plates in rows per printer are laid out on upstream's grid while the file is written, except for the
// automatic backups (every few seconds, read back only by this fork): those keep the rows and say so.
class ProjectSaveScope
{
public:
    ProjectSaveScope(Plater& plater, bool backup);
    ~ProjectSaveScope();
    ProjectSaveScope(const ProjectSaveScope&)            = delete;
    ProjectSaveScope& operator=(const ProjectSaveScope&) = delete;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

// After a project is opened: gives its plates their saved sets (from Model::multi_printer_plates) and,
// once loading is over, swaps in the current plate's set.
void on_project_loaded(Plater& plater, const std::string& plates_json);

// A new or cleared project: no plate has its own set any more.
void on_project_reset();

// The JSON stored in the 3MF, and back. Exposed for tests.
// `rows_layout`: the objects were written where the rows per printer put their plates (backups).
std::string sets_to_json(const std::map<int, PresetSnapshot>& sets_by_plate_index, bool rows_layout = false);
std::map<int, PresetSnapshot> sets_from_json(const std::string& json);
bool json_has_rows_layout(const std::string& json);

// Whether the filament clamp after a filament-count change may touch this object: only objects on
// plates that use the active set, never during a swap.
bool may_clamp_object_filaments(Plater& plater, const ModelObject* object);

} // namespace MultiPrinterGui
} // namespace GUI
} // namespace Slic3r
