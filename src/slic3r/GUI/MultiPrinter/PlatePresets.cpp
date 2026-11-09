// [multi-printer] Printer, process and filaments per plate. See PlatePresets.hpp and fork/DESIGN.md.

#include "PlatePresets.hpp"

#include "PlateLayout.hpp"
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
#include <nlohmann/json.hpp>
#include <wx/menu.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
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

// Owner's decision (2026-11-09): every plate has its own printer, process and filaments, so choosing a
// printer changes only the selected plate. A plate without a set yet takes the one in the bundle now.
// `holder` is the plate that uses the bundle's set when that is the project's set: it becomes the
// active plate, unless it already had a set of its own (then the bundle stays the project's set).
void give_every_plate_a_set(Plater& plater, const PartPlate* holder)
{
    Registry&      r      = registry();
    PartPlateList& plates = plater.get_partplate_list();
    bool           missing = false;
    for (int i = 0; i < plates.get_plate_count(); ++i)
        missing |= r.plate_sets.count(plates.get_plate(i)) == 0;
    if (!missing)
        return;
    const bool           holder_had_set = r.plate_sets.count(holder) != 0;
    const PresetSnapshot now            = capture_presets(*wxGetApp().preset_bundle);
    active_set_storage()                = now;
    for (int i = 0; i < plates.get_plate_count(); ++i)
        r.plate_sets.emplace(plates.get_plate(i), now); // keeps the sets plates already have
    if (r.active == nullptr && holder != nullptr && !holder_had_set)
        r.active = holder;
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
    give_every_plate_a_set(plater, plate);
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

namespace {
// While a file saved with the rows layout is opened: its objects already sit where the rows put their
// plates, so the plates move to the rows without carrying objects along.
bool& opening_rows_file()
{
    static bool b = false;
    return b;
}
} // namespace

bool objects_follow_plates() { return plate_process_settings_enabled() && !opening_rows_file(); }

bool plate_layout_enabled() { return plate_process_settings_enabled(); }

const DynamicPrintConfig& plate_printer_config(const PartPlate* plate, std::string& preset_name)
{
    const Registry&     r      = registry();
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    const auto          it     = plate != nullptr ? r.plate_sets.find(plate) : r.plate_sets.end();
    const PresetSnapshot* set  = nullptr;
    if (plate != nullptr && r.active != plate) {
        if (it != r.plate_sets.end())
            set = &it->second;
        else if (r.active != nullptr && r.has_project_set)
            set = &r.project_set; // no set of its own while another plate's set is loaded
    }
    if (set == nullptr) {
        preset_name = bundle.printers.get_selected_preset_name();
        return bundle.printers.get_edited_preset().config;
    }
    preset_name = set->printers.selected;
    return set->printers.edited;
}

namespace {

struct PlateClipboard
{
    bool               has      = false;
    DynamicPrintConfig plate_config;      // plate options and process overrides
    bool               has_set  = false;  // false: the copied plate used the project's set
    PresetSnapshot     set;
};

PlateClipboard& clipboard()
{
    static PlateClipboard c;
    return c;
}

// The set stored for `plate`, up to date with the bundle when it is the active one.
const PresetSnapshot* stored_set(const PartPlate* plate)
{
    Registry& r = registry();
    const auto it = r.plate_sets.find(plate);
    if (it == r.plate_sets.end())
        return nullptr;
    if (r.active == plate)
        it->second = capture_presets(*wxGetApp().preset_bundle);
    return &it->second;
}

void after_plate_settings_changed(Plater& plater, PartPlate* plate)
{
    plate->update_slice_result_valid_state(false);
    plater.update();
}

} // namespace

void on_plate_added(Plater& plater, PartPlate* source, PartPlate* added)
{
    if (source == nullptr || added == nullptr || source == added || !plate_process_settings_enabled())
        return;
    prune(plater);
    *added->config() = *source->config();
    Registry& r = registry();
    give_every_plate_a_set(plater, source);
    if (const PresetSnapshot* set = stored_set(source)) {
        r.plate_sets[added] = *set;
        // The new plate is selected next without a swap: the bundle already holds this same set.
        if (r.active == source)
            r.active = added;
    }
}

void copy_plate_settings(Plater& plater, PartPlate* plate)
{
    if (plate == nullptr)
        return;
    prune(plater);
    give_every_plate_a_set(plater, plater.get_partplate_list().get_curr_plate());
    PlateClipboard& c = clipboard();
    c.has          = true;
    c.plate_config = *plate->config();
    const PresetSnapshot* set = stored_set(plate);
    c.has_set = set != nullptr;
    c.set     = set != nullptr ? *set : PresetSnapshot{};
}

bool can_paste_plate_settings() { return clipboard().has; }

void paste_plate_settings(Plater& plater, PartPlate* plate)
{
    const PlateClipboard& c = clipboard();
    if (plate == nullptr || !c.has || !plate_process_settings_enabled())
        return;
    prune(plater);
    give_every_plate_a_set(plater, plater.get_partplate_list().get_curr_plate());
    plater.take_snapshot("Paste plate settings");
    *plate->config() = c.plate_config;
    Registry& r = registry();
    if (c.has_set) {
        r.plate_sets[plate] = c.set;
        const bool is_current = plater.get_partplate_list().get_curr_plate() == plate;
        if (is_current) {
            // Keep what was edited in the set that leaves the bundle, then show the pasted one.
            if (r.active != plate)
                active_set_storage() = capture_presets(*wxGetApp().preset_bundle);
            r.active   = plate;
            r.swapping = true;
            load_into_app(plater, c.set);
            r.swapping = false;
        }
    }
    // Every plate keeps a set of its own: a clipboard without one leaves the plate's set as it is.
    after_plate_settings_changed(plater, plate);
}

void append_plate_presets_menu_item(wxMenu* menu, size_t pos, wxWindow* parent)
{
    const wxString use_own     = _L("Use own printer, process and filaments");
    const wxString use_project = _L("Use the project's printer, process and filaments");
    const wxString copy        = _L("Copy plate settings");
    const wxString paste       = _L("Paste plate settings");
    // The plate menu is reused: drop the items added for the previous plate.
    for (const wxString& name : {use_own, use_project, copy, paste})
        if (const int id = menu->FindItem(name); id != wxNOT_FOUND)
            menu->Destroy(id);
    if (!plate_process_settings_enabled())
        return;
    Plater*    plater = wxGetApp().plater();
    PartPlate* plate  = plater->get_partplate_list().get_selected_plate();
    if (plate == nullptr)
        return;
    // Every plate has its own set (see give_every_plate_a_set), so there is no "own / project's" choice.
    append_menu_item(
        menu, wxID_ANY, copy, "", [plate](wxCommandEvent&) { copy_plate_settings(*wxGetApp().plater(), plate); }, "",
        nullptr, []() { return true; }, parent, int(pos));
    append_menu_item(
        menu, wxID_ANY, paste, "", [plate](wxCommandEvent&) { paste_plate_settings(*wxGetApp().plater(), plate); }, "",
        nullptr, []() { return can_paste_plate_settings(); }, parent, int(pos) + 1);
}

namespace {

nlohmann::json config_to_json(const DynamicPrintConfig& config)
{
    nlohmann::json out = nlohmann::json::object();
    for (const std::string& key : config.keys())
        out[key] = config.opt_serialize(key);
    return out;
}

DynamicPrintConfig config_from_json(const nlohmann::json& in)
{
    DynamicPrintConfig config;
    if (!in.is_object())
        return config;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    for (auto it = in.begin(); it != in.end(); ++it) {
        if (!it.value().is_string())
            continue;
        try {
            config.set_deserialize(it.key(), it.value().get<std::string>(), substitutions);
        } catch (const std::exception&) {
            // A setting this build does not know or cannot read: keep the preset's value.
        }
    }
    return config;
}

} // namespace

bool json_has_rows_layout(const std::string& json)
{
    const nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
    return !root.is_discarded() && root.is_object() && root.value("layout", std::string()) == "rows";
}

std::string sets_to_json(const std::map<int, PresetSnapshot>& sets_by_plate_index, bool rows_layout)
{
    nlohmann::json plates = nlohmann::json::array();
    for (const auto& [index, set] : sets_by_plate_index) {
        nlohmann::json ams = nlohmann::json::array();
        for (const auto& counts : set.extruder_ams_counts) {
            nlohmann::json entry = nlohmann::json::object();
            for (const auto& [k, v] : counts)
                entry[std::to_string(k)] = v;
            ams.push_back(entry);
        }
        plates.push_back({{"plate_index", index},
                          {"printer", set.printers.selected},
                          {"process", set.prints.selected},
                          {"filament", set.filaments.selected},
                          {"filament_presets", set.filament_presets},
                          {"printer_config", config_to_json(set.printers.edited)},
                          {"process_config", config_to_json(set.prints.edited)},
                          {"filament_config", config_to_json(set.filaments.edited)},
                          {"project_config", config_to_json(set.project_config)},
                          {"extruder_ams_counts", ams}});
    }
    nlohmann::json root{{"version", 1}, {"plates", plates}};
    if (rows_layout)
        root["layout"] = "rows";
    return root.dump(1);
}

std::map<int, PresetSnapshot> sets_from_json(const std::string& json)
{
    std::map<int, PresetSnapshot> sets;
    const nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("plates") || !root["plates"].is_array())
        return sets;
    for (const nlohmann::json& plate : root["plates"]) {
        if (!plate.is_object() || !plate.contains("plate_index") || !plate["plate_index"].is_number_integer())
            continue;
        PresetSnapshot set;
        set.printers.selected  = plate.value("printer", std::string());
        set.prints.selected    = plate.value("process", std::string());
        set.filaments.selected = plate.value("filament", std::string());
        if (plate.contains("filament_presets") && plate["filament_presets"].is_array())
            for (const nlohmann::json& name : plate["filament_presets"])
                if (name.is_string())
                    set.filament_presets.push_back(name.get<std::string>());
        set.printers.edited  = config_from_json(plate.value("printer_config", nlohmann::json::object()));
        set.prints.edited    = config_from_json(plate.value("process_config", nlohmann::json::object()));
        set.filaments.edited = config_from_json(plate.value("filament_config", nlohmann::json::object()));
        set.project_config   = config_from_json(plate.value("project_config", nlohmann::json::object()));
        if (plate.contains("extruder_ams_counts") && plate["extruder_ams_counts"].is_array())
            for (const nlohmann::json& entry : plate["extruder_ams_counts"]) {
                std::map<int, int> counts;
                if (entry.is_object())
                    for (auto it = entry.begin(); it != entry.end(); ++it)
                        if (it.value().is_number_integer())
                            counts[std::atoi(it.key().c_str())] = it.value().get<int>();
                set.extruder_ams_counts.push_back(counts);
            }
        sets[plate["plate_index"].get<int>()] = std::move(set);
    }
    return sets;
}

struct ProjectSaveScope::Impl
{
    Plater*                                               plater = nullptr;
    std::unique_ptr<PresetBundle::SelectionExportBlocker> no_export;
    const PartPlate*                                      swapped_out = nullptr; // the plate whose set is put back
};

ProjectSaveScope::ProjectSaveScope(Plater& plater, bool backup)
{
    // Off: leave Model::multi_printer_plates as loaded, so a project's sets survive a save.
    if (!plate_process_settings_enabled())
        return;
    prune(plater);
    Registry& r = registry();
    m_impl         = new Impl;
    m_impl->plater = &plater;
    // Objects are written where upstream's grid puts their plates, so upstream OrcaSlicer opens the
    // file with every object on its plate (PlateLayout.cpp). Not for backups: no plate moves every few
    // seconds while the user works.
    const bool rows_layout = backup && layout_active(plater.get_partplate_list());
    if (!backup)
        suspend_plate_layout(plater, true);
    if (r.plate_sets.empty()) {
        plater.model().multi_printer_plates.clear();
        return;
    }
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    // The set in the bundle may have unsaved edits made since it was swapped in.
    active_set_storage() = capture_presets(bundle);
    std::map<int, PresetSnapshot> by_index;
    for (const auto& [plate, set] : r.plate_sets)
        by_index[const_cast<PartPlate*>(plate)->get_index()] = set; // get_index() is not const upstream
    plater.model().multi_printer_plates = sets_to_json(by_index, rows_layout);
    // The project's presets in the 3MF (what upstream Orca opens) are the first plate's.
    const PartPlate* first = plater.get_partplate_list().get_plate(0);
    if (const auto it = r.plate_sets.find(first); it != r.plate_sets.end()) {
        r.project_set     = it->second;
        r.has_project_set = true;
    }
    if (r.active != nullptr && r.active != first && r.has_project_set) {
        m_impl->no_export   = std::make_unique<PresetBundle::SelectionExportBlocker>(bundle);
        m_impl->swapped_out = r.active;
        restore_presets(bundle, r.project_set);
    }
}

ProjectSaveScope::~ProjectSaveScope()
{
    if (m_impl == nullptr)
        return;
    Registry& r = registry();
    if (m_impl->swapped_out != nullptr)
        if (const auto it = r.plate_sets.find(m_impl->swapped_out); it != r.plate_sets.end())
            restore_presets(*wxGetApp().preset_bundle, it->second);
    suspend_plate_layout(*m_impl->plater, false);
    delete m_impl;
}

void on_project_loaded(Plater& plater, const std::string& plates_json)
{
    on_project_reset();
    if (!plate_process_settings_enabled() || plates_json.empty())
        return;
    Registry& r = registry();
    PartPlateList& plates = plater.get_partplate_list();
    for (auto& [index, set] : sets_from_json(plates_json))
        if (index >= 0 && index < plates.get_plate_count())
            r.plate_sets[plates.get_plate(index)] = std::move(set);
    // The bundle holds the project's set now. Swap in the current plate's set once the loading code,
    // which may still refresh the presets, has finished.
    const bool rows_file = json_has_rows_layout(plates_json);
    wxGetApp().CallAfter([rows_file]() {
        Plater* p = wxGetApp().plater();
        if (p == nullptr)
            return;
        opening_rows_file() = rows_file;
        on_plate_selected(*p, p->get_partplate_list().get_curr_plate());
        // Lay the plates out in rows per printer: from upstream's grid the objects move along; from a
        // backup written with the rows they are already in place.
        p->set_bed_shape();
        opening_rows_file() = false;
    });
}

void on_project_reset()
{
    Registry& r = registry();
    // A plate's set may be in the bundle: the next project starts from the project's set, not from it.
    const bool           put_back_project_set = r.active != nullptr && r.has_project_set;
    const PresetSnapshot project_set          = r.project_set;
    r = Registry{};
    Plater* plater = wxGetApp().plater();
    if (put_back_project_set && plater != nullptr && plate_process_settings_enabled()) {
        r.swapping = true;
        load_into_app(*plater, project_set);
        r.swapping = false;
    }
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
