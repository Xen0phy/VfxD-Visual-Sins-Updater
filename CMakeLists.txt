cmake_minimum_required(VERSION 3.20)

# Compiler/target selection now lives in the toolchain file rather than being
# hardcoded here, so the same CMakeLists.txt builds under either backend:
#   MinGW (default, unchanged, no -DCMAKE_TOOLCHAIN_FILE needed):
#     cmake -B build/mingw
#   MSVC-ABI via clang-cl + lld-link (xwin sysroot):
#     cmake -B build/msvc -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-msvc-clangcl.cmake -DXWIN_SYSROOT=~/.xwin
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_SYSTEM_NAME Windows)
    set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
    set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
endif()

project(VfxDSinsUpdater)
set(CMAKE_CXX_STANDARD 17)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

#_ effect_db.cpp's SQLite backend. Not checked in here -- vendor the
# amalgamation (sqlite3.c + sqlite3.h) from sqlite.org into include/,
# same place nlohmann_json.hpp already lives. Trimmed via the defines
# below to match this addon's existing size-consciousness (-Os,
# --gc-sections): no extension loading, no shared-cache/threading (this
# module is single-threaded by design, see effect_db.cpp), no
# unused-here JSON1/FTS/RTREE extensions built in.
set_source_files_properties(include/sqlite3.c PROPERTIES
    COMPILE_DEFINITIONS "SQLITE_THREADSAFE=0;SQLITE_OMIT_LOAD_EXTENSION;SQLITE_OMIT_SHARED_CACHE;SQLITE_DEFAULT_MEMSTATUS=0"
    COMPILE_OPTIONS "-w" #. silence sqlite3.c's own warnings; the project's -fno-rtti still applies to this
                         #. C file too and is harmless there (GCC just notes it's a C++-only flag)
)

add_custom_target(bump_rev
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/bump_rev.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Bumping revision..."
    VERBATIM
)

add_library(VfxDSinsUpdater SHARED src/addon/addon.cpp
    src/addon/entry.cpp
    src/core/backup.cpp
    src/core/effect_db.cpp
    src/core/game_state.cpp
    src/core/live_log.cpp
    src/core/merge.cpp
    src/core/sin_files.cpp
    src/core/sin_generator.cpp
    src/core/specialization_info.cpp
    src/core/tree/installed_tree_overlay.cpp
    src/core/tree/installed_tree_search.cpp
    src/core/tree/installed_tree_store.cpp
    src/core/tree/db_tree_view.cpp
    src/integration/github_update.cpp
    src/integration/sql_update.cpp
    src/integration/webhook_report.cpp
    src/ui/backups_ui.cpp
    src/ui/live_log_ui.cpp
    src/ui/report_ui.cpp
    src/ui/tree/installed_tree_edit.cpp
    src/ui/tree/installed_tree_view.cpp
    include/imgui.cpp
    include/imgui_draw.cpp
    include/imgui_tables.cpp
    include/imgui_widgets.cpp
    include/sqlite3.c
)
add_dependencies(VfxDSinsUpdater bump_rev)

target_include_directories(VfxDSinsUpdater PRIVATE
    include
    src
    src/addon
    src/core
    src/core/tree
    src/integration
    src/ui
    src/ui/tree
)

target_link_libraries(VfxDSinsUpdater PRIVATE winhttp)

set_target_properties(VfxDSinsUpdater PROPERTIES
    OUTPUT_NAME "VfxDSinsUpdater"
    PREFIX ""
    SUFFIX ".dll"
)

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl / lld-link (MSVC ABI) path.
    # /EHsc: clang-cl disables C++ exceptions by default like cl.exe does;
    #        nlohmann_json and imgui's asserts rely on being able to throw.
    # /GR-:  matches -fno-rtti (RTTI stays off).
    # /MT:   static multithreaded CRT -- the MSVC-world analogue of the
    #        mingw -static-libgcc/-static-libstdc++/-static trio, so the
    #        addon DLL doesn't depend on the user having the right VC++
    #        redistributable installed.
    # /O1:   optimize for size, matching -Os.
    target_compile_options(VfxDSinsUpdater PRIVATE
        /O1
        /GR-
        /EHsc
        /MT
    )
    target_link_options(VfxDSinsUpdater PRIVATE
        /OPT:REF   # dead code/data stripping, ~= --gc-sections
        /OPT:ICF   # identical comdat folding, extra win lld-link gives for free
    )
else()
    # mingw-w64 (GNU ABI) path -- unchanged from before.
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
endif()