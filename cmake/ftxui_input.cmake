# Keep physical Ctrl-Space separate from FTXUI's NUL-valued redraw tasks.
set(input_source "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ftxui/src/ftxui/component/terminal_input_parser.cpp")
file(READ "${input_source}" original_input)
set(input_anchor "const std::map<std::string, std::string> g_uniformize = {")
set(input_replacement [=[const std::map<std::string, std::string> g_uniformize = {
    {std::string(1, '\0'), "\x1B[32;5u"},
]=])
string(FIND "${original_input}" "${input_anchor}" input_anchor_position)
if(input_anchor_position LESS 0)
    message(FATAL_ERROR "The pinned FTXUI input normalizer changed; review the Ctrl-Space integration")
endif()
string(REPLACE "${input_anchor}" "${input_replacement}" patched_input "${original_input}")
set(timeout_anchor "const int timeout_threshold = 50;")
string(FIND "${patched_input}" "${timeout_anchor}" timeout_anchor_position)
if(timeout_anchor_position LESS 0)
    message(FATAL_ERROR "The pinned FTXUI escape timeout changed; review the input integration")
endif()
set(timeout_support [=[
#include <atomic>
#include <limits>
namespace mcpp {
namespace { std::atomic<int> terminal_input_delay_ms{50}; }
void set_terminal_input_delay(std::uint64_t microseconds) {
    const auto milliseconds = microseconds / 1000U;
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    const auto delay = milliseconds > maximum ? std::numeric_limits<int>::max()
                      : static_cast<int>(milliseconds);
    terminal_input_delay_ms.store(delay < 1 ? 1 : delay, std::memory_order_relaxed);
}
}
namespace ftxui {
]=])
string(REPLACE "namespace ftxui {" "${timeout_support}" patched_input "${patched_input}")
string(REPLACE "${timeout_anchor}"
    "const int timeout_threshold = mcpp::terminal_input_delay_ms.load(std::memory_order_relaxed);"
    patched_input "${patched_input}")
set(patched_source "${CMAKE_CURRENT_BINARY_DIR}/ftxui/terminal_input_parser.cpp")
set(write_input TRUE)
if(EXISTS "${patched_source}")
    file(READ "${patched_source}" existing_input)
    if(existing_input STREQUAL patched_input)
        set(write_input FALSE)
    endif()
endif()
if(write_input)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ftxui")
    file(WRITE "${patched_source}" "${patched_input}")
endif()

get_target_property(component_directory component SOURCE_DIR)
get_target_property(component_sources component SOURCES)
set(component_patched_sources "")
set(input_source_count 0)
foreach(source IN LISTS component_sources)
    if(source MATCHES "terminal_input_parser[.]cpp$")
        math(EXPR input_source_count "${input_source_count} + 1")
    elseif(IS_ABSOLUTE "${source}" OR source MATCHES "^[$]<")
        list(APPEND component_patched_sources "${source}")
    else()
        list(APPEND component_patched_sources "${component_directory}/${source}")
    endif()
endforeach()
if(NOT input_source_count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one FTXUI input parser source")
endif()
list(APPEND component_patched_sources "${patched_source}")
set_property(TARGET component PROPERTY SOURCES "${component_patched_sources}")
