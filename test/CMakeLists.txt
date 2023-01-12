list(APPEND tests
  add-finalizer
  adjust-external-memory
  call-function
  call-function-ignore-return
  call-function-microtask
  call-function-microtask-nested
  call-function-throw
  create-external
  create-external-with-finalizer
  create-external-arraybuffer
  create-external-arraybuffer-with-finalizer
  create-function
  create-function-args-extra
  create-function-args-fewer
  create-function-throw
  create-function-throw-indirect
  create-promise-reject
  create-promise-resolve
  create-reference
  create-reference-weak
  fatal-exception
  get-arraybuffer-info
  get-typedarray-info-uint8array
  get-typedarray-info-uint16array
  promise-rejection
  promise-rejection-unhandled
  run-module
  run-script
  run-script-ignore-return
  run-script-throw
  set-named-property
  typeof-bigint
  typeof-boolean
  typeof-external
  typeof-function
  typeof-null
  typeof-number
  typeof-object
  typeof-string
  typeof-symbol
  typeof-undefined
  wrap
  wrap-with-finalizer
  wrap-with-reference
  wrap-remove-with-finalizer
)

foreach(test IN LISTS tests)
  add_executable(${test} ../vendor/libjs/test/${test}.c)

  target_link_libraries(
    ${test}
    PRIVATE
      qjs_shared
  )

  target_include_directories(
    ${test}
    PRIVATE
      $<TARGET_PROPERTY:qjs,INCLUDE_DIRECTORIES>
  )

  add_test(
    NAME ${test}
    COMMAND ${test}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  )
endforeach()
