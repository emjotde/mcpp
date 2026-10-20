#pragma once

#include "mcpp/cli_display.hpp"
#include "mcpp/command_line.hpp"
#include "mcpp/config.hpp"
#include "mcpp/editor.hpp"
#include "mcpp/operations.hpp"
#include "mcpp/pane.hpp"
#include "mcpp/viewer.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcpp {

struct AppOptions {
    bool mouse{true};
    std::optional<bool> color;
    std::vector<CliColorOverride> colors;
    bool stickchars{false};
};

class App {
public:
    App(
        std::string left,
        std::string right,
        Config config = {},
        AppOptions options = {}
    );
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void open_viewer(const std::string& path);
    void open_editor(const std::string& path, std::size_t line = 0);
    void open_editors(const std::vector<std::string>& paths, std::size_t first_line = 0);
    void open_editor_files(const std::vector<EditorFile>& files, std::size_t first_line = 0);
    void open_diff(const std::string& first, const std::string& second);
    [[nodiscard]] std::string active_path() const;
    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcpp
