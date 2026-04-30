cmake_minimum_required(VERSION 3.20)
project(psst LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ──────────────────────────────────────────────────────────
option(GGML_CUDA "Enable CUDA backend for whisper.cpp" ON)

# ── System dependencies ──────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple)
pkg_check_modules(X11 IMPORTED_TARGET x11)

# libxdo has no pkg-config file
find_library(XDO_LIBRARY xdo)
find_path(XDO_INCLUDE_DIR xdo.h)
if(XDO_LIBRARY AND XDO_INCLUDE_DIR)
    set(XDO_FOUND TRUE)
endif()

# ── FetchContent: whisper.cpp ────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(whisper)

# ── FetchContent: toml++ ─────────────────────────────────────────────
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

# ── Application ──────────────────────────────────────────────────────
add_executable(psst
    src/main.cpp
    src/config.cpp
    src/audio.cpp
    src/hotkey.cpp
    src/overlay.cpp
    src/transcribe.cpp
    src/inject.cpp
)

target_include_directories(psst PRIVATE src)

target_link_libraries(psst PRIVATE
    whisper
    tomlplusplus::tomlplusplus
    PkgConfig::GTK3
    PkgConfig::PULSE
    pthread
)

# Optional X11 support (for global hotkeys on X11)
if(X11_FOUND)
    target_link_libraries(psst PRIVATE PkgConfig::X11)
    target_compile_definitions(psst PRIVATE HAS_X11=1)
endif()

# Optional libxdo support (for text injection on X11)
if(XDO_FOUND)
    target_link_libraries(psst PRIVATE ${XDO_LIBRARY})
    target_include_directories(psst PRIVATE ${XDO_INCLUDE_DIR})
    target_compile_definitions(psst PRIVATE HAS_XDO=1)
endif()



# ── Install ──────────────────────────────────────────────────────────
install(TARGETS psst RUNTIME DESTINATION bin)
install(FILES config.toml DESTINATION etc/psst)

