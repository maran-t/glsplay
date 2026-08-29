cmake_minimum_required(VERSION 3.24)
project(glsplay_host VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# glsplay host daemon - Windows only.
#
# Captures the desktop with DXGI, encodes on the L4 with NVENC, and ships the
# result over Google libwebrtc. See docs/BUILD-HOST.md for how to obtain the
# two prebuilt dependencies.
# ---------------------------------------------------------------------------

if(NOT WIN32)
  message(FATAL_ERROR "glsplay_host targets Windows only - it depends on DXGI, WASAPI and SendInput.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# libwebrtc is built with the static CRT. Mixing CRTs produces link errors that
# look like missing symbols, so this must match the prebuilt package.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(GLSPLAY_ENABLE_VIGEM "Build the ViGEm gamepad sink (Phase 5)" ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# Google libwebrtc, from the Shiguredo webrtc-build prebuilt package.
# Point WEBRTC_ROOT at the extracted release directory.
set(WEBRTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webrtc" CACHE PATH
    "Root of the extracted Shiguredo webrtc-build package")

if(NOT EXISTS "${WEBRTC_ROOT}/include")
  message(FATAL_ERROR
    "libwebrtc headers not found under ${WEBRTC_ROOT}.\n"
    "Run vm-scripts/fetch-deps.ps1, or set -DWEBRTC_ROOT=<path> to your extracted "
    "Shiguredo webrtc-build release. See docs/BUILD-HOST.md.")
endif()

find_library(WEBRTC_LIBRARY
  NAMES webrtc
  PATHS "${WEBRTC_ROOT}/lib" "${WEBRTC_ROOT}/lib/x64"
  NO_DEFAULT_PATH
  REQUIRED)

add_library(webrtc STATIC IMPORTED GLOBAL)
set_target_properties(webrtc PROPERTIES IMPORTED_LOCATION "${WEBRTC_LIBRARY}")
target_include_directories(webrtc INTERFACE
  "${WEBRTC_ROOT}/include"
  # libwebrtc headers include their vendored deps by these unprefixed paths.
  "${WEBRTC_ROOT}/include/third_party/abseil-cpp"
  "${WEBRTC_ROOT}/include/third_party/boringssl/src/include"
  "${WEBRTC_ROOT}/include/third_party/libyuv/include")
target_compile_definitions(webrtc INTERFACE
  WEBRTC_WIN
  WEBRTC_USE_BUILTIN_OPUS=1
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  # libwebrtc is built without RTTI and without iterator debugging; both must
  # match or the link silently produces a binary that crashes on first use.
  _HAS_ITERATOR_DEBUGGING=0
  _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)

# NVIDIA Video Codec SDK. Headers only - nvEncodeAPI64.dll ships with the
# driver and is loaded at runtime, so there is no import library to link.
set(NVENC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvenc" CACHE PATH
    "Root of the NVIDIA Video Codec SDK (contains Interface/nvEncodeAPI.h)")

find_path(NVENC_INCLUDE_DIR
  NAMES nvEncodeAPI.h
  PATHS "${NVENC_ROOT}/Interface" "${NVENC_ROOT}/include" "${NVENC_ROOT}"
  NO_DEFAULT_PATH
  REQUIRED)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------

add_executable(glsplay-host
  src/main.cpp
  src/config.cpp
  src/util/log.cpp
  src/util/window_herder.cpp
  src/capture/dxgi_duplicator.cpp
  src/capture/d3d11_frame_buffer.cpp
  src/capture/desktop_capture_source.cpp
  src/capture/cursor_compositor.cpp
  src/encode/nvenc_session.cpp
  src/encode/nvenc_video_encoder.cpp
  src/encode/nvenc_encoder_factory.cpp
  src/audio/wasapi_loopback.cpp
  src/audio/loopback_audio_device.cpp
  src/input/input_dispatcher.cpp
  src/input/win32_input_injector.cpp
  # Always compiled: it defines CreateGamepadSink for both the ViGEm and the
  # no-ViGEm case, so omitting it would leave an undefined symbol at link time.
  src/input/vigem_gamepad_sink.cpp
  src/net/signaling_client.cpp
  src/net/peer_session.cpp
  src/net/host_stats_reporter.cpp)

target_include_directories(glsplay-host PRIVATE
  src
  "${NVENC_INCLUDE_DIR}"
  # The binary input protocol header, shared verbatim with the TypeScript side.
  "${CMAKE_CURRENT_SOURCE_DIR}/../../packages/protocol/include")

target_compile_definitions(glsplay-host PRIVATE
  UNICODE
  _UNICODE
  GLSPLAY_VERSION="${PROJECT_VERSION}")

# Whole-program optimisation costs 5-15 minutes of link time against a static
# libwebrtc and buys almost nothing: the hot paths are NVENC on the GPU and
# libwebrtc itself, both already compiled. Off by default so the edit-build
# loop stays usable; turn it on for a final measurement build.
option(GLSPLAY_ENABLE_LTCG "Whole-program optimisation (very slow to link)" OFF)

target_compile_options(glsplay-host PRIVATE
  /W4 /permissive- /Zc:__cplusplus /MP
  # libwebrtc is compiled without RTTI; our translation units must agree.
  /GR-
  $<$<CONFIG:Release>:/O2 /Oi>
  $<$<AND:$<CONFIG:Release>,$<BOOL:${GLSPLAY_ENABLE_LTCG}>>:/GL>)

target_link_options(glsplay-host PRIVATE
  $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
  $<$<AND:$<CONFIG:Release>,$<BOOL:${GLSPLAY_ENABLE_LTCG}>>:/LTCG>)

target_link_libraries(glsplay-host PRIVATE
  webrtc
  # DXGI / D3D11 capture. d3dcompiler builds the cursor blend shader at
  # runtime, which avoids shipping .cso files next to the exe.
  d3d11 dxgi dxguid d3dcompiler
  # WASAPI loopback
  ole32 oleaut32 mmdevapi
  # SendInput, window and monitor queries
  user32 gdi32 shcore dwmapi
  # WebSocket signaling client - WinHTTP has native WebSocket support since
  # Windows 8, so no third-party WebSocket library is needed.
  winhttp
  # libwebrtc transitive system dependencies
  ws2_32 iphlpapi secur32 crypt32 winmm dmoguids wmcodecdspuuid msdmo strmiids
  # Version and process helpers used by the stats reporter
  version advapi32)

if(GLSPLAY_ENABLE_VIGEM)
  set(VIGEM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ViGEmClient" CACHE PATH
      "Root of the ViGEmClient SDK")
  if(EXISTS "${VIGEM_ROOT}/include/ViGEm/Client.h")
    target_include_directories(glsplay-host PRIVATE "${VIGEM_ROOT}/include")
    target_compile_definitions(glsplay-host PRIVATE GLSPLAY_HAVE_VIGEM=1)
    find_library(VIGEM_LIBRARY
      NAMES ViGEmClient
      PATHS "${VIGEM_ROOT}/lib/release/x64" "${VIGEM_ROOT}/lib"
      NO_DEFAULT_PATH)
    if(VIGEM_LIBRARY)
      target_link_libraries(glsplay-host PRIVATE "${VIGEM_LIBRARY}" setupapi)
    else()
      message(FATAL_ERROR "ViGEmClient headers found but the library was not. Set VIGEM_ROOT correctly or configure with -DGLSPLAY_ENABLE_VIGEM=OFF.")
    endif()
  else()
    # Not an error: the host is fully usable for PRD phases 1-4 without a
    # gamepad, and NullGamepadSink covers the interface.
    message(STATUS "ViGEmClient not found - building without gamepad support")
  endif()
endif()

# GPU / encoder utilisation is read from the driver's nvml.dll, loaded at
# runtime in host_stats_reporter.cpp - no SDK header or import library needed.

set_target_properties(glsplay-host PROPERTIES
  OUTPUT_NAME "glsplay-host"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

message(STATUS "")
message(STATUS "glsplay-host ${PROJECT_VERSION}")
message(STATUS "  libwebrtc : ${WEBRTC_LIBRARY}")
message(STATUS "  NVENC     : ${NVENC_INCLUDE_DIR}")
message(STATUS "  ViGEm     : ${GLSPLAY_ENABLE_VIGEM}")
message(STATUS "")
