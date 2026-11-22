# olAAF REAPER extension: reaper_olaaf.dll / .dylib / .so

set(SDK ${PROJECT_SOURCE_DIR}/third_party/reaper-sdk/sdk)
set(WDL ${PROJECT_SOURCE_DIR}/third_party/WDL/WDL)

add_library(reaper_olaaf MODULE main.cpp)
target_link_libraries(reaper_olaaf PRIVATE olaaf_core)
target_include_directories(reaper_olaaf PRIVATE ${SDK})
set_target_properties(reaper_olaaf PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)

if(WIN32)
  # reaper_plugin.h includes <windows.h> on Windows
  target_compile_definitions(reaper_olaaf PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  # reaper_plugin.h includes "../WDL/swell/swell.h" relative to itself
  set(SDK_LINK ${CMAKE_BINARY_DIR}/reaper-sdk)
  file(MAKE_DIRECTORY ${SDK_LINK})
  # copy the SDK headers (a symlink would be resolved back to the original folder)
  file(COPY ${SDK}/reaper_plugin.h ${SDK}/reaper_plugin_functions.h DESTINATION ${SDK_LINK}/sdk)
  file(CREATE_LINK ${WDL} ${SDK_LINK}/WDL SYMBOLIC)
  target_include_directories(reaper_olaaf BEFORE PRIVATE ${SDK_LINK}/sdk)
  target_compile_definitions(reaper_olaaf PRIVATE SWELL_PROVIDED_BY_APP)
endif()

if(APPLE)
  set_target_properties(reaper_olaaf PROPERTIES SUFFIX ".dylib")
  target_link_options(reaper_olaaf PRIVATE -undefined dynamic_lookup)
elseif(UNIX)
  set_target_properties(reaper_olaaf PROPERTIES SUFFIX ".so")
endif()
