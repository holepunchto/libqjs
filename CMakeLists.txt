cmake_minimum_required(VERSION 4.0)

find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)

project(js C)

fetch_package(
  "github:bellard/quickjs#master"
  SOURCE_DIR qjs
  PATCHES
    patches/001-get-version.patch
    patches/002-type-predicates.patch
    patches/003-coercion.patch
    patches/004-new-symbol.patch
    patches/005-uncatchable-error.patch
    patches/006-portable-time.patch
    patches/007-windows-atomics.patch
    patches/008-windows-alloca.patch
    patches/009-windows-bigint-limb.patch
)

file(READ ${qjs}/VERSION qjs_version)

string(STRIP "${qjs_version}" qjs_version)

add_library(qjs OBJECT)

set_target_properties(
  qjs
  PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON
)

target_sources(
  qjs
  PRIVATE
    ${qjs}/quickjs.c
    ${qjs}/dtoa.c
    ${qjs}/libregexp.c
    ${qjs}/libunicode.c
    ${qjs}/cutils.c
)

target_compile_definitions(
  qjs
  PRIVATE
    CONFIG_VERSION="${qjs_version}"
)

target_include_directories(
  qjs
  PUBLIC
    ${qjs}
)

add_library(js OBJECT)

fetch_package("github:holepunchto/libjs" SOURCE_DIR js)
fetch_package("github:holepunchto/libintrusive")

set_target_properties(
  js
  PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON
)

target_sources(
  js
  PRIVATE
    src/js.c
)

target_include_directories(
  js
  INTERFACE
    include
  PUBLIC
    ${js}/include
  PRIVATE
    $<TARGET_PROPERTY:qjs,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:uv,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:utf,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:intrusive,INTERFACE_INCLUDE_DIRECTORIES>
)

add_library(js_shared SHARED)

set_target_properties(
  js_shared
  PROPERTIES
  OUTPUT_NAME js
  WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_include_directories(
  js_shared
  INTERFACE
    $<TARGET_PROPERTY:js,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(
  js_shared
  PUBLIC
    qjs
    uv
    utf_shared
  PRIVATE
    js
)

add_library(js_static STATIC)

set_target_properties(
  js_static
  PROPERTIES
  OUTPUT_NAME js
  PREFIX lib
)

target_include_directories(
  js_static
  INTERFACE
    $<TARGET_PROPERTY:js,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(
  js_static
  PUBLIC
    qjs
    uv_a
    utf_static
  PRIVATE
    js
)

install(TARGETS js_shared js_static)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()

  add_subdirectory(test)
endif()
