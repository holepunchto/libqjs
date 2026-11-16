cmake_minimum_required(VERSION 4.0)

find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-prelude REQUIRED PATHS node_modules/cmake-prelude)
find_package(cmake-test REQUIRED PATHS node_modules/cmake-test)

project(js LANGUAGES C VERSION 0.0.0)

fetch_package("github:holepunchto/libintrusive")

fetch_package(
  "github:bellard/quickjs#e585640"
  SOURCE_DIR qjs
  PATCHES
    patches/001-get-version.patch
    patches/002-type-predicates.patch
    patches/003-coercion.patch
    patches/005-uncatchable-error.patch
    patches/006-portable-time.patch
    patches/007-windows-atomics.patch
    patches/008-windows-alloca.patch
    patches/009-windows-bigint-limb.patch
    patches/010-windows-closure-type.patch
    patches/012-new-error.patch
    patches/013-dataview.patch
    patches/014-new-array.patch
    patches/015-defer-module-resolution.patch
    patches/016-boxed-type-predicates.patch
    patches/017-arraybuffer-functions.patch
    patches/018-weakref.patch
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

add_library_targets(
  js
  FEATURES
    c_std_99
  SOURCES
    src/js.c
  LINK
    utf::<TYPE>
  STATIC_LINK
    uv_a
  SHARED_LINK
    uv
  EXPORT OFF
)

target_link_libraries(
  js
  PRIVATE
    qjs
    intrusive::headers
)

target_include_directories(
  js_headers
  INTERFACE
    $<TARGET_PROPERTY:uv,INTERFACE_INCLUDE_DIRECTORIES>
)

target_sources(
  js_static
  PRIVATE
    $<TARGET_OBJECTS:qjs>
)

if(TARGET js_shared)
  target_sources(
    js_shared
    PRIVATE
      $<TARGET_OBJECTS:qjs>
  )
endif()

fetch_package("github:holepunchto/libjs" SOURCE_DIR js)

target_sources(
  js_headers
  INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${js}/include
    FILES ${js}/include/js.h
)

add_test_subdirectory(test)
