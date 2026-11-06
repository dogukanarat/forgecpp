# hello:          the smallest daemon (lambda style)
# sensor_service: every building block in one service
# pipeline:       multi-threaded processing (threads, channels, thread pool, atomics)
# thermostat:     web-first application with its own multi-page site
forge_add_app(hello SOURCES hello/main.cpp NO_INSTALL)
forge_add_app(sensor_service SOURCES sensor_service/main.cpp WEB sensor_service/web NO_INSTALL)
forge_add_app(pipeline SOURCES pipeline/main.cpp NO_INSTALL)
forge_add_app(thermostat SOURCES thermostat/main.cpp WEB thermostat/web NO_INSTALL)

# Run the examples from the build dir with their config next to them.
foreach(cfg sensor_service/sensor_service.toml pipeline/pipeline.toml thermostat/thermostat.toml)
    get_filename_component(_name "${cfg}" NAME)
    configure_file(${cfg} "${CMAKE_CURRENT_BINARY_DIR}/${_name}" COPYONLY)
endforeach()
