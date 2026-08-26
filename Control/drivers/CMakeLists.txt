cmake_minimum_required(VERSION 3.13)
project(indi-skr-pico-mount CXX)

# This follows the standard layout INDI's own 3rd-party drivers use.
# Expected to be built either dropped into indi-3rdparty's source tree,
# or standalone with libindi's dev package installed
# (Debian/Raspberry Pi OS: sudo apt install libindi-dev)

find_package(INDI REQUIRED)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${INDI_INCLUDE_DIR})

add_executable(indi_skr_pico_mount skr_pico_mount.cpp)

target_link_libraries(indi_skr_pico_mount ${INDI_LIBRARIES} ${INDI_DRIVER_LIBRARIES})

install(TARGETS indi_skr_pico_mount RUNTIME DESTINATION bin)
install(FILES indi_skr_pico_mount.xml DESTINATION ${INDI_DATA_DIR})
