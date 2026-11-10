# hello:          the smallest daemon (lambda style)
# sensor-service: every building block in one service
# pipeline:       multi-threaded processing (threads, channels, thread pool, atomics)
# three-threads:  three named threads and the main loop, in one file
# payload-power:  a payload's power switch over GPIO, as a state machine
# can-node:       a node on a CAN bus: heartbeat, sensor frames, commands over ISO-TP
# telemetry-store: store and forward with SQLite and LZ4 (needs forgecpp::sqlite and ::lz4)
# fault-log:      a fault log in SQLite through sqlite_orm, no SQL (needs forgecpp::orm)
# thermostat:     web-first application with its own multi-page site
# radio-controller: AD9364 daemon: libiio + IQ streaming over ZeroMQ
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
forge_add_app(three-threads SOURCES three-threads/main.cpp
    CONFIG three-threads/three-threads.toml
    DESCRIPTION "forgecpp three threads example"
    RESTART on-failure MAX_RESTARTS 3 WATCHDOG 30   # a thread that ends: restarted
    NO_INSTALL)
if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
    forge_add_test(three_threads_tests SOURCES three-threads/test_three_threads.cpp)
    target_include_directories(three_threads_tests PRIVATE three-threads)
endif()
forge_add_app(payload-power SOURCES payload-power/main.cpp
    CONFIG payload-power/payload-power.toml
    DESCRIPTION "forgecpp payload power example (GPIO)"
    RESTART on-failure MAX_RESTARTS 3 WATCHDOG 30
    NO_INSTALL)
forge_add_app(can-node SOURCES can-node/main.cpp
    CONFIG can-node/can-node.toml
    DESCRIPTION "forgecpp CAN node example"
    RESTART on-failure MAX_RESTARTS 3 WATCHDOG 30
    NO_INSTALL)
if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
    forge_add_test(payload_power_tests SOURCES payload-power/test_payload_power.cpp)
    target_include_directories(payload_power_tests PRIVATE payload-power)
    forge_add_test(can_node_tests SOURCES can-node/test_can_node.cpp)
    target_include_directories(can_node_tests PRIVATE can-node)
endif()
configure_file(can-node/sensor.py "${CMAKE_CURRENT_BINARY_DIR}/sensor.py" COPYONLY)
if(TARGET forgecpp::sqlite AND TARGET forgecpp::lz4)
    forge_add_app(telemetry-store SOURCES telemetry-store/main.cpp
        LIBS forgecpp::sqlite forgecpp::lz4
        CONFIG telemetry-store/telemetry-store.toml
        DESCRIPTION "forgecpp store and forward example (SQLite, LZ4)"
        RESTART on-failure MAX_RESTARTS 3 WATCHDOG 30
        NO_INSTALL)
    if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
        forge_add_test(telemetry_store_tests SOURCES telemetry-store/test_telemetry_store.cpp
            LIBS forgecpp::sqlite forgecpp::lz4)
        target_include_directories(telemetry_store_tests PRIVATE telemetry-store)
    endif()
endif()
if(TARGET forgecpp::orm)
    forge_add_app(fault-log SOURCES fault-log/main.cpp
        LIBS forgecpp::orm
        CONFIG fault-log/fault-log.toml
        DESCRIPTION "forgecpp fault log example (SQLite with sqlite_orm)"
        RESTART on-failure MAX_RESTARTS 3 WATCHDOG 30
        NO_INSTALL)
    if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
        forge_add_test(fault_log_tests SOURCES fault-log/test_fault_log.cpp LIBS forgecpp::orm)
        target_include_directories(fault_log_tests PRIVATE fault-log)
    endif()
endif()
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
forge_add_proto(pump ROOT pump PROTOS pump.proto PYTHON ${CMAKE_CURRENT_BINARY_DIR}/python)  # pump_pb2.py
if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
    forge_add_test(pump_tests SOURCES pump/test_pump.cpp pump/pump.cpp)
    target_include_directories(pump_tests PRIVATE pump)
    forge_add_proto(pump_tests ROOT pump PROTOS pump.proto)
endif()

add_subdirectory(radio-controller)
add_subdirectory(spectrum-analyzer)
add_subdirectory(qam-modem)
add_subdirectory(channel-emulator)  # after qam-modem: its tests use the modem

# Run the examples from the build dir with their config next to them.
foreach(cfg sensor-service/sensor-service.toml pipeline/pipeline.toml three-threads/three-threads.toml
            payload-power/payload-power.toml can-node/can-node.toml telemetry-store/telemetry-store.toml
            fault-log/fault-log.toml
            thermostat/thermostat.toml pump/pump.toml)
    get_filename_component(_name "${cfg}" NAME)
    configure_file(${cfg} "${CMAKE_CURRENT_BINARY_DIR}/${_name}" COPYONLY)
endforeach()
