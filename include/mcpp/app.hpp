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
        std::filesystem::path left,
        std::filesystem::path right,
        Config config = {}
    );
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcpp
