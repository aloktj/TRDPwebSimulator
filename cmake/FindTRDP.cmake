include(FindPackageHandleStandardArgs)

find_path(TRDP_INCLUDE_DIR
    NAMES trdp/api/trdp_if_light.h
    PATH_SUFFIXES api trdp/api)

find_library(TRDP_TRDP_LIBRARY NAMES trdp)
find_library(TRDP_TRDP_SHARED_LIBRARY NAMES trdp_shared)
find_library(TRDP_TRDPAP_LIBRARY NAMES trdpap)
find_library(TRDP_TRDPAP_SHARED_LIBRARY NAMES trdpap_shared)

if(NOT TRDP_TRDP_LIBRARY AND TRDP_TRDP_SHARED_LIBRARY)
    set(TRDP_TRDP_LIBRARY ${TRDP_TRDP_SHARED_LIBRARY})
endif()
if(NOT TRDP_TRDPAP_LIBRARY AND TRDP_TRDPAP_SHARED_LIBRARY)
    set(TRDP_TRDPAP_LIBRARY ${TRDP_TRDPAP_SHARED_LIBRARY})
endif()

set(TRDP_LIBRARIES ${TRDP_TRDP_LIBRARY} ${TRDP_TRDPAP_LIBRARY})

find_package_handle_standard_args(TRDP DEFAULT_MSG TRDP_INCLUDE_DIR TRDP_TRDP_LIBRARY)

if(TRDP_FOUND)
    if(TRDP_TRDP_LIBRARY AND NOT TARGET TRDP::trdp)
        add_library(TRDP::trdp UNKNOWN IMPORTED)
        set_target_properties(TRDP::trdp PROPERTIES
            IMPORTED_LOCATION "${TRDP_TRDP_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${TRDP_INCLUDE_DIR}")
    endif()

    if(TRDP_TRDPAP_LIBRARY AND NOT TARGET TRDP::trdpap)
        add_library(TRDP::trdpap UNKNOWN IMPORTED)
        set_target_properties(TRDP::trdpap PROPERTIES
            IMPORTED_LOCATION "${TRDP_TRDPAP_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${TRDP_INCLUDE_DIR}")
    endif()

    if(TRDP_TRDP_SHARED_LIBRARY AND NOT TARGET TRDP::trdp_shared)
        add_library(TRDP::trdp_shared UNKNOWN IMPORTED)
        set_target_properties(TRDP::trdp_shared PROPERTIES
            IMPORTED_LOCATION "${TRDP_TRDP_SHARED_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${TRDP_INCLUDE_DIR}")
    endif()

    if(TRDP_TRDPAP_SHARED_LIBRARY AND NOT TARGET TRDP::trdpap_shared)
        add_library(TRDP::trdpap_shared UNKNOWN IMPORTED)
        set_target_properties(TRDP::trdpap_shared PROPERTIES
            IMPORTED_LOCATION "${TRDP_TRDPAP_SHARED_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${TRDP_INCLUDE_DIR}")
    endif()
endif()
