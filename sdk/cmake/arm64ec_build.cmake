
# The ARM64EC port is incomplete, modules that fail to build fall back to ARM64 on the media
execute_process(
    COMMAND ${CMAKE_COMMAND} --build ${BINARY_DIR} -- -k 0
    RESULT_VARIABLE _result)

if(NOT _result EQUAL 0)
    message(WARNING "Some ARM64EC modules failed to build, the media uses their ARM64 build")
endif()
