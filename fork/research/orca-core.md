# Multi-printer plates — Orca core map (plates, slicing, 3MF)

Read-only survey of branch `multi-printer-plates` at `994439d917` (2026-11-08).
Line numbers are for that commit. "Churn" = commits touching the file in the last
90 days / last year (`git log --since`), and for hook ranges the number of commits
that ever touched those exact lines (`git log -L`). Nothing was built or run.

Existing fork state: no `src/libslic3r/MultiPrinter/`, no `src/slic3r/GUI/MultiPrinter/`,
no `// [multi-printer]` markers yet.

---

## 1. PartPlate / PartPlateList (`src/slic3r/GUI/PartPlate.{hpp,cpp}`)

Note: PartPlate lives in the **GUI** library, not libslic3r. The CLI (`src/OrcaSlicer.cpp`,
107 references to `PartPlate*`) uses it too.

### 1.1 Per-plate data today (`PartPlate`, hpp:108-780)
- `m_plate_index`, `m_name`, `m_locked`, `m_selected`, `m_printable`, `m_origin` (Vec3d),
  `m_width`, `m_depth`, `m_height`, `m_height_to_lid/rod`, `m_shape`, exclude/extruder areas.
- `obj_to_instance_set` (`std::set<pair<obj_idx, inst_idx>>`, hpp:124) — which instances are on the plate.
- `m_print` (non-owning `Print*`, hpp:140), `m_gcode_result`, `m_print_index`,
  `slice_filaments_info`, `m_slice_result_valid`, `m_tmp_gcode_path`.
- **`DynamicPrintConfig m_config`** (hpp:225) — the per-plate *override* config. Accessed via
  `config()` (hpp:355). Keys used today (all must exist in `print_config_def`):
  `curr_bed_type`, `print_sequence`, `first_layer_print_sequence`, `other_layers_print_sequence`,
  `other_layers_print_sequence_nums`, `spiral_mode`, `filament_map_mode`, `filament_map`,
  `filament_volume_map`, `filament_nozzle_map`, `imex_parallel_mode`, `imex_head_filament_map`,
  `skirt_start_angle`. Getters/setters at cpp:267-370 (`get/set_bed_type`, `set/get_print_seq`).
  Pattern: "btDefault / ByDefault" = key erased → falls back to project/preset value.
- Undo/redo: cereal `load/save` (hpp:748-773) serialises index, name, origin, size, flags,
  instances **and `m_config`**. A new key placed in `m_config` would get undo/redo for free,
  but only if it is a registered `PrintConfig` option (see §3.4 for why that is a bad idea).

### 1.2 PartPlateList (hpp:783-1200)
- Owns `m_plate_list` (`vector<PartPlate*>`), `m_print_list` (`map<int, PrintBase*>`),
  `m_gcode_result_list`, `unprintable_plate`, `m_plate_cols`, `m_current_plate`.
- **One global plate size**: `m_plate_width`, `m_plate_depth`, `m_plate_height`,
  `m_shape`, `m_exclude_areas`, `m_wrapping_exclude_areas`, `m_extruder_areas`,
  `m_extruder_heights`, `m_height_to_lid/rod`, `m_logo_texture_filename`.
- Max plates `MAX_PLATE_COUNT = 36` (hpp:60).

### 1.3 Grid layout / origin (all assume equal plate size)
- `compute_colum_count(count)` (hpp:62) → near-square grid; `update_plate_cols()` cpp:7009.
- `plate_stride_x/y()` cpp:6976-6990 = `m_plate_width/depth * (1 + LOGICAL_PART_PLATE_GAP)`.
  (Commented-out code shows it was once computed from `m_shape`.)
- `compute_shape_position(index, cols)` cpp:5982 = `(col*stride_x, -row*stride_y)`;
  `compute_origin()` cpp:5945; `compute_origin_using_new_size()` cpp:5955;
  `compute_origin_for_unprintable()` cpp:5972.
- `update_all_plates_pos_and_size()` cpp:7018 re-places every plate.
- `reset_size(w, d, h, ...)` cpp:6358 — called on printer change; resizes **all** plates,
  then `reload_all_objects()` or `clear()`.
- `set_shapes(shape, excl, wrap_excl, extruder_areas, heights, texture, lid, rod)` cpp:8003
  loops all plates and calls `PartPlate::set_shape(..., pos, ...)` (cpp:5075) with the **same**
  shape. `PartPlate::set_shape` itself is per plate, so per-plate shapes are possible if the
  loop asks for the plate's own shape.
- Who drives it: `Plater::priv::set_bed_shape()` (Plater.cpp:15312-15360) — from the global
  `config` (`printable_area`, `printable_height`, `extruder_clearance_*`), via `Bed3D bed`
  (single bed model/texture, positioned at the current plate's shape position).

### 1.4 How objects are assigned to plates
- **Purely geometric**. `reload_all_objects()` cpp:7425 walks all model instances and puts each
  into the first plate whose `get_plate_box()` intersects the instance's convex-hull bbox
  (`intersect_instance` cpp:4552, `get_build_volume` cpp:5188); otherwise `unprintable_plate`.
  `notify_instance_update()` cpp:7188, `find_instance*()` cpp:7127-7186, `compute_plate_index()`
  for arrange cpp:7530.
- The 3MF does store `objects_and_instances` per plate, but on load it is ignored and rebuilt
  from positions (comment in `load_from_3mf_structure`, cpp:8494-8497).
- Consequence for us: if plates get different sizes, the grid must use a stride ≥ the largest
  plate (or a per-plate origin table), otherwise plates overlap and objects jump plates.

### 1.5 Print objects per plate
- `create_plate()` cpp:6512 allocates **one `Print` + one `GCodeResult` per plate**
  (`m_print_list[m_print_index]`), `plate->set_print()` cpp:4326 calls
  `print->set_plate_origin(m_origin)`.
- `BackgroundSlicingProcess` has a single active print pointer; `PartPlate::update_slice_context()`
  cpp:5396 does `process.set_fff_print(m_print)`, `set_gcode_result`, `set_current_plate(this)`.
  `PartPlateList::update_slice_context_to_current_plate()` cpp:8061.
- So each plate's `Print` already holds its own full `PrintConfig` after `apply()` —
  libslic3r slicing is naturally per plate.

### 1.6 Global-config reads inside PartPlate
`PartPlate.cpp` reads `wxGetApp().preset_bundle` **55 times** (bed type fallback to
`project_config`, print sequence fallback to `prints.get_edited_preset()`, filament maps,
wipe tower estimate `imex_wipe_tower_hull` cpp:1164, `load_gcode_from_file` cpp:5457,
`set_default_wipe_tower_pos_for_plate` cpp:6312, `delete_plate` wipe tower vectors cpp:6697 …).
Each of these is a place where a non-default-printer plate would see the wrong printer.
They do not all need hooks on day one (see §4), but this is the main source of subtle bugs.

---

## 2. How config reaches slicing

### 2.1 Building the config
- `PresetBundle::full_config(apply_extruder, filament_maps, filament_volume_maps)`
  (`src/libslic3r/PresetBundle.cpp:5488`) → `full_fff_config()` (cpp:5559).
  It merges: `FullPrintConfig::defaults()` → `prints.get_edited_preset()` →
  `filaments.default_preset()` → `printers.get_edited_preset()` → `project_config`,
  then per-filament vectors from `this->filament_presets` (names) with `filaments.find_preset()`,
  plus `*_settings_id`, `inherits_group`, `different_settings_to_system`, filament maps.
- It depends **only on the bundle's currently selected/edited presets**. There is no API to
  build a full config from arbitrary (printer, print, filaments) names.
  Options for a per-plate config: (a) a private `PresetBundle` copy with those presets selected,
  then `full_config()`; (b) new code in `src/libslic3r/MultiPrinter/` that mirrors
  `full_fff_config()` from explicit `Preset*`s. (a) reuses upstream logic (no duplication,
  rules §1 "reuse helpers"), at a memory/time cost; cache it per container and invalidate on
  preset edits.

### 2.2 Apply path (the funnel)
- 6 call sites in `Plater.cpp` build `preset_bundle->full_config(false[, f_maps, f_volume_maps])`
  and call `background_process.apply(model, cfg)`:
  `update_background_process()` 11725/11728, 23088/23091, `Plater::select_plate()` 23137/23140,
  `select_plate_by_hover_id()` 23793/23796. Each picks multi-extruder maps based on the
  **global** printer's `get_printer_extruder_count()`.
- **All of them end in `BackgroundSlicingProcess::apply()`**
  (`src/slic3r/GUI/BackgroundSlicingProcess.cpp:749-778`):
  ```cpp
  // TODO: add partplate config
  DynamicPrintConfig new_config = config;
  new_config.apply(*m_current_plate->config());
  Print::ApplyStatus invalidated = m_print->apply(model, new_config);
  ```
  This is the single best place to swap in a per-plate printer/process/filament config: we know
  `m_current_plate`, and `Print::apply()` diffs and invalidates only what changed.
- Slice-all: `Plater::start_next_slice()` (Plater.cpp:21350) / `m_slice_all` iterate plates via
  `select_plate()` → same funnel. After `process()`, `process_fff()` (BSP.cpp:264-279) writes
  filament/nozzle maps back into the plate config — uses global `preset_bundle.project_config`.
- `Print` uses its own config for `printable_area` (plate-local, Print.cpp:4671-4690),
  `printable_height` (Print.cpp:1107, 2582-2601), wipe tower, G-code export — so once the
  right config is applied, core slicing and G-code export are per printer already.

### 2.3 Other per-plate inputs that are global today
- `Plater::priv::update_print_volume_state()` (Plater.cpp:11520) builds a `BuildVolume` from the
  current plate's shape but the **global bed's** `printable_height`/extruder areas.
- `Plater::priv::config` (single `DynamicPrintConfig*`) is read all over Plater
  (e.g. `config->opt_bool("enable_prime_tower")` 11753, `printable_area` 8825, 10689).
- `Model::curr_plate_index` + `Model::plates_custom_gcodes` (Model.hpp:1752-1757) — per-plate
  custom G-code; read by `Print` (Print.cpp:790). Fine, already per plate.

---

## 3. 3MF (`src/libslic3r/Format/bbs_3mf.{hpp,cpp}`)

### 3.1 Files
Constants at cpp:212-250: `3D/3dmodel.model`, `Metadata/project_settings.config`
(full global config), `Metadata/model_settings.config` (objects + `<plate>` blocks),
`Metadata/slice_info.config`, `Metadata/filament_sequence.json`, `Metadata/plate_N.png/.gcode`,
embedded user presets `Metadata/{process,filament,machine}_settings_N.config`,
Orca-only `Metadata/orca_cad.bin` (precedent for an Orca-specific extra file).

### 3.2 Per-plate metadata (model_settings.config)
- Writer `_add_model_config_file_to_archive()` cpp:8308; plate block cpp:8450-8624. It writes an
  **explicit whitelist** of keys from `PlateData::config`: `plater_id`, `plater_name`, `locked`,
  `bed_type`, `print_sequence`, `first/other_layers_print_sequence(_nums)`, `spiral_mode`,
  `imex_*`, `filament_map_mode`, `filament_map`, `filament_volume_map`, gcode/thumbnail paths,
  `model_instance` children. Unknown keys in `PlateData::config` are **not** written.
- Reader `_handle_start_config_metadata()` cpp:4600-4840: an `if/else if` chain on the key with
  **no final else** → unknown `<metadata key=...>` inside `<plate>` is silently ignored. So a new
  plate key would be invisible to upstream Orca (and, by the same parser lineage, very likely to
  Bambu Studio — not verified against Bambu's source).
- `slice_info.config` writer cpp:8730; per plate it writes `printer_model_id` and
  `nozzle_diameters` (cpp:8794-8795). Plater sets these **to the global printer for every plate**
  (Plater.cpp:21060-21066). With the feature on these could become per plate (Bambu firmware/
  Handy reads this in .gcode.3mf — only safe for single-plate exports; flag for later).
- Bridge to GUI: `PartPlateList::store_to_3mf_structure()` PartPlate.cpp:8357 (copies
  `plate->config()` into `PlateData::config`) and `load_from_3mf_structure()` cpp:8472
  (creates plates, applies `PlateData::config`).

### 3.3 Project settings and embedded presets
- `project_settings.config` = `store_params.config` (the global full config), written by
  `_add_project_config_file_to_archive()` cpp:8211; read at cpp:2046 into the load `config`.
- Embedded presets: `PresetBundle::get_current_project_embedded_presets()` (only the
  **currently selected** user presets) → `StoreParams::project_presets` (Plater.cpp:21031-21039)
  → `machine_settings_N.config` etc. On load, `load_project_embedded_presets()`
  (Plater.cpp:9657, 10106). A per-plate user preset that is not selected globally would
  therefore **not** be embedded today.
- Read dispatch cpp:2046-2138: known names matched with `iequals`/`istarts_with`; everything
  else falls into the final `else` and is logged "skipped … not supported" (cpp:2136-2137).
  → **A new `Metadata/<something>` file is ignored by upstream Orca.**
  Caveat: `istarts_with(name, "Metadata/machine_settings_")` etc. — do not reuse those prefixes.
  Also avoid names ending in `.gcode`, `.png`, or the calibration extension under `Metadata/`
  (cpp:2122-2134 extract those).

### 3.4 Recommended format for per-plate presets
- Keep `project_settings.config` = the **default container** (the "project printer"), so an
  upstream reader opens the project with a sensible printer and all objects.
- Add one new file, e.g. `Metadata/multi_printer_plates.json` (name to decide; must not match
  any prefix above), holding: format version, list of config containers
  (printer/print/filament preset names + a full config snapshot or diff, like Prusa's
  ConfigContainer), and plate_index → container id. Write it **only** when a plate uses a
  non-default container (rule §2.3: one-printer projects saved exactly as upstream).
- Do **not** put container ids into `PartPlate::m_config`: it is a `DynamicPrintConfig`
  (needs a `print_config_def` entry → would leak into `PrintConfig`, profiles, CLI and
  `Print::apply` diffs) and the 3MF writer would drop it anyway.
- Two ways to get the file in/out:
  1. Hooks in `bbs_3mf.cpp` modelled on `orca_cad.bin`: one call in the save sequence near
     cpp:6707 and one `else if` in the read dispatch near cpp:2050, carrying the blob through
     a new `Model` member (precedent: `Model::cad_recipe`, Model.hpp:1738, copied in
     Model.cpp:159/205). ~3 small hooks in libslic3r.
  2. Zero hooks in `bbs_3mf.cpp`: after `store_bbs_3mf()` returns, re-open the zip with miniz
     and append the file; on load, open the zip again and read it. Hooks then live in
     `Plater::export_3mf` (Plater.cpp:20873/21031) and `Plater::priv::load_files`
     (Plater.cpp:~9653). Costs a second zip pass; must also cover autosave/backup
     (`SaveStrategy` backup path) — not checked in detail.

---

## 4. One-printer-per-project assumptions (core side)

| Area | Where | Assumption |
|---|---|---|
| Plate size/grid | PartPlate.cpp:5945-5995, 6358, 6976-6990, 8003 | all plates same w/d/h/shape |
| Bed rendering | `Plater::priv::bed` (Bed3D), set_bed_shape Plater.cpp:15312 | one bed model/texture, moved to current plate |
| Bed type list | PartPlate `get_bed_type` cpp:267 fallback `project_config`; `check_all_plate_local_bed_type` | bed types of the global printer |
| Print volume | Plater.cpp:11520 | global printable_height/extruder areas |
| Extruder count | Plater.cpp:11716, 23131, 23786 `get_printer_extruder_count()` | global printer decides multi-extruder path |
| Filament list | `PresetBundle::filament_presets`, `project_config` `filament_colour` etc., object/volume `extruder` ids | one project-wide filament slot list; objects reference slot numbers |
| Filament maps | BSP.cpp:264-279, PartPlate filament map getters | sized to global filament count/extruders |
| Wipe tower pos | `wipe_tower_x/y` are `coFloats` **indexed by plate** in `project_config` (PrintConfig.cpp:8326; Print.cpp:1496,1613,1749,3784,5792; PartPlate.cpp:6312, 6697) | per-plate values live in the global config — a per-plate config must keep the project's vectors (or the plate's own entry) |
| Printable height | Print.cpp:1107, 2582 (Print-local, OK once applied); Plater.cpp:15354 global | |
| 3MF project | project_settings.config, embedded presets = currently selected only | one printer/process/filament set |
| slice_info | printer_model_id/nozzle_diameters same for all plates (Plater.cpp:21060) | |
| Export all plates | `Plater::export_gcode_3mf(true)` Plater.cpp:20014; `on_action_export_all_sliced_file` 14316 | one .gcode.3mf with one project config and one printer_model_id for all plates |
| Send/print | `send_gcode` Plater.cpp:21626, device/print dialogs | current global printer/device (GUI scope, not mapped here) |
| CLI | src/OrcaSlicer.cpp:1544-1760 (`plate_to_slice`) | one config per run |
| SCALING_FACTOR | Plater.cpp:15320-15324 | chosen from the global bed size (large-bed printers) — **process-wide global** |

Not found / not verified: whether `GCode::do_export` reads any GUI-global state (it appears to
use only `Print` config); Bambu Studio's reader behaviour (inferred from shared code lineage);
undo/redo behaviour of a side table keyed by `PartPlate*` (UndoRedo may recreate plates).

---

## 5. Concrete hook points

Churn columns: file 90d/1y commits; "lines" = commits ever touching that range.

| # | File:line | Hook | File churn | Lines | Risk |
|---|---|---|---|---|---|
| H1 | `BackgroundSlicingProcess.cpp:754-755` (`apply`) | If feature on and `m_current_plate` has a container: replace `config` by `MultiPrinter::plate_full_config(*m_current_plate, config)` (built from the plate's presets, keeping project-level per-plate vectors like `wipe_tower_x/y`, then overlay `m_current_plate->config()`). One branch. | 12/20 | 5 | **Low** — best hook, single funnel for all 6 apply sites |
| H2 | `BackgroundSlicingProcess.cpp:264-279` (`process_fff`) | Filament/nozzle map write-back uses global `project_config`; guard or use the plate container's config. | 12/20 | — | Low |
| H3 | `PartPlate.cpp:6976-6990` (`plate_stride_x/y`) | Return max plate size over all containers when feature on (keeps grid non-overlapping). | 109/186 | 1 | Low (lines stable) |
| H4 | `PartPlate.cpp:8003-8035` (`set_shapes` loop) | Per plate: ask MultiPrinter for that plate's shape/excludes/heights instead of the shared one. | 109/186 | 11 | Medium |
| H5 | `PartPlate.cpp:6358` (`reset_size`) / `Plater.cpp:15312-15360` (`set_bed_shape`) | When the *default* printer changes, only plates on the default container resize; `Bed3D` follows the current plate's printer. | PP 109/186; Plater 299/580 | 17 (set_bed_shape) | **High** (Plater) — keep to one call |
| H6 | `Plater.cpp:23103` (`Plater::select_plate`) and `PartPlateList::select_plate` (~PartPlate.cpp:6900) | On plate switch: notify MultiPrinter (switch shown presets in sidebar, bed model, `update_print_volume_state`). One call. | 299/580 | — | Medium-high |
| H7 | `Plater.cpp:11520` (`update_print_volume_state`) | Use the plate container's printable_height/extruder areas. | 299/580 | — | Medium |
| H8 | `Plater.cpp:11716`, `23131`, `23786` | Extruder-count check uses global printer; if H1 rebuilds maps internally these can stay untouched. | 299/580 | 11 (11705-11730) | Prefer **no hook**; handle in H1 |
| H9 | `PartPlate.cpp:8357` (`store_to_3mf_structure`) / `8472` (`load_from_3mf_structure`) | Copy plate→container mapping to/from the new 3MF entry (or do it in Plater around the call). | 109/186 | 10 / 5 | Medium |
| H10 | `bbs_3mf.cpp:~6707` (save sequence) + `~2050` (read dispatch) + `Model.hpp:1738` (new member) | Write/read `Metadata/multi_printer_plates.json`, like `orca_cad.bin`. Alternative: no bbs_3mf hooks, zip append in Plater (§3.4). | 67/88; Model.hpp 23/45 | 4 / 6 | Medium |
| H11 | `Plater.cpp:21031-21039` (`export_3mf`) | Add embedded user presets used by any plate to `project_presets`; write per-plate `printer_model_id` only if decided. | 299/580 | 3 | Medium |
| H12 | `Plater.cpp:~9653` and `~10102` (`load_files`, project load) | After presets load: build containers from the new 3MF entry; if absent → feature data empty (upstream behaviour). | 299/580 | — | Medium-high |
| H13 | `PartPlate.cpp:6670` (`delete_plate`), `7041` (`move_plate_to_index`), `6599` (`duplicate_plate`), `6512` (`create_plate`) | Keep plate→container map in sync (or key it by `PartPlate*`, then only delete needs a hook). `delete_plate` already shuffles `wipe_tower_x/y` (cpp:6697). | 109/186 | — | Medium |
| H14 | `Plater.cpp:20014` (`export_gcode_3mf(true)`), `14316` | Export-all with mixed printers: per-container files or refuse with a message. | 299/580 | — | Medium |
| H15 | `PartPlate.cpp:1164`, `5457`, `6312` and other `preset_bundle` reads | Plate-level computations (wipe tower estimate, gcode load) should use the plate config. Do lazily, as bugs appear. | 109/186 | — | Medium |

Recommended first slice (minimal footprint, testable in libslic3r tests):
1. `src/libslic3r/MultiPrinter/` — `ConfigContainer` (preset names + resolved config),
   `PlateConfigMap` (plate → container), JSON (de)serialiser for the 3MF entry; a
   `full_config_for(container, project_config)` helper reusing `PresetBundle::full_config()`
   on a bundle copy. Unit tests in `tests/libslic3r/test_multi_printer_plates.cpp`.
2. Hooks H1 (+H2) — slicing with per-plate config, same bed size printers first.
3. H10/H9/H11/H12 — save/load.
4. H3/H4/H5/H6/H7 — different bed sizes and the GUI side.

Biggest open design issue (not solvable by hooks alone): **filament slots**. Objects reference
global filament slot numbers and the project has one filament list (`filament_presets`,
`filament_colour`, flush matrices sized by filament count). A plate with a different printer
(e.g. 1 extruder vs. 4-slot AMS) needs either a per-container filament list with a
slot remapping for objects on that plate, or a rule that containers share the project filament
list and only override printer/process. Decide before H1.
