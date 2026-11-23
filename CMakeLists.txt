#
# MonsterMesh for M5Stack CardputerZero — native LVGL app.
#
# Build plumbing (presets, deps, cp0 toolchain, deb packaging) is adapted from
# CardputerZero/Template (MIT, see third_party/CardputerZero-Template-LICENSE).
#
# Game logic is shared read-only with the Pi build in ../src: this file only
# compiles those sources by path; nothing under ../src is modified.
#

cmake_minimum_required(VERSION 3.31)

project(monstermesh VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(USE_DESKTOP ON CACHE BOOL "Build the desktop simulator (SDL) instead of the device binary")
set(APP_NAME "1999online" CACHE STRING "Application name used for installed asset lookup")
set(APP_ASSETS_ROOT "" CACHE PATH "Optional runtime assets root")
set(APP_CONFIG_FILE "" CACHE FILEPATH "Config file; repo file on desktop, /etc/monstermesh.conf on device")
set(APP_KEY_INPUT_DEVICE "" CACHE STRING "Optional evdev keyboard path, e.g. /dev/input/event0 (empty = auto-detect)")
set(APP_FRAMEBUFFER_DEVICE "/dev/fb0" CACHE STRING "Linux framebuffer device")
set(APP_DRM_DEVICE "/dev/dri/card0" CACHE STRING "Linux DRM device used when APP_USE_DRM=ON")
set(APP_DRM_CONNECTOR_ID "-1" CACHE STRING "DRM connector id; -1 auto-selects")
set(APP_USE_DRM OFF CACHE BOOL "Use the LVGL DRM/KMS backend instead of fbdev")
set(CM0_ALLOW_FETCH_DEPS ON CACHE BOOL "Allow downloading missing CM0 BSP/dependencies during cross builds")
option(MM_ODDS_TEST "10x-more-common rare genes for testing (mirrors the Pi/T-Deck flag)" OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile_commands.json" FORCE)

if(NOT APP_CONFIG_FILE)
    if(USE_DESKTOP)
        set(APP_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config/monstermesh.conf")
    else()
        set(APP_CONFIG_FILE "/etc/1999online.conf")
    endif()
endif()

# LVGL + platform libraries (desktop: Homebrew/vcpkg; device: BSP sysroot).
include(cmake/deps.cmake)

set(MM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../pokemesh/monster_mesh_pi" CACHE PATH "MonsterMesh Pi sources (shared game logic, read-only)")

# ====================================================
# Shared MonsterMesh game logic (read-only, from ../src)
# ====================================================
add_library(mm_shared STATIC
    ${MM_ROOT}/src/shared/BreedingApp.cpp
    ${MM_ROOT}/src/battle/Gen1BattleEngine.cpp
    ${MM_ROOT}/src/battle/LordGyms.cpp
    ${MM_ROOT}/src/battle/LordLogic.cpp
    ${MM_ROOT}/src/battle/LordE4.cpp
    ${MM_ROOT}/src/battle/LordSave.cpp
    ${MM_ROOT}/src/terminal/puff.c
)
target_include_directories(mm_shared
    PUBLIC
        ${MM_ROOT}/src/shared
        ${MM_ROOT}/src/battle
        ${MM_ROOT}/src/terminal
        ${PLATFORM_INCLUDE_DIR}
)
if(MM_ODDS_TEST)
    target_compile_definitions(mm_shared PUBLIC MM_ODDS_TEST)
endif()
target_compile_options(mm_shared PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-w>
    $<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang,GNU>:-w>
)

# ====================================================
# Template infrastructure: display/sim, input, logger, theme
# ====================================================
file(GLOB_RECURSE LOGGER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/logger/*.cpp")
add_library(logger STATIC ${LOGGER_SOURCES})
target_include_directories(logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/logger ${PLATFORM_INCLUDE_DIR})
target_link_libraries(logger PUBLIC fmt::fmt PRIVATE ${PLATFORM_LIBS})

# ====================================================
# The app. New files under src/ are picked up automatically.
# ====================================================
file(GLOB_RECURSE MM_APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/view/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/screens/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sprites/*.cpp"
)

# Parallel screen development: -DMM_ONLY_SCREEN=party compiles only
# screens/party_*.cpp (plus the menu) and stubs the other screens with
# placeholders, so one screen's half-finished file can't break another's build.
set(MM_ONLY_SCREEN "" CACHE STRING "Build only these screens, ;-separated (party|daycare|breeding|gym|battle|wifi|mailbox|map|hood|ring|snake|calc|mines|puzzle|savers|beats); empty = all")
if(MM_ONLY_SCREEN)
    set(_mm_all_screens party daycare breeding gym battle wifi mailbox map hood ring snake calc mines puzzle savers beats)
    set(_mm_stub "${CMAKE_CURRENT_BINARY_DIR}/mm_screen_stubs.cpp")
    set(_mm_stub_src "// Generated: MM_ONLY_SCREEN=${MM_ONLY_SCREEN}\n#include \"screens.h\"\nnamespace mm::ui {\n")
    foreach(_s IN LISTS _mm_all_screens)
        if(NOT _s IN_LIST MM_ONLY_SCREEN)
            list(FILTER MM_APP_SOURCES EXCLUDE REGEX "/src/screens/${_s}_[^/]*\\.cpp$")
            string(APPEND _mm_stub_src "std::unique_ptr<Screen> make_${_s}_screen(Ctx& c) { return make_placeholder_screen(c, \"${_s}\", \"(stubbed: MM_ONLY_SCREEN=${MM_ONLY_SCREEN})\"); }\n")
        endif()
    endforeach()
    string(APPEND _mm_stub_src "}\n")
    file(CONFIGURE OUTPUT "${_mm_stub}" CONTENT "${_mm_stub_src}")
    list(APPEND MM_APP_SOURCES "${_mm_stub}")
endif()

add_executable(${PROJECT_NAME} src/main.cpp ${MM_APP_SOURCES})
# On the device this installs next to the original MonsterMesh app, so it has
# its own binary, config, launcher entry and package name.
if(NOT USE_DESKTOP)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "1999online")
endif()

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${LVGL_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/view
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/screens
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backend
        ${CMAKE_CURRENT_SOURCE_DIR}/src/sprites
)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        APP_NAME="${APP_NAME}"
        APP_VERSION="${PROJECT_VERSION}"
        APP_CONFIG_FILE="${APP_CONFIG_FILE}"
        APP_CMAKE_ASSETS_ROOT="${APP_ASSETS_ROOT}"
        APP_SOURCE_ASSETS_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/assets"
        APP_KEY_INPUT_DEVICE="${APP_KEY_INPUT_DEVICE}"
        APP_FRAMEBUFFER_DEVICE="${APP_FRAMEBUFFER_DEVICE}"
        APP_DRM_DEVICE="${APP_DRM_DEVICE}"
        APP_DRM_CONNECTOR_ID=${APP_DRM_CONNECTOR_ID}
)

target_link_libraries(${PROJECT_NAME} PRIVATE lvgl logger mm_shared ${PLATFORM_LIBS})

if(USE_DESKTOP)
    target_link_libraries(${PROJECT_NAME} PRIVATE PNG::PNG)
    if(TARGET SDL2::SDL2)
        target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2)
    else()
        target_include_directories(${PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${SDL2_LIBRARIES})
    endif()
endif()

# Package deb (cp0-cross-package / cpack)
include(cmake/cm0-package.cmake)
