# hello:          the smallest daemon (lambda style)
# sensor-service: every building block in one service
# pipeline:       multi-threaded processing (threads, channels, thread pool, atomics)
# thermostat:     web-first application with its own multi-page site
# radio-controller: AD9364 daemon: libiio + IQ streaming over forge.v1 (ZeroMQ)
# spectrum-analyzer: AD9364 spectrum analyzer with a web UI
# qam-modem:      16QAM packet modem (UDP <-> radio-controller), 2+ Mbit/s
# channel-emulator: radio channel impairments in the TX path (noise, offsets, multipath, fading)
# Each one gets a generated systemd unit (<build>/examples/<name>.service) with
# the restart policy that suits it; the examples are not installed.
forge_add_app(hello SOURCES hello/main.cpp
    DESCRIPTION "forgecpp hello world"
    RESTART on-failure MAX_RESTARTS 3 WATCHDOG 0   # no watchdog: a demo
    NO_INSTALL)
forge_add_app(sensor-service SOURCES sensor-service/main.cpp WEB sensor-service/web
    CONFIG sensor-service/sensor-service.toml
    DESCRIPTION "forgecpp sensor service example"
    RESTART on-failure MAX_RESTARTS 3 RESTART_WINDOW 600 WATCHDOG 30
    NO_INSTALL)
forge_add_app(pipeline SOURCES pipeline/main.cpp
    CONFIG pipeline/pipeline.toml
    DESCRIPTION "forgecpp processing pipeline example"
    RESTART on-failure MAX_RESTARTS 5 RESTART_WINDOW 300 RESTART_DELAY 1 WATCHDOG 20
    NO_INSTALL)
forge_add_app(thermostat SOURCES thermostat/main.cpp WEB thermostat/web
    CONFIG thermostat/thermostat.toml
    DESCRIPTION "forgecpp thermostat example"
    RESTART always MAX_RESTARTS 0                  # always back, however often
    NO_INSTALL)

# pump: the application of the tutorials (docs/tutorials), with its tests.
forge_add_app(pump SOURCES pump/main.cpp pump/pump.cpp
    CONFIG pump/pump.toml
    DESCRIPTION "forgecpp tutorial pump"
    RESTART on-failure MAX_RESTARTS 3 WATCHDOG 30
    NO_INSTALL)
if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
    forge_add_test(pump_tests SOURCES pump/test_pump.cpp pump/pump.cpp)
    target_include_directories(pump_tests PRIVATE pump)
endif()

add_subdirectory(radio-controller)
add_subdirectory(spectrum-analyzer)
add_subdirectory(qam-modem)
add_subdirectory(channel-emulator)  # after qam-modem: its tests use the modem

# Run the examples from the build dir with their config next to them.
foreach(cfg sensor-service/sensor-service.toml pipeline/pipeline.toml thermostat/thermostat.toml pump/pump.toml)
    get_filename_component(_name "${cfg}" NAME)
    configure_file(${cfg} "${CMAKE_CURRENT_BINARY_DIR}/${_name}" COPYONLY)
endforeach()
