list(APPEND tests
  addr
  connect-not-idle
  listen-not-idle
  echo
  backpressure
  reentrant-close
  drain-after-close
  mtu
  events
  server
)

foreach(test IN LISTS tests)
  add_executable(${test} ${test}.c)

  set_target_properties(
    ${test}
    PROPERTIES
    C_STANDARD 99
  )

  # Asserts are the test; strip the NDEBUG that release flags define
  target_compile_options(
    ${test}
    PRIVATE
      -UNDEBUG
  )

  target_link_libraries(
    ${test}
    PRIVATE
      l2cap_static
  )

  add_test(
    NAME ${test}
    COMMAND ${test}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  )

  set_tests_properties(
    ${test}
    PROPERTIES
    TIMEOUT 30
  )
endforeach()
