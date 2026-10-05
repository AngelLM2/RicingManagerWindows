include_guard(GLOBAL)

# Warnings are errors for first-party code; third-party headers (angle brackets) are silenced.
function(rmw_set_warnings target)
    target_compile_options(${target} PRIVATE
        /W4
        /WX
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /Zc:inline
        /external:anglebrackets
        /external:W0
        /we4062 # enumerator in switch not handled
        /we4265 # class with virtual functions has non-virtual destructor
        /we4296 # expression is always true/false
        /we4826 # sign-extending conversion
        /w14254 # larger bit field assigned to smaller one
        /w14287 # unsigned/negative constant mismatch
        /w14905 # wide string literal cast to LPSTR
        /w14906 # string literal cast to LPWSTR
    )
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        STRICT
    )
endfunction()
