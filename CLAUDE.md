# Nextpad++ — GTK4 + Scintilla Linux port

This is the GTK4 migration of the Linux port. It started 2026-05-15 as an
independent clone of `../nextpad-plus-plus-gtk3/` and is being migrated to
GTK4 + libadwaita. Once the GTK4 build reaches parity, the GTK3 port is
retired and this becomes the sole Linux codebase.

## Stack

- **Toolkit:** GTK4 + libadwaita-1.
- **Editor widget:** Scintilla, GTK4 backend — vendored under `scintilla/`
  from `bugaevc/scintilla` (gtk4 branch). The widget type is `ScintillaView`.
- **Lexers:** Lexilla (vendored, `lexilla/`) — toolkit-agnostic, unchanged.
- **Language:** C11. One `lexilla_bridge.cpp` in C++17.
- **Build:** CMake ≥ 3.20.
- **License:** GPL-3.

## Layout

```
nextpad-plus-plus-gtk4/
├── CMakeLists.txt
├── CLAUDE.md
├── docs/            ← migration analysis (00–04) + inherited GTK3 phase docs
├── src/             ← app code, flat C11
├── scintilla/       ← vendored GTK4 Scintilla (real git clone, has its own .git)
├── lexilla/         ← vendored Lexilla
└── resources/       ← app resources (themes, langs, icons, XML configs)
```

## Build

```sh
cmake -B build -S .
cmake --build build -j
./build/Nextpad++
```

Dev dependencies (Ubuntu 24.04):

```sh
sudo apt install build-essential cmake pkg-config \
                 libgtk-4-dev libadwaita-1-dev libglib2.0-dev libuchardet-dev
```

The Scintilla GTK4 backend builds via its own makefile (`scintilla/gtk4/`),
driven by a CMake custom target; it produces `scintilla/bin/libscintilla.so`.

## The GTK3→GTK4 migration

Tracked as phases M0–M11. See `docs/00_EXECUTIVE_SUMMARY.md` and
`docs/04_complexity_and_options.md`. Status:

- **M0** ✅ Scintilla GTK4 backend builds.
- **M1** ✅ GTK3 tree snapshotted here.
- **M2** ✅ GTK4 build system (`CMakeLists.txt`).
- **M3** ✅ `sci_c.h` bridges `ScintillaObject`→`ScintillaView` etc.
- **M4–M11** — in progress: API sweep (box packing, visibility, dialogs→async,
  events→controllers, menus), libadwaita adoption, Scintilla gap backfill.

### Scintilla bridge (M3)

`src/sci_c.h` aliases the renamed GTK4 Scintilla API back to the GTK2/3 names
the app code uses (`scintilla_new`, `scintilla_send_message`, `SCINTILLA()`).
The `"sci-notify"` signal carrying `SCNotification*` is unchanged. Structural
difference: `ScintillaView` is a `GtkWidget` implementing `GtkScrollable` — it
must be wrapped in a `GtkScrolledWindow` at each editor-creation site.

### GTK4 porting rules

- `gtk_box_pack_start/end` → `gtk_box_append` + per-child expand/margin props.
- `gtk_container_add` → widget-specific `set_child`.
- `gtk_widget_show_all` → delete (visible by default); `gtk_widget_show` →
  `gtk_widget_set_visible(w, TRUE)`.
- `gtk_widget_destroy` → `gtk_window_destroy` / `gtk_widget_unparent`.
- `gtk_dialog_run` → async: connect `"response"`, continue in the handler.
- `GtkMessageDialog`/`GtkFileChooserDialog` → `AdwMessageDialog` / `GtkFileDialog`.
- `GtkMenu` family → `GMenuModel` + `GtkPopoverMenu`.
- `"button-press-event"`/`"key-press-event"` → `GtkGestureClick` /
  `GtkEventControllerKey` added via `gtk_widget_add_controller`.
- `"delete-event"` → `"close-request"`.
- `gtk_drag_dest_set` → `GtkDropTarget`.

## On the project

- notetux-plus-plus is **no longer a reference** — the project has moved past
  it. The macOS Nextpad++ port is the source-of-truth for feature behaviour.
- User is **aletik** (Andrey Letov), macOS port author and Linux port
  maintainer. Wants meticulous, concise work.

## Backporting from GTK3

The GTK3 repo is registered as the `gtk3-src` git remote. During the short
dual-maintenance window, backport GTK3 fixes with
`git fetch gtk3-src && git cherry-pick <sha>`.
