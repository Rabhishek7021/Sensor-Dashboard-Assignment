# Run after sensor-poller is built: prints the sensors detected on the build
# machine using the poller's own discovery code. Never fails the build (a
# container or CI runner may have no sensors at all).
#
#   cmake -DPOLLER=<path to sensor-poller> -P ReportSensors.cmake

execute_process(
  COMMAND "${POLLER}" --list --providers all
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  OUTPUT_STRIP_TRAILING_WHITESPACE)

if(rc EQUAL 0)
  string(REGEX MATCHALL "\n[^\n]+ temperature +core +" core_lines "\n${out}")
  list(LENGTH core_lines n_cores)
  message(STATUS "Sensors detected on this machine (${n_cores} CPU core temperature sensor(s)):\n${out}")
else()
  message(STATUS "No sensors detected on this build machine. The poller discovers sensors at run time, "
                 "so this only matters if you run it here.")
endif()
