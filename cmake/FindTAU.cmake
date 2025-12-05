include(FindPackageHandleStandardArgs)

find_path(TAU_INCLUDE_DIR
    NAMES tau_ctrl.h tau/tau_ctrl.h)

find_library(TAU_LIBRARY NAMES tau)
find_library(TAU_SHARED_LIBRARY NAMES tau_shared)

if(NOT TAU_LIBRARY AND TAU_SHARED_LIBRARY)
    set(TAU_LIBRARY ${TAU_SHARED_LIBRARY})
endif()

find_package_handle_standard_args(TAU DEFAULT_MSG TAU_INCLUDE_DIR TAU_LIBRARY)

if(TAU_FOUND)
    if(TAU_LIBRARY AND NOT TARGET TAU::tau)
        add_library(TAU::tau UNKNOWN IMPORTED)
        set_target_properties(TAU::tau PROPERTIES
            IMPORTED_LOCATION "${TAU_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${TAU_INCLUDE_DIR}")
    endif()

    if(TAU_SHARED_LIBRARY AND NOT TARGET TAU::tau_shared)
        add_library(TAU::tau_shared UNKNOWN IMPORTED)
        set_target_properties(TAU::tau_shared PROPERTIES
            IMPORTED_LOCATION "${TAU_SHARED_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${TAU_INCLUDE_DIR}")
    endif()
endif()
