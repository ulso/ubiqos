# Fail the build when the C library's heap has been squeezed out of SRAM.
# See the comment at the bottom of CMakeLists.txt for why this exists.
set(UBIQOS_MIN_HEAP 4096)

execute_process(COMMAND ${NM} ${ELF} OUTPUT_VARIABLE syms RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "check_heap: could not read symbols from ${ELF}")
endif()

# A conclusion drawn from output that was not produced is worse than none.
if(NOT syms MATCHES "([0-9a-fA-F]+) [ABNRabnr] __StackLimit")
    message(FATAL_ERROR "check_heap: no __StackLimit in ${ELF}")
endif()
set(stack_limit "0x${CMAKE_MATCH_1}")
if(NOT syms MATCHES "([0-9a-fA-F]+) [ABNRabnr] (end|__end__)")
    message(FATAL_ERROR "check_heap: no end symbol in ${ELF}")
endif()
set(heap_base "0x${CMAKE_MATCH_1}")

math(EXPR heap "${stack_limit} - ${heap_base}")
if(heap LESS UBIQOS_MIN_HEAP)
    message(FATAL_ERROR
        "SRAM is full: the C heap is ${heap} bytes, below the ${UBIQOS_MIN_HEAP} needed.\n"
        "  end          = ${heap_base}\n"
        "  __StackLimit = ${stack_limit}\n"
        "The SDK allocates its alarm pool from this heap during pre-init, so a\n"
        "kernel linked this way panics before the console exists to report it.\n"
        "Make something smaller -- UBIQOS_HEAP_SIZE in kernel/main.c is the knob.")
endif()
message(STATUS "  C heap: ${heap} bytes free between end and the stack")
