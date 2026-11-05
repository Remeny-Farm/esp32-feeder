cmake_minimum_required(VERSION 3.16)
project(feeder_host_tests C)

# Host-only tests for the pure-C parts of the firmware (no ESP-IDF).
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Werror -std=c99)

set(COMPONENTS ${CMAKE_CURRENT_LIST_DIR}/../../components)

# Pure-C component libraries. A component that has no sources yet is skipped.
file(GLOB GATE_SRC ${COMPONENTS}/safety_gate/*.c)
add_library(safety_gate STATIC ${GATE_SRC})
target_include_directories(safety_gate PUBLIC ${COMPONENTS}/safety_gate/include)

file(GLOB UTIL_SRC ${COMPONENTS}/feeder_util/*.c)
if(UTIL_SRC)
  add_library(feeder_util STATIC ${UTIL_SRC})
  target_include_directories(feeder_util PUBLIC ${COMPONENTS}/feeder_util/include)
  target_link_libraries(feeder_util PUBLIC safety_gate)
endif()

enable_testing()
file(GLOB TEST_SRC ${CMAKE_CURRENT_LIST_DIR}/test_*.c)
foreach(src ${TEST_SRC})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
  target_link_libraries(${name} PRIVATE safety_gate)
  if(UTIL_SRC)
    target_link_libraries(${name} PRIVATE feeder_util)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endforeach()
