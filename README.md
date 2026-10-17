# mcpp

`mcpp` is an independently implemented, MIT-licensed orthodox terminal file
manager written in C++20. It provides a familiar two-pane workflow without
using Midnight Commander source code or resources.

The first provider is the local Linux filesystem. Planned providers are
SSH/SFTP/SCP through libssh2 and Azure Blob Storage through azcpp.

## Build

```bash
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMCPP_WARNINGS_AS_ERRORS=ON
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure
```

FTXUI is pinned as a Git submodule:

```bash
git clone --recurse-submodules <repository>
```

## Initial key map

| Key | Action |
|---|---|
| `Tab` | Switch pane |
| `Enter` | Enter directory |
| `Backspace` | Parent directory |
| `Insert` / `Space` | Toggle selection |
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
