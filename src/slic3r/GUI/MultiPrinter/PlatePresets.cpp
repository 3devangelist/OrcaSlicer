// [multi-printer] Printer, process and filaments per plate. See PlatePresets.hpp and fork/DESIGN.md.

#include "PlatePresets.hpp"

#include "PlateLayout.hpp"
#include "PlateSettings.hpp"

#include "libslic3r/Color.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiPrinter/PlateOverrides.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/choicdlg.h>
#include <wx/menu.h>
#include <wx/progdlg.h>
#include <wx/utils.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <chrono>
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
    // Every tab and the sidebar are rebuilt (measured: about 450 ms of tab rebuilding upstream, 600 ms
    // without this): draw the window once, at the end.
    wxWindowUpdateLocker no_redraw(wxGetApp().mainframe);
    // A plate's set is not the selection the app should remember for the next start.
    PresetBundle::SelectionExportBlocker no_export(bundle);
    if (!restore_presets(bundle, snapshot))
        BOOST_LOG_TRIVIAL(warning) << "[multi-printer] a preset of the plate's set no longer exists; it keeps the current one";
    wxGetApp().load_current_presets(false, false);
    // Loading the printer re-derives some project settings (TabPrinter::on_preset_loaded).
    bundle.project_config = snapshot.project_config;
    // Loading the printer also brings back the filament list last used with that printer (upstream's
    // per-printer memory), which may not be this plate's: put the plate's list back.
    if (bundle.filament_presets != snapshot.filament_presets) {
        BOOST_LOG_TRIVIAL(info) << "[multi-printer] the printer brought another filament list back; the plate's list is restored";
        bundle.filament_presets = snapshot.filament_presets;
        bundle.project_config   = snapshot.project_config;
        plater.sidebar().update_presets(Preset::TYPE_FILAMENT);
    }
    plater.sidebar().update_presets(Preset::TYPE_PRINTER);
    plater.update_project_dirty_from_presets();
}

// Updates a stored set with what is in the PresetBundle now; the plate's machine is not bundle data.
void refresh_from_bundle(PresetSnapshot& set)
{
    const MachineBinding machine = set.machine;
    set                          = capture_presets(*wxGetApp().preset_bundle);
    set.machine                  = machine;
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
    refresh_from_bundle(active_set_storage());
    const PresetSnapshot now = active_set_storage(); // with the active set's machine
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
    snapshot.filament_ams_list   = bundle.filament_ams_list;
    // User presets travel with the project (another computer may not have them).
    auto keep_user = [&snapshot](const PresetCollection& presets, const std::string& name, Preset::Type type) {
        if (const Preset* preset = presets.find_preset(name, false); preset != nullptr && (preset->is_user() || preset->is_project_embedded))
            snapshot.user_presets[name] = {int(type), preset->config};
    };
    keep_user(bundle.printers, snapshot.printers.selected, Preset::TYPE_PRINTER);
    keep_user(bundle.prints, snapshot.prints.selected, Preset::TYPE_PRINT);
    for (const std::string& name : snapshot.filament_presets)
        keep_user(bundle.filaments, name, Preset::TYPE_FILAMENT);
    return snapshot;
}

bool restore_presets(PresetBundle& bundle, const PresetSnapshot& snapshot)
{
    // User presets this computer does not have: add them as project presets (like upstream does for the
    // presets embedded in a 3MF), so the plate slices with the same settings.
    for (const auto& [name, typed] : snapshot.user_presets) {
        PresetCollection* presets = typed.first == Preset::TYPE_PRINTER ? static_cast<PresetCollection*>(&bundle.printers) :
                                    typed.first == Preset::TYPE_PRINT   ? &bundle.prints :
                                    typed.first == Preset::TYPE_FILAMENT ? &bundle.filaments : nullptr;
        if (presets == nullptr || presets->find_preset(name, false) != nullptr)
            continue;
        Preset& added             = presets->load_preset("", name, typed.second, false);
        added.is_project_embedded = true;
        BOOST_LOG_TRIVIAL(info) << "[multi-printer] added the plate's preset '" << name << "' as a project preset";
    }
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
    // What the plate's machine had loaded: filament sync and the "match the printer" grouping use it.
    bundle.filament_ams_list = snapshot.filament_ams_list;
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
            refresh_from_bundle(active_set_storage()); // keep the project's set up to date
            r.plate_sets[plate]  = capture_presets(*wxGetApp().preset_bundle);
            r.active             = plate;
        } else {
            if (r.active == nullptr)
                refresh_from_bundle(active_set_storage());
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

namespace {

// ---- Objects moved to a plate with other filaments (owner's decision 2026-11-08) ---------------------
// Each object remembers the plate it was on and the filament (type, colour) it used there. When it is
// found on another plate, it takes that plate's closest filament (same type first, then nearest colour)
// and a notification says so.

struct FilamentLook
{
    std::string type, colour;
};

struct ObjectFilament
{
    const PartPlate* plate = nullptr;
    FilamentLook     look;
};

std::map<size_t, ObjectFilament>& object_filaments() // ModelObject id -> where and what it printed with
{
    static std::map<size_t, ObjectFilament> m;
    return m;
}

// A filament slot (1-based) of the set loaded in the bundle.
FilamentLook slot_look(int slot)
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    FilamentLook        look;
    if (slot < 1 || size_t(slot) > bundle.filament_presets.size())
        return look;
    if (const Preset* preset = bundle.filaments.find_preset(bundle.filament_presets[slot - 1], false))
        if (const auto* type = preset->config.option<ConfigOptionStrings>("filament_type"); type != nullptr && !type->values.empty())
            look.type = type->values.front();
    if (const auto* colour = bundle.project_config.option<ConfigOptionStrings>("filament_colour"); colour != nullptr && size_t(slot) <= colour->values.size())
        look.colour = colour->values[slot - 1];
    return look;
}

int object_slot(const ModelObject* object)
{
    const ConfigOption* extruder = object->config.option("extruder");
    const int           slot     = extruder != nullptr ? extruder->getInt() : 0;
    return slot > 0 ? slot : 1;
}

template<typename Fn> void for_objects_on(Plater& plater, const PartPlate* plate, Fn fn)
{
    ModelObjectPtrs& objects = plater.model().objects;
    for (size_t i = 0; i < objects.size(); ++i)
        if (const_cast<PartPlate*>(plate)->contain_instance(int(i), 0))
            fn(int(i), objects[i]);
}

void remember_object_filaments(Plater& plater, const PartPlate* plate)
{
    if (plate == nullptr)
        return;
    for_objects_on(plater, plate, [plate](int, ModelObject* object) {
        object_filaments()[object->id().id] = {plate, slot_look(object_slot(object))};
    });
}

double colour_distance(const std::string& a, const std::string& b)
{
    ColorRGB ca, cb;
    if (!decode_color(a, ca) || !decode_color(b, cb))
        return 1.;
    return std::sqrt(std::pow(ca.r() - cb.r(), 2) + std::pow(ca.g() - cb.g(), 2) + std::pow(ca.b() - cb.b(), 2));
}

void adapt_moved_objects(Plater& plater, const PartPlate* plate)
{
    const int slots = int(wxGetApp().preset_bundle->filament_presets.size());
    if (plate == nullptr || slots < 1)
        return;
    for_objects_on(plater, plate, [&](int obj_idx, ModelObject* object) {
        auto it = object_filaments().find(object->id().id);
        if (it == object_filaments().end() || it->second.plate == plate || it->second.plate == nullptr)
            return; // never seen elsewhere, or not moved
        const FilamentLook was     = it->second.look;
        const int          current = object_slot(object);
        int                best    = current <= slots ? current : 1;
        double             best_score = 1e9;
        for (int slot = 1; slot <= slots; ++slot) {
            const FilamentLook look  = slot_look(slot);
            const double       score = (look.type == was.type ? 0. : 10.) + colour_distance(look.colour, was.colour);
            if (score < best_score) {
                best_score = score;
                best       = slot;
            }
        }
        it->second = {plate, slot_look(best)};
        if (best == current)
            return;
        object->config.set("extruder", best);
        plater.changed_object(obj_idx);
        const FilamentLook now = slot_look(best);
        plater.get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
            (boost::format(_u8L("%1% is on plate %2% now: filament %3% (%4% %5%) became filament %6% (%7% %8%), the closest one there."))
             % object->name % (const_cast<PartPlate*>(plate)->get_index() + 1) % current % was.type % was.colour % best % now.type % now.colour).str());
    });
}

// True when two sets put the same data in the PresetBundle (the machine is not bundle data).
bool same_bundle_data(const PresetSnapshot& a, const PresetSnapshot& b)
{
    auto same = [](const PresetSnapshot::Collection& x, const PresetSnapshot::Collection& y) {
        return x.selected == y.selected && x.edited == y.edited;
    };
    return same(a.printers, b.printers) && same(a.prints, b.prints) && same(a.filaments, b.filaments) &&
           a.filament_presets == b.filament_presets && a.project_config == b.project_config &&
           a.extruder_ams_counts == b.extruder_ams_counts && a.filament_ams_list == b.filament_ams_list;
}

} // namespace

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
    // Keep what was edited in the set that is leaving the bundle, and what its objects printed with.
    refresh_from_bundle(active_set_storage());
    remember_object_filaments(plater, r.active);
    // Reloading the presets refreshes every tab and takes most of a second; a plate with the same
    // printer, process and filaments as the one leaving needs only to become the active one.
    const PresetSnapshot& leaving = r.active != nullptr ? r.plate_sets[r.active] : r.project_set;
    r.active                      = new_owner;
    if (!same_bundle_data(leaving, active_set_storage()))
        load_into_app(plater, active_set_storage());
    r.swapping = false;
    adapt_moved_objects(plater, plate);
    BOOST_LOG_TRIVIAL(info) << "[multi-printer] plate " << plate->get_index() << " uses "
                            << (new_owner ? "its own presets" : "the project's presets") << ", printer "
                            << wxGetApp().preset_bundle->printers.get_selected_preset_name();
    refresh_current_plate_machine(plater); // a plate without a machine takes the only one of its model
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

// ---- The plate's physical machine ------------------------------------------------------------------

namespace {

bool& switching_machine()
{
    static bool b = false;
    return b;
}

// The model id of the printer loaded in the bundle (what DeviceManager calls printer_type).
std::string current_printer_model()
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    return bundle.printers.get_edited_preset().get_printer_type(&bundle);
}

// The user's machines of a model, as the print dialog lists them.
std::vector<MachineObject*> my_machines_of_model(const std::string& model)
{
    std::vector<MachineObject*> out;
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (dev == nullptr || model.empty())
        return out;
    for (const auto& [id, obj] : dev->get_my_machine_list(dev->get_current_printer_agent_id()))
        if (obj != nullptr && obj->printer_type == model)
            out.push_back(obj);
    return out;
}

// The plate's labels show its machine: the name drawn on the plate and its row in the object list.
void refresh_plate_labels(Plater& plater, PartPlate* plate)
{
    if (plate == nullptr)
        return;
    plate->generate_plate_name_texture();
    if (ObjectList* list = wxGetApp().obj_list())
        list->GetModel()->SetCurSelectedPlateFullName(plate->get_index(), plate->get_plate_name());
    plater.get_view3D_canvas3D()->set_as_dirty();
}

} // namespace

MachineBinding plate_machine(const PartPlate* plate)
{
    const Registry& r = registry();
    if (plate == nullptr)
        plate = r.active; // a plate added from the current one gets its machine
    const auto it = r.plate_sets.find(plate);
    return it != r.plate_sets.end() ? it->second.machine : MachineBinding{};
}

void set_plate_machine(Plater& plater, PartPlate* plate, const MachineBinding& machine)
{
    if (plate == nullptr || !plate_process_settings_enabled())
        return;
    prune(plater);
    give_every_plate_a_set(plater, plater.get_partplate_list().get_curr_plate());
    Registry& r  = registry();
    auto      it = r.plate_sets.find(plate);
    if (it == r.plate_sets.end() || (it->second.machine.id == machine.id && it->second.machine.name == machine.name))
        return;
    it->second.machine = machine;
    BOOST_LOG_TRIVIAL(info) << "[multi-printer] plate " << plate->get_index() << " prints on machine '" << machine.name << "' (" << machine.id << ")";
    refresh_plate_labels(plater, plate);
    // Rows per machine; after the caller is done (it may be reacting to a bed change itself).
    wxGetApp().CallAfter([]() {
        if (Plater* p = wxGetApp().plater())
            p->set_bed_shape();
    });
    plater.update_project_dirty_from_presets();
}

std::string plate_machine_name(const PartPlate* plate)
{
    return plate_process_settings_enabled() ? plate_machine(plate).name : std::string();
}

std::string plate_machine_suffix(const PartPlate* plate)
{
    const std::string name = plate_machine_name(plate);
    return name.empty() ? std::string() : " \xE2\x80\x94 " + name; // " — name"
}

void refresh_current_plate_machine(Plater& plater)
{
    if (!plate_process_settings_enabled() || is_swapping_presets())
        return;
    PartPlate* plate = plater.get_partplate_list().get_curr_plate();
    if (plate == nullptr || registry().plate_sets.count(plate) == 0)
        return;
    const std::string    model   = current_printer_model();
    const MachineBinding current = plate_machine(plate);
    if (!current.empty() && !current.model.empty() && current.model != model) {
        set_plate_machine(plater, plate, {}); // another model now: that machine cannot print it
        return;
    }
    if (current.empty()) {
        const std::vector<MachineObject*> machines = my_machines_of_model(model);
        if (machines.size() == 1) // the only machine of this model: no need to ask
            set_plate_machine(plater, plate, {machines.front()->get_dev_id(), machines.front()->get_dev_name(), model});
    }
}

void on_machine_selected_by_user(const std::string& dev_id)
{
    if (!plate_process_settings_enabled() || switching_machine() || dev_id.empty())
        return;
    Plater*        plater = wxGetApp().plater();
    DeviceManager* dev    = wxGetApp().getDeviceManager();
    if (plater == nullptr || dev == nullptr)
        return;
    MachineObject* obj = dev->get_my_machine(dev_id);
    if (obj == nullptr || obj->printer_type != current_printer_model())
        return; // a machine of another model does not print this plate
    set_plate_machine(*plater, plater->get_partplate_list().get_curr_plate(), {obj->get_dev_id(), obj->get_dev_name(), obj->printer_type});
}

bool ensure_plate_machine_selected(Plater& plater, bool ask)
{
    if (!plate_process_settings_enabled())
        return true;
    const MachineBinding machine = plate_machine(plater.get_partplate_list().get_curr_plate());
    DeviceManager*       dev     = wxGetApp().getDeviceManager();
    if (machine.empty() || dev == nullptr)
        return true;
    if (MachineObject* selected = dev->get_selected_machine(); selected != nullptr && selected->get_dev_id() == machine.id)
        return true;
    const wxString name = from_u8(machine.name);
    if (dev->get_my_machine_list(dev->get_current_printer_agent_id()).count(machine.id) == 0) {
        // Owner's decision: an unavailable machine is shown, never blocks.
        MessageDialog(&plater, wxString::Format(_L("This plate prints on %s, which is not available now (off, offline or not bound). The action goes on with the selected machine."), name),
                      _L("Machine not available"), wxOK | wxICON_WARNING).ShowModal();
        return true;
    }
    if (ask) {
        MessageDialog question(&plater, wxString::Format(_L("This plate prints on %s. Switch to that machine?"), name),
                               _L("Switch machine"), wxYES_NO | wxICON_QUESTION);
        if (question.ShowModal() != wxID_YES)
            return false;
    }
    switching_machine() = true;
    const bool selected = dev->set_selected_machine(machine.id);
    switching_machine() = false;
    if (!selected) {
        MessageDialog(&plater, wxString::Format(_L("Could not switch to %s."), name), _L("Switch machine"), wxOK | wxICON_WARNING).ShowModal();
        return true;
    }
    // Owner's decision: wait until the switch is done, so the machine is never switched again halfway.
    wxProgressDialog progress(_L("Switch machine"), wxString::Format(_L("Connecting to %s..."), name), 100, &plater,
                              wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        MachineObject* obj = dev->get_selected_machine();
        if (obj != nullptr && obj->get_dev_id() == machine.id && obj->is_info_ready(false))
            return true;
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(45)) {
            progress.Hide();
            MessageDialog(&plater, wxString::Format(_L("%s did not answer yet. The action goes on; check the machine in the Device tab."), name),
                          _L("Switch machine"), wxOK | wxICON_WARNING).ShowModal();
            return true;
        }
        if (!progress.Pulse())
            return false; // cancelled
        wxMilliSleep(100);
    }
}

bool print_plates_one_by_one(Plater& plater, const std::function<void(int)>& print_plate)
{
    if (!plate_process_settings_enabled())
        return false;
    prune(plater);
    PartPlateList& plates = plater.get_partplate_list();
    // Where each plate with objects goes: its machine, or its printer when it has none.
    struct Target { int plate; std::string key; wxString label; bool sliced; };
    std::vector<Target> targets;
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        PartPlate* plate = plates.get_plate(i);
        if (plate == nullptr || plate->empty())
            continue;
        std::string               printer;
        plate_printer_config(plate, printer);
        const MachineBinding machine = plate_machine(plate);
        targets.push_back({i, machine.empty() ? "printer:" + printer : "machine:" + machine.id,
                           from_u8(machine.empty() ? printer : machine.name + " (" + printer + ")"), plate->is_slice_result_valid()});
    }
    const bool several = std::any_of(targets.begin(), targets.end(), [&targets](const Target& t) { return t.key != targets.front().key; });
    if (!several)
        return false;
    wxString list;
    for (const Target& t : targets)
        list += wxString::Format(_L("Plate %d"), t.plate + 1) + wxString::FromUTF8(" \xE2\x86\x92 ") + t.label +
                (t.sliced ? wxString() : wxString(" (") + _L("not sliced: skipped") + ")") + "\n";
    MessageDialog confirm(&plater, _L("The plates go to different machines. Each plate is sent to its own, one after the other:") + "\n\n" + list,
                          _L("Print all plates"), wxOK | wxCANCEL | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_OK)
        return true;
    for (const Target& t : targets) {
        if (!t.sliced)
            continue;
        plater.select_plate(t.plate);                   // its printer, process, filaments
        if (!ensure_plate_machine_selected(plater, false)) // its machine; already confirmed
            break;
        print_plate(t.plate);
    }
    return true;
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
        refresh_from_bundle(it->second);
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

namespace {

// What "Paste special..." may paste; "Paste plate settings" pastes everything.
struct PasteParts
{
    bool plate_options = true; // bed type, print sequence, ... (the plate's own keys)
    bool process       = true; // process settings changed on the plate
    bool presets       = true; // printer, process and filaments
    bool machine       = true; // the physical machine
};

void paste_parts(Plater& plater, PartPlate* plate, const PasteParts& parts)
{
    const PlateClipboard& c = clipboard();
    if (plate == nullptr || !c.has || !plate_process_settings_enabled())
        return;
    prune(plater);
    give_every_plate_a_set(plater, plater.get_partplate_list().get_curr_plate());
    plater.take_snapshot("Paste plate settings");
    DynamicPrintConfig& target = *plate->config();
    // Drop the parts being pasted, then copy them from the clipboard.
    auto pasted = [&parts](const std::string& key) {
        const bool native = MultiPrinter::is_native_plate_key(key);
        return native ? parts.plate_options : parts.process;
    };
    for (const std::string& key : target.keys())
        if (pasted(key))
            target.erase(key);
    for (const std::string& key : c.plate_config.keys())
        if (pasted(key))
            target.set_key_value(key, c.plate_config.option(key)->clone());
    Registry& r = registry();
    if (c.has_set && (parts.presets || parts.machine)) {
        PresetSnapshot set = r.plate_sets.count(plate) ? r.plate_sets[plate] : c.set;
        if (parts.presets) {
            const MachineBinding keep = set.machine;
            set                       = c.set;
            set.machine               = keep;
        }
        if (parts.machine)
            set.machine = c.set.machine;
        r.plate_sets[plate] = set;
        refresh_plate_labels(plater, plate);
        const bool is_current = plater.get_partplate_list().get_curr_plate() == plate;
        if (is_current && parts.presets) {
            // Keep what was edited in the set that leaves the bundle, then show the pasted one.
            if (r.active != plate)
                refresh_from_bundle(active_set_storage());
            r.active   = plate;
            r.swapping = true;
            load_into_app(plater, set);
            r.swapping = false;
        }
        wxGetApp().CallAfter([]() {
            if (Plater* p = wxGetApp().plater())
                p->set_bed_shape(); // the plate may be in another row now
        });
    }
    after_plate_settings_changed(plater, plate);
}

} // namespace

void paste_plate_settings(Plater& plater, PartPlate* plate) { paste_parts(plater, plate, PasteParts{}); }

void paste_plate_settings_special(Plater& plater, PartPlate* plate)
{
    if (plate == nullptr || !clipboard().has)
        return;
    const wxString choices[] = {_L("Plate options (bed type, print sequence, ...)"), _L("Process settings changed on the plate"),
                                _L("Printer, process and filaments"), _L("Machine")};
    wxMultiChoiceDialog dlg(&plater, _L("What to paste on this plate:"), _L("Paste special"), 4, choices);
    dlg.SetSelections(wxArrayInt{});
    if (dlg.ShowModal() != wxID_OK)
        return;
    PasteParts parts{false, false, false, false};
    for (int i : dlg.GetSelections()) {
        if (i == 0) parts.plate_options = true;
        if (i == 1) parts.process = true;
        if (i == 2) parts.presets = true;
        if (i == 3) parts.machine = true;
    }
    paste_parts(plater, plate, parts);
}

void append_plate_presets_menu_item(wxMenu* menu, size_t pos, wxWindow* parent)
{
    const wxString use_own     = _L("Use own printer, process and filaments");
    const wxString use_project = _L("Use the project's printer, process and filaments");
    const wxString copy        = _L("Copy plate settings");
    const wxString paste       = _L("Paste plate settings");
    const wxString paste_some  = _L("Paste special...");
    const wxString machine     = _L("Choose machine...");
    // The plate menu is reused: drop the items added for the previous plate.
    for (const wxString& name : {use_own, use_project, copy, paste, paste_some, machine})
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
    append_menu_item(
        menu, wxID_ANY, paste_some, "", [plate](wxCommandEvent&) { paste_plate_settings_special(*wxGetApp().plater(), plate); }, "",
        nullptr, []() { return can_paste_plate_settings(); }, parent, int(pos) + 2);
    append_menu_item(
        menu, wxID_ANY, machine, "", [plate](wxCommandEvent&) { choose_plate_machine(*wxGetApp().plater(), plate); }, "",
        nullptr, [plate]() { return wxGetApp().plater()->get_partplate_list().get_curr_plate() == plate; }, parent, int(pos) + 3);
}

void choose_plate_machine(Plater& plater, PartPlate* plate)
{
    if (plate == nullptr || plater.get_partplate_list().get_curr_plate() != plate)
        return; // the machine list follows the printer loaded now: the current plate's
    const std::string                 model    = current_printer_model();
    const std::vector<MachineObject*> machines = my_machines_of_model(model);
    const MachineBinding              current  = plate_machine(plate);
    wxArrayString                     names;
    names.Add(_L("No machine"));
    int selection = 0;
    for (size_t i = 0; i < machines.size(); ++i) {
        names.Add(from_u8(machines[i]->get_dev_name()) + (machines[i]->is_online() ? wxString() : wxString(" (") + _L("offline") + ")"));
        if (machines[i]->get_dev_id() == current.id)
            selection = int(i) + 1;
    }
    if (machines.empty() && !current.empty()) { // keep showing a machine that is not listed now
        names.Add(from_u8(current.name) + wxString(" (") + _L("not available") + ")");
        selection = 1;
    }
    wxSingleChoiceDialog dlg(&plater,
                             machines.empty() ? _L("No machine of this printer model is bound or found on the network. Add it in the Device tab.")
                                              : _L("The machine this plate prints on:"),
                             _L("Choose machine"), names);
    dlg.SetSelection(selection);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const int choice = dlg.GetSelection();
    if (choice == 0)
        set_plate_machine(plater, plate, {});
    else if (choice - 1 < int(machines.size()))
        set_plate_machine(plater, plate, {machines[choice - 1]->get_dev_id(), machines[choice - 1]->get_dev_name(), model});
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
        nlohmann::json loaded = nlohmann::json::object(); // what the machine had loaded, per tray
        for (const auto& [tray, config] : set.filament_ams_list)
            loaded[std::to_string(tray)] = config_to_json(config);
        nlohmann::json user_presets = nlohmann::json::array();
        for (const auto& [name, typed] : set.user_presets)
            user_presets.push_back({{"name", name}, {"type", typed.first}, {"config", config_to_json(typed.second)}});
        plates.push_back({{"plate_index", index},
                          {"machine", {{"id", set.machine.id}, {"name", set.machine.name}, {"model", set.machine.model}}},
                          {"machine_filaments", loaded},
                          {"user_presets", user_presets},
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
        if (plate.contains("machine") && plate["machine"].is_object()) {
            const nlohmann::json& m = plate["machine"];
            set.machine             = {m.value("id", std::string()), m.value("name", std::string()), m.value("model", std::string())};
        }
        if (plate.contains("user_presets") && plate["user_presets"].is_array())
            for (const nlohmann::json& p : plate["user_presets"])
                if (p.is_object() && p.contains("name") && p["name"].is_string() && p.contains("type") && p["type"].is_number_integer())
                    set.user_presets[p["name"].get<std::string>()] = {p["type"].get<int>(),
                                                                      config_from_json(p.value("config", nlohmann::json::object()))};
        if (plate.contains("machine_filaments") && plate["machine_filaments"].is_object())
            for (auto it = plate["machine_filaments"].begin(); it != plate["machine_filaments"].end(); ++it)
                set.filament_ams_list[std::atoi(it.key().c_str())] = config_from_json(it.value());
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

ProjectSaveScope::ProjectSaveScope(Plater& plater, bool keep_rows)
{
    // Off: leave Model::multi_printer_plates as loaded, so a project's sets survive a save.
    if (!plate_process_settings_enabled())
        return;
    prune(plater);
    Registry& r = registry();
    m_impl         = new Impl;
    m_impl->plater = &plater;
    // Objects are written where upstream's grid puts their plates, so upstream OrcaSlicer opens the
    // file with every object on its plate (PlateLayout.cpp). Not for backups (no plate moves every few
    // seconds while the user works) nor sliced files (moving objects drops their slicing results).
    const bool rows_layout = keep_rows && layout_active(plater.get_partplate_list());
    if (!keep_rows)
        suspend_plate_layout(plater, true);
    if (r.plate_sets.empty()) {
        plater.model().multi_printer_plates.clear();
        return;
    }
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    // The set in the bundle may have unsaved edits made since it was swapped in.
    refresh_from_bundle(active_set_storage());
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
