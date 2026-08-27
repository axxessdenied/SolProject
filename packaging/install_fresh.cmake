# Phase 22 stage C. The `package.install` fixture: wipe the test prefix, then
# install into it.
#
# ⚑ THE WIPE IS THE WHOLE REASON THIS IS A SCRIPT RATHER THAN A BARE
# `cmake --install` IN add_test. An install writes over a tree, it does not
# replace one - so a file that a previous run installed and that nothing
# installs any more would still be sitting there, and package.layout would
# happily confirm the presence of something the package no longer contains.
# A layout test on a stale prefix is a test that can only pass.
#
# Expects SOL_BUILD_DIR and SOL_PREFIX on the command line.

if(NOT DEFINED SOL_BUILD_DIR OR NOT DEFINED SOL_PREFIX)
    message(FATAL_ERROR "install_fresh.cmake needs -DSOL_BUILD_DIR=<dir> -DSOL_PREFIX=<dir>")
endif()

file(REMOVE_RECURSE "${SOL_PREFIX}")

execute_process(
    COMMAND ${CMAKE_COMMAND} --install "${SOL_BUILD_DIR}" --prefix "${SOL_PREFIX}" --strip
    RESULT_VARIABLE installResult
    OUTPUT_VARIABLE installOutput
    ERROR_VARIABLE installError)

if(NOT installResult EQUAL 0)
    message(FATAL_ERROR "install into ${SOL_PREFIX} failed (${installResult}):\n${installOutput}\n${installError}")
endif()

message(STATUS "installed a fresh tree into ${SOL_PREFIX}")
