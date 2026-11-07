# forge-hub: the applications on a device in one place (port 80), with their
# health and restart policies. Optional: every forgecpp application runs the
# same with or without it. Built and installed with forgecpp
# (-DFORGECPP_BUILD_HUB=OFF / install.sh --without-hub to leave it out).
set(FORGE_HUB_VERSION ${PROJECT_VERSION})

add_library(forge_hub_core STATIC src/proc.cpp src/systemd.cpp src/hub.cpp)
target_include_directories(forge_hub_core PUBLIC src)
target_link_libraries(forge_hub_core PUBLIC forgecpp::forgecpp)
target_compile_definitions(forge_hub_core PUBLIC FORGE_HUB_VERSION="${FORGE_HUB_VERSION}")
if(FORGECPP_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(forge_hub_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

forge_add_app(forge-hub
    SOURCES src/main.cpp
    LIBS    forge_hub_core
    CONFIG  config/forge-hub.toml
    WEB     web
    DESCRIPTION "forge-hub: applications on this device"
    RESTART always MAX_RESTARTS 0 RESTART_DELAY 2 WATCHDOG 30)

if(FORGECPP_BUILD_TESTS)
    # A small application the tests start, crash, hang and stop.
    forge_add_app(forge_hub_test_app SOURCES tests/test_app.cpp NO_INSTALL)
    forge_add_test(forge_hub_tests SOURCES tests/test_hub.cpp LIBS forge_hub_core)
    target_compile_definitions(forge_hub_tests PRIVATE
        HUB_TEST_APP="$<TARGET_FILE:forge_hub_test_app>"
        HUB_CONFIG_FILE="${CMAKE_CURRENT_SOURCE_DIR}/config/forge-hub.toml")
    add_dependencies(forge_hub_tests forge_hub_test_app)
endif()
