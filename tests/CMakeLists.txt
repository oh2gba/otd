find_package(Qt6 REQUIRED COMPONENTS Test)

function(otd_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE otd_core Qt6::Test)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

otd_test(test_eibi)
otd_test(test_schedule)
otd_test(test_stationdb)
otd_test(test_hfcc)
otd_test(test_aoki)
otd_test(test_zip)
otd_test(test_userlist)
otd_test(test_bandplan)
otd_test(test_updatecheck)
otd_test(test_kiwi)
otd_test(test_traficom)
otd_test(test_rigclient)

# The app's own classes (settings, KiwiSDR player, main window) come from
# the otd_app object library, so these tests build what otd itself runs.
function(otd_app_test name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
  target_link_libraries(${name} PRIVATE otd_app Qt6::Test)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

otd_app_test(test_settings)     # AppSettings in the database, the Settings dialog
otd_app_test(test_kiwiplayer)   # the player against a fake KiwiSDR on localhost
otd_app_test(test_mainwindow)   # the real main window against a fake rigctld
otd_app_test(test_stationmodel)  # what the station list shows for an entry
otd_app_test(test_kiwisound)     # the player through a real (silent) sound server, PulseAudio

# manual tool, not a test: connects to a KiwiSDR and reports what arrives
add_executable(kiwi_probe kiwi_probe.cpp)
target_link_libraries(kiwi_probe PRIVATE otd_core Qt6::Multimedia)
