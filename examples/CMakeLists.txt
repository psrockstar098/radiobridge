add_executable(example_ble_scan c/ble_scan.c)
target_include_directories(example_ble_scan PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(example_ble_scan PRIVATE radiobridge)

add_executable(example_wifi_status c/wifi_status.c)
target_include_directories(example_wifi_status PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(example_wifi_status PRIVATE radiobridge)
