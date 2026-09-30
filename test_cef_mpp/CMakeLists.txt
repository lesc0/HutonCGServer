cmake_minimum_required(VERSION 3.19)
project(cef_mpp C CXX)

# 사용: cmake -G Ninja -DCEF_ROOT=/path/to/cef_binary_*_linuxarm64 -DCMAKE_BUILD_TYPE=Release ..
set(CEF_ROOT "" CACHE PATH "CEF binary distribution root")
if(NOT CEF_ROOT)
  message(FATAL_ERROR "-DCEF_ROOT=... 를 지정하세요")
endif()

set(CMAKE_CXX_STANDARD 17)
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
# CEF 는 CMAKE_HOST_SYSTEM_PROCESSOR == "arm64" 만 검사 → Linux 네이티브("aarch64")는
# x86_64 로 오인해 -m64 -march=x86-64 를 붙임. 여기서 직접 지정.
if(NOT DEFINED PROJECT_ARCH AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(PROJECT_ARCH "arm64")
endif()
find_package(CEF REQUIRED)
add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

include_directories(${CEF_ROOT})

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
find_package(X11 REQUIRED)
if(NOT X11_Xext_FOUND)
  message(FATAL_ERROR "libXext(-dev) 필요 (MIT-SHM, --preview 용)")
endif()

# libcef.so 자체 (wrapper 만으로는 cef_string_* 등 C API 심볼이 없음)
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

add_executable(cef_mpp main.cpp mpp_encoder.cpp ts_muxer.cpp video_source.cpp hdmirx_source.cpp)
option(ENABLE_ACCEL_PAINT "OnAcceleratedPaint(dmabuf) 실험 경로 (--accel)" OFF)
if(ENABLE_ACCEL_PAINT)
  target_compile_definitions(cef_mpp PRIVATE CG_ACCEL=1)
endif()
SET_EXECUTABLE_TARGET_PROPERTIES(cef_mpp)
add_dependencies(cef_mpp libcef_dll_wrapper)
target_link_libraries(cef_mpp libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS} rockchip_mpp rga PkgConfig::FFMPEG pthread ${X11_LIBRARIES} ${X11_Xext_LIB})
target_include_directories(cef_mpp PRIVATE ${X11_INCLUDE_DIR})

# CEF 런타임 파일(libcef.so, pak, icudtl 등)을 실행 파일 옆으로 복사
set(CEF_TARGET_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIGURATION>)
set_target_properties(cef_mpp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CEF_TARGET_OUT_DIR})
COPY_FILES(cef_mpp "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
COPY_FILES(cef_mpp "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")

# 저작/플레이어 웹 리소스(GrapesJS 포함)를 실행 파일 옆 web/ 으로 복사
add_custom_command(TARGET cef_mpp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/web ${CEF_TARGET_OUT_DIR}/web)
