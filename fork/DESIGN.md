# Design proposal: per-plate printer, process and filament settings

Status (2026-11-08): **partly implemented**, see [§0](#0-what-is-implemented).
Branch `multi-printer-plates`.

> **For developers reading this fork.** The feature idea and the user
> experience below come from the fork owner, who uses OrcaSlicer daily but is
> not a programmer. The technical choices (data model, hook points, file
> format, phases) were proposed by an AI assistant (Claude) from a reading of
> the OrcaSlicer and PrusaSlicer sources. Nobody has reviewed them as a
> developer yet. Please treat them as a starting point: if you see a cleaner
> or more efficient way, take it. The research notes in
> [research/](research/) give the code facts (file:line) behind each choice so
> you can check them and decide differently.

Rules for code in this fork: [RULES.md](RULES.md).

## 0. What is implemented

Everything is behind one preference, **Preferences > Features > "Printer and
settings per plate"** (key `fork_plate_process_settings`, off by default,
needs a restart). With it off, OrcaSlicer behaves as upstream.

| Part | Where | Tests |
|---|---|---|
| Any process setting can be overridden on a plate (plate tab shows every process page); objects resolve global → plate → object; plate row and 3D plate icon show "changed" | `src/slic3r/GUI/MultiPrinter/PlateSettings.*`, hooks in `Tab.cpp`, `PartPlate.cpp` | MCP: plate layer height 0.12 over global 0.2 slices to 166 layers; object 0.16 on top gives 125 |
| Plate overrides saved in the 3MF plate block as `plate_override:<key>` | `src/libslic3r/MultiPrinter/PlateOverrides.*`, hooks in `bbs_3mf.cpp` | `tests/libslic3r/test_plate_overrides.cpp` |
| Every plate has its own printer + process + filaments set (owner's decision after testing, 2026-11-09: choosing a printer changes only the selected plate; there is no "use the project's set" choice). A plate without a set yet takes the one in use when first needed (`give_every_plate_a_set`). Selecting the plate swaps its set into the PresetBundle, so the sidebar, tabs and slicing use it. The 3MF's project presets are the first plate's | `src/slic3r/GUI/MultiPrinter/PlatePresets.*`, hooks in `Plater.cpp` (`select_plate`, `select_plate_by_hover_id`), `GUI_Factories.cpp` | MCP: plate 1 G-code `printer_settings_id = Printer A`, plate 2 `Printer B`; plate 2 → X2D leaves plate 1 on A1 mini |
| Plates of different printers shown at the same time, one row per printer (owner's decision 2026-11-09, like PrusaSlicer 3.0's `BedPlacement.cpp`): plates of one printer side by side, rows in the order of their first plate, each row as deep as its bed, each plate drawing its own bed shape and size. One printer for all plates keeps upstream's square grid. Saving writes upstream's grid sized for plate 1's printer, so upstream OrcaSlicer opens the file with every object on its plate; automatic backups keep the rows (`"layout": "rows"` in the JSON) | `src/slic3r/GUI/MultiPrinter/PlateLayout.*`; hooks in `PartPlate.cpp` (`compute_shape_position`, `compute_origin_for_unprintable`, `create_plate`, `set_shapes`, `update_all_plates_pos_and_size`, `reset_size`), `Plater::priv::set_bed_shape` (relayout, reassign objects, re-register plate picking), `ProjectSaveScope` | MCP: plates 1 and 3 A1 mini in row 1, plate 2 X2D in row 2; slice plate 2 OK; save → file holds the grid with A1 mini as project printer; reopen → rows, printers and objects as before; mouse over the plates after a save and after a backup |
| "Slice all" slices each plate with its own set and keeps every plate's result: extruder params per plate (`update_slice_all_extruder_params`), no "all plates not sliced" while a set is swapped in (`on_select_preset`, two places in `Tab.cpp`), objects reassigned only when plates move; sliced 3MF files keep the rows so their G-code stays | `Plater.cpp`, `Tab.cpp` | MCP: slice all on A1 mini / X2D / A1 mini → `.gcode.3mf` with 3 G-codes, each with its own printer and bed; "All Plates Stats" sums time and cost |
| Objects stay on their plate when a plate's printer has another bed size: plates are resized and moved and their objects move with them (same place relative to the plate centre) | `reset_size(..., move_instances)` in `PartPlate.*` (default off = upstream), hook in `Plater::priv::set_bed_shape`, which also reassigns objects after `set_shapes` | MCP: A1 mini (180) ↔ X2D (256), a cube on each plate stays on its plate both ways |
| Filament-count changes only clamp objects on plates that use the active set | hooks in `Plater.cpp`, `GUI_ObjectList.cpp` | not tested yet |
| Plate sets saved in `Metadata/multi_printer_plates.json`; while saving, the bundle holds the project's set so `project_settings.config` stays the project's printer; on open the sets go back to their plates | `src/libslic3r/MultiPrinter/PlatePresetsFile.*`, `Model::multi_printer_plates`, hooks in `bbs_3mf.cpp`, `Plater.cpp` | `test_plate_overrides.cpp` (file round trip); MCP: save with plate 2 on Printer B, reopen, plates keep A / B |

| New plate ("Add plate") copies the current plate: plate options, process overrides and its own set if any | `on_plate_added` in `PlatePresets.cpp`, hook in `Plater::add_plate` | MCP: plate 1 on Printer B "by object" → new plate is Printer B "by object" |
| Plate right-click "Copy plate settings" / "Paste plate settings": everything, including the printer set | `copy_plate_settings` / `paste_plate_settings` in `PlatePresets.cpp` | MCP: paste plate 1 (B) onto a project-set plate → B; paste a project-set plate onto plate 1 → A |

Not done yet: user presets used only by other plates are not embedded in the
3MF (another computer needs them installed); arrange and fill bed still use
one bed size and upstream's grid for all plates (wrong with rows per printer);
the "outside the plate" check and the logo/texture follow the current plate's
printer;
"all plates" print/send actions with several printers; undo of set changes;
a test of the filament clamp guard.
Selecting a plate re-slices it after a set swap, even when nothing changed.

## 1. The problem

Many users own several printers of different sizes and capabilities, and want
to keep all the parts of one job in one project: e.g. ABS parts for a Voron
300, a PLA housing for an A1, small buttons for an A1 mini, a TPU seal on
another plate. Today OrcaSlicer has one printer, one process and one filament
list per project, so users open several instances or several projects.

Upstream requests (none implemented or claimed as of 2026-11-08):
[#1277](https://github.com/OrcaSlicer/OrcaSlicer/issues/1277),
[#3942](https://github.com/OrcaSlicer/OrcaSlicer/issues/3942),
[#7221](https://github.com/OrcaSlicer/OrcaSlicer/issues/7221),
[#7238](https://github.com/OrcaSlicer/OrcaSlicer/issues/7238),
[#11292](https://github.com/OrcaSlicer/OrcaSlicer/issues/11292) (open),
[#8596](https://github.com/OrcaSlicer/OrcaSlicer/issues/8596) (open),
[#8420](https://github.com/OrcaSlicer/OrcaSlicer/issues/8420) (open),
[#10551](https://github.com/OrcaSlicer/OrcaSlicer/issues/10551) (open),
discussion [#6357](https://github.com/OrcaSlicer/OrcaSlicer/discussions/6357).

PrusaSlicer added the same capability in 3.0.0-alpha11 ("multiple beds with
different configurations ... even mix different printers in one project").

## 2. The concept: settings belong to the plate

**Each plate owns all its settings: printer, process preset and filaments**,
next to the per-plate settings Orca already has (bed type, print sequence,
first-layer filament order, spiral mode, ...).

There are no stored "printer groups". Plates that use the same printer are
simply plates with equal settings. (PrusaSlicer 3.0 stores groups,
"ConfigContainers", that own several beds; see §6 for why we chose
differently.)

### Settings hierarchy: global → plate → object

Settings resolve in three levels, each one overriding the one above:

1. **Global / project**: the printer, process and filament presets of the
   project, as today.
2. **Plate**: the plate's own printer, process and filaments, plus **any
   process setting overridden for that plate**. A user can put all parts
   that share settings on one plate and change the plate once, instead of
   every object.
3. **Object** (and part / layer range): the per-object overrides Orca
   already has. They are read on top of the plate, not on top of the global
   settings.

In the UI this mirrors the existing object mode: when a **plate** is
selected in the object list, the right-hand settings area shows the plate's
settings in the same "only changed settings, add more with +" style used for
objects (`GUI_ObjectSettings`). An object then shows what it inherits from
its plate and only its own differences.

The slicing order already fits: `BackgroundSlicingProcess::apply()` builds
`global + plate config` (`new_config.apply(*m_current_plate->config())`),
and `Print::apply()` then applies each object's overrides on top. Today the
plate level only carries a few keys (bed type, print sequence, ...); this
proposal widens it to the plate's presets and any process key.

The plate's "changed" icon (below) means: the plate differs from the
global/project level.

### User experience

| What | Where | Already exists in Orca? |
|---|---|---|
| Change a plate's printer, process, filaments | **Plate Settings** dialog (`PlateSettingsDialog`), opened from the plate's settings icon in the 3D view | Dialog: yes. Fields: new |
| See that a plate differs from the base | Icon in the right-hand column (`colEditing`) of the plate row in the object list, like the "modified settings" icon on objects | Plate rows (`itPlate`) and the column: yes. Icon on plates: new |
| Same, in 3D | The plate settings icon switches to its "changed" variant (`plate_settings_changed.svg`) | Yes |
| Copy settings to other plates | Right-click on the plate row (`MenuFactory::plate_menu`, today Rename / Lock / Delete): **Copy plate settings** and **Paste plate settings**. Same on the 3D plate icon | Menu: yes. Items: new |
| Edit the active plate's presets in the sidebar / Tabs | When a plate is selected, its presets become the active ones | New behaviour, existing UI |

Rules:
- Changing a plate changes **only that plate**. There is no automatic
  propagation; copy/paste is the way to repeat settings.
- Each plate has **its own filament list**. Objects on a plate use that list.
- Each plate has **the bed shape and size of its printer**.
- With the feature switched off (default), Orca behaves exactly as upstream.
  A project where every plate has the same settings is saved exactly as
  upstream saves it.

## 3. Proposed technical approach

### 3.1 Data
- Each `PartPlate` gets a reference to a *plate preset set*: the selected
  printer, process and filament presets plus their unsaved edits. Equal sets
  are shared (deduplicated) in memory and on disk, but this is an
  implementation detail, not a user concept.
- New code lives in new files (`src/libslic3r/MultiPrinter/`,
  `src/slic3r/GUI/MultiPrinter/`); upstream files only get small hooks marked
  `// [multi-printer]`.

### 3.2 Presets: one global bundle, swapped on plate selection
- Keep Orca's single global `PresetBundle`. When a plate is selected, its set
  is swapped into the bundle; the Sidebar and Tabs then edit the selected
  plate with no changes of their own.
- Base: the existing snapshot API `PresetBundle::Selection`, `selection()`,
  `restore_selection()` (`src/libslic3r/PresetBundle.hpp:411-429`). It needs a
  small generalisation: today `selection()` returns nothing while presets
  embedded in a 3MF are loaded, and the restore drops them.
- Swap point: right after `partplate_list.select_plate` in
  `Plater::select_plate` and `select_plate_by_hover_id`, before
  `background_process.apply`. Not inside `PartPlateList::select_plate` (it
  holds a non-recursive mutex and is also called during setup).

### 3.3 Slicing
- Each plate already has its own `Print`. In
  `BackgroundSlicingProcess::apply()` (where upstream already has
  `// TODO: add partplate config`) the plate's full config is used instead of
  the global one. A per-set cache of the full config avoids rebuilding it.

### 3.4 3MF file format
- `Metadata/project_settings.config` keeps the settings of **plate 1**, so
  upstream Orca and Bambu Studio open the project as a normal one-printer
  project.
- The other plates' sets and the plate → set table go in a **new file**,
  e.g. `Metadata/multi_printer_plates.json`, written only when plates differ.
  Upstream readers skip unknown `Metadata/*` files.
- Presets used only by some plates must also be embedded (today only the
  selected ones are), via a hook in `export_3mf`.
- Do not store set ids inside `PartPlate::m_config`: the 3MF writer drops
  unknown keys, and it would need a real print setting.

### 3.5 Known traps (from the code)
- `on_filament_count_change` / `on_filaments_delete` clamp the filament index
  of **every volume in the model**. With per-plate filament lists this would
  wipe filament assignments on other plates; it must be limited to the
  plate's objects.
- Re-centring objects on all plates, resetting all wipe towers and
  `reset_size` (all plates the same size) run on printer change.
- Plate layout assumes equal plate size (`plate_stride_x/y`, `set_shapes`,
  one `Bed3D`). Different sizes need the stride of the largest plate and a
  per-plate shape (PrusaSlicer does the same).
- "Print all", "Export all sliced", "Send all", the CLI and `slice_info`
  assume one printer.
- Undo/redo does not record preset changes; the plate → set link must go into
  `PartPlate`'s undo archive. Dirty-state checks only look at the active set.
- `Plater.cpp` is the most-changed upstream file (~300 commits in 3 months):
  hooks there must stay one line each.

## 4. Proposed phases

0. Preference switch (off by default), folders, first Catch2 tests.
1. Data model + 3MF read/write (+ tests: upstream opens our files; with the
   switch off nothing changes).
2. Slicing with the plate's config.
3. GUI: Plate Settings fields, swap on plate selection, plate icon in the
   object list, copy/paste plate settings, undo and dirty state.
4. Per-plate filament lists and safe filament remapping.
5. Print / send / export per plate; "all plates" actions grouped by printer
   or blocked.
6. Plates of different sizes.

Agreed order (§5): **process and filaments per plate on the same printer**
first (covers #8596, #8420, #10551), then printers per plate and plates of
different sizes.

## 5. Decisions (owner, 2026-11-08)

1. **Base for the plate's changed icon:** the global/project level (see the
   settings hierarchy in §2).
2. **New plate:** copies the settings of the plate selected when it is
   created (printer, process, filaments, plate options).
3. **Object moved to a plate with a different filament list:** OrcaSlicer
   picks the closest filament on the target plate (same material type first,
   then the nearest colour) and shows a notification saying what changed,
   with an undo. It does not ask every time.
4. **Paste plate settings:** pastes everything (printer, process, filaments,
   plate options). A later "Paste special…" may let the user pick parts.
5. **Order:** first **process and filaments per plate on the same printer**
   (phases 0–4 without the printer swap), then **printers per plate** and
   plates of different sizes on top of it.

## 6. Alternatives considered

- **Port PrusaSlicer 3.0 literally.** Not feasible: 3.0 rewrote the
  architecture (`slic3r-domain`, `slic3r-shared`, Lua, ~2,000 new files).
  We port the design only. Both projects are AGPL-3.0; ported code keeps its
  copyright lines and a `// Ported from PrusaSlicer <tag> <path>` note.
- **Printer groups as a user concept (PrusaSlicer's ConfigContainers).** One
  edit updates every bed of the group, but users must manage groups. The
  owner preferred plate-owned settings with copy/paste, which matches how
  Orca already treats per-plate settings and per-object overrides.
- **N independent `PresetBundle` objects.** Cleaner in theory, but every
  Sidebar/Tab code path reads the single global bundle: far more upstream
  changes and merge conflicts.
- **One `Metadata/PrusaSlicer3_project.json`-style file replacing
  `project_settings.config`.** PrusaSlicer 3.0 does this, and older
  slicers then load geometry only. We keep the upstream files intact.

## 7. Research notes

AI-generated maps of the code, with file:line references (read on
2026-11-08; upstream `main` e23af6d7b0, PrusaSlicer `version_3.0.0-alpha14`).
Verify before relying on them.

- [research/orca-core.md](research/orca-core.md): plates, slicing, 3MF,
  one-printer assumptions, core hook points.
- [research/orca-gui-presets.md](research/orca-gui-presets.md): presets,
  sidebar, plate selection, send/export, undo, GUI hook points.
- [research/prusa-design.md](research/prusa-design.md): how PrusaSlicer 3.0
  implements beds with different configs, and what ports to Orca.
