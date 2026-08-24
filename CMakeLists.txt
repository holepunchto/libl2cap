cmake_minimum_required(VERSION 4.0)

find_package(cmake-harden REQUIRED PATHS node_modules/cmake-harden)

project(l2cap C)

add_library(l2cap OBJECT)

harden(l2cap)

set_target_properties(
  l2cap
  PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON
)

target_sources(
  l2cap
  INTERFACE
    include/l2cap.h
  PRIVATE
    src/l2cap.c
)

target_include_directories(
  l2cap
  PUBLIC
    include
)

add_library(l2cap_shared SHARED)

set_target_properties(
  l2cap_shared
  PROPERTIES
  OUTPUT_NAME l2cap
)

target_link_libraries(
  l2cap_shared
  PUBLIC
    l2cap
)

add_library(l2cap_static STATIC)

set_target_properties(
  l2cap_static
  PROPERTIES
  OUTPUT_NAME l2cap
  PREFIX lib
)

target_link_libraries(
  l2cap_static
  PUBLIC
    l2cap
)

install(TARGETS l2cap_shared l2cap_static)

install(FILES include/l2cap.h DESTINATION include)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()

  add_subdirectory(test)
endif()
