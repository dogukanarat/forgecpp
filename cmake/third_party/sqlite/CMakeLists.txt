# SQLite (third_party/sqlite, public domain) as a shared library, the way
# install.sh builds every bundled library.
#
# SQLite has no CMake build of its own. install.sh first makes the
# amalgamation (sqlite3.c, shell.c) on the build machine with the checkout's
# own configure, which brings its Tcl (JimTcl) along; this project then
# compiles it with the target's compiler, so a toolchain file works:
#
#   <checkout>/configure --disable-tcl && make sqlite3.c shell.c     # in <dir>
#   cmake -S cmake/third_party/sqlite -B build -DSQLITE_AMALGAMATION_DIR=<dir>
#
# It installs what the distributions install: libsqlite3.so.0, sqlite3.h,
# sqlite3ext.h, sqlite3.pc, and the sqlite3 command line tool.
cmake_minimum_required(VERSION 3.16)

set(SQLITE_AMALGAMATION_DIR "" CACHE PATH "Directory with sqlite3.c, sqlite3.h, sqlite3ext.h and shell.c")
if(NOT EXISTS "${SQLITE_AMALGAMATION_DIR}/sqlite3.c")
    message(FATAL_ERROR "SQLITE_AMALGAMATION_DIR: no sqlite3.c in '${SQLITE_AMALGAMATION_DIR}'")
endif()
file(STRINGS "${SQLITE_AMALGAMATION_DIR}/sqlite3.h" _version REGEX "^#define SQLITE_VERSION +\"[0-9.]+\"")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _version "${_version}")

project(sqlite3 VERSION ${_version} LANGUAGES C)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

option(BUILD_SHARED_LIBS "Build libsqlite3 as a shared library" ON)
option(SQLITE_BUILD_SHELL "Build the sqlite3 command line tool" ON)

add_library(sqlite3 "${SQLITE_AMALGAMATION_DIR}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC
    $<BUILD_INTERFACE:${SQLITE_AMALGAMATION_DIR}> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Close to the distributions' builds (Debian, OpenEmbedded): thread safe,
# column metadata, full-text search, R-tree, math functions.
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_COLUMN_METADATA
    SQLITE_ENABLE_FTS4
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_RTREE
    SQLITE_ENABLE_MATH_FUNCTIONS
    SQLITE_ENABLE_DBSTAT_VTAB
    SQLITE_ENABLE_UNLOCK_NOTIFY
    SQLITE_SECURE_DELETE)
target_link_libraries(sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
# libsqlite3.so.0, the name every distribution uses.
set_target_properties(sqlite3 PROPERTIES OUTPUT_NAME sqlite3 VERSION 0.8.6 SOVERSION 0 C_VISIBILITY_PRESET default)

install(TARGETS sqlite3
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES "${SQLITE_AMALGAMATION_DIR}/sqlite3.h" "${SQLITE_AMALGAMATION_DIR}/sqlite3ext.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(prefix "${CMAKE_INSTALL_PREFIX}")
set(libdir "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
set(includedir "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
configure_file(sqlite3.pc.in "${CMAKE_CURRENT_BINARY_DIR}/sqlite3.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sqlite3.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(SQLITE_BUILD_SHELL AND EXISTS "${SQLITE_AMALGAMATION_DIR}/shell.c")
    add_executable(sqlite3_shell "${SQLITE_AMALGAMATION_DIR}/shell.c")
    target_link_libraries(sqlite3_shell PRIVATE sqlite3 ${CMAKE_DL_LIBS} m)
    set_target_properties(sqlite3_shell PROPERTIES OUTPUT_NAME sqlite3)
    install(TARGETS sqlite3_shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
