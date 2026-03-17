if(NOT DEFINED SRC OR NOT DEFINED OUTDIR)
    message(FATAL_ERROR "SRC and OUTDIR are required")
endif()

if(NOT DEFINED SERIES_MAJOR)
    set(SERIES_MAJOR "2")
endif()

if(NOT DEFINED PATCH_WIDTH)
    set(PATCH_WIDTH "3")
endif()

if(NOT DEFINED LATEST_NAME)
    set(LATEST_NAME "latite.dll")
endif()

file(MAKE_DIRECTORY "${OUTDIR}")

file(GLOB existing_dlls "${OUTDIR}/omoti-v${SERIES_MAJOR}.*.dll")
set(max_patch -1)

foreach(path IN LISTS existing_dlls)
    get_filename_component(name "${path}" NAME)
    string(REGEX MATCH "^omoti-v${SERIES_MAJOR}\\.([0-9]+)\\.dll$" _ "${name}")
    if(CMAKE_MATCH_1)
        math(EXPR parsed_patch "${CMAKE_MATCH_1}")
        if(parsed_patch GREATER max_patch)
            set(max_patch "${parsed_patch}")
        endif()
    endif()
endforeach()

if(max_patch LESS 0)
    set(next_patch 0)
else()
    math(EXPR next_patch "${max_patch} + 1")
endif()

set(patch_str "${next_patch}")
string(LENGTH "${patch_str}" patch_len)
while(patch_len LESS PATCH_WIDTH)
    set(patch_str "0${patch_str}")
    string(LENGTH "${patch_str}" patch_len)
endwhile()
set(versioned_name "omoti-v${SERIES_MAJOR}.${patch_str}.dll")
set(versioned_dst "${OUTDIR}/${versioned_name}")
set(latest_dst "${OUTDIR}/${LATEST_NAME}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC}" "${versioned_dst}"
    RESULT_VARIABLE copy_result
    ERROR_VARIABLE copy_error
)

if(NOT copy_result EQUAL 0)
    message(WARNING "Versioned DLL copy skipped: ${copy_error}")
else()
    message(STATUS "Copied Omoti DLL to ${versioned_dst}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC}" "${latest_dst}"
    RESULT_VARIABLE copy_latest_result
    ERROR_VARIABLE copy_latest_error
)

if(NOT copy_latest_result EQUAL 0)
    message(WARNING "Latest DLL copy skipped: ${copy_latest_error}")
else()
    message(STATUS "Updated latest DLL at ${latest_dst}")
endif()
