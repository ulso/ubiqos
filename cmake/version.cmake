# What this build calls itself, taken from git rather than typed in.
#
# Run at every build, not at configure time: a version read once when cmake
# last ran would say v0.1.5 on a board flashed three commits later, which is
# exactly the confusion this is meant to end. The file is only rewritten when
# the string changes, so the kernel recompiles when the version moves and not
# otherwise.
#
# `git describe` gives the release when the build IS the release -- v0.1.6 --
# and v0.1.6-3-g1849a48 three commits after it, which is the useful part: a
# board can say that it is ahead of any release. Without git, or without tags,
# the fallback below is what it says.

set(FALLBACK "0.1.6")

execute_process(COMMAND git -C "${SRC}" describe --tags --always --dirty
                OUTPUT_VARIABLE V OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE RC)
if(NOT RC EQUAL 0 OR V STREQUAL "")
    set(V "${FALLBACK}")
endif()
string(REGEX REPLACE "^v" "" V "${V}")

set(TEXT "#define UBIQOS_VERSION \"${V}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" OLD)
else()
    set(OLD "")
endif()
if(NOT OLD STREQUAL TEXT)
    file(WRITE "${OUT}" "${TEXT}")
endif()
