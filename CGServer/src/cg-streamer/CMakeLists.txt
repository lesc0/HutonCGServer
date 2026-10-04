cmake_minimum_required(VERSION 3.19)
project(cg_streamer C CXX)

# 사용: cmake -G Ninja -DCEF_ROOT=/path/to/cef_binary_*_linuxarm64 -DCMAKE_BUILD_TYPE=Release ..
set(CEF_ROOT "" CACHE PATH "CEF binary distribution root")
if(NOT CEF_ROOT)
  message(FATAL_ERROR "-DCEF_ROOT=... 를 지정하세요")
endif()

set(CMAKE_CXX_STANDARD 17)
get_filename_component(CG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)   # 저장소 루트 (inc/, bin/)
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
# CEF 는 CMAKE_HOST_SYSTEM_PROCESSOR == "arm64" 만 검사 → Linux 네이티브("aarch64")는
# x86_64 로 오인해 -m64 -march=x86-64 를 붙임. 여기서 직접 지정.
if(NOT DEFINED PROJECT_ARCH AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(PROJECT_ARCH "arm64")
endif()
find_package(CEF REQUIRED)
# 이 보드의 CEF 배포본에는 Release/ 만 있다. Debug 빌드는 Release 의 libcef.so·런타임 파일을 그대로 쓴다.
if(NOT EXISTS "${CEF_BINARY_DIR_DEBUG}")
  set(CEF_BINARY_DIR_DEBUG ${CEF_BINARY_DIR_RELEASE})
  set(CEF_LIB_DEBUG ${CEF_LIB_RELEASE})
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CEF_BINARY_DIR ${CEF_BINARY_DIR_RELEASE})
  endif()
endif()
add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

include_directories(${CEF_ROOT})

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
find_package(X11 REQUIRED)
if(NOT X11_Xext_FOUND)
  message(FATAL_ERROR "libXext(-dev) 필요 (MIT-SHM, --preview 용)")
endif()

# libcef.so 자체 (wrapper 만으로는 cef_string_* 등 C API 심볼이 없음)
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

add_executable(cg_streamer
  main.cpp
  mpp_encoder.cpp
  ts_muxer.cpp
  audio_mixer.cpp
  video_source.cpp
  hdmirx_source.cpp
  cef_dumper.cpp)
option(ENABLE_ACCEL_PAINT "OnAcceleratedPaint(dmabuf) 실험 경로 (--accel)" OFF)
if(ENABLE_ACCEL_PAINT)
  target_compile_definitions(cg_streamer PRIVATE CG_ACCEL=1)
endif()
option(ENABLE_EXTERNAL_BEGIN_FRAME "SendExternalBeginFrame 으로 CEF 내부 타이머 대신 EncodeLoop 틱이 직접 그리기 요청 (실험)" OFF)
if(ENABLE_EXTERNAL_BEGIN_FRAME)
  target_compile_definitions(cg_streamer PRIVATE CG_EBF=1)
endif()
option(ENABLE_DRM_OUTPUT "--preview 를 X 창 대신 송출 모니터(HDMI-2)에 DRM/KMS 로 직접 출력 (X 의 RandR lease 사용, CG_DRM_OUT)" OFF)
if(ENABLE_DRM_OUTPUT)
  pkg_check_modules(DRM_OUT REQUIRED IMPORTED_TARGET libdrm xcb xcb-randr)
  target_sources(cg_streamer PRIVATE drm_out.cpp)
  target_compile_definitions(cg_streamer PRIVATE CG_DRM_OUT=1)
  target_link_libraries(cg_streamer PkgConfig::DRM_OUT)
endif()
SET_EXECUTABLE_TARGET_PROPERTIES(cg_streamer)
# CEF 가 Release 에 -O2 를 붙이므로(뒤에 오는 쪽이 이김) 우리 코드만 -O3 로 올린다.
target_compile_options(cg_streamer PRIVATE $<$<CONFIG:Release>:-O3>)
add_dependencies(cg_streamer libcef_dll_wrapper)
target_link_libraries(cg_streamer libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS} rockchip_mpp rga PkgConfig::FFMPEG pthread ${CMAKE_DL_LIBS} ${X11_LIBRARIES} ${X11_Xext_LIB} ${X11_Xrandr_LIB})
target_include_directories(cg_streamer PRIVATE ${X11_INCLUDE_DIR} ${CG_ROOT}/inc)

# 실행 파일은 bin/ 에 만든다. CEF 런타임 파일(libcef.so, pak, icudtl 등)도 그 옆으로 복사.
# 웹 리소스(player.html, cg-runtime.js, 프로젝트별 폴더)는 bin/web/ 에 있으므로 복사하지 않는다
# (실행 파일 옆 web/ 을 그대로 사용).
# build.sh 는 CG_OUT_DIR 을 빌드 폴더 안으로 지정해 debug/release 가 bin/ 을 덮어쓰지 않게 하고, install 때 bin/ 으로 복사한다.
set(CG_OUT_DIR ${CG_ROOT}/bin CACHE PATH "cg-streamer 와 CEF 런타임 파일을 만들 폴더")
set(CEF_TARGET_OUT_DIR ${CG_OUT_DIR})
set_target_properties(cg_streamer PROPERTIES OUTPUT_NAME cg-streamer RUNTIME_OUTPUT_DIRECTORY ${CEF_TARGET_OUT_DIR})   # 실행 파일: bin/cg-streamer
COPY_FILES(cg_streamer "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
COPY_FILES(cg_streamer "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")
