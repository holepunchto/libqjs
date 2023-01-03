cmake_minimum_required(VERSION 3.22)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(WIN32)
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

project(qjs C)

find_library(QUICKJS quickjs PATH_SUFFIXES quickjs REQUIRED)
find_path(QUICKJS_INCLUDE_DIR quickjs.h PATH_SUFFIXES quickjs)

if(NOT TARGET uv)
  add_subdirectory(vendor/libuv EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET js)
  add_subdirectory(vendor/libjs EXCLUDE_FROM_ALL)
endif()

add_library(qjs OBJECT)

set_target_properties(
  qjs
  PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE 1
)

target_sources(
  qjs
  PRIVATE
    src/qjs.c
)

target_include_directories(
  qjs
  PUBLIC
    include
    $<TARGET_PROPERTY:js,INCLUDE_DIRECTORIES>
    ${QUICKJS_INCLUDE_DIR}
)

add_library(qjs_shared SHARED $<TARGET_OBJECTS:qjs>)

set_target_properties(
  qjs_shared
  PROPERTIES
  OUTPUT_NAME qjs
)

target_link_libraries(
  qjs_shared
  PUBLIC
    uv
    ${QUICKJS}
)

add_library(qjs_static STATIC $<TARGET_OBJECTS:qjs>)

set_target_properties(
  qjs_static
  PROPERTIES
  OUTPUT_NAME qjs
)

target_link_libraries(
  qjs_static
  PUBLIC
    uv_a
    ${QUICKJS}
)

install(TARGETS qjs_shared qjs_static)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_subdirectory(test)
endif()
