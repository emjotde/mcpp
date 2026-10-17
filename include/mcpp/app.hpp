#pragma once

#include "mcpp/config.hpp"
#include "mcpp/editor.hpp"
#include "mcpp/operations.hpp"
#include "mcpp/pane.hpp"
#include "mcpp/viewer.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace mcpp {

class App {
public:
    App(
        std::string left,
        std::string right,
        Config config = {}
    );
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void open_viewer(const std::string& path);
    void open_editor(const std::string& path);
    [[nodiscard]] std::string active_path() const;
    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcpp
