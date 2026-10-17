# mcpp

`mcpp` is an independently implemented, MIT-licensed orthodox terminal file
manager written in C++20. It provides a familiar two-pane workflow without
using Midnight Commander source code or resources.

The first provider is the local Linux filesystem. Planned providers are
Azure Blob Storage through azcpp. SSH/SFTP work is currently paused.

## Current functionality

- Independent dual panes with keyboard and mouse focus
- Directory history, quick change, hidden-file toggle, sorting, and hotlist
- Selection, masks, inversion, and recursive search results
- F2 rename, F3 range/hex viewer, and F4 conflict-aware editor
- F5 copy, F6 move, F7 mkdir, F8 delete, F9 menu, and F10 quit
- Asynchronous copy/move/delete progress and cancellation
- Atomic no-clobber writes and symlink/root confinement
- Mode, owner/group, file information, free-space display, and links
- Directory comparison and quick-view panel
- Local command line, history, and restored-terminal subshell
- Persisted panel paths and preferences
- MC-style visual state cues using a grayscale-only palette
- Toggleable VS Code Dark+-style syntax colors within grayscale UI chrome
- Toggleable editor line-number gutter and mcedit-style position header
- Persistent linear/column blocks and atomic cross-session editor clipboard

The implementation is behaviorally familiar but does not use Midnight
Commander source code, resources, strings, tests, or artwork. A shallow MC
checkout under ignored `reference/` is used only to study behavior and
documentation; no MC implementation is copied.

## Build

```bash
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMCPP_WARNINGS_AS_ERRORS=ON
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure
```

The binary remains in the selected build directory; installation is not
required.

FTXUI is pinned as a Git submodule:

```bash
git clone --recurse-submodules <repository>
```

Sandboxed destructive tests run without host mounts:

```bash
bash tests/run_container.sh
```

The current parity gaps against GNU Midnight Commander are documented in
[docs/mc-gap-analysis.md](docs/mc-gap-analysis.md).

## Initial key map

| Key | Action |
|---|---|
| `Tab` | Switch pane |
| `Enter` | Enter directory |
| `Left` / `Ctrl-PageUp` | Parent directory |
| `Right` / `Ctrl-PageDown` | Enter selected directory |
| `Insert` | Toggle selection |
| `Ctrl-S` / `Alt-S` | Incremental panel search |
| `F3` | View |
| `F4` | Edit |
| `F5` | Copy |
| `F6` | Move |
| `F7` | Create directory |
| `F8` | Delete |
| `F9` | Menu |
| `F10` | Quit |

## License

mcpp is MIT-licensed. FTXUI is also MIT-licensed and retains its own
copyright notice under `third_party/ftxui/LICENSE`.
