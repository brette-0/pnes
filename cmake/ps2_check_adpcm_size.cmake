# Build-time guard for the PS2 SPU2 ADPCM music sample.
#
# Run via `cmake -DADP=<file> -DMAX=<bytes> -P` from the ps2 branch's encode
# rule. Exists because oversizing this sample does NOT produce a runtime error
# to detect: audsrv_load_adpcm() does not reject a sample too big for the
# SPU2's sound RAM, it hangs the console during audio::Init -- measured on
# hardware, a 1.13MB sample never reached the title screen while 756KB played
# fine. Without this check the only symptom of nudging PS2_ADPCM_RATE or
# PS2_ADPCM_SECONDS too far is an ELF that boots to nothing, which is a
# miserable thing to debug. Fail the build instead.
if(NOT EXISTS "${ADP}")
    message(FATAL_ERROR "ps2 adpcm: expected encoder output '${ADP}' is missing")
endif()
file(SIZE "${ADP}" _adp_size)
if(_adp_size GREATER MAX)
    message(FATAL_ERROR
        "ps2 adpcm: encoded music sample is ${_adp_size} bytes, over the ${MAX} byte "
        "limit for SPU2 sound RAM.\n"
        "   A sample this large hangs the console in audio::Init rather than failing "
        "cleanly, so this is a build error by design.\n"
        "   Lower PS2_ADPCM_RATE or PS2_ADPCM_SECONDS, or trim the source asset to its "
        "musical loop. SPU2 ADPCM costs rate * seconds * 16/28 bytes "
        "(6300 B/s at 11025Hz, 12600 B/s at 22050Hz).\n"
        "   If you have measured a higher ceiling on hardware, raise "
        "PS2_ADPCM_MAX_BYTES -- but note the measured-good value is 756032 and the "
        "measured-bad one is 1134032.")
endif()
message(STATUS "ps2 adpcm: ${_adp_size} bytes (limit ${MAX})")
