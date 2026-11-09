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
#include <functional>
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

// The physical machine a plate prints on (a Bambu-style device known to DeviceManager): its id (serial),
// the name the user gave it and its model id. Empty: no machine chosen.
struct MachineBinding
{
    std::string id;
    std::string name;
    std::string model;
    bool        empty() const { return id.empty(); }
};

// What a plate's set holds: the selected presets with their unsaved edits, and the project data that
// belongs to the filament list (colours, flush volumes, ...), plus the plate's machine and what that
// machine had loaded the last time it was read (AMS trays / nozzles, PresetBundle::filament_ams_list).
struct PresetSnapshot
{
    struct Collection
    {
        std::string        selected;
        DynamicPrintConfig edited;
    };
    Collection                        printers, prints, filaments;
    std::vector<std::string>          filament_presets;
    DynamicPrintConfig                project_config;
    std::vector<std::map<int, int>>   extruder_ams_counts;
    std::map<int, DynamicPrintConfig> filament_ams_list;
    MachineBinding                    machine; // kept by refreshes from the PresetBundle
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

// The machine a plate prints on (nullptr plate: the plate about to be added from the current one).
MachineBinding plate_machine(const PartPlate* plate);
void           set_plate_machine(Plater& plater, PartPlate* plate, const MachineBinding& machine);
// " — <machine name>" for the plate's labels, or empty.
std::string    plate_machine_suffix(const PartPlate* plate);
std::string    plate_machine_name(const PartPlate* plate);

// Called when the printer of the current plate may have changed: drops a machine of another model and,
// when the plate has none, takes the only machine of that model the user has.
void refresh_current_plate_machine(Plater& plater);

// Before syncing filaments or printing: when the current plate's machine is not the selected one, asks
// to switch to it and waits (with a progress dialog) until it is connected. False: the user cancelled.
// A machine that is not available only shows a warning: the action goes on.
bool ensure_plate_machine_selected(Plater& plater, bool ask = true);

// "Print all" when the plates use several machines or printers: lists "plate → machine" once for the
// user to confirm, then for each sliced plate selects it (its presets and machine) and calls
// `print_plate(plate index)`, which opens the usual print or upload dialog for it. False: one machine
// for all plates, so the caller prints all plates the upstream way.
bool print_plates_one_by_one(Plater& plater, const std::function<void(int)>& print_plate);

// The user picked a machine (Device tab, print dialog): the current plate now prints on it, when its
// model matches the plate's printer. Ignored while ensure_plate_machine_selected switches.
void on_machine_selected_by_user(const std::string& dev_id);

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
// Asks which parts to paste: plate options, process settings, printer/process/filaments, machine.
void paste_plate_settings_special(Plater& plater, PartPlate* plate);
// Lists the user's machines of the current plate's printer model and binds the chosen one.
void choose_plate_machine(Plater& plater, PartPlate* plate);

// While a project file is written: the PresetBundle holds the project's set (so project_settings.config
// is what plates without their own set use, and what upstream OrcaSlicer opens), and
// Model::multi_printer_plates holds the plates' own sets as JSON. The plate's set is back afterwards.
// Plates in rows per printer are laid out on upstream's grid while the file is written, except when
// `keep_rows`: automatic backups (every few seconds) and sliced files (moving objects would drop their
// slicing results). Those keep the rows and say so in the JSON.
class ProjectSaveScope
{
public:
    ProjectSaveScope(Plater& plater, bool keep_rows);
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
