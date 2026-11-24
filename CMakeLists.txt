cmake_minimum_required(VERSION 3.18)
project(olAAF VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "")
endif()

option(OLAAF_BUILD_EXTENSION "Build the REAPER extension" ON)
option(OLAAF_BUILD_CLI "Build the olaaf-cli test tool" ON)

# ---- libAAF (GPL v2+), built statically into olAAF
file(GLOB LIBAAF_SRC
  third_party/libaaf/src/LibCFB/*.c
  third_party/libaaf/src/AAFCore/*.c
  third_party/libaaf/src/AAFIface/*.c
  third_party/libaaf/src/common/*.c)
add_library(aaf STATIC ${LIBAAF_SRC})
target_include_directories(aaf PUBLIC third_party/libaaf/include PRIVATE third_party/libaaf/src)
set_target_properties(aaf PROPERTIES C_STANDARD 99)
if(MSVC)
  target_compile_definitions(aaf PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(aaf PRIVATE -w)
  if(NOT APPLE AND NOT WIN32)
    target_compile_definitions(aaf PRIVATE _XOPEN_SOURCE=500 _DEFAULT_SOURCE)
  endif()
endif()

# ---- olAAF core
add_library(olaaf_core STATIC
  src/core/aaf_reader.cpp
  src/core/audiofile.cpp
  src/core/importer.cpp
  src/core/peaks.cpp
  src/core/report.cpp
  src/core/track_template.cpp
  src/core/choices.cpp
  src/core/util.cpp)
find_package(Threads REQUIRED)
target_link_libraries(olaaf_core PUBLIC aaf Threads::Threads)
if(WIN32)
  target_compile_definitions(olaaf_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
  target_compile_options(olaaf_core PRIVATE /utf-8)
else()
  target_compile_options(olaaf_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(OLAAF_BUILD_CLI)
  add_executable(olaaf-cli src/cli/main.cpp)
  target_link_libraries(olaaf-cli PRIVATE olaaf_core)
endif()

if(OLAAF_BUILD_EXTENSION)
  add_subdirectory(src/ext)
endif()
