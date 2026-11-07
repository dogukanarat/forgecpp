# Helper functions shipped with forgecpp. Available after find_package(forgecpp).
#
#   forge_add_app(<name>
#       SOURCES <files...>
#       [LIBS <targets...>]          extra libraries to link
#       [CONFIG <file.toml>]         installed as <sysconfdir>/<name>/<name>.toml
#                                    (and searched there at runtime)
#       [SERVICE <file.service>]     installed as a systemd unit (your own), or generated:
#       [SYSTEMD]                    generate <name>.service (any of the options below implies it)
#       [RESTART never|on-failure|always]   restart policy (default on-failure)
#       [MAX_RESTARTS <n>]           restarts allowed within RESTART_WINDOW, then the unit
#                                    fails and stays down (default 3; 0 = unlimited)
#       [RESTART_WINDOW <s>]         default 600
#       [RESTART_DELAY <s>]          default 2
#       [WATCHDOG <s>]               hung main loop -> killed and restarted (default 30; 0 = off)
#       [DESCRIPTION <text>] [AFTER <units...>] [USER <user>]
#       [WEB <dir>]                  embed <dir> as the app's web pages (see forge_add_web)
#       [NO_INSTALL])
#
#   forge_add_web(<target>
#       DIR <dir>                    every file under <dir> is compiled into <target>
#       [PREFIX <url prefix>])       URL prefix, default "/" (web/index.html -> /index.html)
#
#   forge_add_test(<name>
#       SOURCES <files...>
#       [LIBS <targets...>])         doctest-based test registered with CTest
#
#   forge_add_proto(<target>
#       PROTOS <files...>            .proto files, relative to ROOT
#       [ROOT <dir>]                 import root of the files, default: current source dir
#       [IMPORT_DIRS <dirs...>]      extra import roots (forge/v1 and google/protobuf are always found)
#       [PYTHON <dir>])              also generate Python modules (*_pb2.py) into <dir>
#
#   forge_install_python(DIRECTORY <dir>)
#     Install the Python modules under <dir> (e.g. the PYTHON dir of
#     forge_add_proto: <dir>/radio/v1/radio_pb2.py) into site-packages, next to
#     forgecpp's forge-v1 package, so clients simply `from radio.v1 import radio_pb2`.
#     The directory is FORGE_PYTHON_SITEDIR (relative to the install prefix);
#     by default the interpreter's own for this prefix, else lib/python3.X/site-packages.
#     Yocto: -DFORGE_PYTHON_SITEDIR=${PYTHON_SITEPACKAGES_DIR}.
#     Generates C++ with protoc and compiles it into <target>; the generated
#     headers are found as "<path>.pb.h" (e.g. #include "radio/v1/radio.pb.h").
#     Cross builds: set FORGE_PROTOC to a protoc that runs on the build machine
#     (same version as the target's libprotobuf).

include_guard(GLOBAL)
include(GNUInstallDirs)

set(_FORGE_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/ForgeEmbed.cmake")

if(TARGET forgecpp::forgecpp)
    set(_FORGE_LIB forgecpp::forgecpp)
else()
    set(_FORGE_LIB forgecpp)
endif()

# forge/v1/*.proto: next to this file when installed, in the source tree otherwise.
if(NOT DEFINED FORGECPP_PROTO_DIR)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../proto/forge/v1/common.proto")
        get_filename_component(FORGECPP_PROTO_DIR "${CMAKE_CURRENT_LIST_DIR}/../proto" ABSOLUTE)
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../share/forgecpp/proto/forge/v1/common.proto")
        get_filename_component(FORGECPP_PROTO_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../share/forgecpp/proto" ABSOLUTE)
    endif()
endif()

# protoc that runs on the build machine.
function(_forge_protoc out)
    if(FORGE_PROTOC)
        set(${out} "${FORGE_PROTOC}" PARENT_SCOPE)
    elseif(Protobuf_PROTOC_EXECUTABLE)
        set(${out} "${Protobuf_PROTOC_EXECUTABLE}" PARENT_SCOPE)
    elseif(TARGET protobuf::protoc AND NOT CMAKE_CROSSCOMPILING)
        set(${out} "$<TARGET_FILE:protobuf::protoc>" PARENT_SCOPE)
    else()
        find_program(_forge_protoc_found protoc)
        if(NOT _forge_protoc_found)
            message(FATAL_ERROR "protoc not found: set FORGE_PROTOC to the protoc of the build machine")
        endif()
        set(${out} "${_forge_protoc_found}" PARENT_SCOPE)
    endif()
endfunction()

function(forge_add_proto target)
    cmake_parse_arguments(ARG "" "ROOT;PYTHON" "PROTOS;IMPORT_DIRS" ${ARGN})
    if(NOT ARG_PROTOS)
        message(FATAL_ERROR "forge_add_proto(${target}): PROTOS is required")
    endif()
    if(NOT ARG_ROOT)
        set(ARG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    get_filename_component(root "${ARG_ROOT}" ABSOLUTE)
    _forge_protoc(protoc)

    set(out "${CMAKE_CURRENT_BINARY_DIR}/forge_proto/${target}")
    file(MAKE_DIRECTORY "${out}")
    set(includes -I "${root}")
    foreach(dir ${ARG_IMPORT_DIRS} ${FORGECPP_PROTO_DIR})
        list(APPEND includes -I "${dir}")
    endforeach()
    # google/protobuf/*.proto (well-known types) live next to the protobuf headers.
    get_target_property(_pb_inc protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
    foreach(dir ${_pb_inc})
        if(EXISTS "${dir}/google/protobuf/struct.proto")
            list(APPEND includes -I "${dir}")
        endif()
    endforeach()

    set(sources)
    set(python_outputs)
    foreach(proto ${ARG_PROTOS})
        get_filename_component(abs "${root}/${proto}" ABSOLUTE)
        file(RELATIVE_PATH rel "${root}" "${abs}")
        string(REGEX REPLACE "\\.proto$" "" stem "${rel}")
        set(outputs "${out}/${stem}.pb.cc" "${out}/${stem}.pb.h")
        set(python_arg)
        if(ARG_PYTHON)
            string(REGEX REPLACE "\\.proto$" "_pb2.py" py "${rel}")
            list(APPEND outputs "${ARG_PYTHON}/${py}")
            list(APPEND python_outputs "${ARG_PYTHON}/${py}")
            set(python_arg "--python_out=${ARG_PYTHON}")
        endif()
        add_custom_command(
            OUTPUT ${outputs}
            COMMAND ${protoc} --cpp_out=${out} ${python_arg} ${includes} "${rel}"
            WORKING_DIRECTORY "${root}"
            DEPENDS "${abs}"
            COMMENT "protoc ${rel}"
            VERBATIM)
        list(APPEND sources "${out}/${stem}.pb.cc")
    endforeach()
    if(ARG_PYTHON)
        file(MAKE_DIRECTORY "${ARG_PYTHON}")
    endif()
    target_sources(${target} PRIVATE ${sources})
    target_include_directories(${target} PUBLIC $<BUILD_INTERFACE:${out}>)
    target_link_libraries(${target} PUBLIC protobuf::libprotobuf)
    # Generated code triggers warnings we cannot fix.
    set_source_files_properties(${sources} PROPERTIES COMPILE_OPTIONS "-w")
endfunction()

# Where Python modules go, relative to CMAKE_INSTALL_PREFIX (see forge_install_python).
function(_forge_python_sitedir out)
    if(DEFINED FORGE_PYTHON_SITEDIR AND NOT FORGE_PYTHON_SITEDIR STREQUAL "")
        set(${out} "${FORGE_PYTHON_SITEDIR}" PARENT_SCOPE)
        return()
    endif()
    set(dir "lib/python3/site-packages")
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
        # The interpreter's own directory when it is under the prefix (Debian:
        # /usr/local/lib/python3.X/dist-packages), else lib/python3.X/site-packages.
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -c
                "import sys,sysconfig,os;p=sysconfig.get_path('purelib');b=sys.argv[1];print(os.path.relpath(p,b) if p.startswith(b.rstrip('/')+'/') else 'lib/python%d.%d/site-packages'%sys.version_info[:2])"
                "${CMAKE_INSTALL_PREFIX}"
            OUTPUT_VARIABLE dir OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0 OR dir STREQUAL "")
            set(dir "lib/python3/site-packages")
        endif()
    endif()
    set(FORGE_PYTHON_SITEDIR "${dir}" CACHE STRING "Python modules, relative to the install prefix")
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()

function(forge_install_python)
    cmake_parse_arguments(ARG "" "DIRECTORY" "" ${ARGN})
    if(NOT ARG_DIRECTORY)
        message(FATAL_ERROR "forge_install_python: DIRECTORY is required")
    endif()
    _forge_python_sitedir(site)
    install(DIRECTORY "${ARG_DIRECTORY}/" DESTINATION "${site}"
            FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE)
endfunction()

function(forge_add_app name)
    cmake_parse_arguments(ARG "NO_INSTALL;SYSTEMD"
        "CONFIG;SERVICE;WEB;RESTART;MAX_RESTARTS;RESTART_WINDOW;RESTART_DELAY;WATCHDOG;DESCRIPTION;USER"
        "SOURCES;LIBS;AFTER" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "forge_add_app(${name}): SOURCES is required")
    endif()

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${_FORGE_LIB} ${ARG_LIBS})
    target_compile_features(${name} PRIVATE cxx_std_17)
    set_target_properties(${name} PROPERTIES INSTALL_RPATH_USE_LINK_PATH ON)
    target_compile_definitions(${name} PRIVATE
        FORGE_APP_INSTALLED_CONFIG="${CMAKE_INSTALL_FULL_SYSCONFDIR}/${name}/${name}.toml")
    if(ARG_WEB)
        forge_add_web(${name} DIR "${ARG_WEB}")
    endif()

    # Generated systemd unit: the restart policy lives in systemd.
    foreach(opt RESTART MAX_RESTARTS RESTART_WINDOW RESTART_DELAY WATCHDOG)
        if(DEFINED ARG_${opt})
            set(ARG_SYSTEMD ON)
        endif()
    endforeach()
    if(ARG_SYSTEMD AND NOT ARG_SERVICE)
        forge_generate_unit(${name} "${CMAKE_CURRENT_BINARY_DIR}/${name}.service"
            RESTART "${ARG_RESTART}" MAX_RESTARTS "${ARG_MAX_RESTARTS}" RESTART_WINDOW "${ARG_RESTART_WINDOW}"
            RESTART_DELAY "${ARG_RESTART_DELAY}" WATCHDOG "${ARG_WATCHDOG}" DESCRIPTION "${ARG_DESCRIPTION}"
            USER "${ARG_USER}" AFTER ${ARG_AFTER})
        set(ARG_SERVICE "${CMAKE_CURRENT_BINARY_DIR}/${name}.service")
    endif()

    if(ARG_NO_INSTALL)
        return()
    endif()

    install(TARGETS ${name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(ARG_CONFIG)
        install(FILES ${ARG_CONFIG}
                DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/${name}
                RENAME ${name}.toml)
    endif()
    if(ARG_SERVICE)
        if(NOT DEFINED FORGE_SYSTEMD_UNIT_DIR)
            # systemd searches <prefix>/lib/systemd/system (also on lib64 systems).
            set(FORGE_SYSTEMD_UNIT_DIR "lib/systemd/system")
        endif()
        install(FILES ${ARG_SERVICE} DESTINATION ${FORGE_SYSTEMD_UNIT_DIR})
    endif()
endfunction()

function(forge_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    if(TARGET forgecpp::testing)
        set(_testing forgecpp::testing)
    elseif(TARGET forgecpp_testing)
        set(_testing forgecpp_testing)
    else()
        message(FATAL_ERROR "forge_add_test(${name}): forgecpp was built without doctest")
    endif()

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${_testing} ${ARG_LIBS})
    target_compile_features(${name} PRIVATE cxx_std_17)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Embed every file under DIR into <target>; served by the forgecpp HTTP server
# from memory. Edit the files and rebuild; or point [web].root at the directory
# to serve it from disk while developing (no rebuild needed).
function(forge_add_web target)
    cmake_parse_arguments(ARG "" "DIR;PREFIX;SYMBOL" "" ${ARGN})
    if(NOT ARG_DIR)
        message(FATAL_ERROR "forge_add_web(${target}): DIR is required")
    endif()
    if(NOT DEFINED ARG_PREFIX)
        set(ARG_PREFIX "/")
    endif()
    if(NOT ARG_PREFIX MATCHES "/$")
        string(APPEND ARG_PREFIX "/")
    endif()
    if(NOT ARG_SYMBOL)
        string(MAKE_C_IDENTIFIER "${target}_web_assets" ARG_SYMBOL)
        set(_register ON)
    else()
        set(_register OFF)  # explicit symbol: the caller registers the table itself
    endif()

    get_filename_component(_dir "${ARG_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT IS_DIRECTORY "${_dir}")
        message(FATAL_ERROR "forge_add_web(${target}): ${_dir} is not a directory")
    endif()
    file(GLOB_RECURSE _files LIST_DIRECTORIES false CONFIGURE_DEPENDS "${_dir}/*")
    list(SORT _files)

    set(_manifest "")
    foreach(_f IN LISTS _files)
        file(RELATIVE_PATH _rel "${_dir}" "${_f}")
        string(APPEND _manifest "${_f}|${ARG_PREFIX}${_rel}\n")
    endforeach()

    set(_out "${CMAKE_CURRENT_BINARY_DIR}/forge_web/${ARG_SYMBOL}")
    file(MAKE_DIRECTORY "${_out}")
    file(WRITE "${_out}/manifest.txt.in" "${_manifest}")
    configure_file("${_out}/manifest.txt.in" "${_out}/manifest.txt" COPYONLY)  # only touch on change

    add_custom_command(
        OUTPUT "${_out}/${ARG_SYMBOL}.cpp"
        COMMAND "${CMAKE_COMMAND}"
                "-DMANIFEST=${_out}/manifest.txt"
                "-DOUTPUT=${_out}/${ARG_SYMBOL}.cpp"
                "-DSYMBOL=${ARG_SYMBOL}"
                "-DREGISTER=${_register}"
                -P "${_FORGE_EMBED_SCRIPT}"
        DEPENDS ${_files} "${_out}/manifest.txt" "${_FORGE_EMBED_SCRIPT}"
        COMMENT "Embedding web assets from ${ARG_DIR} into ${target}"
        VERBATIM)
    target_sources(${target} PRIVATE "${_out}/${ARG_SYMBOL}.cpp")
endfunction()

# forge_generate_unit(<name> <output> [RESTART ..] [MAX_RESTARTS ..] [RESTART_WINDOW ..]
#                     [RESTART_DELAY ..] [WATCHDOG ..] [DESCRIPTION ..] [USER ..] [AFTER ..])
# Writes a systemd unit for the application <name> (see forge_add_app).
set(_FORGE_HELPERS_DIR "${CMAKE_CURRENT_LIST_DIR}")
function(forge_generate_unit name output)
    cmake_parse_arguments(ARG "" "RESTART;MAX_RESTARTS;RESTART_WINDOW;RESTART_DELAY;WATCHDOG;DESCRIPTION;USER" "AFTER" ${ARGN})
    foreach(pair "RESTART;on-failure" "MAX_RESTARTS;3" "RESTART_WINDOW;600" "RESTART_DELAY;2" "WATCHDOG;30")
        list(GET pair 0 key)
        list(GET pair 1 def)
        if("${ARG_${key}}" STREQUAL "")
            set(ARG_${key} "${def}")
        endif()
    endforeach()
    if(ARG_RESTART STREQUAL "never")
        set(ARG_RESTART "no")
    endif()
    if(NOT ARG_RESTART MATCHES "^(no|on-failure|always|on-abnormal|on-watchdog|on-abort|on-success)$")
        message(FATAL_ERROR "forge_add_app(${name}): RESTART must be never, on-failure or always (not ${ARG_RESTART})")
    endif()
    set(FORGE_UNIT_NAME "${name}")
    set(FORGE_UNIT_DESCRIPTION "${ARG_DESCRIPTION}")
    if(NOT FORGE_UNIT_DESCRIPTION)
        set(FORGE_UNIT_DESCRIPTION "${name} (forgecpp)")
    endif()
    set(FORGE_UNIT_AFTER "")
    set(FORGE_UNIT_WANTS "")
    foreach(u IN LISTS ARG_AFTER)
        string(APPEND FORGE_UNIT_AFTER " ${u}")
    endforeach()
    if(ARG_MAX_RESTARTS EQUAL 0)
        set(FORGE_UNIT_START_LIMIT_INTERVAL 0)  # no limit
        set(FORGE_UNIT_START_LIMIT_BURST 5)
    else()
        set(FORGE_UNIT_START_LIMIT_INTERVAL "${ARG_RESTART_WINDOW}")
        math(EXPR FORGE_UNIT_START_LIMIT_BURST "${ARG_MAX_RESTARTS} + 1")  # the first start counts too
    endif()
    set(FORGE_UNIT_RESTART "${ARG_RESTART}")
    set(FORGE_UNIT_RESTART_DELAY "${ARG_RESTART_DELAY}")
    set(FORGE_UNIT_WATCHDOG "${ARG_WATCHDOG}")
    set(FORGE_UNIT_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/${name} --config ${CMAKE_INSTALL_FULL_SYSCONFDIR}/${name}/${name}.toml")
    set(FORGE_UNIT_EXTRA "")
    if(ARG_USER)
        set(FORGE_UNIT_EXTRA "User=${ARG_USER}\n")
    endif()
    configure_file("${_FORGE_HELPERS_DIR}/forge-app.service.in" "${output}" @ONLY)
endfunction()
