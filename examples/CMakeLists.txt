# hello:          the smallest daemon (lambda style)
# sensor-service: every building block in one service
# pipeline:       multi-threaded processing (threads, channels, thread pool, atomics)
# thermostat:     web-first application with its own multi-page site
# radio_controller: AD9364 daemon: libiio + IQ streaming over ZeroMQ (needs forgecpp::zmq)
# spectrum_analyzer: AD9364 spectrum analyzer with a web UI (needs forgecpp::zmq)
forge_add_app(hello SOURCES hello/main.cpp NO_INSTALL)
forge_add_app(sensor-service SOURCES sensor_service/main.cpp WEB sensor_service/web NO_INSTALL)
forge_add_app(pipeline SOURCES pipeline/main.cpp NO_INSTALL)
forge_add_app(thermostat SOURCES thermostat/main.cpp WEB thermostat/web NO_INSTALL)

if(TARGET forgecpp::zmq)
    add_subdirectory(radio_controller)
    add_subdirectory(spectrum_analyzer)
else()
    message(STATUS "examples: radio_controller and spectrum_analyzer skipped (forgecpp built without ZeroMQ)")
endif()

# Run the examples from the build dir with their config next to them.
foreach(cfg sensor_service/sensor-service.toml pipeline/pipeline.toml thermostat/thermostat.toml)
    get_filename_component(_name "${cfg}" NAME)
    configure_file(${cfg} "${CMAKE_CURRENT_BINARY_DIR}/${_name}" COPYONLY)
endforeach()
