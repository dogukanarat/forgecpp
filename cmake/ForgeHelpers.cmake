# Helper functions shipped with forgecpp. Available after find_package(forgecpp).
#
#   forge_add_app(<name>
#       SOURCES <files...>
#       [LIBS <targets...>]          extra libraries to link
#       [CONFIG <file.toml>]         installed as <sysconfdir>/<name>/<name>.toml
#                                    (and searched there at runtime)
#       [SERVICE <file.service>]     installed as a systemd unit
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

function(forge_add_app name)
    cmake_parse_arguments(ARG "NO_INSTALL" "CONFIG;SERVICE;WEB" "SOURCES;LIBS" ${ARGN})
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
