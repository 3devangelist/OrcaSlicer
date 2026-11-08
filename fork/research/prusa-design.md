# PrusaSlicer 3.0.0-alpha14: multiple beds with different configs (design study)

Source: local clone `PrusaSlicer-3.0-ref`, tag `version_3.0.0-alpha14`. Read-only study, nothing built or run.
Goal: understand the design well enough to port it *by design* to OrcaSlicer (classic Slic3r architecture).

Path abbreviations used below:

- `D/` = `src/slic3r-domain/` (pure data model, new in 3.0)
- `S/` = `src/slic3r-shared/` (`Biz/` = business logic "interactors", `App/` = new Yoga/ImGui UI)
- `L/` = `src/libslic3r/`

Terminology in the code: a **ConfigContainer** is shown in the UI as a **"printer group"**. A printer
group owns N **BedInstances** ("Bed 1", "Bed 2"...) that all share one config and one **Bed** geometry.

---

## 1. Data model

```
Project
 +- ConfigContainerList m_config_containers   (vector<unique_ptr<ConfigContainer>>)
 |    ConfigContainer  (= "printer group", has ObjectBase id)
 |     +- Preset::SelectedPreset m_preset       hw_config + printer + print + tools[] + materials[] (full values)
 |     +- ProjectSettings m_project_settings    FDM "project" keys (extruder colours, wiping matrix)
 |     +- SLAProjectSettings
 |     +- VirtualExtruders                      colour-mix / gradient virtual extruders
 |     +- PhysicalPrinterSelection              {uuid, name} of the physical printer to send to
 |     +- const Bed* m_bed                      shared geometry, owned by Project::BedContainer
 |     +- BedInstanceList m_bed_instances       the actual plates of this group
 |          BedInstance: bed ref, Transformation (scene offset), model_instances[],
 |                       colliding_instances, wipe_tower, custom_gcode (colour changes), index/name
 +- BedContainer m_bed_container               deduplicated Bed geometries (vector<unique_ptr<Bed>>)
 +- Model m_model                              ONE model for the whole project (all objects, all beds)
 +- ModelInstanceList m_unplaced_model_instances   instances not fully inside any bed
```

References:

- `Project` members: `D/include/Slic3r/Domain/Project.hpp:171-180`; lookup helpers
  `find_config_container[_by_bed_instance_id|_by_element]`, `find_bed_instance_by_id`, `find_bed_by_id`:
  `Project.hpp:96-130`, implemented in `D/src/Slic3r/Domain/Project.cpp:153-211`.
- Deep copy (for undo / clipboard) remaps `Bed*` and `ModelInstance*` through translation tables:
  `Project.cpp:10-40`, `ConfigContainer.cpp:6-29`, `BedInstance.cpp:7-40`.
- `ConfigContainer`: `D/include/Slic3r/Domain/ConfigContainer.hpp:19-131`.
- `BedInstance`: `D/include/Slic3r/Domain/BedInstance.hpp:29-64`. Name is `"Bed " + index`
  (`BedInstance.cpp:47-53`). Per-bed state = transformation, wipe tower position, custom G-code
  (colour change list), `print_volume_enabled`.
- `Bed`: `D/include/Slic3r/Domain/Bed.hpp:49-127` (type, contour, contour mesh, max print height,
  bed segments, auxiliary travel anchor, model/texture file names, centre, AABB).
- `BedRef {config_container_id, instance_id}`: `D/include/Slic3r/Domain/BedRef.hpp:6-25`. This pair is
  the universal "which bed" key used by selection, slicing and the object list.
- `SlicingId {project_id, bed_instance_id}`: `D/include/Slic3r/Domain/SlicingId.hpp`.

### What a ConfigContainer holds

`Preset::SelectedPreset` (`D/include/Slic3r/Domain/Preset/SelectedPreset.hpp:22-47`):

| field | content |
|---|---|
| `hw_config` | `HwPrinterConfig`: technology, tool count, tools, feeders, material slots, features (new 3.0 "hardware config" concept above printer presets) |
| `printer` | `EvaluatedPrinterPreset::Preset` |
| `print` | `EvaluatedPrintPreset::Preset` |
| `tools` | `vector<EvaluatedToolPrintPreset::Preset>` (per-tool print overrides, new in 3.0) |
| `materials` | `vector<EvaluatedMaterialPreset::Preset>` (one per material slot) |
| `material_matdb_uuids` | material database links per slot |
| `macro_overrides` | "print mode" macro override layers |

Important: each `EvaluatedPreset` is a **full value snapshot** (`id`, `root_id`, `name`, `origin`,
`values` config box, `features`) - `D/include/Slic3r/Domain/Preset/EvaluatedPreset.hpp:33-151`. The
container does not reference presets by name; it owns a copy of the values. User edits are written
straight into this copy. `SelectedPreset::config()` / `ConfigContainer::build_print_config()`
(`D/src/Slic3r/Domain/ConfigContainer.cpp:31-41`) produce the `ConfigPack` used for slicing by adding
the container's `ProjectSettings` and `VirtualExtruders`.

Container-level ("project") FDM keys are only three: `extruder_colour`, `wiping_volumes_matrix`,
`wiping_volumes_use_custom_matrix` (`D/src/Slic3r/Domain/ConfigDefsFDM.cpp:979-982, 4781-4796`,
location `FDMConfigLocation::Project`). So wiping/purge matrix and slot colours are per printer group.
Managed by `S/include/Slic3r/Biz/ProjectSettingsInteractor.hpp:35-90`.

### How a bed instance links to its container

Ownership: the container owns its `BedInstance`s (`ConfigContainer::add_bed_instance`,
`ConfigContainer.cpp:43-48`); a bed instance holds a `reference_wrapper<const Bed>` to the shared
geometry. There is no back-pointer from instance to container; lookups go through `BedRef` or
`Project::find_config_container_by_bed_instance_id`.

### How objects are assigned to beds

Purely **geometric**, recomputed, never stored in the 3MF:

- `BedTracking::update_instance_bed_placement` (`S/src/Slic3r/Biz/Scene/BedTracking.cpp:242-271`): for
  each `ModelInstance`, find the bed instance whose (offset) contour fully contains its 2D hull. If
  `Inside` -> pushed to `bed_instance->model_instances` and `inst.set_last_bed(BedRef)`; if colliding ->
  `colliding_instances`; otherwise -> `Project::unplaced_model_instances`.
- `ModelInstance::m_last_bed` (`D/include/Slic3r/Domain/ModelInstance.hpp:36,47-48`) caches the last bed,
  used e.g. by `Project::find_config_container_by_element` (`Project.cpp:179-211`) to know which
  config applies to an object (per-object settings UI).
- Full rebuild after load: `SceneInteractor::prepare_added_project`
  (`S/src/Slic3r/Biz/Scene/SceneInteractor.cpp:2124-2138`).
- Change notifications use `BedTrackingChanges` (`S/include/Slic3r/Biz/Scene/BedTracking.hpp:18-52`).

So "moving an object to another printer" = moving it in the scene onto another bed.

### How bed geometry comes from the printer preset

- `Scene::get_or_create_bed` (`S/src/Slic3r/Biz/Scene/BedFactory.cpp:9-26`) reads `bed_shape` from the
  container's printer preset, detects bed type, builds the contour mesh.
- `BedContainer::get_or_create_bed` (`D/src/Slic3r/Domain/BedContainer.cpp:30-117`) adds
  `max_print_height`, `bed_custom_model/texture`, vendor model/texture (only if `bed_shape` equals the
  system preset's shape, via a `system_preset_bed_shape_getter` callback,
  `PresetInteractor::system_preset_bed_shape`, `S/src/Slic3r/Biz/Preset/PresetInteractor.cpp:1453-1488`),
  and hw features `bed_segments_x/y`, `auxiliary_travel_anchor_x/y`. It deduplicates with
  `Bed::matches` so two groups with the same printer share one `Bed`.
- When a container's printer preset changes, `SceneInteractor::on_preset_selection_changed`
  (`SceneInteractor.cpp:2713`) -> `update_config_container_bed` (`SceneInteractor.cpp:2637-2711`): get or
  create the new `Bed`, rebind all bed instances, re-run `BedPlacement::layout`, drop the old `Bed` if
  unused, re-run bed tracking for affected instances, notify listeners.

---

## 2. Presets per container

### Evaluation

- Presets are evaluated globally once (the `Bundle`, with `evaluated_presets` keyed by hw config id).
  Per project there is a `PresetInteractorProjectContext`
  (`S/include/Slic3r/Biz/Preset/PresetInteractorProjectContext.hpp:71-87`) holding the selected
  container id, a map of per-container contexts (`PresetInteractorConfigContainerContext` only stores
  `config_container_id`, `PresetInteractorConfigContainerContext.hpp:96-99`) and **RuntimePresets**
  (`PresetInteractorProjectContext.hpp:18-69`): presets that came from a 3MF and do not exist in the
  local bundle, keyed by parent (hw config -> printer -> print -> tools/materials).
- `ProjectPresetView.hpp` (`S/include/Slic3r/Biz/Preset/ProjectPresetView.hpp:57-302+`) gives a unified
  read-only view `bundle presets ++ runtime presets` for each level, with `find_with_same_values` and
  `has_conflicting_id`. Pure logic over two containers.
- Choosing a printer for a container: `PresetInteractor::fill_config_container_with_selected_preset`
  (`PresetInteractor.cpp:1582-1691`). Full switch picks print = printer's `default_print` (or first),
  tools = `default_tool_print`, materials = `default_material` per slot, builds a new `SelectedPreset`,
  resizes tool/material vectors to the hw config. Printer-only switch keeps print/tools/materials.
  Before switching it runs `process_all_save_changes` (save / transfer / discard unsaved edits,
  `PresetInteractor.cpp:3093-3134`) and afterwards applies "transfer" values.
- New container: `initialize_config_container_with_selected` copies the currently selected container's
  `SelectedPreset` (`PresetInteractor.cpp:914-918`); the very first one uses
  `initialize_config_container_with_default` (hard-coded "Prusa CORE One", `PresetInteractor.cpp:879-912`).
- Switching container: `on_selected_config_container_changed` (`PresetInteractor.cpp:928-963`) refills all
  combo lists (printer configs, printers, tools, sheets, prints, tool prints, materials) from the newly
  selected container and notifies listeners. **Only the selected container is "live" in the preset UI**;
  all editing goes through `mutable_selected_printer_preset()` (`PresetInteractor.cpp:1490-1496`).

### Edit / save (`Project::update_preset`)

- `Project::update_preset(...)` (4 overloads, `D/include/Slic3r/Domain/Project.hpp:132-154`,
  `D/src/Slic3r/Domain/Project.cpp:213-325`) walks every container and overwrites any
  printer/print/tool/material snapshot whose `id` equals the saved preset's id. It is the intended
  "propagate a saved preset to all groups using it" hook.
- **Finding: in alpha14 `Project::update_preset` has no callers** (grep over the whole tree finds only the
  declaration and definition). Saving goes through `PresetInteractor::save_user_preset` ->
  `save_user_preset_internal` -> free function `save_preset` (`PresetInteractor.cpp:445-509, 562-816`):
  system/runtime presets are copied to a new user preset (new uuid, `based_id` feature), written to disk,
  inserted in the bundle; then `reload_vendor_presets` re-evaluates everything and
  `refresh_original_config_boxes` re-binds the original boxes. Only the **selected** container's snapshot
  gets the new id; other containers that held the old preset keep their old snapshot. Treat
  `update_preset` as designed-but-unused.

### Dirty / modified state per container

- There is no stored "dirty" flag in the domain. Because the container owns the values, unsaved edits
  simply live in its `SelectedPreset` and survive switching between containers.
- Dirtiness is computed by the config-box interactors comparing the snapshot with the original preset
  from the bundle: `is_printer_preset_selected_and_dirty`, `is_print_...`, `is_tool_print_...`,
  `is_tool_material_...` (`PresetInteractor.cpp:1285-1353`), `print_has_unsaved_changes` etc.
  (`PresetInteractor.cpp:1917-1938`). All of them only answer for the currently selected container.
- UI shows it for the selected group only (printer label colour: `SidebarBed::refresh_printer_label_color`,
  `S/src/Slic3r/App/SidebarBed.cpp:299-303`).
- Discarding: `discard_selected_*_preset_changes` (`PresetInteractor.cpp:1355-1443`).

### Loading presets from a 3MF

`PresetInteractor::load_selected_preset_from_3mf` (`PresetInteractor.cpp:3716-3830+`), called per container
from `ProjectInteractor::do_load_project` (`S/src/Slic3r/Biz/ProjectInteractor.cpp:171-215`): marks
all as `Runtime`, migrates the hw config, then for each level hw config -> printer -> print -> tools ->
materials: if a bundle/runtime preset **with identical values** exists, reuse it; otherwise add it to
RuntimePresets (new uuid if the id collides with different values). This is the dedup that lets two
groups from a file share one runtime preset.

---

## 3. Slicing and export

- **One background process (one `IPrint`) per BedInstance**, not per container:
  `std::map<SlicingId, BackgroundProcess> m_processes`
  (`S/include/Slic3r/Biz/Slicing/SlicingInteractor.hpp:184-185`). `IPrint` is created by
  `init_print(technology, ...)` (`S/src/Slic3r/Biz/Slicing/BackgroundProcess.cpp:101`), so an SLA group and
  an FDM group can coexist.
- Input of a process = `UpdateRequest {Model&, ProjectMetadata, SelectedPresetMetadata, ConfigPack, BedInstance&}`
  (`SlicingInteractor.hpp:73-80`).
- Config pick: `ProjectInteractor::on_slicing_input_changed(BedRef)` (`ProjectInteractor.cpp:645-665`)
  finds the bed instance and its container and calls
  `update_process(model, metadata, cc->selected_preset().metadata(), cc->build_print_config(), bed)`.
  The same is done for all beds of a container on colour / virtual extruder changes
  (`ProjectInteractor.cpp:672-720`).
- `SlicingInteractor::update_process` (`S/src/Slic3r/Biz/Slicing/SlicingInteractor.cpp:67-91`): creates the
  process or queues an update (stopping the running slice). If the hw config id changed, the process is
  destroyed and recreated (`SlicingInteractor.cpp:425-439`); `BackgroundProcess::update` asserts the hw
  id is unchanged (`BackgroundProcess.cpp:149`).
- Bed restriction inside the print: `Print::update` (`L/src/libslic3r/Print.cpp:417-440`) uses
  `with_limited_instances(model, bed.model_instances, ...)` and temporarily transforms instances by
  `bed.transformation.inverse()`, so the shared Model is sliced in bed-local coordinates. This is the
  same trick PrusaSlicer 2.9 multi-bed uses, applied per bed instance.
- Concurrency: at most 2 active slicing threads (`SlicingInteractor.cpp:378`). `slice_all()` queues all
  modified processes (`SlicingInteractor.cpp:166-178`). Auto-slicing is enabled for one bed id.
- **G-code export / send is per selected bed only**: `ExportActions::export_gcode`,
  `export_gcode_to_flash`, `send_gcode_to_connect`, `upload_gcode_to_print_host`
  (`S/src/Slic3r/App/ResultExport/ExportActions.cpp:21-160`) all use
  `ProjectInteractor::selected_bed_slicing_id()` = last selected bed (`ProjectInteractor.cpp:627-630`).
  No "export all beds" action was found.
- Physical printer per group: `ConfigContainer::physical_printer_selection` set in
  `PhysicalPrinterInteractor.cpp:176-179`, validated against the group's hw config
  (`PhysicalPrinterInteractor.cpp:242-245`), reselected on container change (`:606`). So "send to
  printer" naturally goes to the group's own printer.
- G-code metadata is built from the container's `SelectedPresetMetadata`
  (`BackgroundProcess.cpp:37-90`).

---

## 4. 3MF format

### Files

| file | content |
|---|---|
| `3D/3dmodel.model` | standard geometry + build items (all objects of all beds, scene coordinates) |
| `Metadata/PrusaSlicer3_project.json` | **all 3.0 project data** incl. per-bed configs |
| `Metadata/Slic3r_facets_annotation.json` | painting (also dual-written as legacy `slic3rpe:mmu_segmentation` attributes for 2.x readers, `S/src/Slic3r/Biz/Format/3mf/Model3mf.cpp:1251-1260, 1884-1933`) |
| `Metadata/thumbnail.png` | thumbnail (`S/src/Slic3r/Biz/Format/3mf.cpp:59`) |

3.0 does **not** write `Metadata/Slic3r_PE.config` / `Slic3r_PE_model.config` (only the legacy reader
references them: `S/src/Slic3r/Biz/Config/Legacy/3mf_legacy.cpp`; file-type sniffing in
`S/src/Slic3r/Biz/FileOpen/FileOpeningInteractor.cpp:179-180`).

### Keys (`S/include/Slic3r/Biz/Format/ProjectFileConstants.hpp:7-14`)

Writer: `ProjectFileSerialization::write` (`S/src/Slic3r/Biz/Format/3mf/PrusaFile.cpp:1627-1720`).

```jsonc
{
  "objects": [ ... per-object/volume settings, keyed by 3MF object id ... ],
  "project": { "id": "<uuid>", "version": N },
  "config_containers": [                        // one per printer group, in order
    {
      "beds": [                                 // one per BedInstance, in order
        { "position_x": 0.0, "position_y": 0.0,   // BedInstance offset in scene
          "wipe_tower": {"x":..,"y":..,"rotation_angle":..} | null,
          "custom_gcode": {"mode": int, "gcodes": [{"gcode": {"type","print_z","extruder","color","extra"}}]} | null }
      ],
      "preset": {                               // SelectedPresetMetadata (SelectedPresetJson.cpp:47-57)
        "hw_config": {...}, "printer": {name,id,root_id,conditions,features?},
        "print": {...}, "tools": [...], "materials": [...], "material_matdb_uuids": [...]?
      },
      "virtual_extruders": {...},               // optional
      "physical_printer": {"uuid": "...", "name": "..."},   // optional
      "configuration": {                        // full values of build_print_config()
        "printer_settings": {...}, "print_settings": {...},
        "toolprint_settings": [...], "filament_settings": [...],
        "project_settings": {...}, "macro_overrides": {...}?
      }
    }
  ]
}
```

- Location names: `D/src/Slic3r/Domain/ConfigDef.cpp:341-353`; config pack to JSON:
  `S/src/Slic3r/Biz/Config/ConfigSerialize.cpp:135-171`; load: `ConfigLoad.cpp:469-500`.
- **Object-to-bed mapping is not stored.** Only bed offsets are; on load, objects are re-assigned
  geometrically (section 1). Bed-to-config mapping is implicit: a bed belongs to the container whose
  `beds` array contains it.
- Loader: `ProjectFileSerialization::load` (`PrusaFile.cpp:1724-1830`) fills
  `Loaded3MF::ConfigContainerData` (preset metadata, config pack, virtual extruders, physical printer,
  bed offsets, wipe towers, custom G-codes). Note the comment `// TODO: handle multiple config
  containers, add check that the keys exist in the JSON.` (`PrusaFile.cpp:1750`) although the loop does
  iterate all containers. `convert_to_project` (`S/src/Slic3r/Biz/FileLoadingLogic.cpp:1007-1100`)
  builds the containers, beds and bed instances from that data.

### Old 2.x projects

- Model parser detects a pre-3.0 file (`last_old_stored_version = 2.99.1`,
  `Model3mf.cpp:151, 917-957`) and throws `legacy_loader_required`; `load_from_project` falls back to
  `load_legacy_project` (`FileLoadingLogic.cpp:598-694`), which reads `Slic3r_PE.config` into one config
  pack and maps it to 3.0 preset metadata (`PresetMetadataLegacyLoader`).
- Result is **one container**. Bed positions are inferred like 2.9 did:
  `infer_bed_positions_and_create_beds` (`FileLoadingLogic.cpp:358-433`) builds 9 virtual beds with the
  2.x grid gap rule (`get_bed_translation`, `:333-354`), finds the highest bed index that fully contains
  an instance, and emits that many bed offsets.
- If preset mapping fails (`hw_config.id` empty), the UI says "Loading legacy 3MF projects is not supported
  yet." (`FileLoadingLogic.cpp:1029-1036`).
- 3MF without any PrusaSlicer config (Bambu/Orca): geometry only, with an info dialog
  (`FileLoadingLogic.cpp:1011-1026`).

### Can other slicers read it?

Only geometry (and MMU painting through the dual-written legacy attributes). Orca, Bambu and PrusaSlicer
2.9 do not know `PrusaSlicer3_project.json`, and no `Slic3r_PE.config` is written, so they will load the
objects (all beds' objects, at their scene positions) with their own current config. I did not find any
export-compatibility mode.

---

## 5. UI flow

- Object list window: button **"Add printer group"** (`S/src/Slic3r/App/ObjectListWindow.cpp:40-50`) ->
  `ProjectInteractor::add_config_container` (`ProjectInteractor.cpp:1201-1219`): new container, copy of
  the selected container's presets, `initialize_bed` (`:121-136`, creates/gets Bed + first bed instance),
  select it, undo snapshot.
- Each group row (`PrinterGroupItem`, name = `hw_config.name`,
  `S/src/Slic3r/Biz/ObjectList/ConfigContainerObservableList.cpp:47-51`) has an **"Add bed"** button and a
  context menu **Delete / Duplicate** (`S/src/Slic3r/App/ObjectList/PrinterGroupItem.cpp:45-58`).
  Duplicate = select + add (`ProjectInteractor.cpp:1247-1251`). Delete removes all bed instances then the
  container (`:1253-1286`, needs at least 2 groups).
- Each bed row (`ConfigContainerBedItem` / `CCBedButton`) shows a live thumbnail, name, slicing state badge
  (SLICED / UPDATING / STOPPING / SLICING / INVALID / clickable SLICE) and "Remove this bed"
  (`S/src/Slic3r/App/ObjectList/ConfigContainerBedItem.cpp:61, 170-200`), a wipe tower child row, then
  the objects on that bed.
- Selecting a bed (click in list or scene) changes `BedSelection`; `ProjectInteractor::on_selected_bed_instances_changed`
  (`ProjectInteractor.cpp:632-639`) selects that bed's container, which fans out
  `on_selected_config_container_changed` to PresetInteractor, PhysicalPrinterInteractor,
  ExportDestinationInteractor, etc.
- Sidebar (`SidebarBed`, `S/src/Slic3r/App/SidebarBed.cpp`): bed name (`:56, 278-297`), printer button
  (logical printer + physical printer, opens `PrinterSettingsDialog` / `PrinterAddDialog`, `:61, 139-160`),
  **Preset** combo for the printer preset (`:68-92`, calls `select_printer_preset` on the selected
  container), "Preset settings" (advanced dialog), "Color mix settings", material slot buttons (one per
  slot, opening the material selection dialog), "Purging volumes". Changing the printer there changes it
  for the **whole group** (all its beds).
- Bed menu (`S/src/Slic3r/App/MenuCommandRegistrar.cpp:214-330`): delete bed removes a bed instance, or the
  whole container if it was the group's last bed.
- Scene layout (`S/src/Slic3r/Biz/Scene/BedPlacement.cpp:30-113`): **one row per container along +Y, bed
  instances of that container side by side along +X**. Row height = max(bed contour AABB, bed 3D model
  AABB) + gap; gap `BED_GAP = {20, 20}` mm (`SceneInteractor.cpp:111`). `layout()` also moves all model
  instances with their bed by the delta transform, so changing a group's printer to a larger bed reflows
  every row below it without objects falling off their beds. `next_bed_placement` gives the slot for the
  next bed of a group.

---

## 6. Limitations / TODOs seen in the code

1. `Project::update_preset` is unused: saving a preset does not refresh other groups that used the same
   preset (`Project.cpp:283-325`, no callers).
2. Dirty state and preset editing exist only for the selected container; no per-group "modified" badge
   in the object list.
3. Export / send only for the selected bed; no batch export of all beds (`ExportActions.cpp`).
4. Object-to-bed assignment is geometric only and not saved; an object straddling two beds becomes
   "unplaced" (`BedTracking.cpp:254-271`).
5. Legacy 2.x loading may fail with "Loading legacy 3MF projects is not supported yet."
   (`FileLoadingLogic.cpp:1029-1036`); 2.x bed inference is capped at 9 beds (`FileLoadingLogic.cpp:394`).
6. `// TODO: handle multiple config containers, add check that the keys exist in the JSON.`
   (`PrusaFile.cpp:1750`) - loader assumes keys are present (`project_json[CONFIG_CONTAINERS]`).
7. Default printer hard-coded to "Prusa CORE One" with `// TODO: remove this when config wizard is ready`
   (`PresetInteractor.cpp:340`; name at `:882`).
8. `// TODO: better choose tool-print preset + ask for config values transfer` when switching printer
   (`PresetInteractor.cpp:1630, 1645`).
9. `// TODO: initialize ConfigContainerContext here` (`PresetInteractor.cpp:3578`);
   `// TODO: preset_bundle may change, making its copy wouldn't help` (`ProjectInteractor.cpp:324`).
10. `OnBeds.hpp` is a "Temporary header (hopefully)" for wipe tower / custom G-code maps keyed by bed index.
11. Max 2 concurrent slicing processes (`SlicingInteractor.cpp:378`); each process keeps references into
    the shared `Model` (warning in `SlicingInteractor.hpp:111-115`).
12. Facet painting state ids are project-wide while virtual extruders are per group, so removing a
    virtual extruder must scan all groups (`ProjectInteractor.cpp:743-760`).
13. Result is not readable by other slicers beyond geometry (section 4).

Not found: per-bed (as opposed to per-group) config overrides; any notion of a bed belonging to two
groups; an explicit "assign object to printer" command other than moving it; a compatibility writer for
2.9/Orca.

---

## 7. Portability to OrcaSlicer

### Nearly as-is (pure logic, little or no 3.0 layering)

| piece | why it ports | adaptation |
|---|---|---|
| `BedRef` / `SlicingId` (`BedRef.hpp`, `SlicingId.hpp`) | plain structs | rename to `{group_id, plate_index}` |
| Container/bed ownership shape (`ConfigContainer` + `BedInstance` lists, `Project::find_*`) | simple containers | hold Orca `DynamicPrintConfig` (or preset names + dirty diff) instead of `SelectedPreset` |
| `BedContainer::get_or_create_bed` dedup by geometry | small function | use Orca's `bed_shape`, `printable_height`, bed model/texture options; `hw_config` features have no Orca equivalent |
| `BedPlacement::layout` / `next_bed_placement` (rows per group, beds along X, objects moved by delta) | ~80 lines of math | Orca `PartPlateList` lays plates in a grid of equal size; needs per-row size |
| `Project::update_preset` id-matching walk | trivial | match by preset name/inherits in Orca; actually call it on save |
| `RuntimePresets` + `ProjectPresetView` dedup-by-values idea and `load_selected_preset_from_3mf` algorithm | pure logic over two collections | Orca already has "project presets" loaded from 3MF; reuse the "same values -> reuse, else add with new id" rule per group |
| `infer_bed_positions_and_create_beds` (legacy grid inference) | standalone | Orca has its own plate grid; only the idea is useful |
| `BedTracking` containment rule (Inside / Colliding / Unplaced, cached hull) | standalone geometry | Orca's `PartPlate::contain_instance` already does this for plates |
| `with_limited_instances` + inverse bed transform for slicing | Orca already slices per plate the same way (plate origin offset) | nothing new |
| 3MF JSON schema of section 4 | data format | write as an extra Orca file (e.g. per-plate `Metadata/plate_N_*.config`) and keep standard Orca files for compatibility |

### Must be re-designed for a classic Slic3r/Orca codebase

1. **Preset storage.** 3.0 stores full evaluated value snapshots per container plus a new
   `HwPrinterConfig` + per-tool print presets. Orca has one global `PresetBundle` with "edited" presets
   per type. Per-group configs need either (a) N lightweight "selections" (printer/process/filament names
   + per-group dirty `DynamicPrintConfig` diff) swapped into the global `PresetBundle` when a group is
   selected, or (b) a per-group `PresetBundle`-like object. The 3.0 approach maps best to (a): the group
   owns the values, the UI edits only the selected one.
2. **Background slicing.** 3.0 has one `BackgroundProcess`/`IPrint` per bed with its own config. Orca
   already has one `Print` per `PartPlate` (`PartPlate::m_print`) but all fed from the single global
   config in `BackgroundSlicingProcess`/`Plater::priv::update_background_process`. The port needs the
   plate's `Print::apply()` to receive its group's full config, and recreation when printer technology
   or extruder count changes (3.0 recreates on hw id change).
3. **Plate geometry.** 3.0 beds of one group share geometry; groups can differ. Orca `PartPlateList`
   assumes all plates have identical size (`m_plate_width/depth/height`, grid layout, shared bed
   texture). Needs per-plate (or per-group) size, origin computation like `BedPlacement`, and per-plate
   bed rendering/exclusion areas.
4. **Selection and UI.** 3.0 uses interactors + listeners (`ISelectedConfigContainerChangedListener`) and a
   Yoga UI. Orca uses wxWidgets `Sidebar` with global preset combos and `Plater` plate selection. The
   equivalent is: when the current plate changes, if its group differs, push that group's selection into
   the sidebar combos / tabs; edits go back to the group. "Add printer group", "Add bed", "Duplicate" map to
   object-list / plate context menu entries.
5. **3MF.** Orca already writes per-plate data (`Metadata/model_settings.config`, plate JSON, per-plate
   G-code). The 3.0 `config_containers` JSON cannot be reused as-is; the mapping
   (group -> presets + full config, plate -> group, plate offset) must be expressed in Orca's files, with the
   main `project_settings.config` kept = first group so older Orca builds still open the project.
6. **Undo/redo.** 3.0 snapshots whole container lists (`S/src/Slic3r/App/Undo/ConfigContainerListSerialize.cpp`,
   `ProjectInteractor::reload_config_containers_after_undo`). Orca's undo stack (cereal-based
   `UndoRedo::Stack`) does not snapshot configs; group changes would need their own snapshot type.
7. **Export.** 3.0 exports only the selected bed; Orca already has "export all plates" / "send all"; each
   plate must use its group's printer (host/physical printer) - Orca's physical printer is also global.
8. **Filament/extruder colours and purge matrix** are per group in 3.0 (`project_settings`). In Orca these
   live in the global project config (`filament_colour`, `flush_volumes_matrix`); they must move to
   the group.

### What I could not determine

- Whether a future 3.0 build wires `Project::update_preset` into saving (no call site in alpha14).
- Exact behaviour of the PrinterSettingsDialog/PrinterAddDialog flow beyond `select_printer_preset`
  (not read in detail).
- Whether G-code for SLA groups mixed with FDM groups is fully exercised; only the process recreation path
  was read.
