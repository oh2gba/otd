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

# manual tool, not a test: connects to a KiwiSDR and reports what arrives
add_executable(kiwi_probe kiwi_probe.cpp)
target_link_libraries(kiwi_probe PRIVATE otd_core Qt6::Multimedia)
