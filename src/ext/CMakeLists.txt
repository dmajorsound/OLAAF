# olAAF REAPER extension: reaper_olaaf.dll / .dylib / .so

add_library(reaper_olaaf MODULE main.cpp)
target_link_libraries(reaper_olaaf PRIVATE olaaf_core)
# reaper_plugin.h finds "../WDL/swell/swell.h" next to it on mac/linux
target_include_directories(reaper_olaaf PRIVATE ${PROJECT_SOURCE_DIR}/third_party/reaper-sdk/sdk)
set_target_properties(reaper_olaaf PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)

if(NOT WIN32)
  target_compile_definitions(reaper_olaaf PRIVATE SWELL_PROVIDED_BY_APP)
endif()

if(APPLE)
  set_target_properties(reaper_olaaf PROPERTIES SUFFIX ".dylib")
  target_link_options(reaper_olaaf PRIVATE -undefined dynamic_lookup)
elseif(UNIX)
  set_target_properties(reaper_olaaf PROPERTIES SUFFIX ".so")
endif()
