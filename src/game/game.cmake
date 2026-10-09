# SPDX-License-Identifier: GPL-3.0-or-later
#
# Hand-written replacements for lifted functions. Included, not add_subdirectory()'d, from
# the top-level CMakeLists.txt AFTER tsfp_lifted and tsfp_host exist:
#
#     include(src/game/game.cmake)
#
# It runs in the including directory's scope on purpose: setting a COMPILE_OPTIONS property
# on a source file that another directory owns would need TARGET_DIRECTORY, and the lifted
# chunks are owned here.
#
# WHAT IT DOES, in the order it happens
#   1. Asks tools/replace which lifted chunk defines each address src/game registers, and
#      has it write one header per chunk containing `#pragma weak sub_XXXXXXXX` lines.
#   2. Force-includes that header into that chunk only, which makes the lifted definition
#      weak. Only the chunks that contain a replaced function recompile.
#   3. Compiles src/game with the project's own warning set into an OBJECT library and links
#      its objects into tsfp_host. They define the same sub_XXXXXXXX symbols strongly, so the
#      linker resolves every caller, in any chunk and through the dispatch table, to the
#      hand-written adapter. No re-lift and no runtime lookup is involved.
#   4. Builds `game_manifest`, which prints the registry linked into it. That registry is
#      what tools/coverage.py counts, never the file list.
#
# A fresh clone has no lifted tree, so none of this applies there (the caller guards on it).

set(TSFP_GAME_DIR "${CMAKE_SOURCE_DIR}/src/game")
file(GLOB TSFP_GAME_SOURCES CONFIGURE_DEPENDS "${TSFP_GAME_DIR}/*.c")
list(FILTER TSFP_GAME_SOURCES EXCLUDE REGEX "/game_manifest\\.c$")

# Editing a registration line must re-run the wiring, and CONFIGURE_DEPENDS on the glob only
# notices files appearing and disappearing.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TSFP_GAME_SOURCES})
# Re-lifting can move a replacement into another chunk without changing the file list.
# Recompute the weak-symbol mapping when the generated definitions change.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TSFP_LIFTED_SOURCES})

find_program(TSFP_PYTHON3 NAMES python3 python)
if(NOT TSFP_PYTHON3)
  message(FATAL_ERROR
    "src/game needs python3 at configure time to find which lifted chunk defines each "
    "replaced function (tools/replace wire, standard library only)")
endif()

set(TSFP_GAME_WEAK_DIR "${CMAKE_BINARY_DIR}/game-weak")
execute_process(
  COMMAND ${TSFP_PYTHON3} -m tools.replace wire
          --game-dir "${TSFP_GAME_DIR}" --gen-dir "${TSFP_LIFTED_DIR}"
          --out "${TSFP_GAME_WEAK_DIR}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  RESULT_VARIABLE TSFP_GAME_WIRE_RESULT
  OUTPUT_VARIABLE TSFP_GAME_WIRE_OUTPUT
  ERROR_VARIABLE TSFP_GAME_WIRE_ERROR)
if(NOT TSFP_GAME_WIRE_RESULT EQUAL 0)
  # Fail, do not skip: linking the hand-written objects over unweakened lifted chunks is a
  # duplicate-symbol error, and a half-wired build is worse than a stopped one.
  message(FATAL_ERROR "tools/replace wire failed:\n${TSFP_GAME_WIRE_ERROR}")
endif()
string(STRIP "${TSFP_GAME_WIRE_OUTPUT}" TSFP_GAME_WIRE_OUTPUT)
message(STATUS "game replacements: ${TSFP_GAME_WIRE_OUTPUT}")

file(GLOB TSFP_GAME_WEAK_HEADERS "${TSFP_GAME_WEAK_DIR}/weak_recomp_*.h")
foreach(weak_header IN LISTS TSFP_GAME_WEAK_HEADERS)
  get_filename_component(weak_name "${weak_header}" NAME_WE)
  string(REGEX REPLACE "^weak_" "" chunk_name "${weak_name}")
  set(chunk_path "${TSFP_LIFTED_DIR}/${chunk_name}.c")
  set_source_files_properties("${chunk_path}" PROPERTIES
    COMPILE_OPTIONS "-include;${weak_header}"
    OBJECT_DEPENDS "${weak_header}")
endforeach()

add_library(tsfp_game OBJECT ${TSFP_GAME_SOURCES})
target_include_directories(tsfp_game PRIVATE "${TSFP_GAME_DIR}")
set_target_properties(tsfp_game PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_sources(tsfp_host PRIVATE $<TARGET_OBJECTS:tsfp_game>)

add_executable(game_manifest src/game/game_manifest.c $<TARGET_OBJECTS:tsfp_game>)
target_include_directories(game_manifest PRIVATE "${TSFP_GAME_DIR}")

