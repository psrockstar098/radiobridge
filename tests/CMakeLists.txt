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
rb_add_test(test_app)

# Fake-bus test: Linux + libdbus only. The test binary starts its own
# dbus-daemon and fake BlueZ/NetworkManager service, so it needs no
# hardware and no system bus. Skipped (not built) elsewhere.
if(DBUS_FOUND AND NOT WIN32 AND NOT APPLE)
  add_executable(rb_fake_bus fake_bus/fake_bus.c)
  target_include_directories(rb_fake_bus PRIVATE ${DBUS_INCLUDE_DIRS})
  target_link_libraries(rb_fake_bus PRIVATE ${DBUS_LIBRARIES}
                        Threads::Threads)
  if(NOT MSVC)
    target_compile_options(rb_fake_bus PRIVATE -Wall -Wextra)
  endif()
  rb_add_test(test_linux_bus)
  target_compile_definitions(test_linux_bus PRIVATE
    RB_FAKE_BUS_BIN="$<TARGET_FILE:rb_fake_bus>")
  target_include_directories(test_linux_bus PRIVATE ${DBUS_INCLUDE_DIRS})
  target_link_libraries(test_linux_bus PRIVATE ${DBUS_LIBRARIES})
  add_dependencies(test_linux_bus rb_fake_bus)
endif()

# Cross-platform conformance (Phase 5): same scripted sequence
# against mock + (when a fake bus exists) Linux. The runner script
# skips the Linux leg when no fake-bus binary is available. This
# block must stay after the fake-bus block above: it depends on the
# rb_fake_bus target existing first.
add_executable(test_conformance test_conformance.c)
target_include_directories(test_conformance PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(test_conformance PRIVATE
  _POSIX_C_SOURCE=200809L)
target_link_libraries(test_conformance PRIVATE radiobridge)
if(NOT MSVC)
  target_compile_options(test_conformance PRIVATE -Wall -Wextra)
endif()
if(DBUS_FOUND AND NOT WIN32 AND NOT APPLE)
  add_test(NAME test_conformance
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_conformance.sh
            $<TARGET_FILE:test_conformance>
            $<TARGET_FILE:rb_fake_bus>)
  add_dependencies(test_conformance rb_fake_bus)
else()
  add_test(NAME test_conformance
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_conformance.sh
            $<TARGET_FILE:test_conformance>)
endif()
set_tests_properties(test_conformance PROPERTIES TIMEOUT 120)
