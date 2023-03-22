cmake_minimum_required(VERSION 3.22)

project(qjs C)

if(NOT TARGET quickjs)
  find_program(quickjs_prebuilds NAMES quickjs-prebuilds)

  execute_process(
    COMMAND ${quickjs_prebuilds} paths lib
    OUTPUT_VARIABLE quickjs
  )

  execute_process(
    COMMAND ${quickjs_prebuilds} paths include
    OUTPUT_VARIABLE quickjs_include
  )

  add_library(quickjs STATIC IMPORTED)

  set_target_properties(
    quickjs
    PROPERTIES
    IMPORTED_LOCATION ${quickjs}
  )

  target_include_directories(
    quickjs
    INTERFACE
      ${quickjs_include}
  )
endif()

if(NOT TARGET uv)
  add_subdirectory(vendor/libuv EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET mem)
  add_subdirectory(vendor/libmem EXCLUDE_FROM_ALL)
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
    $<TARGET_PROPERTY:mem,INTERFACE_INCLUDE_DIRECTORIES>
)

add_library(qjs_shared SHARED $<TARGET_OBJECTS:qjs>)

set_target_properties(
  qjs_shared
  PROPERTIES
  OUTPUT_NAME qjs
  WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_link_libraries(
  qjs_shared
  PUBLIC
    uv
    quickjs
    mem_shared
)

add_library(qjs_static STATIC $<TARGET_OBJECTS:qjs>)

set_target_properties(
  qjs_static
  PROPERTIES
  OUTPUT_NAME qjs
  PREFIX lib
)

target_link_libraries(
  qjs_static
  PUBLIC
    uv_a
    quickjs
    mem_static
)

install(TARGETS qjs_shared qjs_static)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_subdirectory(test)
endif()
