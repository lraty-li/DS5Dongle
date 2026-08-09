cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

# Use BouffaloSDK's Opus 1.3-fixed archive.  It is built by the pinned SDK with
# the exact E907/P-extension ABI and -O2 flags; the repository's generic Opus
# 1.6.1 fixed-point build cannot sustain 160 kbit/s stereo on complex signals.
# Opus packet compatibility is version-independent, and the application also
# validates frame count, duration and channel count before publishing a packet.
set(DS5_SDK_OPUS_ROOT $ENV{BL_SDK_BASE}/components/multimedia/opus)
if(NOT EXISTS ${DS5_SDK_OPUS_ROOT}/libopus.a)
    message(FATAL_ERROR "Pinned BouffaloSDK Opus archive is missing")
endif()

# The 10 ms, 160 kbit/s stereo stream selects Opus' CELT path.  Keep its
# measured encoder/decoder working set (plus the audio task that calls it) in
# on-chip RAM so flash-cache stalls do not consume the real-time margin.
set(DS5_APP_TCM_OBJECTS
    ds5_usb_audio.cpp.obj
)
set(DS5_OPUS_TCM_OBJECTS
    opus.c.obj
    opus_encoder.c.obj
    opus_decoder.c.obj
    repacketizer.c.obj
    bands.c.obj
    celt.c.obj
    celt_decoder.c.obj
    celt_encoder.c.obj
    celt_lpc.c.obj
    cwrs.c.obj
    entcode.c.obj
    entdec.c.obj
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
set(DS5_RUNTIME_TCM_OBJECTS
    lib_abs.c.obj
    lib_memmove.c.obj
    lib_vikmemcpy.c.obj
    lib_memset.c.obj
)
set(DS5_LIBGCC_TCM_OBJECTS
    _clzsi2.o
    extendsfdf2.o
    floatsidf.o
    gedf2.o
    ledf2.o
    muldf3.o
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
           "        *libopus.a:${DS5_OPUS_TCM_OBJECT}(.text*)\n")
    string(APPEND DS5_OPUS_TCM_RODATA_RULES
           "        *libopus.a:${DS5_OPUS_TCM_OBJECT}(.rodata*)\n"
           "        *libopus.a:${DS5_OPUS_TCM_OBJECT}(.srodata*)\n")
endforeach()
foreach(DS5_RUNTIME_TCM_OBJECT IN LISTS DS5_RUNTIME_TCM_OBJECTS)
    string(APPEND DS5_OPUS_TCM_TEXT_RULES
           "        *liblibc.a:${DS5_RUNTIME_TCM_OBJECT}(.text*)\n")
    string(APPEND DS5_OPUS_TCM_RODATA_RULES
           "        *liblibc.a:${DS5_RUNTIME_TCM_OBJECT}(.rodata*)\n"
           "        *liblibc.a:${DS5_RUNTIME_TCM_OBJECT}(.srodata*)\n")
endforeach()
foreach(DS5_LIBGCC_TCM_OBJECT IN LISTS DS5_LIBGCC_TCM_OBJECTS)
    string(APPEND DS5_OPUS_TCM_TEXT_RULES
           "        *libgcc.a:${DS5_LIBGCC_TCM_OBJECT}(.text*)\n")
    string(APPEND DS5_OPUS_TCM_RODATA_RULES
           "        *libgcc.a:${DS5_LIBGCC_TCM_OBJECT}(.rodata*)\n"
           "        *libgcc.a:${DS5_LIBGCC_TCM_OBJECT}(.srodata*)\n")
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
    ${DS5_SDK_OPUS_ROOT}/include/opus
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
)

set_source_files_properties(
    usb/ds5_usb_audio.cpp
    PROPERTIES COMPILE_OPTIONS "-fsigned-char"
)

sdk_set_main_file(main.c)

project(ds5dongle_bl616)

# The audio bridge is C++ but does not use the C++ standard library. Keep the
# firmware link on the SDK's bare-metal C runtime instead of pulling in
# libstdc++ exception/TLS support through the g++ driver.
set_target_properties(ds5dongle_bl616_bl616.elf PROPERTIES LINKER_LANGUAGE C)
set(CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "")
