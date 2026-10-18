cmake_minimum_required(VERSION 3.22.1)
project(motogps LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared C++ core from the upstream repo: <repo>/shared, six levels above
# platforms/android/app/src/main/cpp.
get_filename_component(SHARED_DIR
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../shared ABSOLUTE)
if (NOT EXISTS ${SHARED_DIR}/ble_protocol/src/ble_protocol.cpp)
    message(FATAL_ERROR "shared/ not found at ${SHARED_DIR}; run from a full checkout")
endif ()

add_library(moto_core SHARED
        moto_jni.cpp)

target_include_directories(moto_core PRIVATE
        ${SHARED_DIR}
        ${SHARED_DIR}/ble_protocol/include
        ${SHARED_DIR}/coordinates/include
        ${SHARED_DIR}/port_api
        ${SHARED_DIR}/nav_core
        ${SHARED_DIR}/nav_app/include
        ${SHARED_DIR}/nav_presenter/include)

target_sources(moto_core PRIVATE
        ${SHARED_DIR}/ble_protocol/src/ble_protocol.cpp
        ${SHARED_DIR}/coordinates/coordinates.cpp
        ${SHARED_DIR}/nav_core/nav_core.cpp
        ${SHARED_DIR}/nav_app/nav_app.cpp)

find_package(Threads REQUIRED)
target_link_libraries(moto_core ${log-lib} Threads::Threads)
