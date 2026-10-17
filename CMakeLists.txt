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
option(MCPP_ENABLE_SSH "Enable the libssh2 provider" OFF)

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
            $<$<BOOL:${MCPP_WARNINGS_AS_ERRORS}>:-Werror>
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
    src/local_provider.cpp
    src/operations.cpp
    src/pane.cpp
    src/viewer.cpp
    src/editor.cpp
)
target_include_directories(mcpp_core PUBLIC include)
target_link_libraries(mcpp_core PUBLIC Threads::Threads)
target_compile_definitions(mcpp_core PUBLIC MCPP_VERSION="${PROJECT_VERSION}")
mcpp_target_defaults(mcpp_core)

add_library(mcpp_ui src/app.cpp)
target_include_directories(mcpp_ui PUBLIC include)
target_link_libraries(mcpp_ui PUBLIC mcpp_core ftxui::ftxui)
mcpp_target_defaults(mcpp_ui)

add_executable(mcpp src/main.cpp)
target_link_libraries(mcpp PRIVATE mcpp_ui)
mcpp_target_defaults(mcpp)

install(TARGETS mcpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(MCPP_BUILD_TESTS)
    enable_testing()
    add_executable(mcpp_tests tests/test_main.cpp)
    target_link_libraries(mcpp_tests PRIVATE mcpp_core)
    mcpp_target_defaults(mcpp_tests)
    add_test(NAME mcpp_tests COMMAND mcpp_tests)
endif()

set(CPACK_PACKAGE_NAME "mcpp")
set(CPACK_PACKAGE_VENDOR "mcpp contributors")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "TGZ")
include(CPack)
