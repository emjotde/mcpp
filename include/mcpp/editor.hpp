#pragma once

#include "mcpp/provider.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

class EditorModel {
public:
    void open(
        ProviderPtr provider,
        std::string path,
        const Cancellation& cancellation = {}
    );
    void close();
    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::vector<std::string>& lines() const noexcept;
    [[nodiscard]] std::size_t row() const noexcept;
    [[nodiscard]] std::size_t column() const noexcept;

    void move(int rows, int columns);
    void home();
    void end();
    void insert(char character);
    void insert(std::string_view text);
    void newline();
    void backspace();
    void erase();
    bool undo();
    bool redo();
    void save(
        bool force,
        const Cancellation& cancellation = {}
    );
    [[nodiscard]] bool find(
        std::string_view needle,
        bool case_sensitive
    );

private:
    struct Snapshot {
        std::vector<std::string> lines;
        std::size_t row{0};
        std::size_t column{0};
    };

    void record_undo();
    void restore(Snapshot snapshot);

    ProviderPtr provider_;
    std::string path_;
    std::string revision_;
    std::vector<std::string> lines_;
    std::size_t row_{0};
    std::size_t column_{0};
    bool dirty_{false};
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
};

}  // namespace mcpp
