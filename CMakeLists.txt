cmake_minimum_required(VERSION 3.20)
project(
    mcpp
    VERSION 0.1.0
    DESCRIPTION "Independent C++ orthodox terminal file manager"
    LANGUAGES CXX
)

include(GNUInstallDirs)

option(MCPP_BUILD_TESTS "Build mcpp tests" ON)
option(MCPP_WARNINGS_AS_ERRORS "Treat mcpp warnings as errors" OFF)
option(MCPP_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MCPP_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_MODULES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS_FUZZER OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_DEV_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/ftxui)

find_package(Threads REQUIRED)

function(mcpp_target_defaults target)
    target_compile_options(
        ${target}
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
            -fstack-protector-strong
            $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>
            $<$<BOOL:${MCPP_WARNINGS_AS_ERRORS}>:-Werror>
    )
    target_link_options(
        ${target}
        PRIVATE
            -Wl,-z,relro
            -Wl,-z,now
    )
    if(MCPP_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
    if(MCPP_ENABLE_UBSAN)
        target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
    endif()
endfunction()

add_library(
    mcpp_core
    src/config.cpp
    src/compare.cpp
    src/location.cpp
    src/local_provider.cpp
    src/operations.cpp
    src/pane.cpp
    src/search.cpp
    src/syntax.cpp
    src/user_menu.cpp
    src/viewer.cpp
    src/editor.cpp
    src/editor_clipboard.cpp
    src/editor_search.cpp
)
target_include_directories(mcpp_core PUBLIC include)
target_link_libraries(mcpp_core PUBLIC Threads::Threads screen)
target_compile_definitions(mcpp_core PUBLIC MCPP_VERSION="${PROJECT_VERSION}")
mcpp_target_defaults(mcpp_core)

add_library(mcpp_ui src/app.cpp)
target_include_directories(mcpp_ui PUBLIC include)
target_link_libraries(mcpp_ui PUBLIC mcpp_core ftxui::ftxui)
mcpp_target_defaults(mcpp_ui)

add_executable(mcpp src/main.cpp)
target_link_libraries(mcpp PRIVATE mcpp_ui)
set_target_properties(mcpp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_options(mcpp PRIVATE -pie)
mcpp_target_defaults(mcpp)

install(TARGETS mcpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(
    FILES third_party/ftxui/LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    RENAME FTXUI-LICENSE
)

if(MCPP_BUILD_TESTS)
    enable_testing()
    add_executable(mcpp_tests tests/test_main.cpp)
    target_link_libraries(mcpp_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_tests)
    add_test(NAME mcpp_tests COMMAND mcpp_tests)
    if(EXISTS "/review/.operation-regressions-disposable")
        set_tests_properties(
            mcpp_tests PROPERTIES ENVIRONMENT
            "MCPP_DISPOSABLE_TESTS=1;TMPDIR=/review/runtime"
        )
    endif()
    add_executable(mcpp_editor_regressions tests/test_editor_regressions.cpp)
    target_link_libraries(mcpp_editor_regressions PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_editor_regressions)
    add_test(
        NAME mcpp_editor_regressions
        COMMAND mcpp_editor_regressions
                ${CMAKE_CURRENT_BINARY_DIR}/editor-regression-state
    )
    find_program(MCPP_SCRIPT_EXECUTABLE script)
    find_program(MCPP_TIMEOUT_EXECUTABLE timeout)
    if(MCPP_SCRIPT_EXECUTABLE AND MCPP_TIMEOUT_EXECUTABLE)
        add_test(
            NAME mcpp_terminal
            COMMAND
                bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_terminal.sh
                $<TARGET_FILE:mcpp>
                ${MCPP_SCRIPT_EXECUTABLE}
                ${MCPP_TIMEOUT_EXECUTABLE}
        )
        add_test(
            NAME mcpp_operation_ui
            COMMAND
                bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_operation_ui.sh
                $<TARGET_FILE:mcpp>
                ${MCPP_SCRIPT_EXECUTABLE}
                ${MCPP_TIMEOUT_EXECUTABLE}
        )
        add_test(
            NAME mcpp_editor_ui
            COMMAND
                bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_editor_ui.sh
                $<TARGET_FILE:mcpp>
                ${MCPP_SCRIPT_EXECUTABLE}
                ${MCPP_TIMEOUT_EXECUTABLE}
        )
        add_test(
            NAME mcpp_shortcuts
            COMMAND
                bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shortcuts.sh
                $<TARGET_FILE:mcpp>
                ${MCPP_SCRIPT_EXECUTABLE}
                ${MCPP_TIMEOUT_EXECUTABLE}
        )
        add_test(
            NAME mcpp_tree_ui
            COMMAND
                bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tree_ui.sh
                $<TARGET_FILE:mcpp>
                ${MCPP_SCRIPT_EXECUTABLE}
                ${MCPP_TIMEOUT_EXECUTABLE}
        )
    endif()
    find_program(MCPP_PYTHON_EXECUTABLE python3)
    find_program(MCPP_TMUX_EXECUTABLE tmux)
    if(MCPP_PYTHON_EXECUTABLE AND MCPP_TMUX_EXECUTABLE)
        add_test(
            NAME mcpp_save_setup
            COMMAND
                ${MCPP_PYTHON_EXECUTABLE}
                -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_save_setup.py
                $<TARGET_FILE:mcpp>
                ${MCPP_TMUX_EXECUTABLE}
        )
        set_tests_properties(mcpp_save_setup PROPERTIES TIMEOUT 45)
        add_test(
            NAME mcpp_viewer_navigation
            COMMAND
                ${MCPP_PYTHON_EXECUTABLE}
                -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_viewer_navigation.py
                $<TARGET_FILE:mcpp>
                ${MCPP_TMUX_EXECUTABLE}
        )
        set_tests_properties(mcpp_viewer_navigation PROPERTIES TIMEOUT 45)
        add_test(
            NAME mcpp_tree_workflows
            COMMAND
                ${MCPP_PYTHON_EXECUTABLE}
                -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tree_workflows.py
                $<TARGET_FILE:mcpp>
                ${MCPP_TMUX_EXECUTABLE}
        )
        set_tests_properties(mcpp_tree_workflows PROPERTIES TIMEOUT 240)
        add_test(
            NAME mcpp_panel_prompts
            COMMAND
                ${MCPP_PYTHON_EXECUTABLE}
                -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_panel_prompts.py
                $<TARGET_FILE:mcpp>
                ${MCPP_TMUX_EXECUTABLE}
        )
        set_tests_properties(mcpp_panel_prompts PROPERTIES TIMEOUT 90)
        add_test(
            NAME mcpp_editor_workflows
            COMMAND
                ${MCPP_PYTHON_EXECUTABLE}
                -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_editor_workflows.py
                $<TARGET_FILE:mcpp>
                ${MCPP_TMUX_EXECUTABLE}
                ${CMAKE_CURRENT_BINARY_DIR}/editor-ui
        )
        set_tests_properties(mcpp_editor_workflows PROPERTIES TIMEOUT 180)
        add_test(
            NAME mcpp_editor_mouse
            COMMAND
                ${MCPP_PYTHON_EXECUTABLE}
                -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_editor_mouse.py
                $<TARGET_FILE:mcpp>
                ${MCPP_TMUX_EXECUTABLE}
        )
        set_tests_properties(mcpp_editor_mouse PROPERTIES TIMEOUT 60)
        if(EXISTS "/review/.operation-regressions-disposable")
            add_test(
                NAME mcpp_operation_controls
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_operation_controls.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
            )
            set_tests_properties(mcpp_operation_controls PROPERTIES TIMEOUT 60)
        endif()
    endif()
endif()

set(CPACK_PACKAGE_NAME "mcpp")
set(CPACK_PACKAGE_VENDOR "mcpp contributors")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "TGZ")
include(CPack)
