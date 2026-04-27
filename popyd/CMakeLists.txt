# popyd / popy / popy-render
#
# Three binaries sharing a popy_core static library.
#
#   popy_core   — paths, naming, sidecar, hash, uuid, mime, config, log, safe_fs
#   popy        — CLI: fetch, list, release, delete, read, status, pause/resume
#   popyd       — daemon: watcher loop + AF_UNIX status socket
#   popy-render — forked render child (sandboxed); built only if libmupdf is found
#
# Layout, dependencies, and build rules follow plan.md.

cmake_minimum_required(VERSION 3.20)
project(popy
  VERSION      0.1.0
  DESCRIPTION  "Download quarantine daemon and CLI for AI agents"
  LANGUAGES    CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- Warnings & hardening ----------------------------------------------------
# Tight by default. Debug builds add ASan + UBSan.
add_library(popy_warnings INTERFACE)
target_compile_options(popy_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
  -Wconversion -Wsign-conversion -Wold-style-cast
  -Wcast-align -Wunused -Woverloaded-virtual
  -fstack-protector-strong)

option(POPY_WERROR "Treat warnings as errors" OFF)
if(POPY_WERROR)
  target_compile_options(popy_warnings INTERFACE -Werror)
endif()

# _FORTIFY_SOURCE only meaningful with optimisation.
target_compile_definitions(popy_warnings INTERFACE
  $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)

# ASan/UBSan: opt-in via -DPOPY_SANITIZERS=ON. Not auto-enabled for Debug
# because Apple Clang 16 on macOS 26 has a malloc-interceptor init race that
# crashes every ASan-instrumented binary (sanitizer_malloc_mac.inc:189).
# Use it on Linux and older macOS for catching memory bugs.
option(POPY_SANITIZERS "Enable ASan + UBSan" OFF)
if(POPY_SANITIZERS)
  target_compile_options(popy_warnings INTERFACE -fsanitize=address,undefined)
  target_link_options   (popy_warnings INTERFACE -fsanitize=address,undefined)
endif()

# --- Vendored third-party headers -------------------------------------------
add_library(popy_thirdparty INTERFACE)
target_include_directories(popy_thirdparty INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
# stb_image_write uses the deprecated sprintf() in one HDR-export branch we
# never call; silence the diagnostic where stb headers are compiled in.
set(POPY_STB_FLAGS -Wno-deprecated-declarations)

# --- popy_core static library -----------------------------------------------
add_library(popy_core STATIC
  src/core/paths.cpp
  src/core/naming.cpp
  src/core/uuid.cpp
  src/core/log.cpp
  src/core/hash.cpp
  src/core/mime.cpp
  src/core/sidecar.cpp
  src/core/config.cpp
  src/core/safe_fs.cpp
  src/store/quarantine.cpp
  src/net/fetch.cpp
  src/ipc/status.cpp)

target_include_directories(popy_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(popy_core PUBLIC popy_warnings popy_thirdparty)

# nlohmann/json + toml++ are header-only but their implementation is gated by
# macros. Compile each into exactly one .cpp inside popy_core.
target_compile_definitions(popy_core PRIVATE TOML_IMPLEMENTATION)

# picosha2.h triggers -Wsign-conversion in its STL iterator arithmetic. The
# vendored single-header is unmodified upstream; suppress the warning only for
# the one TU that includes it.
set_source_files_properties(src/core/hash.cpp PROPERTIES
  COMPILE_OPTIONS "-Wno-sign-conversion;-Wno-conversion")

# libcurl for popy fetch
find_package(CURL REQUIRED)
target_link_libraries(popy_core PUBLIC CURL::libcurl)

# --- popy CLI ---------------------------------------------------------------
add_executable(popy
  src/main_popy.cpp
  src/cli/commands.cpp)
target_link_libraries(popy PRIVATE popy_core)

# --- popyd daemon -----------------------------------------------------------
# Picks the platform's filesystem watcher: FSEvents on macOS, inotify on Linux.
set(POPYD_WATCHER_SRC)
if(APPLE)
  list(APPEND POPYD_WATCHER_SRC src/watch/watcher_fsevents.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND POPYD_WATCHER_SRC src/watch/watcher_inotify.cpp)
else()
  message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

add_executable(popyd
  src/main_popyd.cpp
  ${POPYD_WATCHER_SRC})
target_link_libraries(popyd PRIVATE popy_core)
if(APPLE)
  target_link_libraries(popyd PRIVATE
    "-framework CoreServices" "-framework CoreFoundation")
endif()

# --- popy-render (sandboxed child for libmupdf / stb_image) -----------------
# Built only if libmupdf is available. Without it, `popy read --mode png|text`
# for PDF/image returns unsupported_type at runtime — text/raw still work.
find_path(MUPDF_INCLUDE_DIR mupdf/fitz.h
  HINTS /opt/homebrew/include /usr/local/include /usr/include)
find_library(MUPDF_LIB NAMES mupdf
  HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)

if(MUPDF_INCLUDE_DIR AND MUPDF_LIB)
  add_executable(popy-render
    src/main_popy_render.cpp
    src/render/sandbox.cpp
    src/render/pdf.cpp
    src/render/image.cpp)
  target_include_directories(popy-render PRIVATE ${MUPDF_INCLUDE_DIR})
  target_link_libraries(popy-render PRIVATE popy_core ${MUPDF_LIB})
  target_compile_options (popy-render PRIVATE ${POPY_STB_FLAGS})
  target_compile_definitions(popy-render PRIVATE
    STB_IMAGE_IMPLEMENTATION
    STB_IMAGE_WRITE_IMPLEMENTATION)
  set(POPY_HAS_RENDER ON)
else()
  message(STATUS "libmupdf not found — popy-render will not be built. "
                 "`popy read --mode text|png` for PDF/image will return "
                 "unsupported_type. Install via `brew install mupdf` "
                 "or `apt install libmupdf-dev` and reconfigure.")
  set(POPY_HAS_RENDER OFF)
endif()

# --- Tests (ctest) ----------------------------------------------------------
option(POPY_BUILD_TESTS "Build the test suite" ON)
if(POPY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# --- Install ----------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS popy popyd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(POPY_HAS_RENDER)
  install(TARGETS popy-render RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY dist/  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/popy/dist)
install(DIRECTORY mcp/   DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/popy/mcp)

# Indexer-exclusion installer step. Idempotent. Skipped when DESTDIR is set
# (i.e. packaging into a staging root, not installing onto the live system).
install(CODE "
  if(DEFINED ENV{DESTDIR} AND NOT \"\$ENV{DESTDIR}\" STREQUAL \"\")
    message(STATUS \"DESTDIR set; skipping indexer-exclusion step\")
  else()
    set(stage \"\$ENV{HOME}/.popy-stage\")
    file(MAKE_DIRECTORY \"\${stage}\")
    if(APPLE)
      execute_process(COMMAND mdutil -i off \"\${stage}\"
        RESULT_VARIABLE _r OUTPUT_QUIET ERROR_QUIET)
      file(TOUCH \"\${stage}/.metadata_never_index\")
    else()
      file(TOUCH \"\${stage}/.trackerignore\")
    endif()
    message(STATUS \"popy: indexer exclusion applied to \${stage}\")
  endif()")

message(STATUS "popy ${PROJECT_VERSION}")
message(STATUS "  build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  sanitizers:  ${POPY_SANITIZERS}")
message(STATUS "  werror:      ${POPY_WERROR}")
message(STATUS "  render:      ${POPY_HAS_RENDER}")
