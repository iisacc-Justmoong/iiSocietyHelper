# Preserve CTest's cross-compiling emulator handling; native tests use a
# private resource workspace selected by the test runner.
function(iisociety_fixture_test name target)
    if(CMAKE_CROSSCOMPILING)
        add_test(NAME "${name}" COMMAND "${target}" ${ARGN})
    else()
        add_test(NAME "${name}" COMMAND ${_helper_test_runner} "$<TARGET_FILE:${target}>" ${ARGN})
    endif()
endfunction()
