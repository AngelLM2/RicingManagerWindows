include_guard(GLOBAL)

# AddressSanitizer (docs/08 §4). Incompatible with /RTC and incremental linking, so the
# asan preset is based on RelWithDebInfo. Memory budgets and Job limits do not apply.
function(rmw_sanitize target)
    if(NOT RMW_ASAN)
        return()
    endif()
    target_compile_options(${target} PRIVATE /fsanitize=address)
    target_compile_definitions(${target} PRIVATE RMW_ASAN=1)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE" OR type STREQUAL "SHARED_LIBRARY")
        target_link_options(${target} PRIVATE /INCREMENTAL:NO /DEBUG)
    endif()
endfunction()
