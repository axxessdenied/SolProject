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
# ⚑⚑ SOL_COMPONENT IS REQUIRED SINCE PHASE 24 STAGE V, AND IT IS LOAD-BEARING
# RATHER THAN OPTIONAL. There are two packages now - the game and the Forge -
# and an install with no component writes BOTH into one prefix. `check_layout`
# would then find `forge.exe` in the game tree and fail, which is that test
# doing exactly its job: decision 010 narrowed "the shipping binary carries no
# editor" to the GAME binary, and kept the assertion. Two components, two
# prefixes, two layout checks.
#
# Expects SOL_BUILD_DIR, SOL_PREFIX and SOL_COMPONENT on the command line.

if(NOT DEFINED SOL_BUILD_DIR OR NOT DEFINED SOL_PREFIX OR NOT DEFINED SOL_COMPONENT)
    message(FATAL_ERROR "install_fresh.cmake needs -DSOL_BUILD_DIR=<dir> -DSOL_PREFIX=<dir> -DSOL_COMPONENT=<name>")
endif()

file(REMOVE_RECURSE "${SOL_PREFIX}")

execute_process(
    COMMAND ${CMAKE_COMMAND} --install "${SOL_BUILD_DIR}" --prefix "${SOL_PREFIX}"
                             --component "${SOL_COMPONENT}" --strip
    RESULT_VARIABLE installResult
    OUTPUT_VARIABLE installOutput
    ERROR_VARIABLE installError)

if(NOT installResult EQUAL 0)
    message(FATAL_ERROR "install into ${SOL_PREFIX} failed (${installResult}):\n${installOutput}\n${installError}")
endif()

message(STATUS "installed a fresh ${SOL_COMPONENT} tree into ${SOL_PREFIX}")
