cmake_minimum_required(VERSION 3.22)

project(qjs C)

if(NOT TARGET quickjs)
  add_subdirectory(vendor/quickjs EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET uv)
  add_subdirectory(vendor/libuv EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET utf)
  add_subdirectory(vendor/libutf EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET js)
  add_subdirectory(vendor/libjs EXCLUDE_FROM_ALL)
endif()

add_library(qjs OBJECT)

set_target_properties(
  qjs
  PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON
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
    $<TARGET_PROPERTY:js,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:quickjs,INTERFACE_INCLUDE_DIRECTORIES>
  PRIVATE
    $<TARGET_PROPERTY:utf,INTERFACE_INCLUDE_DIRECTORIES>
)

add_library(qjs_shared SHARED)

set_target_properties(
  qjs_shared
  PROPERTIES
  OUTPUT_NAME qjs
  WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_link_libraries(
  qjs_shared
  PUBLIC
    qjs
    uv
    utf_shared
    quickjs_shared
)

add_library(qjs_static STATIC)

set_target_properties(
  qjs_static
  PROPERTIES
  OUTPUT_NAME qjs
  PREFIX lib
)

target_link_libraries(
  qjs_static
  PUBLIC
    qjs
    uv_a
    utf_static
    quickjs_static
)

install(TARGETS qjs_shared qjs_static)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()

  add_subdirectory(test)
endif()
