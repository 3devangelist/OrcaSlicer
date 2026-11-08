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

// Adds (or refreshes) the plate menu item that gives the selected plate its own printer, process and
// filaments, or takes them back. Does nothing when the feature is off.
void append_plate_presets_menu_item(wxMenu* menu, size_t pos, wxWindow* parent);

// Whether the filament clamp after a filament-count change may touch this object: only objects on
// plates that use the active set, never during a swap.
bool may_clamp_object_filaments(Plater& plater, const ModelObject* object);

} // namespace MultiPrinterGui
} // namespace GUI
} // namespace Slic3r
