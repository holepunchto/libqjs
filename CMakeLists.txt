cmake_minimum_required(VERSION 3.25)

find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)

project(qjs C)

add_library(v8 INTERFACE)
add_library(c++ INTERFACE)

fetch_package("github:holepunchto/libjs" SOURCE_DIR js)
fetch_package("github:holepunchto/quickjs#master")

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
