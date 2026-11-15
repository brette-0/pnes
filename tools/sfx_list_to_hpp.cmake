# sfx_list_to_hpp.cmake
# Runs at build time via: cmake -P sfx_list_to_hpp.cmake
#   -DSFX_LIST_INC=<path to FamiStudio's generated *_sfxlist.inc>
#   -DOUT_HPP=<path to the C++ header to write>
#   [-DNAMESPACE=<namespace to wrap the enum in, default "sfx">]
#
# Turns FamiStudio's sound-effect name list into a C++ enum, so call sites say
# sfx::select instead of 1.
#
# WHY THIS EXISTS. The engine's API is index-based and cannot be otherwise:
# famistudio_sfx_play takes a byte, and FamiStudio's own demo calls it with
# literal 0, 1, 21. The indices are positional -- effect N is the Nth song in
# the SFX project -- so reordering songs in the FamiStudio GUI silently
# repoints every hand-written number at the wrong sound. Passing
# -famistudio-asm-sfx-generate-list makes the exporter emit the name->index
# mapping alongside the data, from the same project, in the same run. Deriving
# the C++ enum from THAT is what makes a reorder harmless: the names and the
# effect bytes cannot disagree, because one export produced both.
#
# WHAT THE INPUT LOOKS LIKE. One assignment per effect, named via FamiStudio's
# own Utils.MakeNiceAsmName(song.Name), then a count:
#
#     sfx_toggle = 0
#     sfx_select = 1
#     sfx_max = 2
#
# plus, for ca65 specifically, a trailing string table the exporter wraps in
# `.if SFX_STRINGS` / `.endif`. That block is assembler, not data this script
# wants, so anything that is not a bare `name = number` is ignored rather than
# parsed -- which also means a future exporter addition cannot break the
# transform by appearing in the file.
#
# NES-ONLY, BY CONSTRUCTION. Nothing here is portable: the indices describe one
# FamiStudio export. The non-NES backends address effects by their position in
# the table a project hands to audio::SetSfx, which is the project's own array
# and needs no generated header. CMakeLists.txt therefore invokes this from the
# NES branch alone.

if(NOT SFX_LIST_INC OR NOT OUT_HPP)
    message(FATAL_ERROR
            "sfx_list_to_hpp.cmake: -DSFX_LIST_INC and -DOUT_HPP are both required.")
endif()

if(NOT NAMESPACE)
    set(NAMESPACE "sfx")
endif()

if(NOT EXISTS "${SFX_LIST_INC}")
    message(FATAL_ERROR
            "sfx_list_to_hpp.cmake: ${SFX_LIST_INC} does not exist. It is written by "
            "FamiStudio's SFX export when -famistudio-asm-sfx-generate-list is passed; "
            "if the export ran without that flag, no list was produced.")
endif()

file(STRINGS "${SFX_LIST_INC}" _lines)

set(_entries "")
set(_count "")
foreach(_line IN LISTS _lines)
    # Bare `name = number` only. Assembler directives, labels and .asciiz
    # strings fall through untouched -- see the header comment.
    if(NOT _line MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*([0-9]+)[ \t]*$")
        continue()
    endif()
    set(_name "${CMAKE_MATCH_1}")
    set(_value "${CMAKE_MATCH_2}")

    if(_name STREQUAL "sfx_max")
        set(_count "${_value}")
        continue()
    endif()

    # The exporter prefixes every effect with `sfx_`; the namespace already
    # says that, so sfx::sfx_select would only stutter.
    string(REGEX REPLACE "^sfx_" "" _enumerator "${_name}")

    # MakeNiceAsmName is meant to produce assembler-safe names, which is a
    # weaker guarantee than C++ identifier: a song called "2players" would
    # pass there and not here. Fail loudly -- a silently dropped effect would
    # renumber nothing but would leave a call site with no symbol to use, for
    # reasons nothing in the build explains.
    if(NOT _enumerator MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR
                "sfx_list_to_hpp.cmake: effect name \"${_name}\" does not yield a valid "
                "C++ identifier (\"${_enumerator}\"). Rename the song in the FamiStudio "
                "SFX project.")
    endif()

    string(APPEND _entries "    ${_enumerator} = ${_value},\n")
endforeach()

if(_count STREQUAL "")
    message(FATAL_ERROR
            "sfx_list_to_hpp.cmake: no sfx_max in ${SFX_LIST_INC}. The exporter writes "
            "it after the effect list, so either the file is truncated or its format "
            "has changed.")
endif()

# An empty SFX project is legal (count 0, no entries); an enum with no
# enumerators is not, so the enum is omitted rather than written malformed.
if(_entries STREQUAL "")
    set(_enum_block "// No effects in the project -- nothing to enumerate.\n")
else()
    set(_enum_block "enum Effect : unsigned char {\n${_entries}};\n")
endif()

get_filename_component(_inc_name "${SFX_LIST_INC}" NAME)

# file(CONFIGURE) rather than file(WRITE): it leaves the file alone when the
# content is unchanged, so an export that produced the same names does not bump
# the timestamp and force every dependent source to recompile. Same reason
# CMakeLists.txt writes pnes_audio_segment.inc this way.
#
# @ONLY and the escaped braces matter: the content below is C++, and an
# unescaped ${...} would be substituted away.
file(CONFIGURE OUTPUT "${OUT_HPP}" @ONLY CONTENT
"// GENERATED from @_inc_name@ by tools/sfx_list_to_hpp.cmake -- do not edit.
//
// @_inc_name@ is itself generated, by FamiStudio's SFX export, from the
// project's own song names and in the same run that produced the effect data.
// Both are build artifacts: change the FamiStudio project, not this file.
//
// Indices are POSITIONAL -- effect N is the Nth song in the SFX project -- so
// these names exist precisely so no call site has to hardcode one.
#pragma once

namespace @NAMESPACE@ {

@_enum_block@
// Number of effects exported. The valid range is 0 .. count - 1, and NOTHING
// bounds-checks it: audio::SfxPlay hands the index straight to the engine (see
// include/pnes/audio.hpp). Passing an enumerator from above is what makes that
// safe. Not to be confused with audio::sfxChannels, which is how many effects
// can sound AT ONCE.
constexpr unsigned char count = @_count@;

} // namespace @NAMESPACE@
")

message(STATUS "sfx_list_to_hpp: ${_inc_name} -> ${OUT_HPP} (${_count} effects)")
