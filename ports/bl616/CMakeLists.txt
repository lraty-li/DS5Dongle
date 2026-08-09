cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

set(DS5_OPUS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../lib/opus)
include(${DS5_OPUS_ROOT}/cmake/OpusFunctions.cmake)

get_opus_sources(OPUS_SOURCES ${DS5_OPUS_ROOT}/opus_sources.mk
                 DS5_OPUS_CORE_SOURCES)
get_opus_sources(CELT_SOURCES ${DS5_OPUS_ROOT}/celt_sources.mk
                 DS5_OPUS_CELT_SOURCES)
get_opus_sources(SILK_SOURCES ${DS5_OPUS_ROOT}/silk_sources.mk
                 DS5_OPUS_SILK_SOURCES)
get_opus_sources(SILK_SOURCES_FIXED ${DS5_OPUS_ROOT}/silk_sources.mk
                 DS5_OPUS_SILK_FIXED_SOURCES)

set(DS5_OPUS_SOURCES)
foreach(DS5_OPUS_SOURCE IN LISTS
        DS5_OPUS_CORE_SOURCES
        DS5_OPUS_CELT_SOURCES
        DS5_OPUS_SILK_SOURCES
        DS5_OPUS_SILK_FIXED_SOURCES)
    list(APPEND DS5_OPUS_SOURCES ${DS5_OPUS_ROOT}/${DS5_OPUS_SOURCE})
endforeach()

# Keep the repository's Opus 1.6.1 wire-format implementation used by the
# RP2350 build, but compile its integer backend.  The generic float backend
# misses the BL616 deadline by more than 2x; BouffaloSDK's older 1.3-fixed
# archive is fast but its output makes the DualSense terminate the link.

# The 10 ms, 160 kbit/s stereo stream selects Opus' CELT path.  Keep its
# measured encoder working set (plus the audio task that calls it) in on-chip
# RAM so flash-cache stalls do not consume the remaining real-time margin.
set(DS5_APP_TCM_OBJECTS
    ds5_usb_audio.cpp.obj
)
set(DS5_OPUS_TCM_OBJECTS
    opus_encoder.c.obj
    bands.c.obj
    celt.c.obj
    celt_encoder.c.obj
    celt_lpc.c.obj
    cwrs.c.obj
    entcode.c.obj
    entenc.c.obj
    kiss_fft.c.obj
    laplace.c.obj
    mathops.c.obj
    mdct.c.obj
    modes.c.obj
    pitch.c.obj
    quant_bands.c.obj
    rate.c.obj
    vq.c.obj
)

set(DS5_OPUS_TCM_TEXT_RULES "")
set(DS5_OPUS_TCM_RODATA_RULES "")
foreach(DS5_APP_TCM_OBJECT IN LISTS DS5_APP_TCM_OBJECTS)
    string(APPEND DS5_OPUS_TCM_TEXT_RULES
           "        *libapp.a:${DS5_APP_TCM_OBJECT}(.text*)\n")
    string(APPEND DS5_OPUS_TCM_RODATA_RULES
           "        *libapp.a:${DS5_APP_TCM_OBJECT}(.rodata*)\n"
           "        *libapp.a:${DS5_APP_TCM_OBJECT}(.srodata*)\n")
endforeach()
foreach(DS5_OPUS_TCM_OBJECT IN LISTS DS5_OPUS_TCM_OBJECTS)
    string(APPEND DS5_OPUS_TCM_TEXT_RULES
           "        *libapp.a:${DS5_OPUS_TCM_OBJECT}(.text*)\n")
    string(APPEND DS5_OPUS_TCM_RODATA_RULES
           "        *libapp.a:${DS5_OPUS_TCM_OBJECT}(.rodata*)\n"
           "        *libapp.a:${DS5_OPUS_TCM_OBJECT}(.srodata*)\n")
endforeach()

# Derive the application linker script from the SDK's board script so memory
# layout changes in the pinned SDK remain authoritative. The two insertions
# are consumed by the board's existing startup copy table for tcm_text and
# tcm_rodata; no runtime copy code or SDK source modification is needed.
set(DS5_BOARD_LINKER_SCRIPT
    $ENV{BL_SDK_BASE}/bsp/board/bl616dk/bl616_common.ld.in)
file(READ ${DS5_BOARD_LINKER_SCRIPT} DS5_LINKER_SCRIPT_CONTENT)
string(FIND "${DS5_LINKER_SCRIPT_CONTENT}" "        *(.tcm_code*)"
       DS5_TCM_TEXT_MARKER)
string(FIND "${DS5_LINKER_SCRIPT_CONTENT}" "        *(.tcm_const*)"
       DS5_TCM_RODATA_MARKER)
if((DS5_TCM_TEXT_MARKER EQUAL -1) OR (DS5_TCM_RODATA_MARKER EQUAL -1))
    message(FATAL_ERROR
            "BL616 board linker script no longer exposes TCM insertion points")
endif()
string(REPLACE "        *(.tcm_code*)"
       "        *(.tcm_code*)\n${DS5_OPUS_TCM_TEXT_RULES}"
       DS5_LINKER_SCRIPT_CONTENT "${DS5_LINKER_SCRIPT_CONTENT}")
string(REPLACE "        *(.tcm_const*)"
       "        *(.tcm_const*)\n${DS5_OPUS_TCM_RODATA_RULES}"
       DS5_LINKER_SCRIPT_CONTENT "${DS5_LINKER_SCRIPT_CONTENT}")
set(DS5_LINKER_SCRIPT
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ds5_bl616_linker.ld.in)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(WRITE ${DS5_LINKER_SCRIPT} "${DS5_LINKER_SCRIPT_CONTENT}")
sdk_set_linker_script_macro(${DS5_LINKER_SCRIPT})

sdk_add_include_directories(
    .
    bluetooth
    config
    debug
    ds5
    platform
    usb
    ${DS5_OPUS_ROOT}
    ${DS5_OPUS_ROOT}/celt
    ${DS5_OPUS_ROOT}/include
    ${DS5_OPUS_ROOT}/silk
    ${DS5_OPUS_ROOT}/silk/fixed
)

target_sources(app PRIVATE
    bluetooth/ds5_bt.c
    bluetooth/ds5_bt_policy.c
    bluetooth/ds5_l2cap.c
    debug/ds5_log.c
    ds5/ds5_feature_cache.c
    ds5/ds5_protocol.c
    platform/ds5_audio_mailbox.c
    platform/ds5_cpp_runtime.cpp
    platform/ds5_feature_set_mailbox.c
    platform/ds5_haptics_mailbox.c
    platform/ds5_input_mailbox.c
    platform/ds5_output_mailbox.c
    usb/ds5_usb_audio.cpp
    usb/ds5_usb_audio_adapter.c
    usb/ds5_usb_hid.c
    usb/ds5_usb_log.c
    ${DS5_OPUS_SOURCES}
)

set_source_files_properties(
    ${DS5_OPUS_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS
    "OPUS_BUILD;VAR_ARRAYS;FIXED_POINT;DISABLE_FLOAT_API;HAVE_LRINT;HAVE_LRINTF;PACKAGE_VERSION=\"1.6.1-9-g2d862ea1\""
)

set_source_files_properties(
    usb/ds5_usb_audio.cpp
    PROPERTIES COMPILE_OPTIONS "-fsigned-char"
)

sdk_set_main_file(main.c)

project(ds5dongle_bl616)

# The WDL resampler is C++ but does not use the C++ standard library.  Keep the
# firmware link on the SDK's bare-metal C runtime instead of pulling in
# libstdc++ exception/TLS support through the g++ driver.
set_target_properties(ds5dongle_bl616_bl616.elf PROPERTIES LINKER_LANGUAGE C)
set(CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "")
