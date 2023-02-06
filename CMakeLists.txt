cmake_minimum_required(VERSION 3.22)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(WIN32)
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

project(qjs C)

if(NOT TARGET quickjs)
  find_program(quickjs_prebuilds_paths NAMES quickjs-prebuilds-paths)

  execute_process(
    COMMAND ${quickjs_prebuilds_paths} --lib
    OUTPUT_VARIABLE QUICKJS
  )

  execute_process(
    COMMAND ${quickjs_prebuilds_paths} --include
    OUTPUT_VARIABLE QUICKJS_INCLUDE
  )

  add_library(quickjs STATIC IMPORTED)

  set_target_properties(
    quickjs
    PROPERTIES
    IMPORTED_LOCATION ${QUICKJS}
  )

  target_include_directories(
    quickjs
    INTERFACE
      ${QUICKJS_INCLUDE}
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

add_library(qjs_shared SHARED $<TARGET_OBJECTS:qjs> $<TARGET_OBJECTS:mem>)

set_target_properties(
  qjs_shared
  PROPERTIES
  OUTPUT_NAME qjs
)

target_link_libraries(
  qjs_shared
  PUBLIC
    uv
    quickjs
)

add_library(qjs_static STATIC $<TARGET_OBJECTS:qjs> $<TARGET_OBJECTS:mem>)

set_target_properties(
  qjs_static
  PROPERTIES
  OUTPUT_NAME qjs
)

target_link_libraries(
  qjs_static
  PUBLIC
    uv_a
    quickjs
)

install(TARGETS qjs_shared qjs_static)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_subdirectory(test)
endif()
