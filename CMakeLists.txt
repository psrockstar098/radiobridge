cmake_minimum_required(VERSION 3.16)
project(radiobridge VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(RB_BUILD_TESTS "Build the CTest suite" ON)
option(RB_BUILD_EXAMPLES "Build the C examples" ON)
option(RB_BUILD_SHARED "Build a shared library instead of static" OFF)

find_package(Threads REQUIRED)

# D-Bus transport (Linux backend, Phase 2). Found via pkg-config;
# when absent the Linux backend keeps its honest UNAVAILABLE behavior
# and the bus test is not built. CI must apt-install libdbus-1-dev
# on the ubuntu job for the bus test to run (see docs/ci-workflow.yml
# note in the final report — the workflow itself is never edited via
# git).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT WIN32 AND NOT APPLE)
  pkg_check_modules(DBUS dbus-1)
endif()

if(RB_BUILD_SHARED)
  add_library(radiobridge SHARED
    src/core/rb_core.c
    src/core/rb_thread.c
    src/bluetooth/rb_mock_ble.c
    src/wifi/rb_mock_wifi.c
    src/platform/linux/rb_linux.c
    src/platform/stubs/rb_stubs.c
    src/app/rb_app.c)
else()
  add_library(radiobridge STATIC
    src/core/rb_core.c
    src/core/rb_thread.c
    src/bluetooth/rb_mock_ble.c
    src/wifi/rb_mock_wifi.c
    src/platform/linux/rb_linux.c
    src/platform/stubs/rb_stubs.c
    src/app/rb_app.c)
endif()

target_include_directories(radiobridge
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(radiobridge PRIVATE Threads::Threads)

# OpenSSL (Phase 5 secure channels): X25519 + HKDF-SHA256 +
# AES-256-GCM via EVP. When absent, the secure functions return
# RB_ERROR_NOT_SUPPORTED — never hand-rolled crypto claimed secure.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  target_compile_definitions(radiobridge PUBLIC RB_HAVE_OPENSSL=1)
  target_link_libraries(radiobridge PRIVATE OpenSSL::Crypto)
  message(STATUS "OpenSSL enabled (secure channels available)")
else()
  message(STATUS "OpenSSL not found (secure channels NOT_SUPPORTED)")
endif()
if(DBUS_FOUND)
  target_sources(radiobridge PRIVATE src/platform/linux/rb_dbus.c)
  target_compile_definitions(radiobridge PUBLIC RB_HAVE_DBUS=1)
  target_include_directories(radiobridge PRIVATE ${DBUS_INCLUDE_DIRS})
  target_link_libraries(radiobridge PRIVATE ${DBUS_LIBRARIES})
  message(STATUS "D-Bus transport enabled (libdbus ${DBUS_VERSION})")
else()
  message(STATUS "D-Bus transport disabled (libdbus not found)")
endif()

# Windows backend (Phase 4): C++/WinRT behind a C11 shim. Windows-only;
# the files carry #ifndef _WIN32 #error guards and are never compiled
# elsewhere. COMPILE-UNVERIFIED: written from documented WinRT APIs,
# never compiled (no Windows SDK on the authoring machine). Requires
# the Windows SDK + a Bluetooth-capable Windows device to verify.
if(WIN32)
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  target_sources(radiobridge PRIVATE
    src/platform/windows/rb_windows.c
    src/platform/windows/rb_windows.cpp
    src/platform/windows/rb_windows_wifi.cpp)
  target_link_libraries(radiobridge PRIVATE windowsapp)
endif()
# MSVC rejects GCC/Clang warning flags outright (D8021 on -Wextra),
# so pick warning flags per compiler instead of unconditionally.
if(MSVC)
  target_compile_options(radiobridge PRIVATE /W4)
else()
  target_compile_options(radiobridge PRIVATE -Wall -Wextra)
endif()

# Tests link the same library; they also include internal headers.
if(RB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RB_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# Android JNI bridge + JVM smoke test (Phase 3). Builds only when a
# JDK is present (javac + java + JNI headers); otherwise it is
# skipped, never failed. The bridge compiles against a plain JDK
# (no Android SDK) and the smoke test drives the mock backend through
# real JNI — the compiled + run proof for the bridge. Android
# Framework APIs still need a device (see bindings/android/README.md).
option(RB_BUILD_JNI "Build the JNI bridge + JVM smoke test" ON)
if(RB_BUILD_JNI AND NOT WIN32 AND NOT APPLE)
  find_package(Java QUIET)
  # FindJNI is deliberately NOT used: it requires AWT headers
  # (jawt.h), which headless JDKs do not ship, and the bridge never
  # touches AWT. Locate jni.h directly from the java executable's
  # resolved JVM home (plus $JAVA_HOME as a hint).
  set(RB_JNI_INCLUDE_DIRS "")
  if(Java_JAVA_EXECUTABLE)
    get_filename_component(_rb_java_real "${Java_JAVA_EXECUTABLE}"
                           REALPATH)
    get_filename_component(_rb_jvm_bin "${_rb_java_real}" DIRECTORY)
    get_filename_component(_rb_jvm_home "${_rb_jvm_bin}" DIRECTORY)
    find_path(RB_JNI_H_PATH NAMES jni.h
      HINTS "${_rb_jvm_home}/include" "$ENV{JAVA_HOME}/include"
      NO_DEFAULT_PATH)
    find_path(RB_JNI_MD_PATH NAMES jni_md.h
      HINTS "${_rb_jvm_home}/include/linux"
            "$ENV{JAVA_HOME}/include/linux"
      NO_DEFAULT_PATH)
    if(RB_JNI_H_PATH AND RB_JNI_MD_PATH)
      set(RB_JNI_INCLUDE_DIRS ${RB_JNI_H_PATH} ${RB_JNI_MD_PATH})
    endif()
  endif()
  if(RB_JNI_INCLUDE_DIRS AND Java_JAVAC_EXECUTABLE AND
     Java_JAVA_EXECUTABLE)
    add_library(radiobridge_jni SHARED
      src/core/rb_core.c
      src/core/rb_thread.c
      src/bluetooth/rb_mock_ble.c
      src/wifi/rb_mock_wifi.c
      src/platform/linux/rb_linux.c
      src/platform/stubs/rb_stubs.c
      bindings/android/jni/rb_jni.c)
    target_include_directories(radiobridge_jni PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${RB_JNI_INCLUDE_DIRS})
    target_link_libraries(radiobridge_jni PRIVATE Threads::Threads)
    if(DBUS_FOUND)
      target_sources(radiobridge_jni PRIVATE
        src/platform/linux/rb_dbus.c)
      target_compile_definitions(radiobridge_jni PRIVATE RB_HAVE_DBUS=1)
      target_include_directories(radiobridge_jni PRIVATE
        ${DBUS_INCLUDE_DIRS})
      target_link_libraries(radiobridge_jni PRIVATE ${DBUS_LIBRARIES})
    endif()
    if(NOT MSVC)
      target_compile_options(radiobridge_jni PRIVATE -Wall -Wextra)
    endif()
    set(RB_JNI_CLASSES ${CMAKE_CURRENT_BINARY_DIR}/jni-classes)
    set(RB_JNI_JAVA
      ${CMAKE_CURRENT_SOURCE_DIR}/bindings/android/java/com/radiobridge/RadioBridge.java
      ${CMAKE_CURRENT_SOURCE_DIR}/bindings/android/java/com/radiobridge/TestSmoke.java)
    add_custom_command(
      OUTPUT ${RB_JNI_CLASSES}/.stamp
      COMMAND ${CMAKE_COMMAND} -E make_directory ${RB_JNI_CLASSES}
      COMMAND ${Java_JAVAC_EXECUTABLE} -d ${RB_JNI_CLASSES}
              ${RB_JNI_JAVA}
      COMMAND ${CMAKE_COMMAND} -E touch ${RB_JNI_CLASSES}/.stamp
      DEPENDS ${RB_JNI_JAVA} radiobridge_jni
      COMMENT "Compiling RadioBridge Java wrapper + smoke test")
    add_custom_target(rb_jni_java ALL DEPENDS ${RB_JNI_CLASSES}/.stamp)
    if(RB_BUILD_TESTS)
      add_test(NAME test_jni_smoke
        COMMAND ${Java_JAVA_EXECUTABLE}
                -Djava.library.path=$<TARGET_FILE_DIR:radiobridge_jni>
                -cp ${RB_JNI_CLASSES} com.radiobridge.TestSmoke)
      set_tests_properties(test_jni_smoke PROPERTIES TIMEOUT 60)
    endif()
    message(STATUS "JNI bridge enabled (JDK ${Java_VERSION})")
  else()
    message(STATUS "JNI bridge skipped (no JDK/JNI found)")
  endif()
endif()
