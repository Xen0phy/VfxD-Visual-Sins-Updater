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
    src/entry.cpp
    src/addon.cpp
    src/sin_files.cpp
    src/github_update.cpp
    src/merge.cpp
    src/report.cpp
    src/backup.cpp
    src/live_log.cpp
    src/game_state.cpp
    src/specialization_names.cpp
    include/imgui.cpp
    include/imgui_draw.cpp
    include/imgui_tables.cpp
    include/imgui_widgets.cpp
)
add_dependencies(VfxDSinsUpdater bump_rev)

target_include_directories(VfxDSinsUpdater PRIVATE
    include
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
