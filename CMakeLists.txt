cmake_minimum_required(VERSION 3.24)
# C is here only for miniz, which is a C library (D-003/D-014). Sable's own
# code is C++23 throughout — no .c file belongs under engine/ or app/.
project(sable LANGUAGES C CXX VERSION 0.1.0)

# D-001: C++23, no compiler-specific extensions.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(SABLE_BUILD_APP   "Build the SDL3 + ImGui application" ON)
option(SABLE_BUILD_TESTS "Build the engine unit tests"        ON)
option(SABLE_ASAN        "Build with AddressSanitizer"        OFF)

if(SABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(FetchContent)

# ---------------------------------------------------------------- third party
# D-003: everything except SDL3 is a vendored single file or header.
# SOURCE_SUBDIR points at a directory that does not exist, which is the
# supported way to fetch a repo without running its own CMakeLists.

FetchContent_Declare(lodepng
  GIT_REPOSITORY https://github.com/lvandeve/lodepng.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  do-not-configure)
FetchContent_MakeAvailable(lodepng)

add_library(lodepng STATIC ${lodepng_SOURCE_DIR}/lodepng.cpp)
target_include_directories(lodepng SYSTEM PUBLIC ${lodepng_SOURCE_DIR})
set_target_properties(lodepng PROPERTIES POSITION_INDEPENDENT_CODE ON)

# miniz — ZIP container for the .sable project file (D-011).
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG        3.0.2
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  do-not-configure)
FetchContent_MakeAvailable(miniz)

# miniz's sources include a miniz_export.h that its own CMake generates with
# GenerateExportHeader. We skip that CMakeLists (it also builds tests and
# examples), so write the four lines a static build needs instead.
file(WRITE ${CMAKE_BINARY_DIR}/miniz_generated/miniz_export.h
"#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H
#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT
#endif
")

add_library(miniz STATIC
  ${miniz_SOURCE_DIR}/miniz.c
  ${miniz_SOURCE_DIR}/miniz_tdef.c
  ${miniz_SOURCE_DIR}/miniz_tinfl.c
  ${miniz_SOURCE_DIR}/miniz_zip.c)
target_include_directories(miniz SYSTEM PUBLIC
  ${miniz_SOURCE_DIR} ${CMAKE_BINARY_DIR}/miniz_generated)
set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)

# nlohmann/json — the .sable manifest. One header.
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  do-not-configure)
FetchContent_MakeAvailable(nlohmann_json)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE
  ${nlohmann_json_SOURCE_DIR}/single_include)

# --------------------------------------------------------------------- engine
# D-003: links NO SDL, NO ImGui. A stray SDL include here fails to build.
add_library(engine STATIC
  engine/src/canvas.cpp
  engine/src/input.cpp
  engine/src/paint.cpp
  engine/src/io.cpp
  engine/src/project.cpp)
target_include_directories(engine PUBLIC engine/include)
target_link_libraries(engine PRIVATE lodepng miniz nlohmann_json)
target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic)
add_library(sable::engine ALIAS engine)

# ------------------------------------------------------------------------ app
if(SABLE_BUILD_APP)
  find_package(SDL3 REQUIRED CONFIG)

  # D-016 answers D-104: the docking branch, pinned to a COMMIT rather than to
  # the moving branch head. Docking is still not in master as of v1.92.1.
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        b334d19b667958ed970000073644d911fae17e57   # docking branch
    SOURCE_SUBDIR  do-not-configure)
  FetchContent_MakeAvailable(imgui)

  # D-002: imgui_impl_sdl3 for platform, imgui_impl_sdlrenderer3 for rendering.
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
  target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC SDL3::SDL3)

  add_executable(sable app/src/main.cpp app/src/canvas_view.cpp
                       app/src/widgets.cpp app/src/settings.cpp
                       app/src/shortcuts.cpp app/src/icons.cpp)
  target_include_directories(sable PRIVATE app/src)
  target_link_libraries(sable PRIVATE sable::engine imgui SDL3::SDL3)
  target_compile_options(sable PRIVATE -Wall -Wextra -Wpedantic)

  # ------------------------------------------------------------- packaging
  # D-106. A normal FHS install, which is what a distro package needs and what
  # the AppImage script builds on top of — rather than two separate recipes
  # that drift apart.
  include(GNUInstallDirs)
  install(TARGETS sable RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES packaging/sable.desktop
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
  install(FILES packaging/sable.png
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps)
  install(FILES packaging/org.sable.Sable.metainfo.xml
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
  install(FILES packaging/sable-mime.xml
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/mime/packages)
  install(FILES LICENSE README.md
          DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

# ---------------------------------------------------------------------- tests
if(SABLE_BUILD_TESTS)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-configure)
  FetchContent_MakeAvailable(doctest)

  add_library(doctest INTERFACE)
  target_include_directories(doctest SYSTEM INTERFACE ${doctest_SOURCE_DIR})

  # Headless by design (D-003) — these run in CI with no display server.
  add_executable(engine_tests tests/tests.cpp)
  target_link_libraries(engine_tests PRIVATE sable::engine doctest miniz nlohmann_json)
  # The allocation counter (US-02.9) replaces global operator new/delete, which
  # both compilers misread as a malloc/delete mismatch. It is not one.
  target_compile_options(engine_tests PRIVATE
    -Wall -Wextra -Wpedantic -Wno-mismatched-new-delete)

  enable_testing()
  add_test(NAME engine_tests COMMAND engine_tests)
endif()
