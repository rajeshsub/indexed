# Strict warnings for our own targets only. Applied after FetchContent so
# third-party code (re2, abseil, utf8proc, googletest) isn't held to -Werror.
function(indexed_enable_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
endfunction()
