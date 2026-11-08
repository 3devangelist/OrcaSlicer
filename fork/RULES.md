# Fork rules — 3devangelist/OrcaSlicer

This fork adds **multiple plates with different printers in one project**
(each plate has its own printer, process and filament presets), ported from
the design of PrusaSlicer 3.0 ("Multiple beds with different configs",
3.0.0-alpha11).

Upstream will probably not merge this work. So the main goal of these rules is
to **stay close to upstream OrcaSlicer**, so we can merge each new upstream
release with little effort.

Sources of these rules, read on 2026-11-08 from `OrcaSlicer/OrcaSlicer@main`:
[AGENTS.md](../AGENTS.md), [CONTRIBUTING.md](../CONTRIBUTING.md),
[tests/AGENTS.md](../tests/AGENTS.md), `.claude/skills/`, and the wiki page
[How to Contribute](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute).
When upstream changes those files, update this file in the same merge.

---

## 1. Upstream rules we follow as they are

These come from the OrcaSlicer team. Code in this fork follows them, so a
piece of it can still go upstream as a PR if they ever want it.

### Code style
- C++17, selective C++20. `PascalCase` classes, `snake_case` functions and
  variables. `#pragma once` in headers. Smart pointers and RAII.
- `.clang-format` is for **new files only**. In existing files, match the
  code around the change. Never reformat code you did not change.
- **Include what you use.** Every file includes the header for every symbol
  it uses; headers compile on their own. Do not rely on the precompiled
  header. Check with `scripts\run_clang_tidy.ps1`.
- Slicing runs in parallel with TBB: be careful with shared state.
- wxWidgets: on top-level windows use `SetSizerAndFit(sizer)`, not
  `SetSizer(sizer)` (or call `sizer->SetSizeHints(window)` afterwards).
- Reuse existing helpers before adding new ones. No duplicated code. Keep code
  short; remove bloated generated code before committing.
- Architectural changes are explained in a code comment and in the commit
  message.

### Compatibility (critical)
- **.3mf projects and printer profiles stay backward compatible.**
- Never change what an existing setting means. Add a new key and convert old
  values in `handle_legacy`.
- Profile or format changes need version migration.
- Everything must work on Windows, macOS and Linux.

### New behaviour is off by default
- A new feature or setting is **off by default**. With it off, the output
  (G-code, 3MF, UI) is exactly the same as upstream.

### Translatable strings
- One `_L(...)` per whole sentence, with placeholders:
  `format(_L("%1% is too close to others"), name)`. Never join translated
  fragments.
- Counts use `_L_PLURAL`. Ambiguous short words use `_L_CONTEXT`.
- Mark every visible string. List new source files in
  `localization/i18n/list.txt`.
- Never choose behaviour by comparing translated text.
- In `.po` files only edit `msgstr`.

### Tests
- Catch2, in `tests/`. Choose the suite by the production code tested:
  `libslic3r` (model, config, 3MF, presets), `fff_print` (slicing and
  G-code).
- One file per subsystem, `test_<subsystem>.cpp`, listed in the suite's
  `CMakeLists.txt`.
- Test names are plain present-tense sentences, tagged in PascalCase with the
  subsystem.
- A good test fails without the change and passes with it.
- Use `tests/test_utils.hpp` and `fff_print/test_helpers.hpp`.
- Floats: `WithinAbs` / `WithinRel`. One condition per assertion.
- Windows build with tests: `build_win.bat -ds --run-tests`.

### Documentation
- Design docs go in `docs/HLSD/<subsystem>.md` and describe the design as it
  is (no phases, task lists or "before/after").
- Planning notes go in `docs/superpowers/` (gitignored, never committed).

### AI agents
- The person is responsible for every change: build it, run it, understand it.
- An agent never opens a PR or posts a comment on GitHub by itself.
- Before changing code, check `.claude/skills/` for a matching skill and
  follow it (`orca-profiles`, `orca-translations`, `orca-wxwidgets`).
- If something could not be built or tested, say so.

---

## 2. Fork rules (ours)

### 2.1 Branches
- `main` is a **clean mirror** of `OrcaSlicer/OrcaSlicer:main`. Never commit
  to it; only update it from upstream (GitHub "Sync fork", or
  `git fetch upstream` + `git merge --ff-only upstream/main`).
- Work happens on feature branches (e.g. `multi-printer-plates`).
- Bring upstream changes into a feature branch with
  `git merge upstream/main`, not rebase, so history stays readable and we
  never force-push.
- Merge upstream **often** (at least at every upstream release). Small merges
  are easier than big ones.

### 2.2 Keep the footprint on upstream files small
The fewer upstream lines we change, the fewer merge conflicts we get.
- Put new code in **new files** where possible, in a folder of our own:
  - core (model, config, 3MF): `src/libslic3r/MultiPrinter/`
  - GUI: `src/slic3r/GUI/MultiPrinter/`
  - tests: `tests/libslic3r/test_multi_printer_plates.cpp`
- In upstream files, add only small **hooks** (a call, a member, a branch)
  that point into our files. Do not move, rename or reformat upstream code.
- Mark each hook so it is easy to find on every merge:
  `// [multi-printer] <short reason>`.
- No changes to upstream printer profiles in `resources/profiles/`.

### 2.3 Feature flag and file format
- The feature is behind a switch (preference or project option), **off by
  default**. With it off, Orca behaves exactly as upstream.
- A project with one printer for all plates is saved **exactly** as
  upstream saves it.
- Extra per-plate data is saved in **new, separate entries** of the 3MF (new
  file under `Metadata/` or new keys), never by changing existing ones.
  Upstream Orca and Bambu Studio must still open our 3MF files, and simply
  ignore the extra data.
- Loading an upstream 3MF must always work and give the same result as
  upstream.

### 2.4 Porting from PrusaSlicer
- We port the **design**, not the architecture. PrusaSlicer 3.0 rewrote its
  code base (`slic3r-domain`, `slic3r-shared`, Lua, …). We copy its data model
  — a project has a list of *config containers* (printer + process +
  filaments) and each plate points to one — and fit it into Orca's existing
  `PartPlate` / `PartPlateList`.
- Reuse PrusaSlicer code only when it fits Orca's structure without bringing
  in its new layers.
- Licences: both projects are **AGPL-3.0**, so porting is allowed. Keep the
  original copyright lines in ported code and add a comment with the source:
  `// Ported from PrusaSlicer <tag> <path>`.
- Reference version: `prusa3d/PrusaSlicer` tag `version_3.0.0-alpha14`
  (or newer). Note the tag used in each commit message.
- Prusa's key files for this feature:
  `src/slic3r-domain/include/Slic3r/Domain/{Project,BedContainer,Bed,BedInstance}.hpp`,
  `ConfigContainer*`, `src/slic3r-shared/include/Slic3r/Biz/Preset/ProjectPresetView.hpp`,
  `src/slic3r-shared/include/Slic3r/App/SidebarBed.hpp`.

### 2.5 Commits
- One topic per commit, written so it could become an upstream PR.
- Commit message: what and why, the Prusa source when ported, and how it was
  tested (OS, printer profiles, steps).
- Never commit build output, `deps/build/`, or `docs/superpowers/`.

### 2.6 Before each piece of work
- Search upstream issues and PRs for the same work (see
  [#1277](https://github.com/OrcaSlicer/OrcaSlicer/issues/1277),
  [#11292](https://github.com/OrcaSlicer/OrcaSlicer/issues/11292),
  [#8596](https://github.com/OrcaSlicer/OrcaSlicer/issues/8596)).
  If someone upstream starts the same feature, stop and decide whether to help
  them instead.
- Merge upstream `main` first, so we work on current code.
