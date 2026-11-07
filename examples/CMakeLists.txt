# hello:          the smallest daemon (lambda style)
# sensor-service: every building block in one service
# pipeline:       multi-threaded processing (threads, channels, thread pool, atomics)
# thermostat:     web-first application with its own multi-page site
# radio-controller: AD9364 daemon: libiio + IQ streaming over ZeroMQ (needs forgecpp::zmq)
# spectrum-analyzer: AD9364 spectrum analyzer with a web UI (needs forgecpp::zmq)
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

if(TARGET forgecpp::zmq)
    add_subdirectory(radio-controller)
    add_subdirectory(spectrum-analyzer)
else()
    message(STATUS "examples: radio-controller and spectrum-analyzer skipped (forgecpp built without ZeroMQ)")
endif()

# Run the examples from the build dir with their config next to them.
foreach(cfg sensor-service/sensor-service.toml pipeline/pipeline.toml thermostat/thermostat.toml)
    get_filename_component(_name "${cfg}" NAME)
    configure_file(${cfg} "${CMAKE_CURRENT_BINARY_DIR}/${_name}" COPYONLY)
endforeach()
