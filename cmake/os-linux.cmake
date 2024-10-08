find_package(X11 REQUIRED)

target_compile_definitions(obs-browser PRIVATE ENABLE_BROWSER_QT_LOOP)

target_link_libraries(obs-browser PRIVATE CEF::Wrapper CEF::Library X11::X11)
set_target_properties(obs-browser PROPERTIES BUILD_RPATH "$ORIGIN/" INSTALL_RPATH "$ORIGIN/")

add_executable(browser-helper)
add_executable(OBS::browser-helper ALIAS browser-helper)

target_sources(
  browser-helper PRIVATE # cmake-format: sortable
                         browser-app.cpp browser-app.hpp cef-headers.hpp obs-browser-page/obs-browser-page-main.cpp)

target_include_directories(browser-helper PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps"
                                                  "${CMAKE_CURRENT_SOURCE_DIR}/obs-browser-page")

target_link_libraries(browser-helper PRIVATE CEF::Wrapper CEF::Library)

set(OBS_EXECUTABLE_DESTINATION "${OBS_PLUGIN_DESTINATION}")

# cmake-format: off
set_target_properties_obs(
  browser-helper
  PROPERTIES FOLDER plugins/obs-browser
             BUILD_RPATH "$ORIGIN/"
             INSTALL_RPATH "$ORIGIN/"
             PREFIX ""
             OUTPUT_NAME obs-browser-page)
# cmake-format: on
