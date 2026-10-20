# Host virtual device for hardware-free development.
#
# This target links the real firmware application code (PhoneNavBridge, the
# shared NavPresenter and the shared LVGL UI) against a headless 466x466
# RGB565 display and a TCP link that carries the same v1 BLE frames.
#
# It is intentionally not part of the firmware image and is only built when
# MOTO_BUILD_SIM=ON on a desktop toolchain.

find_package(Threads REQUIRED)

add_executable(moto_device_sim
    src/main.cpp
    src/device_sim.cpp
    src/headless_display.cpp
    src/link_protocol.cpp
    src/phone_client.cpp
    src/selftest_phone.cpp
    src/tcp_link.cpp
    src/virtual_ble_transport.cpp
    src/wifi_provisioning_host.cpp
    host_stubs/board_port_host.cpp
    host_stubs/sim_log.cpp
    "${CMAKE_SOURCE_DIR}/platforms/esp32/main/phone_nav_bridge.cpp"
)

target_include_directories(moto_device_sim PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/host_stubs"
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_SOURCE_DIR}/platforms/esp32/main"
    "${CMAKE_SOURCE_DIR}/shared"
    "${CMAKE_SOURCE_DIR}/shared/ble_protocol/include"
    "${CMAKE_SOURCE_DIR}/shared/nav_presenter/include"
    "${CMAKE_SOURCE_DIR}/shared/nav_ui/include"
)

target_link_libraries(moto_device_sim PRIVATE
    moto_nav_presenter
    moto_nav_ui
    moto_ble_protocol
    moto_nav_core
    Threads::Threads
)

target_compile_features(moto_device_sim PRIVATE cxx_std_17)

# Keep the executable in a predictable place so scripts and CI do not have to
# know the generator's directory layout.
set_target_properties(moto_device_sim PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(WIN32)
    target_link_libraries(moto_device_sim PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(moto_device_sim PRIVATE /W4 /utf-8)
    target_compile_definitions(moto_device_sim PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(moto_device_sim PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

# Ephemeral port so the cases can run in parallel with anything else (and
# inside CTest) without colliding on 9100.
add_test(NAME device_sim_selftest
         COMMAND moto_device_sim --selftest --port 0)

set(MOTO_SIM_CASES
    session-reconnect
    layout-rules
    geometry-mismatch
    geometry-recovery
    wifi-errors
    state-acquiring
    state-arrived
    state-offline
    fragmentation
)
foreach(case IN LISTS MOTO_SIM_CASES)
    add_test(NAME "device_sim_${case}"
             COMMAND moto_device_sim --case "${case}" --port 0)
endforeach()
