// [multi-printer] Printer, process and filaments per plate. See PlatePresets.hpp and fork/DESIGN.md.

#include "PlatePresets.hpp"

#include "PlateSettings.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include <boost/log/trivial.hpp>
#include <wx/menu.h>

#include <algorithm>
#include <map>
#include <string>

namespace Slic3r {
namespace GUI {
namespace MultiPrinterGui {

namespace {

// The sets of the plates that have their own, the project's set, and which one is in the PresetBundle.
// Plates are identified by pointer; entries of plates that no longer exist are dropped on every access.
struct Registry
{
    std::map<const PartPlate*, PresetSnapshot> plate_sets;
    PresetSnapshot                             project_set;
    bool                                       has_project_set = false;
    const PartPlate*                           active          = nullptr; // nullptr: the project's set
    bool                                       swapping        = false;
};

Registry& registry()
{
    static Registry r;
    return r;
}

bool plate_exists(Plater& plater, const PartPlate* plate)
{
    PartPlateList& plates = plater.get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        if (plates.get_plate(i) == plate)
            return true;
    return false;
}

void prune(Plater& plater)
{
    Registry& r = registry();
    for (auto it = r.plate_sets.begin(); it != r.plate_sets.end();)
        it = plate_exists(plater, it->first) ? std::next(it) : r.plate_sets.erase(it);
    if (r.active != nullptr && r.plate_sets.count(r.active) == 0)
        r.active = nullptr; // its plate is gone: the bundle now counts as the project's set
}

// The set whose presets are in the bundle now, after `prune`.
PresetSnapshot& active_set_storage()
{
    Registry& r = registry();
    if (r.active != nullptr)
        return r.plate_sets[r.active];
    r.has_project_set = true;
    return r.project_set;
}

// Loads a snapshot into the bundle and refreshes the GUI the way the G-code preview does after
// restoring a selection (Plater::load_gcode_with_settings).
void load_into_app(Plater& plater, const PresetSnapshot& snapshot)
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    // A plate's set is not the selection the app should remember for the next start.
    PresetBundle::SelectionExportBlocker no_export(bundle);
    if (!restore_presets(bundle, snapshot))
        BOOST_LOG_TRIVIAL(warning) << "[multi-printer] a preset of the plate's set no longer exists; it keeps the current one";
    wxGetApp().load_current_presets(false, false);
    // Loading the printer re-derives some project settings (TabPrinter::on_preset_loaded).
    bundle.project_config = snapshot.project_config;
    plater.sidebar().update_presets(Preset::TYPE_PRINTER);
    plater.update_project_dirty_from_presets();
}

const PartPlate* owner_of(const PartPlate* plate)
{
    const Registry& r = registry();
    return r.plate_sets.count(plate) != 0 ? plate : nullptr;
}

} // namespace

PresetSnapshot capture_presets(const PresetBundle& bundle)
{
    auto capture = [](const PresetCollection& presets, PresetSnapshot::Collection& out) {
        out.selected = presets.get_selected_preset_name();
        out.edited   = presets.get_edited_preset().config;
    };
    PresetSnapshot snapshot;
    capture(bundle.printers, snapshot.printers);
    capture(bundle.prints, snapshot.prints);
    capture(bundle.filaments, snapshot.filaments);
    snapshot.filament_presets    = bundle.filament_presets;
    snapshot.project_config      = bundle.project_config;
    snapshot.extruder_ams_counts = bundle.extruder_ams_counts;
    return snapshot;
}

bool restore_presets(PresetBundle& bundle, const PresetSnapshot& snapshot)
{
    bool all_found = true;
    auto restore = [&all_found](PresetCollection& presets, const PresetSnapshot::Collection& saved) {
        if (!presets.select_preset_by_name(saved.selected, true)) {
            all_found = false;
            return;
        }
        presets.get_edited_preset().config = saved.edited;
        presets.update_dirty();
    };
    // Printer first: the compatible process and filament presets depend on it.
    restore(bundle.printers, snapshot.printers);
    restore(bundle.prints, snapshot.prints);
    restore(bundle.filaments, snapshot.filaments);
    bundle.update_compatible(PresetSelectCompatibleType::Never);
    bundle.update_multi_material_filament_presets();
    // Last, as update_multi_material_filament_presets() resizes filament_presets and the flush data.
    bundle.filament_presets    = snapshot.filament_presets;
    bundle.project_config      = snapshot.project_config;
    bundle.extruder_ams_counts = snapshot.extruder_ams_counts;
    return all_found;
}

bool plate_has_own_presets(Plater& plater, const PartPlate* plate)
{
    prune(plater);
    return registry().plate_sets.count(plate) != 0;
}

void set_plate_own_presets(Plater& plater, PartPlate* plate, bool own)
{
    if (plate == nullptr || !plate_process_settings_enabled())
        return;
    prune(plater);
    Registry& r = registry();
    if (own == (r.plate_sets.count(plate) != 0))
        return;
    const bool plate_is_current = plater.get_partplate_list().get_curr_plate() == plate;
    if (own) {
        // The plate starts with a copy of the set it uses now.
        if (plate_is_current) {
            active_set_storage() = capture_presets(*wxGetApp().preset_bundle); // keep the project's set up to date
            r.plate_sets[plate]  = capture_presets(*wxGetApp().preset_bundle);
            r.active             = plate;
        } else {
            if (r.active == nullptr)
                active_set_storage() = capture_presets(*wxGetApp().preset_bundle);
            r.plate_sets[plate] = r.has_project_set ? r.project_set : capture_presets(*wxGetApp().preset_bundle);
        }
    } else {
        const bool was_active = r.active == plate;
        r.plate_sets.erase(plate);
        if (was_active) {
            r.active = nullptr;
            if (r.has_project_set) {
                r.swapping = true;
                load_into_app(plater, r.project_set);
                r.swapping = false;
            }
        }
    }
    plate->update_slice_result_valid_state(false);
}

void on_plate_selected(Plater& plater, PartPlate* plate)
{
    if (plate == nullptr || !plate_process_settings_enabled())
        return;
    prune(plater);
    Registry& r = registry();
    const PartPlate* new_owner = owner_of(plate);
    if (new_owner == r.active)
        return; // same set: nothing to swap
    r.swapping = true;
    // Keep what was edited in the set that is leaving the bundle.
    active_set_storage() = capture_presets(*wxGetApp().preset_bundle);
    r.active = new_owner;
    load_into_app(plater, active_set_storage());
    r.swapping = false;
    BOOST_LOG_TRIVIAL(info) << "[multi-printer] plate " << plate->get_index() << " uses "
                            << (new_owner ? "its own presets" : "the project's presets") << ", printer "
                            << wxGetApp().preset_bundle->printers.get_selected_preset_name();
}

bool is_swapping_presets() { return registry().swapping; }

void append_plate_presets_menu_item(wxMenu* menu, size_t pos, wxWindow* parent)
{
    const wxString use_own     = _L("Use own printer, process and filaments");
    const wxString use_project = _L("Use the project's printer, process and filaments");
    // The plate menu is reused: drop the item added for the previous plate.
    for (const wxString& name : {use_own, use_project})
        if (const int id = menu->FindItem(name); id != wxNOT_FOUND)
            menu->Destroy(id);
    if (!plate_process_settings_enabled())
        return;
    Plater*    plater = wxGetApp().plater();
    PartPlate* plate  = plater->get_partplate_list().get_selected_plate();
    if (plate == nullptr)
        return;
    const bool own = plate_has_own_presets(*plater, plate);
    append_menu_item(
        menu, wxID_ANY, own ? use_project : use_own, "",
        [plate, own](wxCommandEvent&) { set_plate_own_presets(*wxGetApp().plater(), plate, !own); }, "", nullptr,
        []() { return true; }, parent, int(pos));
}

bool may_clamp_object_filaments(Plater& plater, const ModelObject* object)
{
    if (!plate_process_settings_enabled())
        return true;
    if (is_swapping_presets())
        return false;
    prune(plater);
    const Registry& r = registry();
    if (r.plate_sets.empty())
        return true;
    const ModelObjectPtrs& objects = wxGetApp().model().objects;
    const auto it = std::find(objects.begin(), objects.end(), object);
    if (it == objects.end())
        return true;
    const int obj_idx = int(it - objects.begin());
    PartPlateList& plates = plater.get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        PartPlate* plate = plates.get_plate(i);
        for (int inst = 0; inst < int(object->instances.size()); ++inst)
            if (plate->contain_instance(obj_idx, inst))
                return owner_of(plate) == r.active;
    }
    return r.active == nullptr; // off the plates: the project's set
}

} // namespace MultiPrinterGui
} // namespace GUI
} // namespace Slic3r
