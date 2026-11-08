# Multi-printer plates: GUI and preset map

Investigation of OrcaSlicer's GUI and preset code for "each plate has its own printer, process and
filament presets". Branch `multi-printer-plates` at `994439d917` (upstream `e23af6d7b0`), read on
2026-11-08. Line numbers are from that commit. Nothing was built or run.

Scope: presets (PresetBundle/PresetCollection), Sidebar/Tab, plate selection, print/send/export,
undo and dirty state, hook points. The core side (3MF format, PartPlate layout, slicing) is only
touched where the GUI depends on it.

---

## 0. Summary of findings

1. **Orca already has a "swap the whole preset selection" mechanism.** `PresetBundle::Selection`,
   `PresetBundle::selection()` and `PresetBundle::restore_selection()`
   (`src/libslic3r/PresetBundle.hpp:411-429`, `PresetBundle.cpp:5973-6019`) snapshot and restore the
   selected print/filament/printer, their *edited* (dirty) configs, the saved baseline, visibility,
   `filament_presets`, `project_config` and `extruder_ams_counts`. It was added for G-code preview
   (`Plater.cpp:18540-18566`), which also shows the exact GUI refresh sequence after a restore.
   This is the natural base for "N preset sets, one active".
2. **Limitation:** `selection()` returns `nullopt` while any project-embedded preset is loaded,
   and `restore_selection()` calls `reset_project_embedded_presets()` and restores visibility *by
   index*. Most real 3MF projects load embedded presets, so the existing API cannot be used as-is.
   A variant that keeps embedded presets and stores visibility by name is needed.
3. **Recommended model:** one global `PresetBundle` (unchanged), plus a list of *preset sets*
   (Prusa's "config containers") owned by new code; each plate points to a set; selecting a plate
   swaps the active set into the bundle. With the feature off, or with one set, nothing swaps.
   Selection/edit code paths (Sidebar combos, Tab combos, Tab edits) then need **no** interception:
   they edit the active set, which is by definition the selected plate's set.
4. **The real work is the global side effects of a printer change**, which today touch *all*
   plates and *all* objects:
   - `Plater::priv::set_bed_shape` -> `PartPlateList::reset_size` re-lays out every plate with one
     width/depth/height (`Plater.cpp:15356`, `PartPlate.cpp:6358`); one `Bed3D` for all plates.
   - `on_select_preset` re-centres objects of every plate (`Plater.cpp:13374-13386`) and
     `update_objects_position_when_select_preset` resets the wipe tower of every plate
     (`Plater.cpp:14854-14857`).
   - **`Plater::on_filament_count_change` clamps the filament of every volume of every object**
     (`Plater.cpp:21853-21857` -> `ModelVolume::update_extruder_count`, `Model.cpp:2783`, which erases
     the `extruder` key and limits MMU painting). Swapping from a 4-filament set to a 2-filament set
     would destroy the assignments of objects on other plates. `on_filaments_delete` does the same
     (`Plater.cpp:21880-21884`). These loops must be limited to the plates of the active set.
5. Plate selection funnels through two Plater functions, both of which apply the global
   `full_config()` to the plate's `Print` right after `PartPlateList::select_plate`:
   `Plater::select_plate` (`Plater.cpp:23103`, used by ObjectList, preview toolbar, slice-all) and
   `Plater::select_plate_by_hover_id` (`Plater.cpp:23753`, clicks on the 3D plate). The swap must
   happen between `select_plate()` and `background_process.apply()`.
6. Print/send code reads the printer from the global bundle (`printers.get_edited_preset().config`,
   `full_config()`), so single-plate actions work once the plate's set is active. "All plates"
   actions (Print all, Export all sliced, Send all) assume one printer and must be blocked or split
   per set when sets differ.
7. Undo/redo does not record preset changes at all (only printer technology); the plate -> set link
   must be in `PartPlate`'s cereal archive if plate operations are to undo correctly.

---

## 1. PresetBundle / PresetCollection

### 1.1 Where the selection lives

`PresetBundle` (`src/libslic3r/PresetBundle.hpp:215`), one instance, `wxGetApp().preset_bundle`:

| Member | Line | Meaning |
|---|---|---|
| `prints`, `filaments`, `printers` (`PrinterPresetCollection`) | 507-513 | all presets; each collection has one selected preset |
| `physical_printers` | 514 | selected physical printer (host) |
| `filament_presets` | 517 | filament preset name per slot (multi-material); `filaments` itself only holds the "edited" one |
| `extruder_ams_counts`, `filament_ams_list` | 519-522 | AMS state |
| `project_config` | 531 | project-level values saved in 3MF/gcode: `filament_colour`, flush matrix, `curr_bed_type`, `filament_map`, `nozzle_volume_type`, mixed filaments... |

`PresetCollection` (`src/libslic3r/Preset.hpp:572`):
- `m_idx_selected` -> selected preset in `m_presets`.
- `m_edited_preset` (`Preset.hpp:1100`): working copy shown in the Tab; edits go here. "Dirty" =
  `current_is_dirty()` (edited vs selected, `:895`), cached by `update_dirty()` (`:920`).
- `m_saved_preset` (`:1102`): baseline at the last project save/load; `saved_is_dirty()` (`:905`)
  feeds the project dirty state. No public getter (commented out at `:778`); `PresetBundle` is a
  friend (`:1114`).
- `select_preset_by_name(name, force)` (`:925`), `discard_current_changes()` (`:804`).
- Project-embedded presets: `Preset::is_project_embedded` (`:281`), loaded by
  `load_project_embedded_presets`, removed by `reset_project_embedded_presets()` (`:647`).

`full_config()` / `full_fff_config()` (`PresetBundle.cpp:5559`) merge the edited printer, print,
`filament_presets` and `project_config` into the config used for slicing, export and dialogs.

### 1.2 How a 3MF's presets are loaded (Plater::priv::load_files)

`Plater.cpp` around 9300-9970:
1. `Model::read_from_archive(... &plate_data, &project_presets ...)` (`:9324`) returns the parsed
   project config (`Metadata/project_settings.config`) and the embedded presets
   (`Metadata/print_setting_*.config`, `filament_settings_*`, `machine_settings_*`).
2. `preset_bundle.load_project_embedded_presets(project_presets)` (`:9657`,
   `PresetBundle.cpp:1210`) adds them to the three collections as `is_project_embedded`.
3. `preset_bundle->load_config_model(filename, config, ...)` (`:9845`) ->
   `load_config_file_config` (`PresetBundle.cpp:6183`): selects or creates the printer/print/filament
   presets named by `printer_settings_id`, `print_settings_id`, `filament_settings_id` and applies
   the differing keys as dirty edits; sets `filament_presets` and `project_config`.
4. `wxGetApp().load_current_presets(false, false)` (`:9960`) refreshes every Tab and the Sidebar,
   then `q->on_filament_count_change(...)` (`:9964`).

On save, `Plater::export_3mf` (`Plater.cpp:20873`) writes `full_config_secure()` (`:20895`) as the
project config and `get_current_project_embedded_presets()` (`:21032`, `PresetBundle.cpp:1247`) as
embedded presets, through `StoreParams` (`:21034-21049`). Only one printer is ever written.

Consequence for the fork: set 0 = the upstream data (`project_settings.config` + embedded presets),
so upstream Orca/Bambu Studio open the file as "all plates on set 0's printer". Extra sets need their
own entries (new `Metadata/` file with each set's names, diff keys and its embedded presets),
loaded after step 4. Plate keys in `model_settings.config` are parsed key by key
(`bbs_3mf.cpp:4656-4690`), so an unknown plate key is ignored by upstream; still, the format design
belongs to the core report.

### 1.3 The existing snapshot API

```cpp
struct Selection { struct Collection { selected; edited; saved; visible (by index) };
                   prints, filaments, printers; filament_presets; project_config; extruder_ams_counts; };
std::optional<Selection> selection() const;   // nullopt if any project-embedded preset exists
void restore_selection(const Selection&);     // resets embedded presets, selects, restores edited/saved
```
History: added by `e77ba5f672 Open .gcode files with their own settings (#16463)`.
Restore sequence used by the G-code preview (`Plater.cpp:18549-18562`), i.e. the reference "switch
active set" routine:
```cpp
preset_bundle->restore_selection(*previous);
wxGetApp().load_current_presets(false, false);
preset_bundle->project_config = std::move(previous->project_config); // TabPrinter::on_preset_loaded rewrites some
sidebar().update_presets(Preset::TYPE_PRINTER);
reset_project_dirty_initial_presets(); update_project_dirty_from_presets();
```
and `PresetBundle::SelectionExportBlocker` (`PresetBundle.hpp:442`) stops `export_selections()`
writing the temporary selection into `OrcaSlicer.conf` (used at `Plater.cpp:18542`).

### 1.4 What N preset sets need

New type (proposed `src/libslic3r/MultiPrinter/PresetSet.hpp`), one per set:
- per collection: selected preset **name**, edited config, saved config, visibility by name;
- `filament_presets`, `project_config`, `extruder_ams_counts`, `filament_ams_list`;
- selected physical printer name (host for "send");
- the names of the embedded presets the set uses (for saving).

Gaps in the upstream API that block doing this purely in new files:
- `m_saved_preset` and `m_presets` are protected; only `PresetBundle` (friend) can read/write them.
  Options: (a) generalise `selection()/restore_selection()` with a "keep project-embedded presets"
  flag and name-based visibility (small upstream patch, could even go upstream); or (b) one-line
  `friend` hook for a fork class in `PresetCollection`. (a) is cleaner and reusable.
- Embedded presets of *all* sets must stay loaded together (their names may collide between sets
  coming from different projects; upstream renames duplicates on load, check
  `PresetCollection::load_project_embedded_presets`).
- `restore_selection()` calls `update_compatible(Never)` and `update_multi_material_filament_presets()`
  then overwrites `filament_presets`/`project_config`; keep that order.
- During a swap, wrap in `SelectionExportBlocker` so plate switching does not change the user's
  remembered presets. Decide whether a *user-initiated* change in a non-zero set should be
  remembered (probably not).
- Option `remember_printer_config` (`Tab.cpp:7741-7744`) calls `update_selections(app_config)` when
  the printer changes; it would load the per-printer remembered filaments into the active set. That
  is acceptable (it is a user choice) but must not fire during a swap (a swap does not go through
  `Tab::select_preset`, so it does not).

Rejected alternative: one `PresetBundle` per set. Every GUI file uses `wxGetApp().preset_bundle`;
loading vendors/presets N times is slow and memory-heavy; Tabs bind to one bundle.

---

## 2. Sidebar and Tab: what happens on a printer change

### 2.1 From the Sidebar printer combo

`PlaterPresetComboBox::OnSelect` (`PresetComboBoxes.cpp:1020`) -> `wxEVT_COMBOBOX` bound on the
sidebar (`Plater.cpp:8063`) -> `Plater::priv::on_combobox_select` (`:13153`) ->
`Plater::priv::on_select_preset` (`:13219`) then `sidebar->update_printer_thumbnail()`.

`on_select_preset`, printer branch:
1. Records the selected plate's origin and, for each plate, its object indices (`:13237-13253`).
2. Resolves the preset name (alias, physical printer, "printer model" items) (`:13264-13308`).
3. `update_objects_position_when_select_preset(lambda)` (`:13313`, body `:14847`):
   - lambda: `Tab(PRINTER)::select_preset(name)` then `q->on_config_change(full_config())`;
   - `obj_list()->update_object_list_by_printer_technology()`;
   - **resets wipe-tower position on every plate** (`:14854-14857`); `update()`.
4. `auto_calc_flushing_volumes(-1)` if enabled; sync extruders with the selected device (`:13324-13348`).
5. `q->on_config_change(full_config())` again (`:13365`).
6. **"Model reset by plate center"**: if the selected plate's origin moved (bed size changed), for
   every plate select its objects and `center_selected_plate(i)` (`:13374-13386`).
7. Shared-profiles notification, etc.

`Tab::select_preset` (`Tab.cpp:7489`):
- asks to keep/transfer/discard dirty edits (`may_discard_current_dirty_preset`, `:7561-7621`);
- `select_preset_by_name`, `update_compatible(...)` for print and filament (`:7712-7716`);
- `TabPrinter::update_pages()`; `apply_config_from_cache()`;
- with `remember_printer_config`: `update_selections()` + `sidebar().on_filament_count_change()`;
- `load_current_preset()` (`Tab.cpp:7234`) -> `on_presets_changed()` (`:2802`) which updates the
  dependent tabs, the Sidebar combos (`Sidebar::update_presets`, `Plater.cpp:3937`) and calls
  `update_project_dirty_from_presets()` (`:2848`); `TabPrinter::on_preset_loaded()` (`:6435`)
  adjusts extruder count -> `extruders_count_changed` (`:5867`) -> filament list resize.
- `plater->normalize_bed_types(false)` (`:7746-7754`); lifecycle `PresetSelected`.

`Plater::on_config_change` (`Plater.cpp:21977`) diffs against the plater's own `p->config`:
- `printable_area`, `bed_exclude_area`, `extruder_printable_area`, textures, clearances ->
  `bed_shape_changed` -> `set_bed_shape()` (`:22198`) -> `priv::set_bed_shape` (`:15312`):
  `Bed3D::set_shape` (one bed), and if changed `partplate_list.reset_size(w, d, h)` (`:15356`) and
  `set_shapes(...)` — **every plate gets the new size and is re-positioned** in the grid
  (`PartPlate.cpp:6358`, uniform `m_plate_width/depth/height`, `PartPlate.hpp:798`).
- `printer_technology` -> invalidates **all** slice results (`:22035`).
- nozzle count change -> resizes flush matrix in `project_config` (`:22165-22195`).

Filament count: `Plater::on_filament_count_change` (`:21839`) -> `Sidebar::on_filament_count_change`
(`:5722`), `ObjectList::update_objects_list_filament_column`, `PartPlateList::set_filament_count`,
and **`ModelVolume::update_extruder_count` on all objects** (`:21853-21857`).

### 2.2 From the Tab (printer settings page)

`TabPresetComboBox::OnSelect` (`PresetComboBoxes.cpp:1676`) -> `Tab::select_preset` directly, not
through `Plater::priv::on_select_preset`. So the plater-side steps 1, 3 (wipe tower) and 6 do not
run, but `on_config_change` / bed shape / filament count do (via `load_current_preset`). Edits of a
single value: `Tab::on_value_change` (`Tab.cpp:2088`) -> `update_dirty()` (`:1601`) ->
`on_presets_changed` / `on_config_change`.

### 2.3 Where to intercept (feature on)

With the swap model, the *selection* itself needs no hook: it changes the bundle, which is the
selected plate's set. What needs hooks are the global side effects:

| Side effect | Location | Hook |
|---|---|---|
| Re-centre objects of all plates after bed size change | `Plater.cpp:13374-13386` | loop only over plates of the active set (others keep their bed) |
| Reset wipe tower of all plates | `Plater.cpp:14854-14857` | same filter |
| Plate re-layout to one size | `Plater.cpp:15353-15357` -> `PartPlate.cpp:6358` | with >1 set: per-plate size from its set (core/PartPlate work; `PartPlate` already has its own `m_width/m_depth/m_height`, `PartPlate.hpp:752`) |
| One bed model/texture | `Bed3D` in `priv::set_bed_shape` | render per plate, or accept current-plate-only bed model at first |
| Clamp filament ids of all volumes | `Plater.cpp:21853-21857`, `21880-21884`; `GUI_ObjectList` filament column | only objects on plates of the active set; never during a swap |
| Invalidate all slice results on technology change | `Plater.cpp:22035` | only active set's plates (or forbid SLA in multi-set) |
| Sharing a set between plates | UI decision | changing the printer on a plate that shares its set: change the whole set, or fork a new set for that plate (Prusa: bed points to container) |

Where to offer "change printer of this plate only": plate context menu
(`MenuFactory::create_plate_menu`, `GUI_Factories.cpp:1959`, add one `append_menu_item`) and/or the
plate settings dialog (`Plater::open_platesettings_dialog`, `Plater.cpp:23605`;
`PlateSettingsDialog.cpp:392`). A new plate icon would require changing
`PartPlate::GRABBER_COUNT` (`PartPlate.hpp:306`), which shifts picking ids: avoid.

---

## 3. Plate selection: where to switch the active set

Entry points (all user paths end in two functions):

| Caller | Path |
|---|---|
| Click on plate in 3D view | `GLCanvas3D.cpp:5478` -> `Plater::select_plate_by_hover_id` (`Plater.cpp:23753`), action 0 -> `partplate_list.select_plate` (`:23765`) -> `on_plate_selected` -> `background_process.apply(full_config)` (`:23786-23795`) |
| Plate icons (settings, name, filament map, IMEX) | `select_plate_by_hover_id` actions 5/6/8/9 -> `Plater::select_plate` |
| ObjectList plate node | `GUI_ObjectList.cpp:291` -> `Plater::select_plate` |
| Preview plate toolbar | `GLCanvas3D.cpp:10842`, `:11029` -> `Plater::select_plate` / `select_sliced_plate` (`:23264`) |
| Slice all | `Plater.cpp:13823-13826` -> `q->select_plate(m_cur_slice_plate)` |
| Internal (add/delete/move plate, load, reset) | `Plater.cpp:12891, 16450, 23955, 24031`; 12 calls inside `PartPlate.cpp`; `FillBedJob.cpp:60` |

`Plater::select_plate` (`Plater.cpp:23103`): snapshot, `partplate_list.select_plate(idx)`
(`:23108`), then if the background process can switch: update slice context, apply
`full_config()` to the plate's Print (`:23129-23138`), validate, maybe reslice; finally
`on_plate_selected` (`:23257`, only refreshes the ObjectList, `:14341`).

`PartPlateList::select_plate` (`PartPlate.cpp:6885`) holds `m_plates_mutex` (non-recursive) for its
whole body, takes an undo snapshot, moves the bed origin (`set_bed_position`) and fires lifecycle
`PlateSelected` (not while loading a project).

Recommended hook: a call `MultiPrinter::activate_set_for_plate(idx)` in `Plater::select_plate`
right after `:23108` and in `select_plate_by_hover_id` right after `:23765` (both before the
`apply`). Do **not** hook inside `PartPlateList::select_plate`: it runs under `m_plates_mutex`, and
a swap re-enters `PartPlateList` (`reset_size`, `set_shapes`) -> deadlock risk; it is also called
during initialisation and deletion. Internal callers can be covered by a cheap
"ensure active set == current plate's set" check called from the same helper and from the end of
load/new-project/delete-plate.

Not usable: the lifecycle hook (`src/libslic3r/LifecycleEvents.hpp:206-246`) has a single slot owned
by the plugin layer, and `PlateSelected` is skipped during project load.

Cost: a swap = `restore` + `load_current_presets` (refreshes every Tab page and the Sidebar). During
slice-all this runs per plate; group plates by set or add a "bundle only" swap that refreshes the UI
once at the end. Skip the swap when the target set is already active (the common case).

---

## 4. Print / send / export per plate

| Action | Handler | Printer/host source | Per-plate impact |
|---|---|---|---|
| Print plate (BBL network or `use_printer_agents`) | `on_action_print_plate` `Plater.cpp:14127` -> `open_machine_select_dialog(curr_plate)` `:14141` -> `SelectMachineDialog::prepare` `SelectMachine.cpp:1984` | global `full_config()` (`:1224, 1529, 2057, 2134`), `printers.get_edited_preset()` (`:1481, 2441, 2718`), `filament_presets` (`:1394-1410`); device = `DeviceManager::get_selected_machine()` (single) | works if the plate's set is active (it is: current plate). Remember the chosen device per set (new map) |
| Print plate (legacy hosts) | `send_gcode_legacy(PLATE_CURRENT_IDX)` `Plater.cpp:21419` | `printers.get_edited_preset().config` (`:21423`) -> `PrintHostJob` (print_host, apikey, host_type, use_3mf) | works with active set; `physical_printers` selection must be part of the set |
| Print all | `on_action_print_all` `:14278` -> dialog or `send_gcode_legacy(PLATE_ALL_IDX)` | one host | with >1 set: disable, or run once per set (activate set, send its plates) |
| Send G-code | `on_action_send_gcode` `:14300` -> `send_gcode_legacy()` | same | as print plate |
| Export plate sliced file | `export_gcode_3mf()` `:20014` -> `export_3mf(..., plate)` | `full_config_secure()` (`:20895`) as project config, current embedded presets | correct for the current plate if its set is active |
| Export all sliced file | `export_gcode_3mf(true)` `:14316` | one project config for all plates | with >1 set: the `.gcode.3mf` would claim one printer; block or export one file per set |
| Send to printer / Send all (SD card) | `send_to_printer(bool)` `:20008`, `:14324/14332` | global | same as above |
| Send to multi machine | `on_action_send_to_multi_machine` `:14151` | global, current plate | works with active set |
| Print job internals | `set_print_job_plate_idx`, `export_config_3mf(plate)` `:21655`, `send_gcode(plate)` `:21626` | global bundle | the plate passed must belong to the active set; assert/activate |

Rules for the fork: before any action that takes a plate index other than "current", activate that
plate's set (or refuse). For `PLATE_ALL_IDX` actions with mixed sets, the minimum viable behaviour is
a clear message ("plates use different printers; print each plate") and no action.
`PrintHostSendDialog` and its vendor subclasses (`Plater.cpp:21494-21580`) only read the host config
passed in; not inspected further.

---

## 5. Undo/redo and project dirty state

Undo/redo:
- `UndoRedo::SnapshotData` (`src/slic3r/Utils/UndoRedo.hpp:56`) stores only `printer_technology`,
  flags and layer index. Preset selections and edits are **not** undoable today
  (`Plater::priv::take_snapshot` `Plater.cpp:15586`, `undo_redo_to` `:15709/15732` only switches the
  printer technology back).
- `PartPlateList::serialize` (`PartPlate.hpp:1139-1145`) archives plate size, count, current plate
  and each `PartPlate` (`load/save` `:748-772`: index, name, origin, size, flags, objects, `m_config`).
  The plate -> set index must be saved here (or in `PartPlate::m_config`, already archived) so that
  undoing add/delete/move plate keeps the link. Adding a field to the cereal archive is a one-line
  change in each of `load`/`save`; storing it as a key in `PartPlate::m_config` needs no archive
  change but `m_config` is also written to 3MF by core code (check it is not exported blindly).
- Undo of a plate selection (`take_snapshot("select partplate!")`) restores `m_current_plate`; the
  active set must follow after `undo_redo_to` (hook at the end of `undo_redo_to`; `PartPlateList::rebuild_plates_after_deserialize`, `PartPlate.hpp:1126`).
- Changing a plate's set should take a snapshot (plate data change), but the set contents
  themselves stay outside undo, like upstream presets.

Project dirty state (`ProjectDirtyStateManager.cpp`):
- `update_from_presets()` (`:39`) compares current selected names (`GUI_App::get_selected_presets`),
  `filament_presets`, `filament_colour` and `project_config` with the values recorded by
  `reset_initial_presets()` (`:110`), plus `GUI_App::has_unsaved_preset_changes()`
  (`GUI_App.cpp:9261`, active Tabs only).
- With swaps, every swap would look like a project change. Needed: compare **all** sets (inactive
  sets stored, active from bundle) with initial copies, and suppress/reset during a swap
  (`begin_suppress_notifications` `:94` exists). Hook: one call at the top of `update_from_presets`
  delegating to fork code when the feature is on, and in `reset_initial_presets` / `reset_after_save`.
- Close/switch-project prompts (`GUI_App::check_and_save_current_preset_changes`, `:9315`) only see
  the active set's dirty edits; inactive sets with unsaved edits need the same prompt (hook in
  `has_unsaved_preset_changes`).

---

## 6. Hook points

Churn = commits touching the file since 2026-08-08 (`git log --since`), a proxy for merge-conflict
risk. Last commit date in brackets. All hooks marked `// [multi-printer]`.

| # | File:line | Hook | Churn / risk |
|---|---|---|---|
| H1 | `src/libslic3r/PresetBundle.hpp:411-429`, `.cpp:5973-6019` | Generalise `Selection` (keep embedded presets, visibility by name, physical printer) or add a fork friend; used by the set switcher | 34 hpp / 111 cpp [11-06]; high, but small isolated function added recently |
| H2 | `src/slic3r/GUI/Plater.cpp:23108` (`Plater::select_plate`) | `MultiPrinter::activate_set_for_plate(idx)` before `background_process.apply` | 309 commits [11-06]; very high churn file, 1-line hook, low conflict |
| H3 | `Plater.cpp:23765` (`select_plate_by_hover_id`, action 0) | same call | same |
| H4 | `Plater.cpp:13374-13386` (`on_select_preset`, plate re-centre) | iterate only plates of the active set | high; touches a loop body, medium |
| H5 | `Plater.cpp:14854-14857` (`update_objects_position_when_select_preset`) | wipe-tower reset only for active set's plates | high; small |
| H6 | `Plater.cpp:15353-15357` (`priv::set_bed_shape`) | per-set plate size / layout instead of uniform `reset_size` (depends on core PartPlate design) | high; medium-large |
| H7 | `Plater.cpp:21853-21857` (`on_filament_count_change`), `21880-21884` (`on_filaments_delete`) | filter `model().objects` to plates of the active set; skip during swap. **Required for correctness** | high; small |
| H8 | `Plater.cpp:9960-9965` (after project config load) | load extra sets from fork 3MF metadata, assign plates | high; 1 line |
| H9 | `Plater.cpp:21032-21049` (`export_3mf`) | make sure set 0 is active when writing project config (upstream view) and pass extra sets to the fork writer | high; 1-3 lines |
| H10 | `Plater.cpp:14278` (`on_action_print_all`), `14316`, `14332`, `20008`, `20014` | block or split "all plates" actions when sets differ | high; 1 line each |
| H11 | `Plater.cpp:21423` (`send_gcode_legacy`) | assert/activate plate's set when `plate_idx` is not current | high; 1 line |
| H12 | `Plater.cpp:16381` (`new_project`), `11102` (`priv::reset`) | clear sets | high; 1 line |
| H13 | `Plater.cpp:15709` (`undo_redo_to`) | re-activate set of current plate after restore | high; 1 line |
| H14 | `src/slic3r/GUI/PartPlate.hpp:748-772` | archive plate -> set index for undo (or store in `m_config`) | 48 [10-29]; medium |
| H15 | `src/slic3r/GUI/ProjectDirtyStateManager.cpp:39, 110, 67` | dirty state across all sets; suppress during swap | 2 [10-02]; low |
| H16 | `src/slic3r/GUI/GUI_App.cpp:9261` (`has_unsaved_preset_changes`) | include inactive sets' dirty edits | 103 [11-07]; low (small function) |
| H17 | `src/slic3r/GUI/GUI_Factories.cpp:1959` (`create_plate_menu`) | "Printer for this plate..." menu item -> fork dialog | 32 [11-02]; low |
| H18 | `Plater.cpp:23605` (`open_platesettings_dialog`) / `PlateSettingsDialog.cpp:392` | optional printer/process selector in plate settings | 5 [10-02] for dialog; low |
| H19 | `src/slic3r/GUI/SelectMachine.cpp:1984` (`prepare`) | preselect remembered device for the active set | 26 [10-31]; low |
| H20 | Preferences (`Preferences.cpp`) | feature switch, off by default (not inspected) | n/a |

Hooks not needed with the swap model: `Plater::priv::on_select_preset` filament/print branches,
`Tab::select_preset`, `PlaterPresetComboBox`/`TabPresetComboBox` (`PresetComboBoxes.cpp`, 16 commits).

New files (proposed): `src/libslic3r/MultiPrinter/PresetSet.{hpp,cpp}` (data, capture/restore on
top of H1), `src/slic3r/GUI/MultiPrinter/PresetSetSwitcher.{hpp,cpp}` (activate, GUI refresh,
blockers), `src/slic3r/GUI/MultiPrinter/PlatePrinterDialog.{hpp,cpp}` (choose/fork set),
plus `localization/i18n/list.txt` entries.

---

## 7. Risks and open questions

- **Filament index namespaces.** Each set has its own filament list; object/volume `extruder`,
  MMU painting, support filament, plate `first_layer_print_sequence`, `filament_map` all index the
  active set's list. Moving an object to a plate of another set needs a remap or clamp policy.
  Arrange/fill-bed across plates of different sets would also move objects across namespaces.
- **Plate sizes.** Orca lays out all plates with one size and one `Bed3D`. Different printers mean
  different plate sizes; this is core `PartPlateList` work (compute origins from per-plate sizes,
  picking, unprintable plate, camera "zoom to plate"). Without it, the GUI works but plates are drawn
  at the active printer's size.
- **Printer technology.** Mixing FFF and SLA sets would invalidate all slices and switch the
  ObjectList mode on each swap; simplest is to allow FFF sets only.
- **AMS / device sync.** `Sidebar::sync_extruder_list`, `load_ams_list` and the sync badges assume one
  printer and one selected device; they write into `filament_presets`/`project_config` of the active
  set only, which is acceptable.
- **Swap cost and flicker.** `load_current_presets` rebuilds Tab pages; use `wxWindowUpdateLocker`
  on the sidebar/tabs and skip no-op swaps.
- **`remember_printer_config`** and `export_selections` must be blocked during swaps
  (`SelectionExportBlocker`).

## 8. Not found / not checked

- No existing per-plate printer or "config container" concept anywhere in Orca; plates only carry
  `PartPlate::m_config` (bed type, print sequence, spiral mode, filament map, IMEX).
- Did not inspect `PrintHostSendDialog` subclasses, `SendMultiMachinePage`, `PrintJob`/`SendJob`
  internals, `Preferences.cpp`, or the CLI (`OrcaSlicer.cpp`) path for loading multi-set 3MFs.
- Did not check upstream GitHub issues/PRs (no network use in this task); see fork RULES 2.6.
- Did not verify how `PresetCollection::load_project_embedded_presets` handles name collisions
  between embedded presets of two sets.
- `git log -n 20` per file was replaced by commit counts over 3 months (same information, shorter).
