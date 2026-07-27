cmake_minimum_required(VERSION 3.20)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)

project(VfxDSinsUpdater)
set(CMAKE_CXX_STANDARD 17)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(bump_rev
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/bump_rev.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Bumping revision..."
    VERBATIM
)

add_library(VfxDSinsUpdater SHARED
    src/addon/entry.cpp
    src/addon/addon.cpp
    src/core/tree/installed_tree_store.cpp
    src/core/tree/installed_tree_overlay.cpp
    src/core/tree/installed_tree_search.cpp
    src/ui/tree/installed_tree_edit.cpp
    src/ui/tree/installed_tree_view.cpp
    src/ui/report_ui.cpp
    src/ui/backups_ui.cpp
    src/ui/live_log_ui.cpp
    src/core/sin_files.cpp
    src/integration/github_update.cpp
    src/core/merge.cpp
    src/integration/webhook_report.cpp
    src/core/backup.cpp
    src/core/live_log.cpp
    src/core/game_state.cpp
    src/core/specialization_names.cpp
    include/imgui.cpp
    include/imgui_draw.cpp
    include/imgui_tables.cpp
    include/imgui_widgets.cpp
)
add_dependencies(VfxDSinsUpdater bump_rev)

target_include_directories(VfxDSinsUpdater PRIVATE
    include
    src
)

target_link_libraries(VfxDSinsUpdater PRIVATE winhttp)

set_target_properties(VfxDSinsUpdater PROPERTIES
    OUTPUT_NAME "VfxDSinsUpdater"
    PREFIX ""
    SUFFIX ".dll"
)

target_compile_options(VfxDSinsUpdater PRIVATE
    -Os
    -ffunction-sections
    -fdata-sections
    -fno-rtti
)

target_link_options(VfxDSinsUpdater PRIVATE
    -static-libgcc
    -static-libstdc++
    -static
    -s
    -Wl,--gc-sections
    -Wl,--exclude-libs,ALL
)
