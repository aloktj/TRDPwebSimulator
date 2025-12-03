cmake_minimum_required(VERSION 3.20)
project(TRDPwebSimulator LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The TCOpen TRDP Debian package installs its configs to /usr/lib/cmake/{TRDP,TCOpenTRDP}.
list(APPEND CMAKE_PREFIX_PATH "/usr/lib/cmake")

find_package(Threads REQUIRED)

# Discover an existing Drogon installation (installed once per machine).
find_package(Drogon CONFIG REQUIRED)

find_package(TRDP CONFIG REQUIRED NAMES TRDP trdp)

option(TRDP_USE_SHARED "Link against shared TRDP libraries" ON)

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

include(GNUInstallDirs)
install(TARGETS trdp_config_check drogon_config_check RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
