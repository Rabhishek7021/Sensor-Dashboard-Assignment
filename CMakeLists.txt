cmake_minimum_required(VERSION 3.16)
project(sensor_dashboard CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SD_REPORT_SENSORS "After building, print the sensors detected on this machine" ON)
set(SD_WITH_MQTT "AUTO" CACHE STRING "MQTT bridge via libmosquitto: AUTO (if found), ON (required) or OFF")

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
# WITHOUT ROWID tables need SQLite >= 3.8.2 (2013); nothing newer is required.
if(SQLite3_VERSION VERSION_LESS 3.8.2)
  message(FATAL_ERROR "SQLite >= 3.8.2 required, found ${SQLite3_VERSION}")
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ---- sd_sensors: hardware discovery (no database, no HTTP) -----------------
# New sensor families are added here: one provider .cpp + one line in
# src/sensors/registry.cpp.
add_library(sd_sensors STATIC
  src/sensors/sysfs.cpp
  src/sensors/hwmon.cpp
  src/sensors/manager.cpp
  src/sensors/registry.cpp
  src/sensors/providers/cpu_temp.cpp
  src/sensors/providers/fan.cpp
  src/sensors/providers/battery.cpp)
target_include_directories(sd_sensors PUBLIC src)
# GCC 8 ships std::filesystem in a separate library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
  target_link_libraries(sd_sensors PUBLIC stdc++fs)
endif()

# ---- sd_common: SQLite store shared by poller and server -------------------
add_library(sd_common STATIC src/common/db.cpp)
target_include_directories(sd_common PUBLIC src)
target_link_libraries(sd_common PUBLIC SQLite::SQLite3 Threads::Threads)

# ---- sd_bus: in-process publish/subscribe bus (MQTT-style topics) -----------
add_library(sd_bus STATIC src/bus/bus.cpp)
target_include_directories(sd_bus PUBLIC src)
target_link_libraries(sd_bus PUBLIC Threads::Threads)

# ---- sd_pipeline: sampler threads, DB writer, DB follower, MQTT bridge -----
add_library(sd_pipeline STATIC
  src/pipeline/sampler.cpp
  src/pipeline/db_writer.cpp
  src/pipeline/source.cpp
  src/pipeline/mqtt_bridge.cpp)
target_link_libraries(sd_pipeline PUBLIC sd_sensors sd_common sd_bus)

# Optional MQTT bridge. libmosquitto is small and packaged everywhere
# (libmosquitto-dev / mosquitto-devel / mosquitto); without it the project
# builds and runs normally, just without --mqtt.
if(NOT SD_WITH_MQTT STREQUAL "OFF")
  find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h)
  find_library(MOSQUITTO_LIBRARY NAMES mosquitto)
  if(MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY)
    target_compile_definitions(sd_pipeline PUBLIC SD_HAVE_MQTT=1)
    target_include_directories(sd_pipeline PRIVATE ${MOSQUITTO_INCLUDE_DIR})
    target_link_libraries(sd_pipeline PUBLIC ${MOSQUITTO_LIBRARY})
    message(STATUS "MQTT bridge: enabled (${MOSQUITTO_LIBRARY})")
  elseif(SD_WITH_MQTT STREQUAL "ON")
    message(FATAL_ERROR "SD_WITH_MQTT=ON but libmosquitto was not found (install libmosquitto-dev)")
  else()
    message(STATUS "MQTT bridge: disabled (libmosquitto not found; install libmosquitto-dev to enable)")
  endif()
endif()

# ---- executables ------------------------------------------------------------
add_executable(sensor-poller src/poller/main.cpp)
target_link_libraries(sensor-poller PRIVATE sd_pipeline)

add_executable(sensor-server src/server/main.cpp src/server/alarm_engine.cpp)
# Vendored single-header deps (cpp-httplib, nlohmann/json); SYSTEM silences their warnings.
target_include_directories(sensor-server SYSTEM PRIVATE third_party)
target_link_libraries(sensor-server PRIVATE sd_pipeline)
# gzip for JSON and the dashboard bundle when zlib is present (it is on
# virtually every Linux system). The live event stream is never compressed.
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_compile_definitions(sensor-server PRIVATE CPPHTTPLIB_ZLIB_SUPPORT)
  target_link_libraries(sensor-server PRIVATE ZLIB::ZLIB)
  message(STATUS "HTTP compression: gzip enabled")
endif()

# Build-time report of what this machine exposes. Informational only: the
# poller always discovers sensors again at run time on whatever machine it
# runs on, so the same binary works with any core/fan/battery count.
if(SD_REPORT_SENSORS AND NOT CMAKE_CROSSCOMPILING)
  add_custom_command(TARGET sensor-poller POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DPOLLER=$<TARGET_FILE:sensor-poller>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportSensors.cmake
    VERBATIM)
endif()

# ---- tests ------------------------------------------------------------------
enable_testing()
add_executable(sd-tests tests/test_main.cpp src/server/alarm_engine.cpp)
target_link_libraries(sd-tests PRIVATE sd_pipeline)
add_test(NAME unit COMMAND sd-tests)   # everything in one go
# One CTest entry per test group, so each feature can be run on its own:
#   ctest --test-dir build -R fan
foreach(group cpu_temp_single_socket cpu_temp_multi_socket cpu_temp_fallbacks fan_provider
              battery_provider registry_and_manager custom_provider store_and_aggregates
              migrate_drops_package migrate_adds_category migrate_adds_op alarm_logic
              alarm_below_threshold alarm_engine_per_core
              bus second_window db_writer sampler follower_and_lock)
  add_test(NAME ${group} COMMAND sd-tests ${group})
endforeach()
