# ogc_saveconf_to_hpp.cmake
# Runs at configure time via: cmake -P ogc_saveconf_to_hpp.cmake
#   -DSAVE_CONFIG=<path to a GameCube saveconfig.toml>
#   -DOUT_HPP=<path to the C++ header to write>
#   -DHEADER_SIZE=<bytes the layout must fit in; one card sector, 8192>
#
# Turns the [banner] / [icon] description in a saveconfig.toml into the
# directory-entry field values and file offsets a GameCube save needs, so that
# src/ogc/save.cpp does not hand-maintain arithmetic that the asset
# description already determines.
#
# WHY THIS EXISTS. The GameCube keeps a save's displayed metadata INSIDE the
# save file -- the comment text, the banner and the icon are file contents, and
# card_direntry only holds OFFSETS to them (`comment_addr`, `icon_addr`) plus
# three packed format fields. Those offsets are not free: libogc derives every
# sub-offset from icon_addr and the formats, so the moment the asset
# description changes -- a second icon frame, a banner switched on -- every
# address after it moves. Two places would then have to agree: whatever writes
# the bytes, and whatever fills the directory entry. Deriving both from one
# description is what makes them unable to disagree.
#
# THE LAYOUT RULES ARE NOT GUESSED. devkitPro ships no card.c, so they were
# read out of libogc.a's compiled __card_updateiconoffsets:
#
#   cursor = icon_addr
#   banner CI   : image at cursor, TLUT at cursor+3072, cursor += 3584
#   banner RGB  : image at cursor, no TLUT,             cursor += 6144
#   icon  CI    : image at cursor,                      cursor += 1024
#   icon  RGB   : image at cursor, no TLUT,             cursor += 2048
#   shared TLUT : at (cursor after banner) + frames*1024
#
# and the two validation rules out of CARD_SetStatusAsync, both of which reject
# with CARD_ERROR_FATAL_ERROR:
#
#   icon_addr    == 0xFFFFFFFF, or < 512
#   comment_addr % 8192 <= 8128   (the 64-byte block may not straddle a block)
#
# WHAT IS DELIBERATELY NOT SUPPORTED, and why:
#
#   - MIXED icon formats. libogc computes the shared palette's address as
#     `frames * 1024` past the banner, which is only right when every frame is
#     1024 bytes -- i.e. all CI. One RGB frame in the set and the palette
#     address is wrong, so a mixed set is refused rather than emitted.
#   - The per-frame-palette icon format (the format the hardware calls 3).
#     ogc/card.h does not name it at all -- it offers only NONE/CI/RGB -- so
#     there is no spelling for it in a config that uses those names.
#   - A banner at all, in any layout that does not fit HEADER_SIZE. The size
#     check at the end is the whole point: a region too small to hold what the
#     config asks for is a BUILD failure naming the number it needed, not a
#     save that writes an icon over the player's data.

if(NOT SAVE_CONFIG OR NOT OUT_HPP OR NOT DEFINED HEADER_SIZE)
    message(FATAL_ERROR
        "ogc_saveconf_to_hpp.cmake: -DSAVE_CONFIG, -DOUT_HPP and -DHEADER_SIZE are all required")
endif()
if(NOT EXISTS ${SAVE_CONFIG})
    message(FATAL_ERROR "ogc_saveconf_to_hpp.cmake: no such save config: ${SAVE_CONFIG}")
endif()

file(READ ${SAVE_CONFIG} _toml)

# Strip trailing comments, so a `# 32 ASCII chars max` note cannot be mistaken
# for part of a value. Done line-wise rather than with one global regex because
# a `#` inside a quoted string is legal TOML and must survive -- only the
# scalars this script reads are unquoted, so only those lines are trimmed.
string(REGEX REPLACE "\r\n" "\n" _toml "${_toml}")

# ----------------------------------------------------------------------------
# A TOML scalar, by key, from anywhere in the file.
#
# Section-blind ON PURPOSE: the four keys read here (`present`, `format`,
# `formats`, `speeds`, `animation`) are each unique across the whole config, so
# tracking which [table] is open would buy nothing and cost the one thing a
# hand-rolled parser should not spend -- a way to be subtly wrong about scope.
# If a key ever stops being unique, this is where that must change.
# ----------------------------------------------------------------------------
function(_scalar key out)
    if(_toml MATCHES "(^|\n)[ \t]*${key}[ \t]*=[ \t]*([^\n#]*)")
        string(STRIP "${CMAKE_MATCH_2}" _v)
        string(REGEX REPLACE "^\"(.*)\"$" "\\1" _v "${_v}")
        set(${out} "${_v}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# A TOML array of scalars -> a CMake list.
function(_array key out)
    _scalar("${key}" _raw)
    string(REGEX REPLACE "^\\[(.*)\\]$" "\\1" _raw "${_raw}")
    set(_items "")
    if(NOT "${_raw}" STREQUAL "")
        string(REPLACE "," ";" _raw "${_raw}")
        foreach(_i IN LISTS _raw)
            string(STRIP "${_i}" _i)
            string(REGEX REPLACE "^\"(.*)\"$" "\\1" _i "${_i}")
            if(NOT "${_i}" STREQUAL "")
                list(APPEND _items "${_i}")
            endif()
        endforeach()
    endif()
    set(${out} "${_items}" PARENT_SCOPE)
endfunction()

_scalar("present"   _banner_present)
_scalar("format"    _banner_format)
_scalar("animation" _animation)
_array("formats"    _icon_formats)
_array("speeds"     _icon_speeds)

# ----------------------------------------------------------------------------
# Banner
#
# `present = false` wins over whatever `format` says, so a config can keep a
# banner described and switched off rather than having to delete it to disable
# it. That is also why the format is only validated when it is going to be
# used -- an unused CARD_BANNER_RGBA typo should not fail a build that emits no
# banner at all.
# ----------------------------------------------------------------------------
set(_banner_bits 0)
set(_banner_bytes 0)
if(_banner_present STREQUAL "true")
    if(_banner_format STREQUAL "CARD_BANNER_CI")
        set(_banner_bits 1)
        set(_banner_bytes 3584)      # 96x32 CI8 (3072) + 256-entry RGB5A3 TLUT (512)
    elseif(_banner_format STREQUAL "CARD_BANNER_RGB")
        set(_banner_bits 2)
        set(_banner_bytes 6144)      # 96x32 RGB5A3, no TLUT
    else()
        message(FATAL_ERROR
            "saveconfig [banner] format must be CARD_BANNER_CI or CARD_BANNER_RGB, "
            "got \"${_banner_format}\" (ogc/card.h names no other banner format -- "
            "in particular there is no CARD_BANNER_RGBA)")
    endif()
endif()

# CARD_ANIM_BOUNCE (0x04) lives in banner_fmt even though it governs the ICON
# -- a quirk of the format, not of this script. Defaulting to loop because a
# config that says nothing about animation has one static frame, where the two
# are indistinguishable.
set(_anim_bits 0)
if(_animation STREQUAL "bounce")
    set(_anim_bits 4)
elseif(NOT _animation STREQUAL "loop" AND NOT _animation STREQUAL "")
    message(FATAL_ERROR
        "saveconfig [icon] animation must be \"loop\" or \"bounce\", got \"${_animation}\"")
endif()

math(EXPR _banner_fmt "${_banner_bits} | ${_anim_bits}")

# ----------------------------------------------------------------------------
# Icon
#
# A frame needs BOTH a nonzero format and a nonzero speed to exist: libogc
# forces the format to 0 when the speed is 0, and counts only frames where both
# are set when it places the shared palette. So a single static icon still
# needs speed >= 1 -- leave it at 0 and the frame count is zero, the palette
# lands on top of the image, and the menu shows nothing at all.
# ----------------------------------------------------------------------------
list(LENGTH _icon_formats _frames)
list(LENGTH _icon_speeds  _speed_count)
if(_frames EQUAL 0)
    message(FATAL_ERROR "saveconfig [icon] formats is empty -- at least one frame is required")
endif()
if(_frames GREATER 8)
    message(FATAL_ERROR
        "saveconfig [icon] has ${_frames} frames; CARD_MAXICONS is 8")
endif()
if(NOT _speed_count EQUAL _frames)
    message(FATAL_ERROR
        "saveconfig [icon] has ${_frames} formats but ${_speed_count} speeds -- one speed per frame")
endif()

list(GET _icon_formats 0 _icon_format)
foreach(_f IN LISTS _icon_formats)
    if(NOT _f STREQUAL _icon_format)
        message(FATAL_ERROR
            "saveconfig [icon] mixes formats (${_icon_format} and ${_f}). Every frame must "
            "use the same one: libogc places the shared palette ${_frames}*1024 bytes past "
            "the banner, which is only correct when every frame is 1024 bytes")
    endif()
endforeach()

if(_icon_format STREQUAL "CARD_ICON_CI")
    set(_icon_bits 1)
    set(_frame_bytes 1024)           # 32x32 CI8
    set(_tlut_bytes 512)             # one 256-entry RGB5A3 palette, shared
elseif(_icon_format STREQUAL "CARD_ICON_RGB")
    set(_icon_bits 2)
    set(_frame_bytes 2048)           # 32x32 RGB5A3
    set(_tlut_bytes 0)
    message(WARNING
        "saveconfig [icon] uses CARD_ICON_RGB. libogc's own __card_updateiconoffsets "
        "advances its cursor 3072 bytes per RGB frame where the data is 2048, so any "
        "offset it reports for a later frame is wrong. CARD_ICON_CI avoids this and is "
        "half the size.")
else()
    message(FATAL_ERROR
        "saveconfig [icon] formats must be CARD_ICON_CI or CARD_ICON_RGB, got "
        "\"${_icon_format}\" (ogc/card.h names no other icon format)")
endif()

set(_icon_fmt 0)
set(_icon_speed 0)
set(_n 0)
foreach(_s IN LISTS _icon_speeds)
    if(NOT _s MATCHES "^[0-3]$")
        message(FATAL_ERROR
            "saveconfig [icon] speed ${_n} is \"${_s}\"; must be 0 (end), 1 (fast), "
            "2 (middle) or 3 (slow)")
    endif()
    if(_n EQUAL 0 AND _s EQUAL 0)
        message(FATAL_ERROR
            "saveconfig [icon] speed 0 is 0, which means \"end of animation\" and makes "
            "libogc force frame 0's format to 0 as well -- the icon would not render. "
            "A static icon needs speed 1.")
    endif()
    math(EXPR _shift "2 * ${_n}")
    math(EXPR _icon_fmt   "${_icon_fmt}   | (${_icon_bits} << ${_shift})")
    math(EXPR _icon_speed "${_icon_speed} | (${_s} << ${_shift})")
    math(EXPR _n "${_n} + 1")
endforeach()

# ----------------------------------------------------------------------------
# Layout
#
# comment_addr is 0 so that the "must not straddle an 8192-byte block" rule is
# satisfied by construction rather than by arithmetic that could drift.
#
# The stamp sits between the comment block and the image data, NOT at offset 0,
# because offset 0 is the one address with no freedom worth spending: a comment
# block anywhere else has to be re-checked against that rule every time the
# layout moves.
# ----------------------------------------------------------------------------
set(_comment_addr 0)
set(_comment_bytes 64)               # two 32-byte fields: name, then description
set(_stamp_addr 64)
set(_stamp_bytes 32)
set(_icon_addr 96)

math(EXPR _banner_addr      "${_icon_addr}")
math(EXPR _banner_tlut_addr "${_icon_addr} + 3072")
math(EXPR _frames_addr      "${_icon_addr} + ${_banner_bytes}")
math(EXPR _frames_bytes     "${_frames} * ${_frame_bytes}")
math(EXPR _tlut_addr        "${_frames_addr} + ${_frames_bytes}")
math(EXPR _required         "${_tlut_addr} + ${_tlut_bytes}")

if(_icon_addr GREATER_EQUAL 512)
    message(FATAL_ERROR
        "computed icon_addr is ${_icon_addr}; CARD_SetStatus rejects anything >= 512 "
        "with CARD_ERROR_FATAL_ERROR")
endif()
if(_required GREATER HEADER_SIZE)
    message(FATAL_ERROR
        "this saveconfig needs ${_required} bytes but only ${HEADER_SIZE} are available.\n"
        "  comments      ${_comment_bytes} @ ${_comment_addr}\n"
        "  stamp         ${_stamp_bytes} @ ${_stamp_addr}\n"
        "  banner        ${_banner_bytes} @ ${_banner_addr}\n"
        "  ${_frames} icon frame(s)  ${_frames_bytes} @ ${_frames_addr}\n"
        "  palette       ${_tlut_bytes} @ ${_tlut_addr}\n"
        "A card file is a whole number of 8192-byte blocks, and one sector is all this "
        "backend transfers, so the metadata and the game state together have to fit it.")
endif()

if(_tlut_bytes EQUAL 0)
    set(_tlut_literal "0xFFFFFFFFu")
else()
    set(_tlut_literal "${_tlut_addr}u")
endif()
if(_banner_bytes EQUAL 0)
    set(_banner_literal "0xFFFFFFFFu")
    set(_banner_tlut_literal "0xFFFFFFFFu")
elseif(_banner_bits EQUAL 2)
    set(_banner_literal "${_banner_addr}u")
    set(_banner_tlut_literal "0xFFFFFFFFu")
else()
    set(_banner_literal "${_banner_addr}u")
    set(_banner_tlut_literal "${_banner_tlut_addr}u")
endif()

file(WRITE ${OUT_HPP}
"// GENERATED by tools/ogc_saveconf_to_hpp.cmake -- do not edit.
// Source: ${SAVE_CONFIG}
//
// The GameCube keeps a save's displayed metadata inside the save file; this is
// where each piece goes, and what card_direntry has to be told so the system
// menu can find it. See the generator for the layout rules and where they were
// read from. The application's save struct is what actually declares these
// bytes; this header is the independent check that its offsets are legal.
//
// `constexpr` at namespace scope, so every name here is internally linked and
// including this in more than one translation unit is not an ODR problem.
#pragma once

#include <intsh>

namespace save::ogc::layout {

    using namespace br0::intsh;

    /// @brief Bytes the layout below actually occupies.
    constexpr u32 required = ${_required}u;

    /// @brief The 64-byte comment block: a 32-byte name, then a 32-byte description.
    constexpr u32 commentAddr  = ${_comment_addr}u;
    constexpr u32 commentBytes = ${_comment_bytes}u;
    constexpr u32 commentField = 32u;

    /// @brief Magic + version, between the comment block and the image data.
    constexpr u32 stampAddr  = ${_stamp_addr}u;
    constexpr u32 stampBytes = ${_stamp_bytes}u;

    /// @brief Where libogc starts deriving every image offset from. Must be < 512.
    constexpr u32 iconAddr = ${_icon_addr}u;

    /// @brief Banner image and palette, or 0xFFFFFFFF when there is no banner.
    constexpr u32 bannerAddr     = ${_banner_literal};
    constexpr u32 bannerTlutAddr = ${_banner_tlut_literal};
    constexpr u32 bannerBytes    = ${_banner_bytes}u;

    /// @brief Icon frames, laid out back to back after the banner.
    constexpr u32 frameCount = ${_frames}u;
    constexpr u32 frameBytes = ${_frame_bytes}u;
    constexpr u32 framesAddr = ${_frames_addr}u;

    /// @brief The palette every frame shares, or 0xFFFFFFFF for a format with none.
    constexpr u32 tlutAddr  = ${_tlut_literal};
    constexpr u32 tlutBytes = ${_tlut_bytes}u;

    /// @brief The three packed card_direntry fields, ready to hand to CARD_SetStatus.
    constexpr u8  bannerFmt = ${_banner_fmt}u;
    constexpr u16 iconFmt   = ${_icon_fmt}u;
    constexpr u16 iconSpeed = ${_icon_speed}u;

    // Re-asserted in C++ rather than left to the generator alone: these are the
    // two rules CARD_SetStatus rejects with CARD_ERROR_FATAL_ERROR, and a
    // hand-edited copy of this file should not get past them either.
    static_assert(iconAddr < 512u,
        \"card_direntry.icon_addr must be below 512\");
    static_assert(commentAddr % 8192u <= 8192u - commentBytes,
        \"the comment block may not straddle an 8192-byte block boundary\");
    static_assert(commentAddr + commentBytes <= stampAddr,
        \"the comment block overlaps the stamp\");
    static_assert(stampAddr + stampBytes <= iconAddr,
        \"the stamp overlaps the image data\");
}
")

message(STATUS
    "GameCube save layout: ${_required}/${HEADER_SIZE} bytes used -- "
    "${_frames} ${_icon_format} frame(s), banner ${_banner_bytes} bytes")
