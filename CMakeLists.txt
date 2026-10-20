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
include(cmake/ftxui_input.cmake)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8>=10.34)

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
    src/archive_provider.cpp
    src/extfs_provider.cpp
    src/extfs_helper.cpp
    src/ext2_undelete_provider.cpp
    src/transform_provider.cpp
    src/command_line.cpp
    src/color_resource.cpp
    src/completion.cpp
    src/config.cpp
    src/config_ini.cpp
    src/entry.cpp
    src/extension.cpp
    src/extension_view_checkout.cpp
    src/file_highlight.cpp
    src/listing.cpp
    src/compare.cpp
    src/diff.cpp
    src/hotlist.cpp
    src/help.cpp
    src/input_history.cpp
    src/keymap.cpp
    src/location.cpp
    src/local_provider.cpp
    src/local_tools.cpp
    src/native_menu.cpp
    src/native_regex.cpp
    src/operations.cpp
    src/pane.cpp
    src/search.cpp
    src/shell.cpp
    src/skin.cpp
    src/skin_glyphs.cpp
    src/skin_file.cpp
    src/spelling.cpp
    src/syntax.cpp
    src/syntax_rules.cpp
    src/user_menu.cpp
    src/tree_cache.cpp
    src/translation.cpp
    src/viewer.cpp
    src/viewer_data.cpp
    src/editor.cpp
    src/editor_lock.cpp
    src/editor_clipboard.cpp
    src/editor_search.cpp
)
target_include_directories(mcpp_core PUBLIC include)
target_link_libraries(mcpp_core PUBLIC Threads::Threads screen PkgConfig::PCRE2)
target_compile_definitions(mcpp_core PUBLIC MCPP_VERSION="${PROJECT_VERSION}")
mcpp_target_defaults(mcpp_core)

add_library(
    mcpp_ui
    src/app.cpp
    src/input_styles.cpp
    src/keymap_input.cpp
    src/cli_display.cpp
    src/skin_glyphs_ui.cpp
    src/viewer_help_styles.cpp
)
target_include_directories(mcpp_ui PUBLIC include)
target_link_libraries(mcpp_ui PUBLIC mcpp_core ftxui::ftxui)
mcpp_target_defaults(mcpp_ui)

add_executable(mcpp src/main.cpp)
add_executable(mcppedit src/editor_main.cpp)
add_executable(mcppview src/viewer_main.cpp)
add_executable(mcppdiff src/diff_main.cpp)
foreach(tool IN ITEMS mcpp mcppedit mcppview mcppdiff)
    target_link_libraries(${tool} PRIVATE mcpp_ui)
    set_target_properties(${tool} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_options(${tool} PRIVATE -pie)
    mcpp_target_defaults(${tool})
endforeach()

install(TARGETS mcpp mcppedit mcppview mcppdiff RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
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
    add_executable(mcpp_keymap_tests tests/test_keymap.cpp)
    target_link_libraries(mcpp_keymap_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_keymap_tests)
    add_executable(mcpp_keymap_input_tests tests/test_keymap_input.cpp)
    target_include_directories(mcpp_keymap_input_tests PRIVATE third_party/ftxui/src)
    target_link_libraries(mcpp_keymap_input_tests PRIVATE mcpp_ui)
    mcpp_target_defaults(mcpp_keymap_input_tests)
    add_executable(mcpp_archive_tests tests/test_archive_provider.cpp)
    target_link_libraries(mcpp_archive_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_archive_tests)
    add_executable(mcpp_archive_complete_tests tests/test_archive_complete.cpp)
    target_link_libraries(mcpp_archive_complete_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_archive_complete_tests)
    add_executable(mcpp_extfs_tests tests/test_extfs_provider.cpp)
    target_link_libraries(mcpp_extfs_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_extfs_tests)
    add_executable(mcpp_transform_tests tests/test_transform_provider.cpp)
    target_link_libraries(mcpp_transform_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_transform_tests)
    add_executable(mcpp_archive_operation_tests tests/test_archive_operations.cpp)
    target_link_libraries(mcpp_archive_operation_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_archive_operation_tests)
    add_executable(mcpp_archive_location_tests tests/test_archive_locations.cpp)
    target_link_libraries(mcpp_archive_location_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_archive_location_tests)
    add_executable(mcpp_editor_large_files tests/test_editor_large_files.cpp)
    target_link_libraries(mcpp_editor_large_files PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_editor_large_files)
    add_executable(mcpp_editor_lock_tests tests/test_editor_locks.cpp)
    target_link_libraries(mcpp_editor_lock_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_editor_lock_tests)
    add_executable(mcpp_native_menu_tests tests/test_native_menu.cpp)
    target_link_libraries(mcpp_native_menu_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_native_menu_tests)
    add_executable(mcpp_native_menu_complete_tests tests/test_native_menu_complete.cpp)
    target_link_libraries(mcpp_native_menu_complete_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_native_menu_complete_tests)
    add_executable(mcpp_native_regex_tests tests/test_native_regex.cpp)
    target_link_libraries(mcpp_native_regex_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_native_regex_tests)
    add_executable(mcpp_extension_tests tests/test_extensions.cpp)
    target_link_libraries(mcpp_extension_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_extension_tests)
    add_executable(mcpp_streamed_command_tests tests/test_streamed_command.cpp)
    target_link_libraries(mcpp_streamed_command_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_streamed_command_tests)
    add_executable(mcpp_shell_session_driver tests/shell_session_driver.cpp)
    target_link_libraries(mcpp_shell_session_driver PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_shell_session_driver)
    add_executable(mcpp_extfs_sourceless_tests tests/test_extfs_sourceless.cpp)
    target_link_libraries(mcpp_extfs_sourceless_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_extfs_sourceless_tests)
    add_executable(mcpp_ext2_undelete_tests tests/test_ext2_undelete.cpp)
    target_link_libraries(mcpp_ext2_undelete_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_ext2_undelete_tests)
    add_executable(mcpp_archive_incremental_tests tests/test_archive_incremental.cpp)
    target_link_libraries(mcpp_archive_incremental_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_archive_incremental_tests)
    add_executable(mcpp_viewer_source_tests tests/test_viewer_sources.cpp)
    target_link_libraries(mcpp_viewer_source_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_viewer_source_tests)
    add_executable(mcpp_viewer_large_tests tests/test_viewer_large.cpp)
    target_link_libraries(mcpp_viewer_large_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_viewer_large_tests)
    add_executable(mcpp_append_resume_tests tests/test_append_resume.cpp)
    target_link_libraries(mcpp_append_resume_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_append_resume_tests)
    add_executable(mcpp_copy_mask_tests tests/test_copy_masks.cpp)
    target_link_libraries(mcpp_copy_mask_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_copy_mask_tests)
    add_executable(mcpp_recursive_delete_tests tests/test_recursive_delete.cpp)
    target_link_libraries(mcpp_recursive_delete_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_recursive_delete_tests)
    add_executable(mcpp_native_skin_tests tests/test_native_skins.cpp)
    target_link_libraries(mcpp_native_skin_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_native_skin_tests)
    add_executable(mcpp_skin_glyph_tests tests/test_skin_glyphs.cpp)
    target_link_libraries(mcpp_skin_glyph_tests PRIVATE mcpp_ui)
    mcpp_target_defaults(mcpp_skin_glyph_tests)
    add_executable(mcpp_editor_codepage_tests tests/test_editor_codepages.cpp)
    target_link_libraries(mcpp_editor_codepage_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_editor_codepage_tests)
    add_executable(mcpp_hotlist_tests tests/test_hotlist.cpp)
    target_link_libraries(mcpp_hotlist_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_hotlist_tests)
    add_executable(mcpp_help_tests tests/test_help.cpp)
    target_link_libraries(mcpp_help_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_help_tests)
    add_executable(mcpp_cli_tests tests/test_cli.cpp)
    target_link_libraries(mcpp_cli_tests PRIVATE mcpp_ui)
    mcpp_target_defaults(mcpp_cli_tests)
    add_executable(mcpp_cli_color_tests tests/test_cli_color_roles.cpp)
    target_link_libraries(mcpp_cli_color_tests PRIVATE mcpp_ui)
    mcpp_target_defaults(mcpp_cli_color_tests)
    add_executable(mcpp_viewer_help_style_tests tests/test_viewer_help_styles.cpp)
    target_include_directories(mcpp_viewer_help_style_tests PRIVATE src)
    target_link_libraries(mcpp_viewer_help_style_tests PRIVATE mcpp_ui)
    mcpp_target_defaults(mcpp_viewer_help_style_tests)
    add_executable(mcpp_input_style_tests tests/test_input_styles.cpp)
    target_include_directories(mcpp_input_style_tests PRIVATE src)
    target_link_libraries(mcpp_input_style_tests PRIVATE mcpp_ui)
    mcpp_target_defaults(mcpp_input_style_tests)
    add_executable(mcpp_editor_display_tests tests/test_editor_display_preferences.cpp)
    target_include_directories(mcpp_editor_display_tests PRIVATE src)
    target_link_libraries(mcpp_editor_display_tests PRIVATE mcpp_core ftxui::ftxui)
    mcpp_target_defaults(mcpp_editor_display_tests)
    add_executable(mcpp_editor_clipboard_commands tests/test_editor_clipboard_commands.cpp)
    target_link_libraries(mcpp_editor_clipboard_commands PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_editor_clipboard_commands)
    add_executable(mcpp_panel_selection_tests tests/test_panel_selection_preferences.cpp)
    target_include_directories(mcpp_panel_selection_tests PRIVATE src)
    target_link_libraries(mcpp_panel_selection_tests PRIVATE mcpp_ui)
    mcpp_target_defaults(mcpp_panel_selection_tests)
    add_executable(mcpp_shell_display_tests tests/test_shell_display_preferences.cpp)
    target_include_directories(mcpp_shell_display_tests PRIVATE src)
    target_link_libraries(mcpp_shell_display_tests PRIVATE ftxui::ftxui)
    mcpp_target_defaults(mcpp_shell_display_tests)
    add_executable(mcpp_panel_torben_preferences tests/test_panel_torben_preferences.cpp)
    target_link_libraries(mcpp_panel_torben_preferences PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_panel_torben_preferences)
    add_executable(mcpp_tree_navigation_tests tests/test_tree_navigation_preferences.cpp)
    target_include_directories(mcpp_tree_navigation_tests PRIVATE src)
    target_link_libraries(mcpp_tree_navigation_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_tree_navigation_tests)
    add_executable(mcpp_viewer_eof_tests tests/test_viewer_eof.cpp)
    target_include_directories(mcpp_viewer_eof_tests PRIVATE src)
    target_link_libraries(mcpp_viewer_eof_tests PRIVATE mcpp_ui)
    mcpp_target_defaults(mcpp_viewer_eof_tests)
    add_executable(mcpp_extension_view_checkout_tests tests/test_extension_view_checkout.cpp)
    target_include_directories(mcpp_extension_view_checkout_tests PRIVATE src)
    target_link_libraries(mcpp_extension_view_checkout_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_extension_view_checkout_tests)
    add_executable(mcpp_translation_tests tests/test_translation.cpp)
    target_link_libraries(mcpp_translation_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_translation_tests)
    add_executable(mcpp_native_config_tests tests/test_native_config.cpp)
    target_link_libraries(mcpp_native_config_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_native_config_tests)
    add_executable(mcpp_native_color_tests tests/test_native_colors.cpp)
    target_link_libraries(mcpp_native_color_tests PRIVATE mcpp_ui)
    mcpp_target_defaults(mcpp_native_color_tests)
    add_executable(mcpp_operation_job_tests tests/test_operation_jobs.cpp)
    target_link_libraries(mcpp_operation_job_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_operation_job_tests)
    add_executable(mcpp_input_history_tests tests/test_input_history.cpp)
    target_link_libraries(mcpp_input_history_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_input_history_tests)
    add_executable(mcpp_syntax_rules_tests tests/test_syntax_rules.cpp)
    target_link_libraries(mcpp_syntax_rules_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_syntax_rules_tests)
    add_executable(mcpp_sparse_special_tests tests/test_sparse_special.cpp)
    target_link_libraries(mcpp_sparse_special_tests PRIVATE mcpp_core)
    target_link_options(
        mcpp_sparse_special_tests PRIVATE
        -Wl,--wrap=lseek,--wrap=pread,--wrap=__pread_chk,--wrap=pwrite,--wrap=ftruncate,--wrap=fsync
    )
    mcpp_target_defaults(mcpp_sparse_special_tests)
    if(EXISTS "/review/.operation-regressions-disposable")
        file(MAKE_DIRECTORY
            /review/cases/agent-archives
            /review/cases/agent-archive-operations
            /review/cases/sparse-special-files
            /review/cases/input-histories
            /review/cases/append-resume
            /review/cases/copy-masks
            /review/cases/concurrent-jobs
            /review/cases/independent-screens
            /review/cases/cli-remaining-ctest
            /review/cases/menu-complete-ctest
        )
        add_test(
            NAME mcpp_keymap_tests
            COMMAND mcpp_keymap_tests /review/cases/agent-keymap/ctest
        )
        add_test(NAME mcpp_keymap_input_tests COMMAND mcpp_keymap_input_tests)
        foreach(cli_case IN ITEMS options information operands)
            add_test(
                NAME mcpp_cli_${cli_case}
                COMMAND mcpp_cli_tests ${cli_case}
                        /review/cases/cli-remaining-ctest/${cli_case}
            )
        endforeach()
        foreach(cli_color_case IN ITEMS roles preservation)
            add_test(
                NAME mcpp_cli_color_${cli_color_case}
                COMMAND mcpp_cli_color_tests ${cli_color_case}
                        /review/cases/cli-color-roles/${cli_color_case}-ctest
            )
        endforeach()
        foreach(viewer_help_case IN ITEMS styles cli)
            add_test(
                NAME mcpp_viewer_help_${viewer_help_case}
                COMMAND mcpp_viewer_help_style_tests ${viewer_help_case}
                        /review/cases/viewer-help-colors/models/${viewer_help_case}-ctest
            )
        endforeach()
        add_test(
            NAME mcpp_native_input_styles
            COMMAND mcpp_input_style_tests /review/cases/input-styles/model/ctest
        )
        add_test(
            NAME mcpp_editor_display_preferences
            COMMAND mcpp_editor_display_tests
                    /review/cases/mc-config/editor-display-500191ad/model/ctest
        )
        foreach(clipboard_command_case IN ITEMS success failure)
            add_test(
                NAME mcpp_editor_clipboard_${clipboard_command_case}
                COMMAND mcpp_editor_clipboard_commands
                        /review/cases/native-clipboard-commands/ctest-${clipboard_command_case}
                        ${clipboard_command_case}
            )
            set_tests_properties(mcpp_editor_clipboard_${clipboard_command_case} PROPERTIES TIMEOUT 15)
        endforeach()
        add_test(
            NAME mcpp_panel_selection_preferences
            COMMAND mcpp_panel_selection_tests /review/cases/selection-0d2d/model/ctest
        )
        add_test(NAME mcpp_shell_display_preferences COMMAND mcpp_shell_display_tests model)
        add_test(NAME mcpp_panel_torben_preferences COMMAND mcpp_panel_torben_preferences)
        set_tests_properties(mcpp_panel_torben_preferences PROPERTIES WORKING_DIRECTORY /review/cases)
        add_test(
            NAME mcpp_native_tree_navigation
            COMMAND mcpp_tree_navigation_tests /review/cases/xtree-fd3eff6e/model/ctest
        )
        foreach(viewer_eof_case IN ITEMS render state)
            add_test(
                NAME mcpp_viewer_eof_${viewer_eof_case}
                COMMAND mcpp_viewer_eof_tests ${viewer_eof_case}
                        /review/cases/viewer-eof/models/${viewer_eof_case}-ctest
            )
        endforeach()
        foreach(view_checkout_case IN ITEMS success failure)
            add_test(
                NAME mcpp_extension_view_checkout_${view_checkout_case}
                COMMAND mcpp_extension_view_checkout_tests ${view_checkout_case}
                        /review/cases/l21-view-checkout/model
            )
        endforeach()
        add_test(
            NAME mcpp_archive_tests
            COMMAND mcpp_archive_tests /review/cases/agent-archives
        )
        add_test(
            NAME mcpp_archive_complete_tests
            COMMAND mcpp_archive_complete_tests /review/cases/archive-complete-model-81945 formats snapshots
        )
        foreach(extfs_case IN ITEMS snapshot mutation rejection)
            add_test(
                NAME mcpp_extfs_${extfs_case}
                COMMAND mcpp_extfs_tests ${extfs_case}
                        /review/cases/extfs-local-${extfs_case}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/extfs_fixture.py
            )
        endforeach()
        foreach(transform_case IN ITEMS local namespace errors)
            add_test(
                NAME mcpp_transform_${transform_case}
                COMMAND mcpp_transform_tests ${transform_case}
                        /review/cases/single-transform-${transform_case}
            )
        endforeach()
        add_test(
            NAME mcpp_archive_operation_tests
            COMMAND mcpp_archive_operation_tests /review/cases/agent-archive-operations
        )
        add_test(
            NAME mcpp_archive_location_tests
            COMMAND mcpp_archive_location_tests /review/cases/archive-locations
        )
        add_test(
            NAME mcpp_editor_large_files
            COMMAND mcpp_editor_large_files /review/cases/editor-large-files/model
        )
        add_test(
            NAME mcpp_editor_lock_tests
            COMMAND mcpp_editor_lock_tests /review/cases/editor-locks/model
        )
        add_test(NAME mcpp_native_menu_tests COMMAND mcpp_native_menu_tests)
        foreach(regex_case IN ITEMS core extensions syntax highlight)
            add_test(
                NAME mcpp_native_regex_${regex_case}
                COMMAND mcpp_native_regex_tests ${regex_case}
                        /review/cases/native-regex-${regex_case}
            )
        endforeach()
        foreach(menu_case IN ITEMS words contexts numbers cases extglobs backticks directories)
            add_test(
                NAME mcpp_native_menu_${menu_case}
                COMMAND mcpp_native_menu_complete_tests ${menu_case}
                        /review/cases/menu-complete-ctest/${menu_case}
            )
        endforeach()
        add_test(NAME mcpp_extension_tests COMMAND mcpp_extension_tests)
        add_test(
            NAME mcpp_streamed_command_tests
            COMMAND mcpp_streamed_command_tests /review/cases/streamed-view-filters-model
        )
        set_tests_properties(mcpp_streamed_command_tests PROPERTIES TIMEOUT 15)
        foreach(incremental_case IN ITEMS browse guards)
            add_test(
                NAME mcpp_archive_incremental_${incremental_case}
                COMMAND mcpp_archive_incremental_tests
                        /review/cases/archive-incremental-ctest-${incremental_case} ${incremental_case}
            )
        endforeach()
        foreach(source_less_case IN ITEMS origin errors)
            add_test(
                NAME mcpp_extfs_sourceless_${source_less_case}
                COMMAND mcpp_extfs_sourceless_tests ${source_less_case}
                        /review/cases/extfs-local-sourceless-${source_less_case}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/extfs_sourceless_fixture.py
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/extfs_fixture.py
            )
            set_tests_properties(mcpp_extfs_sourceless_${source_less_case} PROPERTIES TIMEOUT 30)
        endforeach()
        add_test(NAME mcpp_append_resume_bytes COMMAND mcpp_append_resume_tests bytes)
        add_test(NAME mcpp_append_resume_safety COMMAND mcpp_append_resume_tests safety)
        set_tests_properties(
            mcpp_append_resume_bytes mcpp_append_resume_safety
            PROPERTIES WORKING_DIRECTORY /review/cases/append-resume
        )
        foreach(copy_mask_case IN ITEMS masks placement links)
            add_test(
                NAME mcpp_copy_${copy_mask_case}
                COMMAND mcpp_copy_mask_tests ${copy_mask_case}
            )
            set_tests_properties(
                mcpp_copy_${copy_mask_case}
                PROPERTIES WORKING_DIRECTORY /review/cases/copy-masks
            )
        endforeach()
        add_test(
            NAME mcpp_editor_codepage_tests
            COMMAND mcpp_editor_codepage_tests /review/cases/editor-codepages/model
        )
        add_test(
            NAME mcpp_recursive_delete_tests
            COMMAND mcpp_recursive_delete_tests /review/cases/recursive-delete/model
        )
        add_test(
            NAME mcpp_native_skin_tests
            COMMAND mcpp_native_skin_tests /review/cases/native-skins/model
        )
        add_test(
            NAME mcpp_skin_glyph_tests
            COMMAND mcpp_skin_glyph_tests /review/cases/native-skin-glyphs-ctest
        )
        add_test(
            NAME mcpp_viewer_source_growing
            COMMAND mcpp_viewer_source_tests growing /review/cases/streamed-viewer/growing
        )
        add_test(
            NAME mcpp_viewer_source_stream
            COMMAND mcpp_viewer_source_tests stream /review/cases/streamed-viewer/stream
        )
        foreach(viewer_large_case IN ITEMS boundary parsed search mapping)
            add_test(
                NAME mcpp_viewer_large_${viewer_large_case}
                COMMAND mcpp_viewer_large_tests ${viewer_large_case}
                        /review/cases/viewer-large-${viewer_large_case}-8f9fb4ef
            )
        endforeach()
        add_test(
            NAME mcpp_hotlist_tests
            COMMAND mcpp_hotlist_tests /review/cases/hierarchical-hotlists
        )
        add_test(NAME mcpp_help_tests COMMAND mcpp_help_tests)
        foreach(native_config_case IN ITEMS native legacy invalid)
            add_test(
                NAME mcpp_native_config_${native_config_case}
                COMMAND mcpp_native_config_tests ${native_config_case}
                        /review/cases/native-config/model/${native_config_case}
            )
        endforeach()
        add_test(
            NAME mcpp_native_config_remaining_four
            COMMAND mcpp_native_config_tests remaining-four
                    /review/cases/native-config/model/remaining-four
        )
        add_test(
            NAME mcpp_native_config_remaining_three
            COMMAND mcpp_native_config_tests remaining-three
                    /review/cases/native-config/model/remaining-three
        )
        foreach(native_color_case IN ITEMS native legacy invalid)
            add_test(
                NAME mcpp_native_colors_${native_color_case}
                COMMAND mcpp_native_color_tests ${native_color_case}
                        /review/cases/mc-config/colors/model/${native_color_case}
            )
        endforeach()
        add_test(NAME mcpp_operation_job_tests COMMAND mcpp_operation_job_tests)
        add_test(
            NAME mcpp_input_history_tests
            COMMAND mcpp_input_history_tests /review/cases/input-histories
        )
        add_test(
            NAME mcpp_syntax_rules_tests
            COMMAND mcpp_syntax_rules_tests /review/cases/mc-syntax/model
        )
        add_test(
            NAME mcpp_syntax_native_bytes
            COMMAND mcpp_syntax_rules_tests /review/cases/mc-syntax/native-bytes native-bytes
        )
        add_test(
            NAME mcpp_syntax_native_format
            COMMAND mcpp_syntax_rules_tests /review/cases/mc-syntax/native-format native-format
        )
        add_test(
            NAME mcpp_syntax_native_consumption
            COMMAND mcpp_syntax_rules_tests /review/cases/mc-syntax/native-consumption native-consumption
        )
        add_test(
            NAME mcpp_syntax_native_context
            COMMAND mcpp_syntax_rules_tests /review/cases/mc-syntax/native-context native-context
        )
        add_test(
            NAME mcpp_syntax_native_guards
            COMMAND mcpp_syntax_rules_tests /review/cases/mc-syntax/native-guards native-guards
        )
        add_test(NAME mcpp_translation_tests COMMAND mcpp_translation_tests)
        add_test(
            NAME mcpp_sparse_special_tests
            COMMAND mcpp_sparse_special_tests /review/cases/sparse-special-files
        )
        set_tests_properties(mcpp_editor_large_files mcpp_sparse_special_tests PROPERTIES TIMEOUT 90)
    endif()
    add_executable(mcpp_editor_regressions tests/test_editor_regressions.cpp)
    target_link_libraries(mcpp_editor_regressions PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_editor_regressions)
    add_test(
        NAME mcpp_editor_regressions
        COMMAND mcpp_editor_regressions
                ${CMAKE_CURRENT_BINARY_DIR}/editor-regression-state
    )
    add_executable(mcpp_audit_regressions tests/test_audit_regressions.cpp)
    target_link_libraries(mcpp_audit_regressions PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_audit_regressions)
    if(EXISTS "/review/.operation-regressions-disposable")
        add_test(NAME mcpp_audit_regressions COMMAND mcpp_audit_regressions)
    endif()
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
    if(MCPP_PYTHON_EXECUTABLE AND EXISTS "/review/.operation-regressions-disposable")
        add_test(
            NAME mcpp_ext2_undelete
            COMMAND
                ${MCPP_PYTHON_EXECUTABLE}
                -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext2_undelete.py
                --binary $<TARGET_FILE:mcpp_ext2_undelete_tests>
                --root /review/cases/ext2-undelete-ctest
        )
        set_tests_properties(mcpp_ext2_undelete PROPERTIES TIMEOUT 60)
        add_test(
            NAME mcpp_shell_variants
            COMMAND
                ${MCPP_PYTHON_EXECUTABLE}
                -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shell_variants.py
                $<TARGET_FILE:mcpp_shell_session_driver>
                /review/cases/shell-variants-ctest
        )
        set_tests_properties(mcpp_shell_variants PROPERTIES TIMEOUT 45)
    endif()
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
        add_test(
            NAME mcpp_editor_parity
            COMMAND
                ${MCPP_PYTHON_EXECUTABLE}
                -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_editor_parity.py
                $<TARGET_FILE:mcpp>
                ${MCPP_TMUX_EXECUTABLE}
        )
        set_tests_properties(mcpp_editor_parity PROPERTIES TIMEOUT 60)
        add_test(
            NAME mcpp_editor_save
            COMMAND
                ${MCPP_PYTHON_EXECUTABLE}
                -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_editor_save.py
                $<TARGET_FILE:mcpp>
                ${MCPP_TMUX_EXECUTABLE}
        )
        set_tests_properties(mcpp_editor_save PROPERTIES TIMEOUT 60)
        if(EXISTS "/review/.operation-regressions-disposable")
            add_test(
                NAME mcpp_native_completion_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_completion_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
                    ${CMAKE_CURRENT_BINARY_DIR}/native-completion-ui
            )
            set_tests_properties(mcpp_native_completion_ui PROPERTIES TIMEOUT 180)
            add_test(
                NAME mcpp_standalone_editor
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcppedit.py
                    $<TARGET_FILE:mcppedit>
                    ${MCPP_TMUX_EXECUTABLE}
                    ${CMAKE_CURRENT_BINARY_DIR}/standalone-editor-ui
            )
            set_tests_properties(mcpp_standalone_editor PROPERTIES TIMEOUT 90)
            add_test(
                NAME mcpp_local_frontends
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_local_frontends.py
                    $<TARGET_FILE:mcpp>
                    $<TARGET_FILE:mcppview>
                    $<TARGET_FILE:mcppdiff>
                    ${MCPP_TMUX_EXECUTABLE}
                    ${CMAKE_CURRENT_BINARY_DIR}/local-frontends-ui
            )
            set_tests_properties(mcpp_local_frontends PROPERTIES TIMEOUT 120)
            add_test(
                NAME mcpp_spelling_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spelling_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
                    ${CMAKE_CURRENT_BINARY_DIR}/spelling-ui
            )
            set_tests_properties(mcpp_spelling_ui PROPERTIES TIMEOUT 120)
            add_test(
                NAME mcpp_skins_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_skins_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
                    ${CMAKE_CURRENT_BINARY_DIR}/skins-ui
            )
            set_tests_properties(mcpp_skins_ui PROPERTIES TIMEOUT 180)
            add_test(
                NAME mcpp_shell_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shell_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
                    ${CMAKE_CURRENT_BINARY_DIR}/shell-ui
            )
            set_tests_properties(mcpp_shell_ui PROPERTIES TIMEOUT 120)
            add_test(
                NAME mcpp_keymap_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_keymap_ui.py
                    $<TARGET_FILE:mcpp>
                    $<TARGET_FILE:mcppedit>
                    $<TARGET_FILE:mcppview>
                    $<TARGET_FILE:mcppdiff>
                    ${MCPP_TMUX_EXECUTABLE}
                    ${CMAKE_CURRENT_BINARY_DIR}/keymap-ui
            )
            set_tests_properties(mcpp_keymap_ui PROPERTIES TIMEOUT 150)
            add_test(
                NAME mcpp_cli_frontends_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli_frontends_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
                    /review/cases/cli-remaining-frontends-ctest
            )
            set_tests_properties(mcpp_cli_frontends_ui PROPERTIES TIMEOUT 150)
            add_test(
                NAME mcpp_skin_visual_fixes_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_skins_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
                    /review/cases/skin-visual-fixes
                    SkinsUI.test_diff_search_uses_skin_colors_on_changed_rows
                    SkinsUI.test_disabled_dropdown_labels_stay_readable
                    SkinsUI.test_native_popup_menu_uses_its_own_roles
                    SkinsUI.test_native_editor_gutter_uses_line_state_role
                    SkinsUI.test_native_dialog_titles_use_their_own_roles
            )
            set_tests_properties(mcpp_skin_visual_fixes_ui PROPERTIES TIMEOUT 90)
            add_test(
                NAME mcpp_skin_glyphs_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_skin_glyphs_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
                    /review/cases/native-skin-glyphs-ui
            )
            set_tests_properties(mcpp_skin_glyphs_ui PROPERTIES TIMEOUT 90)
            add_test(
                NAME mcpp_concurrent_jobs_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_concurrent_jobs_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
            )
            set_tests_properties(
                mcpp_concurrent_jobs_ui
                PROPERTIES TIMEOUT 90 WORKING_DIRECTORY /review/cases/concurrent-jobs
            )
            add_test(
                NAME mcpp_independent_screens_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_independent_screens_ui.py
                    $<TARGET_FILE:mcpp>
                    $<TARGET_FILE:mcppview>
                    $<TARGET_FILE:mcppdiff>
                    ${MCPP_TMUX_EXECUTABLE}
                    /review/cases/independent-screens
            )
            set_tests_properties(mcpp_independent_screens_ui PROPERTIES TIMEOUT 120)
            add_test(
                NAME mcpp_archives_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_archives_ui.py
                    $<TARGET_FILE:mcpp>
                    $<TARGET_FILE:mcppview>
                    ${MCPP_TMUX_EXECUTABLE}
                    ${CMAKE_CURRENT_BINARY_DIR}/archives-ui
            )
            set_tests_properties(mcpp_archives_ui PROPERTIES TIMEOUT 180)
            add_test(
                NAME mcpp_extfs_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_extfs_ui.py
                    $<TARGET_FILE:mcpp>
                    --mcpp-only
                    --tmux ${MCPP_TMUX_EXECUTABLE}
            )
            set_tests_properties(mcpp_extfs_ui PROPERTIES TIMEOUT 90)
            add_test(
                NAME mcpp_ext2_undelete_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext2_undelete_ui.py
                    $<TARGET_FILE:mcpp>
                    --tmux ${MCPP_TMUX_EXECUTABLE}
            )
            set_tests_properties(mcpp_ext2_undelete_ui PROPERTIES TIMEOUT 90)
            add_test(
                NAME mcpp_archive_incremental_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_archive_incremental_ui.py
                    $<TARGET_FILE:mcpp>
                    --tmux ${MCPP_TMUX_EXECUTABLE}
            )
            set_tests_properties(mcpp_archive_incremental_ui PROPERTIES TIMEOUT 90)
            add_test(
                NAME mcpp_extfs_sourceless_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_extfs_sourceless_ui.py
                    $<TARGET_FILE:mcpp>
                    --tmux ${MCPP_TMUX_EXECUTABLE}
            )
            set_tests_properties(mcpp_extfs_sourceless_ui PROPERTIES TIMEOUT 90)
            add_test(
                NAME mcpp_streamed_views_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_extensions_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
                    /review/cases/mc-extensions/streamed-ctest
                    ExtensionsUI.test_streamed_view_output
                    ExtensionsUI.test_streamed_view_lifetime_and_failure
            )
            set_tests_properties(mcpp_streamed_views_ui PROPERTIES TIMEOUT 90)
            add_test(
                NAME mcpp_editor_clipboard_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_editor_clipboard_commands_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
                    /review/cases/native-clipboard-commands/ctest-ui
            )
            set_tests_properties(mcpp_editor_clipboard_ui PROPERTIES TIMEOUT 40)
            add_test(
                NAME mcpp_hex_cursor_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_hex_cursor_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
                    ${CMAKE_CURRENT_BINARY_DIR}/hex-cursor-ui
            )
            set_tests_properties(mcpp_hex_cursor_ui PROPERTIES TIMEOUT 60)
            add_test(
                NAME mcpp_editor_large_files_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_editor_large_files_ui.py
                    $<TARGET_FILE:mcpp>
                    $<TARGET_FILE:mcppedit>
                    $<TARGET_FILE:mcppdiff>
                    ${MCPP_TMUX_EXECUTABLE}
                    /review/cases/editor-large-files/ui
            )
            set_tests_properties(mcpp_editor_large_files_ui PROPERTIES TIMEOUT 180)
            add_test(
                NAME mcpp_audit_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audit_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
            )
            set_tests_properties(mcpp_audit_ui PROPERTIES TIMEOUT 180)
            add_test(
                NAME mcpp_remaining_audit_ui
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_remaining_audit_ui.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
            )
            set_tests_properties(mcpp_remaining_audit_ui PROPERTIES TIMEOUT 180)
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
            add_test(
                NAME mcpp_editor_save_failures
                COMMAND
                    ${MCPP_PYTHON_EXECUTABLE}
                    -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_editor_save.py
                    $<TARGET_FILE:mcpp>
                    ${MCPP_TMUX_EXECUTABLE}
                    --failures
            )
            set_tests_properties(mcpp_editor_save_failures PROPERTIES TIMEOUT 60)
        endif()
    endif()
endif()

set(CPACK_PACKAGE_NAME "mcpp")
set(CPACK_PACKAGE_VENDOR "mcpp contributors")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "TGZ")
include(CPack)
