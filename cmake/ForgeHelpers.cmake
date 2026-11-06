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

include_guard(GLOBAL)
include(GNUInstallDirs)

set(_FORGE_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/ForgeEmbed.cmake")

if(TARGET forgecpp::forgecpp)
    set(_FORGE_LIB forgecpp::forgecpp)
else()
    set(_FORGE_LIB forgecpp)
endif()

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
