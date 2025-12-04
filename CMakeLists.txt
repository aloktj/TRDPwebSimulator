cmake_minimum_required(VERSION 3.20)
project(TRDPwebSimulator LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The TCOpen TRDP Debian package installs its configs to /usr/lib/cmake/{TRDP,TCOpenTRDP}.
list(APPEND CMAKE_PREFIX_PATH "/usr/lib/cmake")

find_package(Threads REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)

option(TRDP_USE_SHARED "Link against shared TRDP libraries" ON)
option(USE_SYSTEM_DROGON "Prefer system Drogon installation over third_party/drogon" ON)
option(USE_SYSTEM_TRDP "Prefer system TRDP/TAU installation over third_party/tcnopen" ON)

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

if(USE_SYSTEM_TRDP)
    find_package(TRDP CONFIG REQUIRED NAMES TRDP trdp)
else()
    find_package(TRDP CONFIG REQUIRED NAMES TRDP trdp PATHS ${TRDP_THIRD_PARTY_DIR} ${TRDP_THIRD_PARTY_DIR}/lib/cmake)
endif()

if(TRDP_USE_SHARED)
    set(TRDP_LINK_TARGETS TRDP::trdp_shared TRDP::tau_shared)
else()
    set(TRDP_LINK_TARGETS TRDP::trdp TRDP::trdpap)
endif()

add_library(trdp_link INTERFACE)
target_link_libraries(trdp_link INTERFACE ${TRDP_LINK_TARGETS})

add_executable(drogon_config_check cmake/tests/drogon_config_check.cpp)
target_link_libraries(drogon_config_check PRIVATE Drogon::Drogon)

add_executable(trdp_config_check cmake/tests/trdp_config_check.cpp)
target_link_libraries(trdp_config_check PRIVATE trdp_link)

target_compile_definitions(trdp_config_check PRIVATE TRDP_DETECTION_ONLY)

add_library(trdp_telegram_model STATIC src/telegram_model.cpp)
target_include_directories(trdp_telegram_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(trdp_telegram_model PUBLIC tinyxml2::tinyxml2)

add_library(trdp_engine STATIC src/trdp_engine.cpp)
target_include_directories(trdp_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src /usr/include/jsoncpp)
target_link_libraries(trdp_engine PUBLIC trdp_telegram_model trdp_link Threads::Threads)

add_library(trdp_web_backend STATIC
    src/controllers/ConfigController.cpp
    src/controllers/TelegramController.cpp
    src/controllers/WsTelegram.cpp
    src/plugins/TelegramHub.cpp)
target_include_directories(trdp_web_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src /usr/include/jsoncpp)
target_link_libraries(trdp_web_backend PUBLIC trdp_engine trdp_telegram_model Drogon::Drogon)

add_executable(trdp_web_simulator src/main.cpp)
target_link_libraries(trdp_web_simulator PRIVATE trdp_web_backend)

include(GNUInstallDirs)
install(TARGETS trdp_config_check drogon_config_check RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
