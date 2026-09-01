cmake_minimum_required(VERSION 3.20.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/sign.conf)

# Real device secrets (CONFIG_PIGEON_ENDPOINT/CONFIG_PIGEON_TOKEN) live in a
# gitignored prj.local.conf, auto-merged if present -- same convention as the
# sibling pigeon-examples repo's samples. Never commit one or paste its
# contents into a transcript.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/prj.local.conf)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/prj.local.conf)
endif()

# CONFIG_PIGEON_FOTA_CURRENT_VERSION must track this build's own version --
# the pigeon library compares it byte-for-byte against the shadow firmware
# target's version string, and a stale value either loops re-downloading the
# already-running image or never converges (see pigeon's zephyr/Kconfig).
# Derived here from the same VERSION file Zephyr's app-version machinery
# reads, so there is exactly one place to bump and no second constant to
# forget. Written as a generated conf fragment appended to EXTRA_CONF_FILE
# (the prj.local.conf mechanism above) because that is the only assignment
# path sysbuild honors: under sysbuild, plain CMake CONFIG_* set()s are
# only consumed from the sysbuild cache (see zephyr/cmake/modules/
# kconfig.cmake's SYSBUILD branch), never from the app CMakeLists. The
# platform-side firmware catalog entry for an image built from this tree
# must be uploaded under this exact MAJOR.MINOR.PATCHLEVEL string (see
# README, FOTA test procedure).
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/VERSION edb_version_file)
string(REGEX MATCH "VERSION_MAJOR *= *([0-9]+)" edb_unused "${edb_version_file}")
set(edb_version_major ${CMAKE_MATCH_1})
string(REGEX MATCH "VERSION_MINOR *= *([0-9]+)" edb_unused "${edb_version_file}")
set(edb_version_minor ${CMAKE_MATCH_1})
string(REGEX MATCH "PATCHLEVEL *= *([0-9]+)" edb_unused "${edb_version_file}")
set(edb_version_patch ${CMAKE_MATCH_1})
if(NOT (edb_version_major MATCHES "^[0-9]+$" AND edb_version_minor MATCHES "^[0-9]+$"
        AND edb_version_patch MATCHES "^[0-9]+$"))
  message(FATAL_ERROR "Could not parse MAJOR/MINOR/PATCHLEVEL out of app/VERSION")
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/pigeon_fota_version.conf
  "CONFIG_PIGEON_FOTA_CURRENT_VERSION=\"${edb_version_major}.${edb_version_minor}.${edb_version_patch}\"\n")
list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_BINARY_DIR}/pigeon_fota_version.conf)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(embedded-departure-board)

# Generate hex files from pem files
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/)
zephyr_include_directories(${gen_dir})

foreach(inc_file
  pigeon-ca.crt
  swiftly-ca-full.crt
)
  generate_inc_file_for_target(
    app
    keys/public/${inc_file}
    ${gen_dir}/${inc_file}.hex
  )
endforeach()

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/keys/private/swiftly-api.key)
  write_file(${CMAKE_CURRENT_SOURCE_DIR}/keys/private/swiftly-api.key \"ffffffffffffffffffffffffffffffff\")
endif()

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# pigeon's cross-transport TLS handshake lock is declared in the library's
# own src/pigeon_internal.h, not in the <pigeon.h> it publishes. This app's
# Swiftly client is a second TLS client on the same modem and has to take
# that lock (see net/custom_http_client.c), so take the declaration from
# where it lives instead of restating the prototype here: C links a
# locally-declared prototype against a changed definition without
# complaint, where an include fails the build.
target_include_directories(app PRIVATE ${ZEPHYR_PIGEON_MODULE_DIR}/src)

FILE(GLOB_RECURSE app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

target_compile_definitions(app PRIVATE _POSIX_C_SOURCE=200809L)
SET_SOURCE_FILES_PROPERTIES( ${app_sources} APPEND PROPERTIES COMPILE_FLAGS -fanalyzer )
