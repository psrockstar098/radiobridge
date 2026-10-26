# Each test is its own executable + CTest entry. Tests include the
# internal header (for the Linux probe), so src/ is on their path.
function(rb_add_test name)
  add_executable(${name} ${name}.c)
  target_include_directories(${name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
  target_compile_definitions(${name} PRIVATE _POSIX_C_SOURCE=200809L)
  target_link_libraries(${name} PRIVATE radiobridge)
  add_test(NAME ${name} COMMAND ${name})
  # Async tests need real time; 60s is generous on a loaded VM.
  set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

rb_add_test(test_lifecycle)
rb_add_test(test_capabilities)
rb_add_test(test_ble_scan)
rb_add_test(test_ble_gatt)
rb_add_test(test_wifi)
rb_add_test(test_cancel)
rb_add_test(test_errors)
rb_add_test(test_threading)
rb_add_test(test_logging)
rb_add_test(test_permissions)
rb_add_test(test_linux_backend)
rb_add_test(test_mock_hooks)
