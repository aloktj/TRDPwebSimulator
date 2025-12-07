cmake_minimum_required(VERSION 3.20)
project(TRDPwebSimulator LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# The TCOpen TRDP Debian package installs its configs to /usr/lib/cmake/{TRDP,TCOpenTRDP}.
list(APPEND CMAKE_PREFIX_PATH "/usr/lib/cmake")

find_package(Threads REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)

option(TRDP_USE_SHARED "Link against shared TRDP libraries" ON)
option(USE_SYSTEM_DROGON "Prefer system Drogon installation over third_party/drogon" ON)
option(USE_SYSTEM_TRDP "Prefer system TRDP/TAU installation over third_party/tcnopen" ON)
option(TRDP_ENABLE_TAU_DNR "Enable TAU DNR integration when available" ON)

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(DROGON_THIRD_PARTY_DIR "${THIRD_PARTY_DIR}/drogon")
set(TRDP_THIRD_PARTY_DIR "${THIRD_PARTY_DIR}/tcnopen")

if(USE_SYSTEM_DROGON)
    # Discover an existing Drogon installation (installed once per machine).
    find_package(Drogon CONFIG REQUIRED)
else()
    if(EXISTS "${DROGON_THIRD_PARTY_DIR}/CMakeLists.txt")
        add_subdirectory(${DROGON_THIRD_PARTY_DIR} third_party/drogon)
    else()
        # Allow third_party/drogon to provide an installed config package too.
        find_package(Drogon CONFIG REQUIRED PATHS ${DROGON_THIRD_PARTY_DIR} ${DROGON_THIRD_PARTY_DIR}/lib/cmake)
    endif()

    if(NOT TARGET Drogon::Drogon)
        if(TARGET drogon)
            add_library(Drogon::Drogon ALIAS drogon)
        else()
            message(FATAL_ERROR "Drogon target not found; ensure third_party/drogon is populated or enable USE_SYSTEM_DROGON")
        endif()
    endif()
endif()

set(TRDP_FOUND OFF)
set(TAU_FOUND OFF)
set(TAU_LINK_TARGET "")
if(USE_SYSTEM_TRDP)
    find_package(TRDP CONFIG QUIET NAMES TRDP trdp)
else()
    find_package(TRDP CONFIG QUIET NAMES TRDP trdp PATHS ${TRDP_THIRD_PARTY_DIR} ${TRDP_THIRD_PARTY_DIR}/lib/cmake)
endif()

if(NOT TRDP_FOUND)
    find_package(TRDP QUIET MODULE)
endif()

# The upstream TRDP config package also exports TAU targets (TRDP::tau/TRDP::tau_shared)
# but does not install a TAU package config. Prefer those targets before falling back
# to our FindTAU.cmake module.
if(TRDP_FOUND)
    if(TRDP_USE_SHARED AND TARGET TRDP::tau_shared)
        set(TAU_LINK_TARGET TRDP::tau_shared)
    elseif(TARGET TRDP::tau)
        set(TAU_LINK_TARGET TRDP::tau)
    elseif(TARGET TRDP::tau_shared)
        # Allow linking the shared library even if TRDP_USE_SHARED is OFF when no
        # static TAU target is exported.
        set(TAU_LINK_TARGET TRDP::tau_shared)
    endif()

    if(TAU_LINK_TARGET)
        set(TAU_FOUND ON)
    endif()
endif()

if(NOT TAU_FOUND)
    if(USE_SYSTEM_TRDP)
        find_package(TAU CONFIG QUIET NAMES TAU tau)
    else()
        find_package(TAU CONFIG QUIET NAMES TAU tau PATHS ${TRDP_THIRD_PARTY_DIR} ${TRDP_THIRD_PARTY_DIR}/lib/cmake)
    endif()

    if(NOT TAU_FOUND)
        find_package(TAU QUIET MODULE)
    endif()

    if(TAU_FOUND)
        if(TRDP_USE_SHARED AND TARGET TAU::tau_shared)
            set(TAU_LINK_TARGET TAU::tau_shared)
        elseif(TARGET TAU::tau)
            set(TAU_LINK_TARGET TAU::tau)
        elseif(TARGET TAU::tau_shared)
            set(TAU_LINK_TARGET TAU::tau_shared)
        endif()
    endif()
endif()

if(TRDP_FOUND AND TAU_FOUND)
    if(TRDP_USE_SHARED AND TARGET TRDP::trdp_shared)
        set(TRDP_LINK_TARGETS TRDP::trdp_shared)
        if(TARGET TRDP::trdpap_shared)
            list(APPEND TRDP_LINK_TARGETS TRDP::trdpap_shared)
        endif()
    else()
        set(TRDP_LINK_TARGETS TRDP::trdp)
        if(TARGET TRDP::trdpap)
            list(APPEND TRDP_LINK_TARGETS TRDP::trdpap)
        endif()
    endif()

    if(NOT TAU_LINK_TARGET)
        message(WARNING "TAU library target was reported as found but no target could be selected")
    else()
        list(APPEND TRDP_LINK_TARGETS ${TAU_LINK_TARGET})
    endif()
else()
    message(WARNING "TRDP/TAU package not found; building with stub TRDP linkage (no real TRDP stack present)")
    set(TRDP_LINK_TARGETS)
endif()

set(TRDP_HAS_TAU_DNR 0)
if(TRDP_ENABLE_TAU_DNR)
    if(TAU_FOUND AND TAU_LINK_TARGET)
        include(CheckCXXSymbolExists)

        # Some upstream TAU targets do not publish their include directories, but the
        # TRDP targets do. Gather the include hints from both so the tau_dnr header
        # can be located when using the CPack-generated TRDP package.
        set(_tau_dnr_include_dirs)
        get_target_property(_tau_include_prop ${TAU_LINK_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
        if(_tau_include_prop AND NOT _tau_include_prop STREQUAL "NOTFOUND")
            list(APPEND _tau_dnr_include_dirs ${_tau_include_prop})
        endif()
        if(TARGET TRDP::trdp)
            get_target_property(_trdp_include_prop TRDP::trdp INTERFACE_INCLUDE_DIRECTORIES)
            if(_trdp_include_prop AND NOT _trdp_include_prop STREQUAL "NOTFOUND")
                list(APPEND _tau_dnr_include_dirs ${_trdp_include_prop})
            endif()
        endif()
        list(REMOVE_DUPLICATES _tau_dnr_include_dirs)

        if(_tau_dnr_include_dirs)
            set(CMAKE_REQUIRED_INCLUDES ${_tau_dnr_include_dirs})
        endif()
        set(CMAKE_REQUIRED_LIBRARIES ${TAU_LINK_TARGET})
        check_cxx_symbol_exists(tau_initDnr "trdp/api/tau_dnr.h" TRDP_HAS_TAU_DNR)
        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(TRDP_HAS_TAU_DNR)
            message(STATUS "Detected TAU DNR support")
        else()
            message(WARNING "TAU library found but tau_dnr.h or tau_initDnr is unavailable; TAU DNR will be disabled")
        endif()
    elseif(NOT TAU_FOUND)
        message(WARNING "TAU not found; TAU DNR will be disabled")
    endif()
endif()

add_library(trdp_link INTERFACE)
target_link_libraries(trdp_link INTERFACE ${TRDP_LINK_TARGETS})

add_executable(drogon_config_check cmake/tests/drogon_config_check.cpp)
target_link_libraries(drogon_config_check PRIVATE Drogon::Drogon)

if(TRDP_FOUND)
    add_executable(trdp_config_check cmake/tests/trdp_config_check.cpp)
    target_link_libraries(trdp_config_check PRIVATE trdp_link)
    target_compile_definitions(trdp_config_check PRIVATE TRDP_DETECTION_ONLY)
endif()

add_library(trdp_telegram_model STATIC src/telegram_model.cpp)
target_include_directories(trdp_telegram_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(trdp_telegram_model PUBLIC tinyxml2::tinyxml2)

add_library(trdp_engine STATIC src/trdp_engine.cpp)
target_include_directories(trdp_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src /usr/include/jsoncpp
    /usr/include/trdp/vos/api)
target_link_libraries(trdp_engine PUBLIC trdp_telegram_model trdp_link Threads::Threads)
target_compile_definitions(trdp_engine PRIVATE TRDP_HAS_TAU_DNR=${TRDP_HAS_TAU_DNR})
if(TRDP_FOUND AND TAU_FOUND)
    target_compile_definitions(trdp_engine PRIVATE TRDP_STACK_PRESENT)
endif()

add_library(trdp_web_backend OBJECT
    src/controllers/ConfigController.cpp
    src/controllers/TelegramController.cpp
    src/controllers/WsTelegram.cpp
    src/plugins/TelegramHub.cpp)
target_include_directories(trdp_web_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src /usr/include/jsoncpp)
target_link_libraries(trdp_web_backend PUBLIC trdp_engine trdp_telegram_model Drogon::Drogon)

add_executable(trdp_web_simulator src/main.cpp $<TARGET_OBJECTS:trdp_web_backend>)
target_link_libraries(trdp_web_simulator PRIVATE trdp_engine trdp_telegram_model Drogon::Drogon)

include(GNUInstallDirs)
if(TRDP_FOUND)
    install(TARGETS trdp_config_check RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS drogon_config_check RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
