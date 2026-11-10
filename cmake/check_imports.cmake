execute_process(
    COMMAND "${NM}" -u "${LIBRARY}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE errors
)

if(NOT status EQUAL 0)
    message(FATAL_ERROR "Cannot inspect Ackord imports: ${errors}")
endif()

set(instrumented FALSE)

if(symbols MATCHES "_?__(asan|ubsan|sanitizer|sancov)_")
    set(instrumented TRUE)
endif()

string(REPLACE "\n" ";" lines "${symbols}")

foreach(line IN LISTS lines)
    string(STRIP "${line}" line)

    if(line STREQUAL "" OR line MATCHES ":$")
        continue()
    endif()

    string(REGEX MATCH "[^ \t]+$" symbol "${line}")

    # Compilers may replace byte operations with runtime memory calls.
    if(symbol MATCHES "^_?(memcpy|memmove|memset|memcmp|bcopy|bzero|bcmp)$")
        continue()
    endif()

    if(symbol MATCHES "^_?__(asan|ubsan|sanitizer|sancov)_")
        continue()
    endif()

    # ASan's generated stack frames may also require stack-protector support.
    if(instrumented AND symbol MATCHES "^_?__stack_chk_(fail|guard)$")
        continue()
    endif()

    message(FATAL_ERROR "Ackord imports an unexpected external symbol: ${symbol}")
endforeach()
