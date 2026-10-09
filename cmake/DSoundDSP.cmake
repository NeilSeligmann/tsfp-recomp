# T1179 real private GP core; T1242 optional owned CPU bridge. No host PCM policy.
set(TSFP_XEMU_DSP_SOURCE "" CACHE PATH "Pinned ignored xemu checkout for private GP core")
if(TSFP_XEMU_DSP_SOURCE)
  find_package(Git REQUIRED)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${TSFP_XEMU_DSP_SOURCE}" rev-parse HEAD
    OUTPUT_VARIABLE _tsfp_dsp_pin OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _tsfp_dsp_git)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${TSFP_XEMU_DSP_SOURCE}" diff --quiet HEAD -- hw/xbox/mcpx/apu/dsp
    RESULT_VARIABLE _tsfp_dsp_dirty)
  if(NOT _tsfp_dsp_git EQUAL 0 OR NOT _tsfp_dsp_dirty EQUAL 0 OR
     NOT _tsfp_dsp_pin STREQUAL "478b4f496102379c7eaa7f3ec10e714a703c4300")
    message(FATAL_ERROR "T1179 requires clean pinned xemu DSP source 478b4f496102379c7eaa7f3ec10e714a703c4300")
  endif()
  set(_tsfp_dsp "${TSFP_XEMU_DSP_SOURCE}/hw/xbox/mcpx/apu/dsp")
  add_library(tsfp_dsound_dsp STATIC
    src/audio/dsound_effects_cipher.c src/audio/dsound_effects_dsp.c
    "${_tsfp_dsp}/interp/dsp_cpu.c" "${_tsfp_dsp}/dsp_dma.c")
  # Upstream source is preserved verbatim; project wrappers retain strict warnings.
  set_source_files_properties("${_tsfp_dsp}/interp/dsp_cpu.c" "${_tsfp_dsp}/dsp_dma.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-error")
  target_include_directories(tsfp_dsound_dsp PRIVATE src/audio/dsp_compat "${_tsfp_dsp}")
  target_include_directories(tsfp_dsound_dsp PUBLIC src/audio)
  target_compile_definitions(tsfp_xbox PRIVATE TSFP_DSOUND_DSP_ENABLED=1)
  target_link_libraries(tsfp_xbox PRIVATE tsfp_dsound_dsp)
  target_link_libraries(tsfp_dsound_dsp PUBLIC pthread)
  set_target_properties(tsfp_dsound_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
