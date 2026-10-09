# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(tsfp_decomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# PIE is NOT optional. The guest address space is identity-mapped into the low
# 4 GB, and a non-PIE executable loads at 0x400000 -- inside the guest range --
# so the mapping fails with EEXIST. Verified both ways. Anyone passing -no-pie
# breaks the entire memory model, so fail the build rather than let it through.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CheckPIESupported)
check_pie_supported(OUTPUT_VARIABLE pie_output LANGUAGES C)
if(NOT CMAKE_C_LINK_PIE_SUPPORTED)
  message(FATAL_ERROR
    "This toolchain cannot produce a position-independent executable, which the "
    "guest identity mapping requires:\n${pie_output}")
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion
                    -Wstrict-prototypes -Werror)

add_library(tsfp_loader STATIC src/loader/xbe.c)
target_include_directories(tsfp_loader PUBLIC src/loader)

enable_testing()

add_executable(test_xbe_loader tests/c/test_xbe_loader.c)
target_link_libraries(test_xbe_loader PRIVATE tsfp_loader)
set_target_properties(test_xbe_loader PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xbe_loader COMMAND test_xbe_loader)

add_executable(xbe_dump src/loader/xbe_dump.c)
target_link_libraries(xbe_dump PRIVATE tsfp_loader)
set_target_properties(xbe_dump PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Compiles src/loader/xbe_dump.c INTO the test binary (included under a renamed
# main), so mutation testing can reach the dump tool: the XBE-gated pytest that
# also runs it is no mutation target, and a binary absent from ctest is the
# src/host/main.c reachability trap all over again (T383).
add_executable(test_xbe_dump tests/c/test_xbe_dump.c)
target_link_libraries(test_xbe_dump PRIVATE tsfp_loader)
target_compile_definitions(test_xbe_dump PRIVATE _DEFAULT_SOURCE)
set_target_properties(test_xbe_dump PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xbe_dump_tool COMMAND test_xbe_dump)

# T1289/T1510: the guest thread frame trace lives INSIDE libtsfp_xbox.a (added to its sources below) because the gpu
# library calls it and tests hand link only libtsfp_gpu.a + libtsfp_xbox.a. tsfp_gft is an alias target for its users.
find_package(Threads REQUIRED)

add_library(tsfp_xbox STATIC src/xbox/kernel_ordinals.c src/xbox/kernel_hle.c
                            src/xbox/kernel_call.c src/xbox/guest_mem.c
                            src/xbox/kernel_memory.c src/xbox/kernel_sync.c
                            src/xbox/kernel_thread.c
                            src/xbox/kernel_object.c
                            src/xbox/kernel_critsec.c
                            src/xbox/kernel_rtl.c src/xbox/kernel_pool.c
                            src/xbox/kernel_event.c
                            src/xbox/kernel_event_handle.c
                            src/xbox/kernel_hal.c
                            src/xbox/kernel_clock.c
                            src/xbox/kernel_config.c
                            src/xbox/kernel_file.c
                            src/xbox/kernel_file_object.c
                            src/xbox/kernel_io.c
                            src/xbox/kernel_async_io.c
                            src/xbox/kernel_crypto.c
                            src/xbox/kernel_dbg.c
                            src/xbox/kernel_fpstate.c
                            src/xbox/kernel_rtl_string.c
                            src/xbox/kernel_identity.c
                            src/xbox/kernel_xe.c
                            src/xbox/kernel_av.c
                            src/xbox/xonline_hle.c src/xbox/xonline_offline.c
                            src/xbox/xnet_hle.c src/xbox/xnet_collector.c src/xbox/xnet_collector_kernel.c src/xbox/xnet_volume.c src/xbox/xnet_eeprom.c src/xbox/xnet_config.c src/xbox/xnet_nvnet.c src/xbox/xnet_phy.c src/xbox/xnet_offline.c src/xbox/xnet_random.c
                            src/xbox/kernel_register_all.c
                            src/xbox/xdvdfs.c
                            src/host/guest_frame_trace.c)
target_include_directories(tsfp_xbox PRIVATE src/gpu)
add_library(tsfp_gft INTERFACE)
target_link_libraries(tsfp_gft INTERFACE tsfp_xbox)
target_include_directories(tsfp_xbox PUBLIC src/xbox)

# T1147 actual backing-device DATA41/42, no synthesized identity fallback.
target_sources(tsfp_xbox PRIVATE src/xbox/kernel_disk_identity.c src/host/disk_identity_linux.c src/host/disk_identity_xemu.c src/host/host_snapshot.c)
target_include_directories(tsfp_xbox PUBLIC src/host)

# The DirectSound HLE, contributed by a concurrent task and wired here because this
# file has a single owner. NO `if(EXISTS)` GUARD ON PURPOSE, unlike xdk_surface.c
# below: its 39-row surface table is compiled in rather than read from the gitignored
# generated surface, specifically so a fresh clone builds it. Depends on nothing in
# src/xbox/, so the ordering within the library does not matter.
target_sources(tsfp_xbox PRIVATE src/audio/dsound_hle.c src/audio/dsound_device.c src/audio/dsound_hrtf.c src/audio/dsound_effects_metadata.c src/audio/dsound_effects_binding.c src/audio/dsound_stream_scope.c src/audio/dsound_stream.c src/audio/dsound_stream_frequency.c src/audio/dsound_movie_stream.c src/audio/dsound_completion.c src/audio/dsound_buffer_scope.c src/audio/dsound_buffer.c src/audio/dsound_listener.c src/audio/dsound_adpcm.c src/audio/dsound_mixer.c src/audio/dsound_audio_runtime.c src/audio/dsound_mixbin_headroom.c)
target_include_directories(tsfp_xbox PUBLIC src/audio)

# The XAPI input HLE, contributed by a concurrent task, wired here for the same reason.
# The enumeration adapter uses kernel guest accessors; neither module requires
# generated artifacts, so both build in a fresh clone.
target_sources(tsfp_xbox PRIVATE src/input/xinput_hle.c src/input/xinput_devices.c src/input/mu_fatx.c src/input/mu_device.c src/input/xinput_source.c src/input/xinput_host_source.c src/input/xinput_record.c src/input/xinput_route.c src/input/xinput_route_nav.c src/input/xinput_nav_record.c src/input/xvoice_media.c)
target_include_directories(tsfp_xbox PUBLIC src/input)

# The kernel arity oracle, derived from nxdk's CC0-1.0 xboxkrnl.exe.def and contributed by
# a concurrent task. NO `if(EXISTS)` GUARD, like dsound_hle.c above and unlike the
# generated xdk_surface.c below: the table AND the .def it derives from are both committed,
# so a fresh clone builds it and `tests/test_arity_oracle.py` re-derives it. Depends on
# nothing but <stdbool.h>, so the ordering within the library does not matter.
target_sources(tsfp_xbox PRIVATE src/xbox/kernel_arity_oracle.c)

# Guest threads are host threads, so this library needs pthreads. PUBLIC because
# anything linking it inherits the -pthread link flag, which is not optional: a
# missing one does not fail to link, it links against glibc's no-op stubs and the
# mutexes silently stop excluding anything.
find_package(Threads REQUIRED)
target_link_libraries(tsfp_xbox PUBLIC Threads::Threads)

# WHY A FEATURE MACRO IS NEEDED HERE, and why it is this one.
#
# CMAKE_C_EXTENSIONS is OFF, so the compiler is invoked as -std=c11 and defines
# __STRICT_ANSI__. glibc reads that as "ISO C only" and withholds the default
# feature set -- which hides mmap's MAP_ANONYMOUS and, newly relevant,
# pthread_mutexattr_settype and PTHREAD_MUTEX_RECURSIVE. Those are POSIX, not GNU
# extensions; they are hidden by a strictness switch, not by portability.
#
# _DEFAULT_SOURCE rather than _POSIX_C_SOURCE or _GNU_SOURCE: it means exactly
# "give me what you would have given me without __STRICT_ANSI__", so it restores
# what guest_mem.c has always relied on instead of swapping it for a different set.
# _GNU_SOURCE would additionally admit genuine GNU extensions this code does not
# use and should not start using by accident.
target_compile_definitions(tsfp_xbox PRIVATE _DEFAULT_SOURCE)

# The XDK call surface is recovered from the user's own executable, so it is not
# committed. Build the targets that need it only when it has been generated, and
# say how to generate it rather than failing with a missing-file error.
if(EXISTS "${CMAKE_SOURCE_DIR}/src/xbox/xdk_surface.c")
  target_sources(tsfp_xbox PRIVATE src/xbox/xdk_surface.c)
  set(TSFP_HAVE_XDK_SURFACE ON)
else()
  set(TSFP_HAVE_XDK_SURFACE OFF)
  message(STATUS
    "src/xbox/xdk_surface.c absent -- skipping the surface target. Generate it with:\n"
    "    uv run python -m tools.gen_d3d8_surface <your.xbe> [--xtlid xtlid.xml]")
endif()

add_executable(test_kernel_ordinals tests/c/test_kernel_ordinals.c)
target_link_libraries(test_kernel_ordinals PRIVATE tsfp_xbox)
set_target_properties(test_kernel_ordinals PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_ordinals COMMAND test_kernel_ordinals)

add_executable(test_kernel_hle tests/c/test_kernel_hle.c)
target_link_libraries(test_kernel_hle PRIVATE tsfp_xbox)
set_target_properties(test_kernel_hle PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_hle COMMAND test_kernel_hle)

# T1146 actual statistics dispatch, fault prefixes and live mapping classifications.
add_executable(test_t1146_statistics tests/c/test_t1146_statistics.c)
target_link_libraries(test_t1146_statistics PRIVATE tsfp_xbox tsfp_loader)
target_compile_definitions(test_t1146_statistics PRIVATE T1146_FULL_THREAD=1)
target_include_directories(test_t1146_statistics PRIVATE tests/c)
# The fixture checks with assert(): keep them (and the variables only they read) active in Release.
target_compile_options(test_t1146_statistics PRIVATE -UNDEBUG)
add_test(NAME t1146_statistics COMMAND test_t1146_statistics)

add_executable(test_kernel_memory tests/c/test_kernel_memory.c)
target_link_libraries(test_kernel_memory PRIVATE tsfp_xbox)
set_target_properties(test_kernel_memory PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_memory COMMAND test_kernel_memory)

add_executable(test_contiguous_neighbour tests/c/test_contiguous_neighbour.c)
target_link_libraries(test_contiguous_neighbour PRIVATE tsfp_xbox)
set_target_properties(test_contiguous_neighbour PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME contiguous_neighbour COMMAND test_contiguous_neighbour)

add_executable(test_kernel_thread tests/c/test_kernel_thread.c)
target_link_libraries(test_kernel_thread PRIVATE tsfp_xbox)
set_target_properties(test_kernel_thread PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_thread COMMAND test_kernel_thread)

# The thread model: stacks, guard bands, the entry frame and the watchdog.
#
# DELIBERATELY FREE OF LIFTED CODE. `kernel_thread.c` takes the two host-specific
# operations as injected hooks, so this suite supplies fakes and runs in a fresh
# clone that has no generated C and no XBE. _DEFAULT_SOURCE for the same reason
# tsfp_xbox needs it, plus sigaction/sigsetjmp: the guard-band test proves the
# bands fault by catching a real SIGSEGV, which is the only way to tell a guard
# that is enforced from one that is merely recorded.
add_executable(test_guest_thread tests/c/test_guest_thread.c)
target_link_libraries(test_guest_thread PRIVATE tsfp_xbox)
target_compile_definitions(test_guest_thread PRIVATE _DEFAULT_SOURCE)
set_target_properties(test_guest_thread PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME guest_thread COMMAND test_guest_thread)

# The object at Prcb+0x250: the field whose zero routed this title's main thread
# into the real kernel's PE headers and a fault at 0x8001003C.
#
# DELIBERATELY FREE OF LIFTED CODE, like test_guest_thread above. The guest's gate
# is `cmp dword ptr [eax+0x250], 0` over memory our own code writes, so which way
# that branch goes is checkable by reading the same addresses the guest reads --
# no generated C and no XBE required. _DEFAULT_SOURCE for the same reason tsfp_xbox
# needs it (that definition is PRIVATE and does not propagate).
add_executable(test_prcb_monitor tests/c/test_prcb_monitor.c)
target_link_libraries(test_prcb_monitor PRIVATE tsfp_xbox)
target_compile_definitions(test_prcb_monitor PRIVATE _DEFAULT_SOURCE)
set_target_properties(test_prcb_monitor PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME prcb_monitor COMMAND test_prcb_monitor)
# A mutation in this module's thread path can deadlock in join_all rather than
# returning a wrong answer, and a hung test reports nothing. With a timeout the
# same mutation becomes a named failing test.
set_tests_properties(prcb_monitor PROPERTIES TIMEOUT 120)

add_executable(test_kernel_guest_copy tests/c/test_kernel_guest_copy.c)
target_link_libraries(test_kernel_guest_copy PRIVATE tsfp_xbox)
set_target_properties(test_kernel_guest_copy PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_guest_copy COMMAND test_kernel_guest_copy)
set_tests_properties(kernel_guest_copy PROPERTIES TIMEOUT 30)

# T1289: the per vblank interval guest thread trace.
add_executable(test_guest_frame_trace tests/c/test_guest_frame_trace.c)
target_link_libraries(test_guest_frame_trace PRIVATE tsfp_gft)
add_test(NAME guest_frame_trace COMMAND test_guest_frame_trace)
set_tests_properties(guest_frame_trace PROPERTIES TIMEOUT 60)

# T1289: the one pass small access fast path and the 4096 slot probe cache.
add_executable(test_kernel_guest_small_fast tests/c/test_kernel_guest_small_fast.c)
target_link_libraries(test_kernel_guest_small_fast PRIVATE tsfp_xbox)
set_target_properties(test_kernel_guest_small_fast PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_guest_small_fast COMMAND test_kernel_guest_small_fast)
set_tests_properties(kernel_guest_small_fast PROPERTIES TIMEOUT 60)

# T81: the readability probe behind kernel_guest_at (PROT_NONE guard pages refused).
add_executable(test_kernel_guest_probe tests/c/test_kernel_guest_probe.c)
target_link_libraries(test_kernel_guest_probe PRIVATE tsfp_xbox)
set_target_properties(test_kernel_guest_probe PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_guest_probe COMMAND test_kernel_guest_probe)

# T827: guest reads and writes as plain copies, the quiescence protocol behind them.
add_executable(test_kernel_guest_fast_copy tests/c/test_kernel_guest_fast_copy.c)
target_link_libraries(test_kernel_guest_fast_copy PRIVATE tsfp_xbox Threads::Threads)
set_target_properties(test_kernel_guest_fast_copy PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_guest_fast_copy COMMAND test_kernel_guest_fast_copy)
set_tests_properties(kernel_guest_probe PROPERTIES TIMEOUT 30)

add_executable(test_kernel_sync tests/c/test_kernel_sync.c)
target_link_libraries(test_kernel_sync PRIVATE tsfp_xbox)
set_target_properties(test_kernel_sync PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_sync COMMAND test_kernel_sync)

# Critical sections: ordinals 277 Enter, 294 Leave, 291 Initialize.
#
# DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. Every critical section in this
# suite lives in scratch memory from the region allocator, never at an image VA
# like 0x00549148, so the suite runs in a clone with no generated C.
#
# _DEFAULT_SOURCE for the same reason tsfp_xbox needs it (that definition is
# PRIVATE and does not propagate), plus nanosleep: the mutual-exclusion case proves
# a second thread really blocks by checking it has NOT got in yet, which needs a
# real sleep rather than a counter.
#
# TIMEOUT because the failure mode of a mutation in this module is a deadlock, and
# a hung test that never reports is worth nothing. With a timeout the same mutation
# becomes a named failing test.
add_executable(test_kernel_clock tests/c/test_kernel_clock.c)
target_link_libraries(test_kernel_clock PRIVATE tsfp_xbox)
set_target_properties(test_kernel_clock PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_clock COMMAND test_kernel_clock)

add_executable(test_kernel_rtl tests/c/test_kernel_rtl.c)
target_link_libraries(test_kernel_rtl PRIVATE tsfp_xbox)
set_target_properties(test_kernel_rtl PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_rtl COMMAND test_kernel_rtl)

add_executable(test_kernel_rtl_string tests/c/test_kernel_rtl_string.c)
target_link_libraries(test_kernel_rtl_string PRIVATE tsfp_xbox)
set_target_properties(test_kernel_rtl_string PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_rtl_string COMMAND test_kernel_rtl_string)

add_executable(test_kernel_fpstate tests/c/test_kernel_fpstate.c)
target_link_libraries(test_kernel_fpstate PRIVATE tsfp_xbox Threads::Threads)
set_target_properties(test_kernel_fpstate PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_fpstate COMMAND test_kernel_fpstate)
set_tests_properties(kernel_fpstate PROPERTIES TIMEOUT 60)

add_executable(test_kernel_dbg tests/c/test_kernel_dbg.c)
target_link_libraries(test_kernel_dbg PRIVATE tsfp_xbox)
set_target_properties(test_kernel_dbg PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_dbg COMMAND test_kernel_dbg)

# The object group's FIRST suite. NtClose and the two Obf* ordinals were previously
# covered only incidentally, from test_guest_thread and the file suites; ordinal 246 is
# the first handler here with failure paths worth reaching on purpose.
add_executable(test_kernel_object tests/c/test_kernel_object.c)
target_link_libraries(test_kernel_object PRIVATE tsfp_xbox)
set_target_properties(test_kernel_object PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_object COMMAND test_kernel_object)

# T851 keeps the NtReadFile Event HANDLE review's generation/type controls independent.
add_executable(test_t851_event_handle_review tests/c/test_t851_event_handle_review.c)
target_link_libraries(test_t851_event_handle_review PRIVATE tsfp_xbox)
set_target_properties(test_t851_event_handle_review PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME t851_event_handle_review COMMAND test_t851_event_handle_review)

# Fabricated FILE_OBJECT bodies and ordinal 76. Builds its own synthetic XDVDFS
# image in /tmp, so the GDFX branch is proved without the user's disc.
add_executable(test_kernel_file_object tests/c/test_kernel_file_object.c)
target_compile_definitions(test_kernel_file_object PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_kernel_file_object PRIVATE tsfp_xbox)
set_target_properties(test_kernel_file_object PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_file_object COMMAND test_kernel_file_object)

add_executable(test_kernel_pool tests/c/test_kernel_pool.c)
target_link_libraries(test_kernel_pool PRIVATE tsfp_xbox)
set_target_properties(test_kernel_pool PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_pool COMMAND test_kernel_pool)

add_executable(test_kernel_event tests/c/test_kernel_event.c)
target_link_libraries(test_kernel_event PRIVATE tsfp_xbox)
set_target_properties(test_kernel_event PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_event COMMAND test_kernel_event)
# NtSetSystemTime (228): the guest wall clock is the host clock plus an offset the title sets.
add_executable(test_kernel_system_time tests/c/test_kernel_system_time.c)
target_link_libraries(test_kernel_system_time PRIVATE tsfp_xbox)
set_target_properties(test_kernel_system_time PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_system_time COMMAND test_kernel_system_time)
add_executable(test_kernel_event_handle tests/c/test_kernel_event_handle.c)
target_compile_definitions(test_kernel_event_handle PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_kernel_event_handle PRIVATE tsfp_thunk tsfp_xbox)
add_test(NAME kernel_event_handle COMMAND test_kernel_event_handle)

add_executable(test_kernel_mutant_dup tests/c/test_kernel_mutant_dup.c)
target_compile_definitions(test_kernel_mutant_dup PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_kernel_mutant_dup PRIVATE tsfp_xbox)
add_test(NAME kernel_mutant_dup COMMAND test_kernel_mutant_dup)
add_executable(test_kernel_sync_wait tests/c/test_kernel_sync_wait.c)
target_compile_definitions(test_kernel_sync_wait PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_kernel_sync_wait PRIVATE tsfp_xbox)
add_test(NAME kernel_sync_wait COMMAND test_kernel_sync_wait)

add_executable(test_kernel_critsec tests/c/test_kernel_critsec.c)
target_link_libraries(test_kernel_critsec PRIVATE tsfp_xbox)
target_compile_definitions(test_kernel_critsec PRIVATE _DEFAULT_SOURCE)
set_target_properties(test_kernel_critsec PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_critsec COMMAND test_kernel_critsec)
set_tests_properties(kernel_critsec PROPERTIES TIMEOUT 120)

# HAL ordinal 47, HalRegisterShutdownNotification: the registration set.
#
# DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. The handler reads two stack
# arguments from a synthetic frame built in scratch guest memory and keeps the
# registration set host-side, so there is nothing to look up in the image.
# Publishing IRQL into the guest's own copy at KPCR.Irql (fs:[0x24]).
#
# DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. All 6 guest read sites are the
# same `MEM8(XBOX_FS_BASE + 0x24)` load compared against DISPATCH_LEVEL, so the
# suite performs that load against the same address and asserts which arm the
# guest would take -- no generated C required. _DEFAULT_SOURCE for the same reason
# tsfp_xbox needs it (that definition is PRIVATE and does not propagate), plus
# pthread_create: the per-thread case is the one a publisher that captured one
# thread's `fs` base would fail, and it needs a real second thread to show it.
add_executable(test_irql_publish tests/c/test_irql_publish.c)
target_link_libraries(test_irql_publish PRIVATE tsfp_xbox)
target_compile_definitions(test_irql_publish PRIVATE _DEFAULT_SOURCE)
set_target_properties(test_irql_publish PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME irql_publish COMMAND test_irql_publish)
# A mutation in the publish path can leave the worker thread blocked rather than
# returning a wrong answer, and a hung test reports nothing.
set_tests_properties(irql_publish PROPERTIES TIMEOUT 120)

add_executable(test_kernel_hal tests/c/test_kernel_hal.c)
target_link_libraries(test_kernel_hal PRIVATE tsfp_xbox)
set_target_properties(test_kernel_hal PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_hal COMMAND test_kernel_hal)

# ExQueryNonVolatileSetting (24): the console configuration store and its policy
# for an index we hold no value for.
#
# DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. Five stack arguments read from a
# frame built in scratch guest memory, and the out-parameters are read back from the
# same scratch -- no image address is resolved.
add_executable(test_kernel_config tests/c/test_kernel_config.c)
target_link_libraries(test_kernel_config PRIVATE tsfp_xbox)
set_target_properties(test_kernel_config PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_config COMMAND test_kernel_config)

# NtOpenFile (202): OBJECT_ATTRIBUTES and OBJECT_STRING decoding, the openable set,
# and the policy for a name no volume is mounted behind.
#
# DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. The three guest structures are
# built in scratch guest memory from the offsets guest_structs.h derives, so the
# suite reads and writes the same bytes the guest does without resolving any image
# address.
add_executable(test_kernel_file tests/c/test_kernel_file.c)
target_link_libraries(test_kernel_file PRIVATE tsfp_xbox)
set_target_properties(test_kernel_file PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_file COMMAND test_kernel_file)

# The writable hard-disk backing store: --hdd, and NtCreateFile actually creating. The
# title was REBOOTING ITSELF because this host refused every create (measured at guest
# 0x00381321), so this suite covers the create path, the four refusals that keep the
# user's disc read-only and the guest inside its own directory, and the fact that
# WITHOUT a mount nothing changes.
#
# DELIBERATELY FREE OF LIFTED CODE, OF THE XBE AND OF ANY REAL DISC. The host side is a
# temporary directory the suite makes relative to its working directory and removes; the
# one disc is 70 KB of bytes the suite lays out itself. Nothing here skips.
#
# _DEFAULT_SOURCE for the reason tsfp_xbox needs it, and the same macro rather than a
# different one: the suite drives the host filesystem directly, with mkdtemp, openat,
# fstatat and symlink, and -std=c11 defines __STRICT_ANSI__ so glibc withholds all of
# them. Matching tsfp_xbox means the suite and the code under test see one feature set.
add_executable(test_hdd_backing tests/c/test_hdd_backing.c)
target_link_libraries(test_hdd_backing PRIVATE tsfp_xbox)
target_compile_definitions(test_hdd_backing PRIVATE _DEFAULT_SOURCE)
set_target_properties(test_hdd_backing PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME hdd_backing COMMAND test_hdd_backing)

# NtQueryFullAttributesFile (210): name resolution with no handle, and the 0x38-byte
# FILE_NETWORK_OPEN_INFORMATION it fills. Host-dir, disc and fabricated backings.
add_executable(test_kernel_attributes tests/c/test_kernel_attributes.c)
target_link_libraries(test_kernel_attributes PRIVATE tsfp_xbox)
target_compile_definitions(test_kernel_attributes PRIVATE _DEFAULT_SOURCE)
set_target_properties(test_kernel_attributes PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_attributes COMMAND test_kernel_attributes)

# NtDeleteFile (195): the XONLINE local-cache closer's delete, by name, through the same
# resolver, escape guard and cache-view gate as NtOpenFile. Host-dir, cache, disc, refusals.
add_executable(test_kernel_delete tests/c/test_kernel_delete.c)
target_link_libraries(test_kernel_delete PRIVATE tsfp_xbox)
target_compile_definitions(test_kernel_delete PRIVATE _DEFAULT_SOURCE)
set_target_properties(test_kernel_delete PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_delete COMMAND test_kernel_delete)

# NtQueryVolumeInformationFile (218), NtQueryInformationFile (211) and
# NtSetInformationFile (226): the 24-byte class-3 geometry structure whose field PRODUCT
# the guest validates against its own XBE InitFlags, and the file-position round trip.
#
# DELIBERATELY FREE OF LIFTED CODE, OF THE XBE AND OF ANY DISC. Frames are synthesised
# with kernel_frame_build in scratch guest memory and every structure is built at the
# offsets guest_structs.h derives, so no image address is resolved.
add_executable(test_kernel_io tests/c/test_kernel_io.c)
target_link_libraries(test_kernel_io PRIVATE tsfp_xbox)
set_target_properties(test_kernel_io PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_io COMMAND test_kernel_io)
add_executable(test_kernel_async_io tests/c/test_kernel_async_io.c)
target_link_libraries(test_kernel_async_io PRIVATE tsfp_xbox)
target_compile_definitions(test_kernel_async_io PRIVATE _DEFAULT_SOURCE)
set_target_properties(test_kernel_async_io PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_async_io COMMAND test_kernel_async_io)

# Xc* crypto ordinals 335/336/337 (SHA-1), 340 (HMAC-SHA1) and 346 (DES key parity).
# THE ORACLE IS EXTERNAL: every expected digest is a published FIPS 180-1 / RFC 2202 /
# FIPS 74 constant, never a value this implementation produced. A suite that compared the
# code to itself would prove only that it is deterministic.
#
# DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. Frames are built with
# kernel_frame_build in scratch guest memory, so no image address is resolved.
add_executable(test_kernel_crypto tests/c/test_kernel_crypto.c)
target_link_libraries(test_kernel_crypto PRIVATE tsfp_xbox)
set_target_properties(test_kernel_crypto PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_crypto COMMAND test_kernel_crypto)

# XeLoadSection (327) and XeUnloadSection (328): XBE section residency, reference counted
# in GUEST MEMORY at Section+0x18 where hardware keeps it rather than in a host shadow,
# because two sources of truth is how a bookkeeping bug survives its own tests.
#
# DELIBERATELY FREE OF LIFTED CODE, THE XBE AND ANY DISC: the suite lays out its own
# section headers in scratch guest memory.
add_executable(test_kernel_xe tests/c/test_kernel_xe.c)
target_link_libraries(test_kernel_xe PRIVATE tsfp_xbox)
set_target_properties(test_kernel_xe PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_xe COMMAND test_kernel_xe)

# Av* ordinals 1-4 (src/xbox/kernel_av.h): the AV pack is FABRICATED and operator-chosen.
# DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE: the suite lays out its own XBE header.
add_executable(test_kernel_av tests/c/test_kernel_av.c)
target_link_libraries(test_kernel_av PRIVATE tsfp_xbox)
set_target_properties(test_kernel_av PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_av COMMAND test_kernel_av)

# XDVDFS (src/xbox/xdvdfs.h): the filesystem on the user's own disc image.
#
# DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. The suite builds its own synthetic
# images in temporary files -- a real binary tree with nonzero links, a nil link encoded
# as 0 and another as 0xFFFF, a non-sector-aligned directory, 0xFF padding and a
# deliberate cycle -- so the walk, the bounds and the nil handling are all exercised
# without any disc present.
#
# The ground-truth half of it is CONDITIONAL AT RUNTIME, not at configure time: it
# checks for the user's image and skips, still passing, when it is absent. The disc is
# the user's property, `tools/ci/check-no-disc-data.sh` bans committing anything derived
# from it, and a suite that failed without it would make a clean checkout look broken.
add_executable(test_xdvdfs_c tests/c/test_xdvdfs_c.c)
target_link_libraries(test_xdvdfs_c PRIVATE tsfp_xbox)
set_target_properties(test_xdvdfs_c PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xdvdfs_c COMMAND test_xdvdfs_c)
# The image is 4 GB and one case reads near its end, so this is not instant on a cold
# page cache. A hung test reports nothing, so it gets a bound rather than none.
set_tests_properties(xdvdfs_c PROPERTIES TIMEOUT 300)

# THE SAME BINARY, REGISTERED A SECOND TIME, so the ground truth gets its own status.
#
# A skip that only reaches stdout is invisible: `ctest` printed "Passed" for this suite
# whether the disc had been read or not, and the green summary is what anyone actually
# looks at. One registration could not express both halves honestly -- exit 0 on a
# missing disc overstated what ran, and exit 77 would have thrown away the 185
# synthetic checks that genuinely did. So the synthetic half is `xdvdfs_c` above and
# always runs, and the ground truth is this, which reports SKIPPED when the user's own
# image is absent.
add_test(NAME xdvdfs_c_ground_truth COMMAND test_xdvdfs_c --require-disc)
set_tests_properties(xdvdfs_c_ground_truth PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# THE DISC SEAM, end to end: a guest drive letter becomes real bytes off the user's own
# disc, through the symbolic-link table, the mount table, XDVDFS and NtReadFile.
#
# CONDITIONAL AT RUNTIME, NOT AT CONFIGURE TIME, and it announces the skip. The image is
# the user's property and is never committed. A skipped run prints that NOTHING WAS
# PROVED, so a green suite without the disc is not mistaken for a green suite with it.
add_executable(test_disc_seam tests/c/test_disc_seam.c)
target_link_libraries(test_disc_seam PRIVATE tsfp_xbox)
set_target_properties(test_disc_seam PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME disc_seam COMMAND test_disc_seam)
# One case reads near the 1 GB mark of a 4 GB image, which is not instant on a cold page
# cache. A hung test reports nothing, so it gets a bound rather than none.
#
# SKIP_RETURN_CODE 77 is not cosmetic. Every case in this suite needs the user's disc,
# so without one it proves NOTHING -- and it used to exit 0 and be reported as
# "Passed", which is the one thing a run that proved nothing must not be called.
set_tests_properties(disc_seam PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# The DirectSound HLE's own suite. Contributed alongside src/audio/dsound_hle.c and
# wired here for the same reason.
add_executable(test_dsound_hle tests/c/test_dsound_hle.c)
target_link_libraries(test_dsound_hle PRIVATE tsfp_xbox)
set_target_properties(test_dsound_hle PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_hle COMMAND test_dsound_hle)

# The arity oracle's own suite. Needs nothing generated, by design: the oracle is
# committed precisely so it is checked in a fresh clone.
add_executable(test_arity_oracle tests/c/test_arity_oracle.c)
target_link_libraries(test_arity_oracle PRIVATE tsfp_xbox)
set_target_properties(test_arity_oracle PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME arity_oracle COMMAND test_arity_oracle)

# The XAPI input HLE's own suite.
#
# THE `add_test` LINE IS THE LOAD-BEARING ONE, and we have the scar to prove it:
# src/gpu/* and its four suites were committed with no CMake wiring at all and had
# NEVER ONCE RUN until the block below was added. `tools/coverage.py` counts
# `add_test(NAME ...)`, so a suite that is built but not registered is invisible to
# both ctest and the coverage figure -- it looks like coverage while providing none.
add_executable(test_xinput_hle tests/c/test_xinput_hle.c)
target_link_libraries(test_xinput_hle PRIVATE tsfp_xbox)
set_target_properties(test_xinput_hle PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xinput_hle COMMAND test_xinput_hle)

add_executable(test_t1086_voice_media tests/c/test_t1086_voice_media.c)
target_link_libraries(test_t1086_voice_media PRIVATE tsfp_xbox)
set_target_properties(test_t1086_voice_media PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME t1086_voice_media COMMAND test_t1086_voice_media)

add_executable(test_xinput_devices tests/c/test_xinput_devices.c)
target_link_libraries(test_xinput_devices PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_xinput_devices PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xinput_devices COMMAND test_xinput_devices)

add_executable(test_xinput_pad tests/c/test_xinput_pad.c)
target_link_libraries(test_xinput_pad PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_xinput_pad PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xinput_pad COMMAND test_xinput_pad)

add_executable(test_dsound_adpcm tests/c/test_dsound_adpcm.c)
target_link_libraries(test_dsound_adpcm PRIVATE tsfp_xbox)
add_test(NAME dsound_adpcm COMMAND test_dsound_adpcm)

add_executable(test_dsound_mixer tests/c/test_dsound_mixer.c)
target_link_libraries(test_dsound_mixer PRIVATE tsfp_xbox)
set_target_properties(test_dsound_mixer PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_mixer COMMAND test_dsound_mixer)

add_executable(test_xinput_source tests/c/test_xinput_source.c)
target_link_libraries(test_xinput_source PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_xinput_source PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xinput_source COMMAND test_xinput_source)
add_executable(test_xinput_record tests/c/test_xinput_record.c)
target_link_libraries(test_xinput_record PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_xinput_record PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xinput_record COMMAND test_xinput_record)
add_executable(test_host_snapshot tests/c/test_host_snapshot.c)
target_link_libraries(test_host_snapshot PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_host_snapshot PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME host_snapshot COMMAND test_host_snapshot)
add_executable(test_xinput_host_source tests/c/test_xinput_host_source.c)
target_link_libraries(test_xinput_host_source PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_xinput_host_source PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xinput_host_source COMMAND test_xinput_host_source)

add_executable(test_dsound_device tests/c/test_dsound_device.c)
target_link_libraries(test_dsound_device PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_dsound_device PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_device COMMAND test_dsound_device)

add_executable(test_dsound_device_movie_calls tests/c/test_dsound_device_movie_calls.c)
target_link_libraries(test_dsound_device_movie_calls PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_dsound_device_movie_calls PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_device_movie_calls COMMAND test_dsound_device_movie_calls)

add_executable(test_dsound_device_lease tests/c/test_dsound_device_lease.c)
target_link_libraries(test_dsound_device_lease PRIVATE tsfp_xbox tsfp_gpu)
target_link_options(test_dsound_device_lease PRIVATE -Wl,--wrap=malloc)
set_target_properties(test_dsound_device_lease PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_device_lease COMMAND test_dsound_device_lease)

add_executable(test_dsound_device_identity tests/c/test_dsound_device_identity.c)
target_link_libraries(test_dsound_device_identity PRIVATE tsfp_xbox tsfp_gpu)
target_link_options(test_dsound_device_identity PRIVATE -Wl,--wrap=mmap)
set_target_properties(test_dsound_device_identity PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_device_identity COMMAND test_dsound_device_identity)

add_executable(test_dsound_listener tests/c/test_dsound_listener.c)
target_link_libraries(test_dsound_listener PRIVATE tsfp_xbox tsfp_gpu)
target_link_options(test_dsound_listener PRIVATE -Wl,--wrap=mmap)
set_target_properties(test_dsound_listener PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_listener COMMAND test_dsound_listener)
add_executable(test_dsound_listener_commit tests/c/test_dsound_listener_commit.c)
target_link_libraries(test_dsound_listener_commit PRIVATE tsfp_xbox tsfp_gpu)
target_link_options(test_dsound_listener_commit PRIVATE -Wl,--wrap=mmap)
set_target_properties(test_dsound_listener_commit PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_listener_commit COMMAND test_dsound_listener_commit)

add_executable(test_dsound_listener_work tests/c/test_dsound_listener_work.c)
target_link_libraries(test_dsound_listener_work PRIVATE tsfp_xbox tsfp_gpu)
target_link_options(test_dsound_listener_work PRIVATE -Wl,--wrap=mmap)
set_target_properties(test_dsound_listener_work PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_listener_work COMMAND test_dsound_listener_work)

add_executable(test_dsound_stream tests/c/test_dsound_stream.c)
target_link_libraries(test_dsound_stream PRIVATE tsfp_xbox tsfp_gpu)
target_link_options(test_dsound_stream PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc
  -Wl,--wrap=guest_heap_create -Wl,--wrap=guest_heap_alloc -Wl,--wrap=guest_heap_destroy)
set_target_properties(test_dsound_stream PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_stream COMMAND test_dsound_stream)
add_executable(test_t1245_frequency_clock tests/c/t1245_frequency_clock.c)
target_link_libraries(test_t1245_frequency_clock PRIVATE tsfp_xbox m)
target_compile_options(test_t1245_frequency_clock PRIVATE -UNDEBUG)
add_test(NAME t1245_frequency_clock COMMAND test_t1245_frequency_clock)
add_executable(test_t1245_frequency_mixer tests/c/t1245_frequency_mixer.c)
target_link_libraries(test_t1245_frequency_mixer PRIVATE tsfp_xbox m)
target_compile_options(test_t1245_frequency_mixer PRIVATE -UNDEBUG)
add_test(NAME t1245_frequency_mixer COMMAND test_t1245_frequency_mixer)
add_executable(test_t1129_stream_routing tests/c/test_t1129_stream_routing.c)
target_link_libraries(test_t1129_stream_routing PRIVATE tsfp_xbox tsfp_gpu)
target_link_options(test_t1129_stream_routing PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc
  -Wl,--wrap=guest_heap_create -Wl,--wrap=guest_heap_alloc -Wl,--wrap=guest_heap_destroy)
set_target_properties(test_t1129_stream_routing PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME t1129_stream_routing COMMAND test_t1129_stream_routing)
add_executable(test_t1148_mixbin_headroom tests/c/test_t1148_mixbin_headroom.c)
target_link_libraries(test_t1148_mixbin_headroom PRIVATE tsfp_xbox tsfp_gpu)
target_link_options(test_t1148_mixbin_headroom PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc
  -Wl,--wrap=guest_heap_create -Wl,--wrap=guest_heap_alloc -Wl,--wrap=guest_heap_destroy)
set_target_properties(test_t1148_mixbin_headroom PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME t1148_mixbin_headroom COMMAND test_t1148_mixbin_headroom)


add_executable(test_dsound_movie_stream tests/c/test_dsound_movie_stream.c)
target_compile_definitions(test_dsound_movie_stream PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_dsound_movie_stream PRIVATE tsfp_xbox tsfp_gpu)
add_test(NAME dsound_movie_stream COMMAND test_dsound_movie_stream)
add_executable(test_dsound_stream_pause tests/c/test_dsound_stream_pause.c)
target_compile_definitions(test_dsound_stream_pause PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_dsound_stream_pause PRIVATE tsfp_xbox tsfp_gpu)
add_test(NAME dsound_stream_pause COMMAND test_dsound_stream_pause)
add_executable(test_dsound_stream_status tests/c/test_dsound_stream_status.c)
target_compile_definitions(test_dsound_stream_status PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_dsound_stream_status PRIVATE tsfp_xbox tsfp_gpu)
add_test(NAME dsound_stream_status COMMAND test_dsound_stream_status)

add_executable(test_dsound_stream_volume tests/c/test_dsound_stream_volume.c)
target_compile_definitions(test_dsound_stream_volume PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_dsound_stream_volume PRIVATE tsfp_xbox tsfp_gpu)
add_test(NAME dsound_stream_volume COMMAND test_dsound_stream_volume)

add_executable(test_dsound_completion tests/c/test_dsound_completion.c)
target_compile_definitions(test_dsound_completion PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_dsound_completion PRIVATE tsfp_xbox tsfp_gpu)
add_test(NAME dsound_completion COMMAND test_dsound_completion)
add_executable(test_t1102_buffer_audio tests/c/test_t1102_buffer_audio.c)
target_compile_definitions(test_t1102_buffer_audio PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_t1102_buffer_audio PRIVATE tsfp_xbox tsfp_gpu)
add_test(NAME t1102_buffer_audio COMMAND test_t1102_buffer_audio)
add_executable(test_dsound_audio_integration tests/c/test_dsound_audio_integration.c)
target_compile_definitions(test_dsound_audio_integration PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_dsound_audio_integration PRIVATE tsfp_xbox tsfp_gpu)
add_test(NAME dsound_audio_integration COMMAND test_dsound_audio_integration)
add_executable(test_dsound_stream_start_caller tests/c/test_dsound_stream_start_caller.c)
target_compile_definitions(test_dsound_stream_start_caller PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_dsound_stream_start_caller PRIVATE tsfp_xbox tsfp_gpu)
add_test(NAME dsound_stream_start_caller COMMAND test_dsound_stream_start_caller)
add_executable(test_dsound_stream_second tests/c/test_dsound_stream_second.c)
target_compile_definitions(test_dsound_stream_second PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_dsound_stream_second PRIVATE tsfp_xbox tsfp_gpu)
add_test(NAME dsound_stream_second COMMAND test_dsound_stream_second)

add_executable(test_dsound_stream_discontinuity tests/c/test_dsound_stream_discontinuity.c)
target_compile_definitions(test_dsound_stream_discontinuity PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_dsound_stream_discontinuity PRIVATE tsfp_xbox tsfp_gpu)
add_test(NAME dsound_stream_discontinuity COMMAND test_dsound_stream_discontinuity)

add_executable(test_dsound_stream_flush tests/c/test_dsound_stream_flush.c)
target_compile_definitions(test_dsound_stream_flush PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_dsound_stream_flush PRIVATE tsfp_xbox tsfp_gpu)
add_test(NAME dsound_stream_flush COMMAND test_dsound_stream_flush)

add_executable(test_dsound_buffer tests/c/test_dsound_buffer.c)
target_link_libraries(test_dsound_buffer PRIVATE tsfp_xbox tsfp_gpu)
target_link_options(test_dsound_buffer PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc
  -Wl,--wrap=guest_heap_create -Wl,--wrap=guest_heap_alloc -Wl,--wrap=guest_heap_destroy)
set_target_properties(test_dsound_buffer PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_buffer COMMAND test_dsound_buffer)

add_executable(test_t1182_stream_set_position tests/c/test_t1182_stream_set_position.c)
target_link_libraries(test_t1182_stream_set_position PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_t1182_stream_set_position PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME t1182_stream_set_position COMMAND test_t1182_stream_set_position)

add_executable(test_dsound_buffer_apply1_setters tests/c/dsound_buffer_apply1_setters_runner.c)
target_link_libraries(test_dsound_buffer_apply1_setters PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_dsound_buffer_apply1_setters PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_buffer_apply1_setters COMMAND test_dsound_buffer_apply1_setters)

add_executable(test_dsound_stream_scope tests/c/test_dsound_stream_scope.c)
target_link_libraries(test_dsound_stream_scope PRIVATE tsfp_xbox)
set_target_properties(test_dsound_stream_scope PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_stream_scope COMMAND test_dsound_stream_scope)

add_executable(test_dsound_buffer_scope tests/c/test_dsound_buffer_scope.c)
target_link_libraries(test_dsound_buffer_scope PRIVATE tsfp_xbox)
set_target_properties(test_dsound_buffer_scope PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_buffer_scope COMMAND test_dsound_buffer_scope)

add_executable(test_dsound_effects_binding tests/c/test_dsound_effects_binding.c)
target_link_libraries(test_dsound_effects_binding PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_dsound_effects_binding PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_effects_binding COMMAND test_dsound_effects_binding)

add_executable(test_dsound_hrtf tests/c/test_dsound_hrtf.c)
target_link_libraries(test_dsound_hrtf PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_dsound_hrtf PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_hrtf COMMAND test_dsound_hrtf)
# T452: assertions closing the survivors of the src/audio mutation set (tools/mutate/sets/dsound.py).
add_executable(test_dsound_gaps tests/c/test_dsound_gaps.c)
target_link_libraries(test_dsound_gaps PRIVATE tsfp_xbox tsfp_gpu)
target_link_options(test_dsound_gaps PRIVATE -Wl,--wrap=guest_heap_destroy)
set_target_properties(test_dsound_gaps PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_gaps COMMAND test_dsound_gaps)
# T452: the buffer stop-boundary probe with the host symbol ABSENT (every other buffer suite defines it).
add_executable(test_dsound_stops_absent tests/c/test_dsound_stops_absent.c)
target_link_libraries(test_dsound_stops_absent PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_dsound_stops_absent PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME dsound_stops_absent COMMAND test_dsound_stops_absent)


# ---------------------------------------------------------------------------
# The GPU / Direct3D 8 HLE, contributed by a concurrent task.
#
# ITS OWN LIBRARY, not folded into tsfp_xbox, for two reasons. It has no dependency on
# src/xbox/ -- d3d8_surface_adapter.h says so explicitly and mirrors the generated
# surface row shape rather than including its header, precisely to stay independent -- and
# keeping it separate means a GPU build failure cannot take the kernel suites down with
# it.
#
# NO `if(EXISTS)` GUARD: these sources are committed, unlike the generated
# src/xbox/xdk_surface.c below, so a fresh clone has them.
#
# -ldl is NOT optional: gpu_device.c resolves its GL entry points with dlopen/dlsym at
# runtime rather than linking a GL library at build time, which is what lets the rest of
# the project build and test on a machine with no GPU at all.
add_library(tsfp_gpu STATIC
  src/gpu/gpu_device.c
  src/gpu/gpu_png.c
  src/gpu/d3d8_hle.c
  src/gpu/d3d8_surface_adapter.c
  src/gpu/d3d8_render_state.c
  src/gpu/d3d8_render_state_table.c
  src/gpu/d3d8_guest.c
  src/gpu/d3d8_display.c
  src/gpu/d3d8_surface.c
  src/gpu/d3d8_pushbuffer.c
  src/gpu/d3d8_method_packet.c
  src/gpu/d3d8_device.c
  src/gpu/d3d8_state.c
  src/gpu/d3d8_set_state.c
  src/gpu/d3d8_resource.c
  src/gpu/d3d8_indexed.c
  src/gpu/d3d8_quadlist.c
  src/gpu/d3d8_viewport_matrix.c
  src/gpu/d3d8_scaled_viewport.c
  src/gpu/d3d8_matrix_inverse.c
  src/gpu/d3d8_scissor.c
  src/gpu/d3d8_vertex_constants.c
  src/gpu/d3d8_visibility.c
  src/gpu/d3d8_vertex_program.c
  src/gpu/d3d8_target_query.c
  src/gpu/d3d8_cube_surface.c
  src/gpu/d3d8_reference.c
  src/gpu/d3d8_lock.c
  src/gpu/d3d8_callbacks.c
  src/gpu/d3d8_first_vblank.c
  src/gpu/d3d8_vblank_effects.c
  src/gpu/d3d8_flip.c
  src/gpu/d3d8_dirty.c
  src/gpu/d3d8_texture_dirty.c
  src/gpu/d3d8_combiner.c
  src/gpu/d3d8_immediate.c
  src/gpu/d3d8_copy.c
  src/gpu/xgrph_hle.c
  src/gpu/xgrph_texture.c
  src/gpu/xgrph_object_lifetime.c
  src/gpu/xgrph_shader_query.c
  src/gpu/xgrph_swizzle.c
  src/gpu/d3d8_frame.c
  src/gpu/d3d8_gpu.c
  src/gpu/d3d8_bind.c
  src/gpu/d3d8_present.c
  src/gpu/d3d8_overlay.c
  src/gpu/d3d8_overlay_image.c
  src/gpu/d3d8_overlay_key.c
  src/gpu/d3d8_overlay_present.c
  src/gpu/d3d8_frame_profile.c
  src/gpu/d3d8_shader.c
  src/gpu/d3d8_shader_bind.c
  src/gpu/d3d8_pixel_bind.c
  src/gpu/d3d8_pixel_constants.c
  src/gpu/gpu_vsh_select.c
  src/gpu/gpu_vsh_draw.c
  src/gpu/gpu_sha256.c
  src/gpu/gpu_fog.c
  src/gpu/gpu_fog_factor.c
  src/gpu/gpu_combiner.c
  src/gpu/gpu_pgraph.c
  src/gpu/gpu_pgraph_replay.c
  src/gpu/gpu_pgraph_replay_copy_rules.c
  src/gpu/live_pipeline.c
  src/gpu/live_vk_pipeline.c
  src/gpu/live_vk_query.c
  src/gpu/d3d8_gpu_memory.c
  src/gpu/d3d8_gpu_pgraph.c
  src/gpu/d3d8_surface_model.c
  src/gpu/d3d8_swap_replay.c
  src/gpu/live_target.c
  src/gpu/live_vk_target.c
  src/gpu/live_vk_draw.c
  src/gpu/live_texture.c
  src/gpu/live_texture_watch.c
  src/gpu/live_texture_inputs.c
  src/gpu/live_report_pending.c
  src/gpu/live_module_maker.c)
target_include_directories(tsfp_gpu PUBLIC src/gpu)
# The init handlers read and write guest memory and call kernel exports, so tsfp_gpu now
# depends on tsfp_xbox. One direction only: src/xbox does not use src/gpu.
target_link_libraries(tsfp_gpu PUBLIC tsfp_xbox)
if(NOT WIN32)
  target_link_libraries(tsfp_gpu PUBLIC m)
endif()
set_target_properties(tsfp_gpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_library(TSFP_LIBDL dl)
if(TSFP_LIBDL)
  target_link_libraries(tsfp_gpu PUBLIC ${TSFP_LIBDL})
endif()

foreach(gpu_suite gpu_png gpu_render d3d8_hle d3d8_surface_adapter d3d8_render_state
        d3d8_display d3d8_surface d3d8_pushbuffer d3d8_begin_push d3d8_init d3d8_state d3d8_resource d3d8_indexed d3d8_quadlist d3d8_viewport_matrix d3d8_scissor d3d8_vertex_constants d3d8_vertex_program
        d3d8_cube_surface d3d8_surface_level d3d8_reference d3d8_target_query d3d8_lock d3d8_callbacks d3d8_first_vblank d3d8_frame
        d3d8_gpu d3d8_bind d3d8_present d3d8_overlay d3d8_overlay_present d3d8_vblank_observers d3d8_vblank_effects d3d8_flip d3d8_shader d3d8_shader_bind d3d8_pixel_bind d3d8_pixel_constants d3d8_dirty d3d8_texture_dirty d3d8_cascade_refill d3d8_frame_profile d3d8_method_packet d3d8_state_8a_registered d3d8_state_88_registered d3d8_state_97_registered
        xgrph_hle xgrph_texture xgrph_swizzle xgrph_shader_query gpu_vsh_select gpu_vsh_draw gpu_pgraph gpu_combiner gpu_pgraph_replay d3d8_gpu_pgraph d3d8_surface_model d3d8_swap_replay live_target live_vk_target live_pipeline live_vk_pipeline live_texture live_texture_watch live_texture_inputs gpu_pgraph_clone)
  add_executable(test_${gpu_suite} tests/c/test_${gpu_suite}.c)
  target_link_libraries(test_${gpu_suite} PRIVATE tsfp_gpu)
  set_target_properties(test_${gpu_suite} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_test(NAME ${gpu_suite} COMMAND test_${gpu_suite})
endforeach()
add_executable(test_d3d8_state_9c_registered tests/c/test_d3d8_state_9c_registered.c)
target_link_libraries(test_d3d8_state_9c_registered PRIVATE tsfp_gpu)
set_target_properties(test_d3d8_state_9c_registered PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME d3d8_state_9c_registered COMMAND test_d3d8_state_9c_registered)
add_executable(test_d3d8_state_92_registered tests/c/test_d3d8_state_92_registered.c)
target_link_libraries(test_d3d8_state_92_registered PRIVATE tsfp_gpu)
set_target_properties(test_d3d8_state_92_registered PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME d3d8_state_92_registered COMMAND test_d3d8_state_92_registered)
add_executable(test_d3d8_state_8e_registered tests/c/test_d3d8_state_8e_registered.c)
target_link_libraries(test_d3d8_state_8e_registered PRIVATE tsfp_gpu)
set_target_properties(test_d3d8_state_8e_registered PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME d3d8_state_8e_registered COMMAND test_d3d8_state_8e_registered)
add_executable(test_d3d8_state_96_registered tests/c/test_d3d8_state_96_registered.c)
target_link_libraries(test_d3d8_state_96_registered PRIVATE tsfp_gpu)
set_target_properties(test_d3d8_state_96_registered PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME d3d8_state_96_registered COMMAND test_d3d8_state_96_registered)
add_executable(test_d3d8_state_89_registered tests/c/test_d3d8_state_89_registered.c)
target_link_libraries(test_d3d8_state_89_registered PRIVATE tsfp_gpu)
set_target_properties(test_d3d8_state_89_registered PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME d3d8_state_89_registered COMMAND test_d3d8_state_89_registered)
add_executable(test_d3d8_state_9d_registered tests/c/test_d3d8_state_9d_registered.c)
target_link_libraries(test_d3d8_state_9d_registered PRIVATE tsfp_gpu)
set_target_properties(test_d3d8_state_9d_registered PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME d3d8_state_9d_registered COMMAND test_d3d8_state_9d_registered)
add_executable(test_d3d8_state_a5_registered tests/c/test_d3d8_state_a5_registered.c)
target_link_libraries(test_d3d8_state_a5_registered PRIVATE tsfp_gpu)
set_target_properties(test_d3d8_state_a5_registered PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME d3d8_state_a5_registered COMMAND test_d3d8_state_a5_registered)
add_executable(test_d3d8_state_98_registered tests/c/test_d3d8_state_98_registered.c)
target_link_libraries(test_d3d8_state_98_registered PRIVATE tsfp_gpu)
set_target_properties(test_d3d8_state_98_registered PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME d3d8_state_98_registered COMMAND test_d3d8_state_98_registered)
# live_module_maker (T847): the on-demand module maker with a fake translator, skips with 77 without python3.
add_executable(test_live_module_maker tests/c/test_live_module_maker.c)
target_link_libraries(test_live_module_maker PRIVATE tsfp_gpu)
add_test(NAME live_module_maker COMMAND test_live_module_maker)
set_tests_properties(live_module_maker PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# gpu_vsh_draw exits 77 (a SKIP) when no Vulkan device with transform feedback exists.
set_tests_properties(gpu_vsh_draw PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# live_vk_pipeline (T791): pixel parity of the live Vulkan draw renderer with the replay, skips with 77 without a device.
set_tests_properties(live_vk_pipeline PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
# gpu_pgraph_replay (T84) likewise skips with exit code 77 and a stated reason without a device.
set_tests_properties(gpu_pgraph_replay PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# gpu_combiner (T75): the decoder, plan and refusal checks need no device, the draws skip with 77.
set_tests_properties(gpu_combiner PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# live_vk_target (T793) runs on a real Vulkan device without a surface, skips with 77 when there is none.
set_tests_properties(live_vk_target PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# d3d8_swap_replay (T84a) skips the same way when no device exists.
set_tests_properties(d3d8_swap_replay PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# gpu_pgraph_vtxtypes (T84d): vertex type conversions and refusals, vertex assembly only, no device.
add_executable(test_gpu_pgraph_vtxtypes tests/c/test_gpu_pgraph_vtxtypes.c)
target_link_libraries(test_gpu_pgraph_vtxtypes PRIVATE tsfp_gpu)
set_target_properties(test_gpu_pgraph_vtxtypes PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME gpu_pgraph_vtxtypes COMMAND test_gpu_pgraph_vtxtypes)
# gpu_pgraph_edges (T259): decoder and replay-assembly edges the mutation sweep found unpinned, device-free.
add_executable(test_gpu_pgraph_edges tests/c/test_gpu_pgraph_edges.c)
target_link_libraries(test_gpu_pgraph_edges PRIVATE tsfp_gpu)
set_target_properties(test_gpu_pgraph_edges PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME gpu_pgraph_edges COMMAND test_gpu_pgraph_edges)
# gpu_combiner_edges (T259): combiner plan, refusals and SPIR-V scan edges the mutation sweep found unpinned.
add_executable(test_gpu_combiner_edges tests/c/test_gpu_combiner_edges.c)
target_link_libraries(test_gpu_combiner_edges PRIVATE tsfp_gpu)
set_target_properties(test_gpu_combiner_edges PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME gpu_combiner_edges COMMAND test_gpu_combiner_edges)
# gpu_pgraph_state (T267): the opt-in output state groups. Decode and resolve checks need no device, the
# pixel checks run on every device that exists and the suite exits 77 (SKIP) when none does.
add_executable(test_gpu_pgraph_state tests/c/test_gpu_pgraph_state.c)
target_link_libraries(test_gpu_pgraph_state PRIVATE tsfp_gpu)
set_target_properties(test_gpu_pgraph_state PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME gpu_pgraph_state COMMAND test_gpu_pgraph_state)
set_tests_properties(gpu_pgraph_state PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# d3d8_gpu_state (T267): the output state the real ported emitters write, over a real ring, decoded. No device.
add_executable(test_d3d8_gpu_state tests/c/test_d3d8_gpu_state.c)
target_link_libraries(test_d3d8_gpu_state PRIVATE tsfp_gpu)
set_target_properties(test_d3d8_gpu_state PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME d3d8_gpu_state COMMAND test_d3d8_gpu_state)

# T936: independently check registered texture virtual spans, stale cache identities and
# refusal preservation at a readable low-28-bit decoy. Device-free.
add_executable(test_t936_resource_read_review tests/c/test_t936_resource_read_review.c)
target_link_libraries(test_t936_resource_read_review PRIVATE tsfp_gpu)
set_target_properties(test_t936_resource_read_review PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME t936_resource_read_review COMMAND test_t936_resource_read_review)

# T943: verified registered aliases notify the live texture cache at their guest VA.
add_executable(test_t943_alias_lock_watch tests/c/test_t943_alias_lock_watch.c)
target_link_libraries(test_t943_alias_lock_watch PRIVATE tsfp_gpu)
set_target_properties(test_t943_alias_lock_watch PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME t943_alias_lock_watch COMMAND test_t943_alias_lock_watch)

# gpu_pgraph_composition (T462, T541): the SURFACE, FIXED, TEXTURE and IMMEDIATE groups the Swap copy composition and the
# SetRenderTarget packet need, and the SPIR-V default for an unwritten varying. Device free.
add_executable(test_gpu_pgraph_composition tests/c/test_gpu_pgraph_composition.c)
target_link_libraries(test_gpu_pgraph_composition PRIVATE tsfp_gpu)
set_target_properties(test_gpu_pgraph_composition PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME gpu_pgraph_composition COMMAND test_gpu_pgraph_composition)
set_tests_properties(gpu_pgraph_composition PROPERTIES TIMEOUT 120)

# d3d8_clear_decode (T440): the commands the ported Clear writes, over a real ring, decoded by gpu_pgraph's
# CLEAR group (T267). No device.
add_executable(test_d3d8_clear_decode tests/c/test_d3d8_clear_decode.c)
target_link_libraries(test_d3d8_clear_decode PRIVATE tsfp_gpu)
set_target_properties(test_d3d8_clear_decode PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME d3d8_clear_decode COMMAND test_d3d8_clear_decode)

# The D3D8 init handlers over the user's REAL executable. Exit code 77 is a SKIP: the retail XBE
# is never committed, so in a fresh clone this reports "skipped" and the synthetic suites above
# carry the logic. The path is where the harness extracts it.
add_executable(test_kernel_thread_lifecycle tests/c/test_kernel_thread_lifecycle.c)
target_compile_definitions(test_kernel_thread_lifecycle PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_kernel_thread_lifecycle PRIVATE tsfp_thunk tsfp_xbox)
add_test(NAME kernel_thread_lifecycle COMMAND test_kernel_thread_lifecycle)
add_executable(test_kernel_thread_wait tests/c/test_kernel_thread_wait.c)
target_compile_definitions(test_kernel_thread_wait PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_kernel_thread_wait PRIVATE tsfp_thunk tsfp_xbox)
add_test(NAME kernel_thread_wait COMMAND test_kernel_thread_wait)
add_executable(test_kernel_thread_pseudo_wait tests/c/test_kernel_thread_pseudo_wait.c)
target_compile_definitions(test_kernel_thread_pseudo_wait PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_kernel_thread_pseudo_wait PRIVATE tsfp_thunk tsfp_xbox)
target_link_options(test_kernel_thread_pseudo_wait PRIVATE "-Wl,--wrap=clock_nanosleep")
add_test(NAME kernel_thread_pseudo_wait COMMAND test_kernel_thread_pseudo_wait)
add_executable(test_kernel_thread_object tests/c/test_kernel_thread_object.c)
target_compile_definitions(test_kernel_thread_object PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_kernel_thread_object PRIVATE tsfp_thunk tsfp_xbox)
add_test(NAME kernel_thread_object COMMAND test_kernel_thread_object)

add_executable(test_d3d8_init_real tests/c/test_d3d8_init_real.c)
target_link_libraries(test_d3d8_init_real PRIVATE tsfp_gpu tsfp_loader)
set_target_properties(test_d3d8_init_real PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME d3d8_init_real
         COMMAND test_d3d8_init_real ${CMAKE_SOURCE_DIR}/tmp/oxm-extract/retail/default.xbe)
set_tests_properties(d3d8_init_real PROPERTIES SKIP_RETURN_CODE 77)

# ---------------------------------------------------------------------------
# THE THUNK LAYER: the two seams lifted guest code arrives through.
#
# `thunk_trace.c` is the single ordered call trace both boundaries append to.
# `xdk_thunk.c` is the ADDRESS-KEYED dispatcher, which is what gives
# d3d8_hle_call / dsound_hle_call / xinput_hle_call a production caller at all --
# before it existed all three were reachable only from their own test suites, and
# the XDK-surface metric would have read 0/236 however much of each was written.
#
# ITS OWN LIBRARY, NOT PART OF tsfp_host, for one reason that matters: the suite
# must be able to link it WITHOUT the 2.56 M lines of lifted C. It references the
# guest register file by declaration only (src/host/recomp_abi.h), so tsfp_host
# resolves those symbols from recomp_runtime.c and the suite defines its own four.
#
# host_runtime.c moved in here from tsfp_host's own source list for the same
# reason -- the suite has to be able to catch a stop -- and NOT duplicated into
# both, which would be two copies of the signal-handler reference count.
#
# _POSIX_C_SOURCE is PUBLIC, not PRIVATE: host_runtime.h #errors without it, so
# every consumer of this library needs it too. See that header for why it cannot
# be defined in the header itself.
# Command-line options: the struct, every default, and the parser.
#
# ITS OWN LIBRARY, tiny on purpose and depending on NOTHING. It was in main.c, which is
# compiled into tsfp_host alone -- and tsfp_host is not a ctest binary, so nothing in it
# can be covered by a suite or reached by tools/mutate/c_suites.py. The defaults here are
# safety arguments rather than conveniences (nothing writable unless an operator said
# where; a DEVICE rather than `D:` because the title creates that alias itself), and a
# silent change to one would fail no build and no test while changing what the title sees.
#
# Separate from tsfp_thunk rather than folded into it: tsfp_thunk pulls in tsfp_xbox and
# tsfp_gpu and requires _POSIX_C_SOURCE, none of which argv parsing needs. A suite that
# has to link the GPU stack to assert a default is a suite that breaks for unrelated
# reasons.
# T937: redirect only lifted copy call sites; host/allocator libc copies stay unobserved.
add_library(tsfp_recomp_copy STATIC src/host/recomp_copy.c)
target_include_directories(tsfp_recomp_copy PRIVATE src/host)
target_link_libraries(tsfp_recomp_copy PUBLIC tsfp_gpu)
add_executable(test_recomp_copy tests/c/test_recomp_copy.c)
target_compile_options(test_recomp_copy PRIVATE -fno-builtin-memcpy -fno-builtin-memmove
  -include "${CMAKE_CURRENT_SOURCE_DIR}/src/host/recomp_copy_redirect.h")
target_link_libraries(test_recomp_copy PRIVATE tsfp_recomp_copy)
add_test(NAME recomp_copy COMMAND test_recomp_copy)

add_library(tsfp_host_options STATIC src/host/host_options.c src/input/xinput_route_spec.c src/input/xinput_route_nav_spec.c src/input/xinput_hotkey.c src/host/guest_dump_ranges.c src/host/guest_watch_filter.c src/host/present_sink.c src/host/audio_stretch.c src/host/pad_sdl_feed.c src/host/hotkey_actions.c src/host/shot_hotkey.c)
target_include_directories(tsfp_host_options PUBLIC src/host src/input src/gpu)
target_link_libraries(tsfp_host_options PUBLIC tsfp_gft)
set_target_properties(tsfp_host_options PROPERTIES POSITION_INDEPENDENT_CODE ON)

# T760: SDL3 is an OPTIONAL dependency (owner decision T744). The window sink in present_sink.c is
# compiled only when SDL3 is found, the headless build is unchanged and stays the default, CI does not
# have it. -DTSFP_WITH_SDL3=OFF forces the headless build on a machine that has SDL3.
option(TSFP_WITH_SDL3 "Build the SDL3 window sink (--present window) when SDL3 is found" ON)
set(TSFP_SDL3_FOUND OFF)
if(TSFP_WITH_SDL3)
  find_package(SDL3 QUIET)
  if(SDL3_FOUND)
    set(TSFP_SDL3_FOUND ON)
    target_compile_definitions(tsfp_host_options PUBLIC TSFP_HAVE_SDL3=1)
    target_link_libraries(tsfp_host_options PUBLIC SDL3::SDL3)
    target_sources(tsfp_host_options PRIVATE src/host/audio_sink_sdl.c)
    target_sources(tsfp_host_options PRIVATE src/gpu/gpu_window.c)
  endif()
endif()
message(STATUS "SDL3 window sink: ${TSFP_SDL3_FOUND}")

# T942: optional SDL backend, separate from argv/presenter-only test linkage.
add_library(tsfp_controller_config STATIC src/input/controller_config.c)
target_include_directories(tsfp_controller_config PUBLIC src/input)
add_executable(test_controller_config tests/c/test_controller_config.c)
target_link_libraries(test_controller_config PRIVATE tsfp_controller_config)
# This fixture uses assert for checks and setup; keep them active in Release.
target_compile_options(test_controller_config PRIVATE -UNDEBUG)
add_test(NAME controller_config COMMAND test_controller_config "${CMAKE_CURRENT_BINARY_DIR}/controller-test.conf")
add_executable(test_controller_options tests/c/test_controller_options.c)
target_link_libraries(test_controller_options PRIVATE tsfp_host_options)
add_test(NAME controller_options COMMAND test_controller_options)
add_library(tsfp_controller_sdl STATIC src/host/controller_sdl.c)
target_include_directories(tsfp_controller_sdl PUBLIC src/host src/input)
target_link_libraries(tsfp_controller_sdl PUBLIC tsfp_host_options tsfp_controller_config tsfp_xbox)
add_executable(test_controller_sdl tests/c/test_controller_sdl.c)
target_compile_definitions(test_controller_sdl PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(test_controller_sdl PRIVATE tsfp_controller_sdl tsfp_gpu)
add_test(NAME controller_sdl COMMAND test_controller_sdl)
set_tests_properties(controller_sdl PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1")


# T926: an isolated build of the same sink sources with deterministic, test-only
# synchronization hooks. The shipping host_options library stays uninstrumented.
add_library(tsfp_host_options_t926 STATIC src/host/host_options.c src/input/xinput_route_spec.c src/input/xinput_route_nav_spec.c src/input/xinput_hotkey.c src/host/present_sink.c src/host/audio_stretch.c src/host/pad_sdl_feed.c src/host/hotkey_actions.c src/host/shot_hotkey.c)
target_include_directories(tsfp_host_options_t926 PUBLIC src/host src/input src/gpu)
target_link_libraries(tsfp_host_options_t926 PUBLIC tsfp_gft)
target_compile_definitions(tsfp_host_options_t926 PUBLIC TSFP_T926_TEST_HOOKS=1)
target_link_libraries(tsfp_host_options_t926 PUBLIC Threads::Threads)
set_target_properties(tsfp_host_options_t926 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(TSFP_SDL3_FOUND)
  target_compile_definitions(tsfp_host_options_t926 PUBLIC TSFP_HAVE_SDL3=1)
  target_link_libraries(tsfp_host_options_t926 PUBLIC SDL3::SDL3)
  target_sources(tsfp_host_options_t926 PRIVATE src/host/audio_sink_sdl.c src/gpu/gpu_window.c)
endif()

add_executable(test_t926_present_audio_detach_race tests/c/test_t926_present_audio_detach_race.c)
target_link_libraries(test_t926_present_audio_detach_race PRIVATE tsfp_host_options_t926)
add_test(NAME t926_present_audio_detach_race COMMAND test_t926_present_audio_detach_race)
set_tests_properties(t926_present_audio_detach_race PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15
  ENVIRONMENT "SDL_VIDEODRIVER=dummy")

add_executable(test_host_options tests/c/test_host_options.c)
target_link_libraries(test_host_options PRIVATE tsfp_host_options)
set_target_properties(test_host_options PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME host_options COMMAND test_host_options)
add_executable(test_present_sink tests/c/test_present_sink.c)
target_link_libraries(test_present_sink PRIVATE tsfp_host_options)
add_test(NAME present_sink COMMAND test_present_sink)
# The window half: SDL's dummy video driver, never a display. Skips (77, reported as skipped by ctest and
# printed by the test) when this build has no SDL3.
add_executable(test_present_window tests/c/test_present_window.c)
target_link_libraries(test_present_window PRIVATE tsfp_host_options)
add_test(NAME present_window COMMAND test_present_window)
# T1246: the presenter queue of the pipelined live renderer (SDL dummy video driver).
add_executable(test_present_post tests/c/test_present_post.c)
target_link_libraries(test_present_post PRIVATE tsfp_host_options)
add_test(NAME present_post COMMAND test_present_post)
set_tests_properties(present_post PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
set_tests_properties(present_window PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
if(TSFP_SDL3_FOUND)
  add_executable(test_gpu_window tests/c/test_gpu_window.c)
  target_link_libraries(test_gpu_window PRIVATE tsfp_host_options)
  add_test(NAME gpu_window COMMAND test_gpu_window)
  set_tests_properties(gpu_window PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
  # T793: the real CopyRects and Swap present on a swapchain. Skips (77) under the dummy video driver, run it under Xvfb.
  add_executable(test_live_vk_present tests/c/test_live_vk_present.c)
  target_link_libraries(test_live_vk_present PRIVATE tsfp_host_options tsfp_gpu)
  add_test(NAME live_vk_present COMMAND test_live_vk_present)
  set_tests_properties(live_vk_present PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
  # T792: the Vulkan texture upload/sampler on the gpu_window device. A static library so the draw loop links it when it
  # wants it. The test needs a real Vulkan device (SDL_VIDEODRIVER=x11 under Xvfb, llvmpipe is enough) and skips (77) otherwise.
  add_library(tsfp_live_vk_texture STATIC src/gpu/live_vk_texture.c)
  target_link_libraries(tsfp_live_vk_texture PUBLIC tsfp_gpu tsfp_host_options)
  add_executable(test_live_vk_texture tests/c/test_live_vk_texture.c)
  target_link_libraries(test_live_vk_texture PRIVATE tsfp_live_vk_texture)
  add_test(NAME live_vk_texture COMMAND test_live_vk_texture)
  set_tests_properties(live_vk_texture PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
  # T791: the window frame loop (a static library like T792's), same skip convention (the dummy driver cannot make a
  # Vulkan window).
  add_library(tsfp_live_vk_frame STATIC src/gpu/live_vk_frame.c)
  target_link_libraries(tsfp_live_vk_frame PUBLIC tsfp_gpu tsfp_host_options tsfp_live_vk_texture tsfp_live_vk_bind)
  add_library(tsfp_live_vk_bind STATIC src/gpu/live_vk_bind.c)
  target_link_libraries(tsfp_live_vk_bind PUBLIC tsfp_gpu tsfp_live_vk_texture)
  add_executable(test_live_vk_bind tests/c/test_live_vk_bind.c)
  target_link_libraries(test_live_vk_bind PRIVATE tsfp_live_vk_bind)
  add_test(NAME live_vk_bind COMMAND test_live_vk_bind)
  set_tests_properties(live_vk_bind PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
  add_executable(test_live_vk_draw tests/c/test_live_vk_draw.c)
  target_link_libraries(test_live_vk_draw PRIVATE tsfp_live_vk_bind)
  add_test(NAME live_vk_draw COMMAND test_live_vk_draw)
  # T1490: cube map, DOT_ST and DOTPRODUCT draws against a CPU reference of xemu's psh.c, modules made at build time (no retail bytes)
  find_program(T1490_PYTHON python3)
  find_program(T1490_GLSLANG glslangValidator)
  if(T1490_PYTHON AND T1490_GLSLANG)
    set(T1490_MODULE_DIR ${CMAKE_CURRENT_BINARY_DIR}/t1490-modules)
    add_custom_command(OUTPUT ${T1490_MODULE_DIR}/manifest.txt
      COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
        ${T1490_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/t1490_modes/make_modules.py ${T1490_MODULE_DIR}
      DEPENDS tests/fixtures/t1490_modes/make_modules.py tools/nv2a_combiner/replay_modules.py tools/nv2a_combiner/glsl.py
              tools/nv2a_combiner/config.py tools/nv2a/translate.py)
    add_custom_target(t1490_modules DEPENDS ${T1490_MODULE_DIR}/manifest.txt)
    add_executable(test_t1490_texture_modes tests/c/test_t1490_texture_modes.c)
    target_link_libraries(test_t1490_texture_modes PRIVATE tsfp_live_vk_bind m)
    add_dependencies(test_t1490_texture_modes t1490_modules)
    add_test(NAME t1490_texture_modes COMMAND test_t1490_texture_modes ${T1490_MODULE_DIR})
    set_tests_properties(t1490_texture_modes PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
  endif()
  set_tests_properties(live_vk_draw PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
  add_executable(test_live_vk_frame tests/c/test_live_vk_frame.c)
  target_link_libraries(test_live_vk_frame PRIVATE tsfp_live_vk_frame)
  add_test(NAME live_vk_frame COMMAND test_live_vk_frame)
  set_tests_properties(live_vk_frame PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
endif()
# T751: the SDL window provider of the host pad sources (injected SDL events, dummy driver, skips without SDL3).
add_executable(test_hotkey_actions tests/c/test_hotkey_actions.c)
target_link_libraries(test_hotkey_actions PRIVATE tsfp_host_options tsfp_xbox tsfp_gpu)
set_target_properties(test_hotkey_actions PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME hotkey_actions COMMAND test_hotkey_actions)

# T1720: the `shot` hotkey label (PNG writer, BMP loader, manifest, take through a fake capture)
add_executable(test_shot_hotkey tests/c/test_shot_hotkey.c)
target_link_libraries(test_shot_hotkey PRIVATE tsfp_host_options tsfp_xbox tsfp_gpu)
set_target_properties(test_shot_hotkey PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME shot_hotkey COMMAND test_shot_hotkey)

add_executable(test_pad_sdl_feed tests/c/test_pad_sdl_feed.c)
target_link_libraries(test_pad_sdl_feed PRIVATE tsfp_host_options tsfp_xbox tsfp_gpu)
set_target_properties(test_pad_sdl_feed PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME pad_sdl_feed COMMAND test_pad_sdl_feed)
set_tests_properties(pad_sdl_feed PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
# T761: the audio half, SDL's dummy audio driver. Skips (77) without SDL3.
add_executable(test_present_audio tests/c/test_present_audio.c)
target_link_libraries(test_present_audio PRIVATE tsfp_host_options)
add_test(NAME present_audio COMMAND test_present_audio)
set_tests_properties(present_audio PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_AUDIODRIVER=dummy")

# T819: the audio clock mode (prefill, rebuffer, kick, anchor) on a ring-only sink, no SDL needed.
add_executable(test_present_audio_clock tests/c/test_present_audio_clock.c)
target_link_libraries(test_present_audio_clock PRIVATE tsfp_host_options)
add_test(NAME present_audio_clock COMMAND test_present_audio_clock)
add_executable(test_present_audio_latency tests/c/test_present_audio_latency.c)
target_link_libraries(test_present_audio_latency PRIVATE tsfp_host_options)
add_test(NAME present_audio_latency COMMAND test_present_audio_latency)
add_executable(test_present_audio_continuous tests/c/test_present_audio_continuous.c)
target_link_libraries(test_present_audio_continuous PRIVATE tsfp_host_options)
add_test(NAME present_audio_continuous COMMAND test_present_audio_continuous)
# T1250: bursty guest on the 1024 frame device pull pattern, per 10 ms window continuity, WSOLA stretch engine.
add_executable(test_present_audio_bursty tests/c/test_present_audio_bursty.c)
target_link_libraries(test_present_audio_bursty PRIVATE tsfp_host_options m)
add_test(NAME present_audio_bursty COMMAND test_present_audio_bursty)
# T1487: stretch wobble measures (pitch cents, AM depth, spikes) and the A/V media clock offset.
add_executable(test_present_audio_wobble tests/c/test_present_audio_wobble.c)
target_link_libraries(test_present_audio_wobble PRIVATE tsfp_host_options m)
add_test(NAME present_audio_wobble COMMAND test_present_audio_wobble)
# T819: the window sink's playback mode (media-clock presentation, bounded queue, no guest pacing) on SDL's dummy
# video driver. Skips (77) without SDL3.
add_executable(test_present_playback tests/c/test_present_playback.c)
target_link_libraries(test_present_playback PRIVATE tsfp_host_options)
add_test(NAME present_playback COMMAND test_present_playback)
set_tests_properties(present_playback PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=dummy")

# The run report: every block tsfp_host prints after the guest stops, plus the
# stop-record table and the exit verdict. Out of main.c for the same reason
# host_options is: tsfp_host is not a ctest binary, so a formatter whose
# "FABRICATED" annotation went missing would fail no build and no test.
#
# ITS OWN LIBRARY, depending on nothing: the name/state lookups the lines need are
# injected (host_report_names), so the suite links neither the thunk layer nor the
# lifted code. src/xbox is an INCLUDE path only, for the kernel_file_attempt type.
# _POSIX_C_SOURCE is PUBLIC because host_report.h includes host_runtime.h, which
# #errors without it.
add_library(tsfp_host_report STATIC src/host/host_report.c)
target_include_directories(tsfp_host_report PUBLIC src/host src/xbox)
target_compile_definitions(tsfp_host_report PUBLIC _POSIX_C_SOURCE=200809L)
set_target_properties(tsfp_host_report PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(test_host_report tests/c/test_host_report.c)
target_link_libraries(test_host_report PRIVATE tsfp_host_report)
set_target_properties(test_host_report PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME host_report COMMAND test_host_report)

# T505: function names for a fault report, read from the executable's own .symtab at report
# time. Its own library so a suite can link it without the lifted code, and so tsfp_host
# (which also feeds tsfp_host_main_probe) gets it once. dladdr names libc frames.
add_library(tsfp_host_symbols STATIC src/host/host_symbols.c)
target_include_directories(tsfp_host_symbols PUBLIC src/host)
target_compile_definitions(tsfp_host_symbols PUBLIC _POSIX_C_SOURCE=200809L)
set_target_properties(tsfp_host_symbols PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(TSFP_LIBDL)
  target_link_libraries(tsfp_host_symbols PUBLIC ${TSFP_LIBDL})
endif()

# T505: a deliberate fault in fake lifted functions (named sub_XXXXXXXX, built at -O0 with
# frame pointers as the lifted code is) must leave RIP, the rbp chain and the guest ebp chain
# in the stop record, and the report must name them.
add_executable(test_host_fault_report tests/c/test_host_fault_report.c)
target_compile_options(test_host_fault_report PRIVATE -O0 -fno-omit-frame-pointer)
target_link_libraries(test_host_fault_report PRIVATE tsfp_thunk tsfp_host_report tsfp_host_symbols)
set_target_properties(test_host_fault_report PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME host_fault_report COMMAND test_host_fault_report)

add_library(tsfp_thunk STATIC
  src/host/host_runtime.c
  src/host/recomp_cooperative.c
  src/host/recomp_second_vblank.c
  src/host/recomp_vblank_quiescence.c
  src/host/monitor_thunk.c
  src/host/thunk_trace.c
  src/host/call_profile.c
  src/host/cpu_sampler.c
  src/host/voice_observer.c
  src/host/function_census.c
  src/host/guest_dump.c
  src/host/guest_watch.c
  src/host/button_dump.c
  src/host/button_dump_host.c
  src/host/guest_poke.c
  src/host/host_route.c
  src/host/route_probe.c
  src/host/async_io_spin.c
  src/host/xdk_thunk.c
  src/host/xdk_original.c
  src/host/xmv_original.c)
target_include_directories(tsfp_thunk PUBLIC src/host)
target_compile_definitions(tsfp_thunk PUBLIC _POSIX_C_SOURCE=200809L)
# T465: the arity-resolution and dispatch LOGIC of kernel_thunk.c. The suite #includes the .c (the
# resolver is static) and a synthetic measured table from tests/c/fixtures/thunk_arity, which is
# why that directory comes first and why no lifted tree is needed.
add_executable(test_kernel_thunk_arity tests/c/test_kernel_thunk_arity.c)
target_include_directories(test_kernel_thunk_arity PRIVATE src/host
  tests/c/fixtures/thunk_arity)
target_compile_definitions(test_kernel_thunk_arity PRIVATE TSFP_HAVE_MEASURED_ARITIES=1
  _DEFAULT_SOURCE)
target_link_libraries(test_kernel_thunk_arity PRIVATE tsfp_thunk tsfp_xbox)
add_test(NAME kernel_thunk_arity COMMAND test_kernel_thunk_arity)
# The measured XDK ABI table, generated beside xdk_surface.c by
#   ./.venv/bin/python -m tools.xdk_abi --xbe <default.xbe> --emit-c src/xbox/xdk_abi.inc
# Gitignored and conditional for the same reason the surface is: it is keyed by that
# surface, and a committed copy could go stale against an uncommitted key with nothing in
# CI able to see it. Absent, xdk_thunk.c builds a zero-row table and every address keeps
# stopping for want of an ABI. Evaluated at configure time, so re-run cmake after
# generating it, exactly as for kernel_arity.inc.
if(EXISTS "${CMAKE_SOURCE_DIR}/src/xbox/xdk_abi.inc")
  target_compile_definitions(tsfp_thunk PRIVATE TSFP_HAVE_XDK_ABI_INC=1)
  set(TSFP_HAVE_XDK_ABI_INC ON)
else()
  set(TSFP_HAVE_XDK_ABI_INC OFF)
endif()
target_link_libraries(tsfp_thunk PUBLIC tsfp_xbox tsfp_gpu)
set_target_properties(tsfp_thunk PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(test_xgrph_shader_query PRIVATE tsfp_thunk)

# T1067: original-backed one-word vtable reset for the XGRPH CompMask member.
add_executable(test_t1067_xgrph_vtable_reset tests/c/test_t1067_xgrph_vtable_reset.c)
target_include_directories(test_t1067_xgrph_vtable_reset PRIVATE tests/c)
target_link_libraries(test_t1067_xgrph_vtable_reset PRIVATE tsfp_thunk)
add_test(NAME t1067_xgrph_vtable_reset COMMAND test_t1067_xgrph_vtable_reset)

foreach(original_capability_test test_xdk_original test_xdk_original_absent)
  add_executable(${original_capability_test} tests/c/test_xdk_original.c)
  target_compile_definitions(${original_capability_test} PRIVATE _DEFAULT_SOURCE)
  target_link_libraries(${original_capability_test} PRIVATE tsfp_thunk)
  add_test(NAME ${original_capability_test} COMMAND ${original_capability_test})
endforeach()
target_compile_definitions(test_xdk_original_absent PRIVATE XDK_ORIGINAL_NO_CAPABILITY=1)
# T92/T234 retained XMV exports: fixture-body test and its absent-capability twin.
foreach(xmv_capability_test test_xmv_original test_xmv_original_absent)
  add_executable(${xmv_capability_test} tests/c/test_xmv_original.c)
  target_compile_definitions(${xmv_capability_test} PRIVATE _DEFAULT_SOURCE)
  target_link_libraries(${xmv_capability_test} PRIVATE tsfp_thunk)
  add_test(NAME ${xmv_capability_test} COMMAND ${xmv_capability_test})
endforeach()
target_compile_definitions(test_xmv_original_absent PRIVATE XMV_ORIGINAL_NO_CAPABILITY=1)
foreach(missing_original_symbol RANGE 1 3)
  set(partial_original_target test_xdk_original_partial_${missing_original_symbol})
  add_executable(${partial_original_target} tests/c/test_xdk_original_partial.c)
  target_compile_definitions(${partial_original_target} PRIVATE XDK_ORIGINAL_MISSING_SYMBOL=${missing_original_symbol})
  target_link_libraries(${partial_original_target} PRIVATE tsfp_thunk)
  add_test(NAME ${partial_original_target} COMMAND ${partial_original_target})
endforeach()

foreach(missing_original_symbol RANGE 1 3)
  set(partial_original_target test_xdk_original_partial_v2_${missing_original_symbol})
  add_executable(${partial_original_target} tests/c/test_xdk_original_partial.c)
  target_compile_definitions(${partial_original_target} PRIVATE XDK_ORIGINAL_MISSING_SYMBOL=${missing_original_symbol} XDK_ORIGINAL_FIXTURE_VERSION=2)
  target_link_libraries(${partial_original_target} PRIVATE tsfp_thunk)
  add_test(NAME ${partial_original_target} COMMAND ${partial_original_target})
endforeach()

foreach(virtual_target test_stream_virtual_dispatch test_stream_virtual_dispatch_absent)
  add_executable(${virtual_target} tests/c/test_stream_virtual_dispatch.c)
  target_link_libraries(${virtual_target} PRIVATE tsfp_thunk)
  add_test(NAME ${virtual_target} COMMAND ${virtual_target})
endforeach()
target_compile_definitions(test_stream_virtual_dispatch_absent PRIVATE STREAM_VIRTUAL_NO_MARKERS=1)

add_executable(test_recomp_second_vblank tests/c/test_recomp_second_vblank.c)
target_link_libraries(test_recomp_second_vblank PRIVATE tsfp_thunk)
add_test(NAME recomp_second_vblank COMMAND test_recomp_second_vblank)
add_executable(test_recomp_second_vblank_coupled tests/c/test_recomp_second_vblank_coupled.c)
target_link_libraries(test_recomp_second_vblank_coupled PRIVATE tsfp_thunk)
add_test(NAME recomp_second_vblank_coupled COMMAND test_recomp_second_vblank_coupled)
add_executable(test_recomp_second_vblank_owner_waits tests/c/test_recomp_second_vblank_owner_waits.c)
target_compile_definitions(test_recomp_second_vblank_owner_waits PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_recomp_second_vblank_owner_waits PRIVATE tsfp_thunk tsfp_xbox)
add_test(NAME recomp_second_vblank_owner_waits COMMAND test_recomp_second_vblank_owner_waits)
# Separate process: the existing matrix nearly fills the retained 64-thread table.
add_executable(test_recomp_second_vblank_startup_worker tests/c/test_recomp_second_vblank_owner_waits.c)
target_compile_definitions(test_recomp_second_vblank_startup_worker PRIVATE _DEFAULT_SOURCE TSFP_STARTUP_WORKER_ONLY)
target_link_libraries(test_recomp_second_vblank_startup_worker PRIVATE tsfp_thunk tsfp_xbox)
add_test(NAME recomp_second_vblank_startup_worker COMMAND test_recomp_second_vblank_startup_worker)
add_executable(test_recomp_vblank_quiescence tests/c/test_recomp_vblank_quiescence.c)
target_compile_definitions(test_recomp_vblank_quiescence PRIVATE _DEFAULT_SOURCE)
target_link_libraries(test_recomp_vblank_quiescence PRIVATE tsfp_thunk tsfp_xbox)
add_test(NAME recomp_vblank_quiescence COMMAND test_recomp_vblank_quiescence)

foreach(status_target test_stream_status_dispatch test_stream_status_dispatch_absent)
  add_executable(${status_target} tests/c/test_stream_status_dispatch.c)
  target_link_libraries(${status_target} PRIVATE tsfp_thunk)
  add_test(NAME ${status_target} COMMAND ${status_target})
endforeach()
target_compile_definitions(test_stream_status_dispatch_absent PRIVATE STREAM_STATUS_NO_MARKERS=1)

add_executable(test_movie_method_dispatch tests/c/test_movie_method_dispatch.c)
target_link_libraries(test_movie_method_dispatch PRIVATE tsfp_thunk)
add_test(NAME movie_method_dispatch COMMAND test_movie_method_dispatch)

add_executable(test_synch_dispatch tests/c/test_synch_dispatch.c)
target_link_libraries(test_synch_dispatch PRIVATE tsfp_thunk)
add_test(NAME synch_dispatch COMMAND test_synch_dispatch)

add_executable(test_host_stop_scopes tests/c/test_host_stop_scopes.c)
target_link_libraries(test_host_stop_scopes PRIVATE tsfp_thunk)
add_test(NAME host_stop_scopes COMMAND test_host_stop_scopes)

# T827: scope push, pop and the jump make no signal mask system call (a seccomp filter proves it).
add_executable(test_host_scope_no_mask tests/c/test_host_scope_no_mask.c)
target_link_libraries(test_host_scope_no_mask PRIVATE tsfp_thunk)
add_test(NAME host_scope_no_mask COMMAND test_host_scope_no_mask)
set_tests_properties(host_scope_no_mask PROPERTIES SKIP_RETURN_CODE 77)

foreach(cooperative_target test_recomp_cooperative test_recomp_cooperative_absent)
  add_executable(${cooperative_target} tests/c/test_recomp_cooperative.c)
  target_link_libraries(${cooperative_target} PRIVATE tsfp_thunk)
  add_test(NAME ${cooperative_target} COMMAND ${cooperative_target})
endforeach()
target_compile_definitions(test_recomp_cooperative_absent PRIVATE RECOMP_COOPERATIVE_NO_MARKER=1)

# The address-keyed dispatch path: routing, the ABI refusal, the stop policy and
# the proof that all three `*_call` entry points now have a production caller.
#
# DELIBERATELY FREE OF LIFTED CODE, THE XBE AND ANY DISC. It defines the four
# guest registers the dispatcher reads, synthesises every frame in scratch guest
# memory the way tests/c/test_kernel_io.c does, and injects its own surface rows,
# so it runs in a fresh clone with nothing generated.
#
# THE `add_test` LINE IS THE LOAD-BEARING ONE. src/gpu/* and its four suites were
# committed with no CMake wiring at all and had NEVER ONCE RUN until that was
# fixed; tools/coverage.py counts `add_test(NAME ...)`, so a suite that builds but
# is not registered is invisible to both ctest and the coverage figure.
add_executable(test_xdk_dispatch tests/c/test_xdk_dispatch.c)
target_link_libraries(test_xdk_dispatch PRIVATE tsfp_thunk)
set_target_properties(test_xdk_dispatch PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xdk_dispatch COMMAND test_xdk_dispatch)

add_executable(test_xonline_offline tests/c/test_xonline_offline.c)
target_link_libraries(test_xonline_offline PRIVATE tsfp_thunk)
set_target_properties(test_xonline_offline PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xonline_offline COMMAND test_xonline_offline)
add_executable(test_xnet_offline tests/c/test_xnet_offline.c)
target_link_libraries(test_xnet_offline PRIVATE tsfp_thunk)
set_target_properties(test_xnet_offline PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xnet_offline COMMAND test_xnet_offline)

# T51 dispatch probe (TSFP_XDK_PROBE): same injected-surface setup as xdk_dispatch.
add_executable(test_xdk_probe tests/c/test_xdk_probe.c)
target_link_libraries(test_xdk_probe PRIVATE tsfp_thunk)
add_test(NAME xdk_probe COMMAND test_xdk_probe)

add_executable(test_stream_stop_marker tests/c/test_stream_stop_marker.c)
target_include_directories(test_stream_stop_marker PRIVATE src/host)
target_link_libraries(test_stream_stop_marker PRIVATE tsfp_thunk tsfp_xbox)
add_test(NAME stream_stop_marker COMMAND test_stream_stop_marker)

foreach(marker_mode IN ITEMS present absent)
  set(marker_target test_audio_dispatch_marker)
  if(marker_mode STREQUAL "absent")
    set(marker_target test_audio_dispatch_marker_absent)
  endif()
  add_executable(${marker_target} tests/c/test_audio_dispatch_marker.c)
  target_include_directories(${marker_target} PRIVATE src/host)
  target_link_libraries(${marker_target} PRIVATE tsfp_thunk tsfp_xbox)
  if(marker_mode STREQUAL "absent")
    target_compile_definitions(${marker_target} PRIVATE DSOUND_NO_DISPATCH_MARKER=1)
  endif()
  string(REGEX REPLACE "^test_" "" marker_test "${marker_target}")
  add_test(NAME ${marker_test} COMMAND ${marker_target})
endforeach()

# The measured ABI table's wiring. Free of the generated table by design: it declares rows
# it writes itself and checks only what holds whether or not src/xbox/xdk_abi.inc exists.
add_executable(test_xdk_abi_table tests/c/test_xdk_abi_table.c)
target_link_libraries(test_xdk_abi_table PRIVATE tsfp_thunk)
set_target_properties(test_xdk_abi_table PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xdk_abi_table COMMAND test_xdk_abi_table)
# Every case here crosses a stop, which is a siglongjmp out of a dispatcher. A
# mutation that loses the stop can leave the suite spinning rather than failing,
# and a hung test reports nothing.
set_tests_properties(xdk_dispatch PROPERTIES TIMEOUT 120)

# The Prcb debug-monitor notify at `monitor+0x14`: the synthetic slot, the measured
# arity, the callee cleanup, the trace entry and the injection into the control page.
#
# WHY IT IS ANSWERED FROM xdk_thunk.c AND NOT kernel_thunk.c. src/host/kernel_thunk.c is
# compiled into tsfp_host ALONE, tsfp_host is not a ctest binary, and so nothing in it
# can be covered by a suite or reached by a mutation. The pop count is the one line in
# this change that must not be wrong, and `recomp_lookup_manual` puts it somewhere a test
# can execute it. The generated RECOMP_ICALL_SAFE consults manual first, so the routing
# is production and not a test hook.
#
# DELIBERATELY FREE OF LIFTED CODE, THE XBE AND ANY DISC. It defines the four guest
# registers the dispatcher reads, lays every frame and control page out in scratch guest
# memory the way tests/c/test_kernel_io.c does, and arms the notify itself.
#
# THE `add_test` LINE IS THE LOAD-BEARING ONE. src/gpu/* and its four suites were
# committed with no CMake wiring at all and had NEVER ONCE RUN until that was fixed;
# tools/coverage.py counts registered tests, so a suite that builds but is not registered
# is invisible to both ctest and the coverage figure.
add_executable(test_call_profile tests/c/test_call_profile.c)
target_link_libraries(test_call_profile PRIVATE tsfp_thunk)
set_target_properties(test_call_profile PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME call_profile COMMAND test_call_profile)

# T821: the read-only indirect call census. Synthetic stack and present counter, no lifted code or XBE.
add_executable(test_function_census tests/c/test_function_census.c)
target_link_libraries(test_function_census PRIVATE tsfp_thunk)
set_target_properties(test_function_census PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME function_census COMMAND test_function_census)

# T1599: the read-only guest memory dump (ranges, bounds, per-phase files). Synthetic guest region, no lifted code.
add_executable(test_guest_dump tests/c/test_guest_dump.c)
target_link_libraries(test_guest_dump PRIVATE tsfp_thunk tsfp_host_options)
set_target_properties(test_guest_dump PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME guest_dump COMMAND test_guest_dump)
add_executable(test_guest_watch tests/c/test_guest_watch.c)
target_link_libraries(test_guest_watch PRIVATE tsfp_host_options)
set_target_properties(test_guest_watch PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME guest_watch COMMAND test_guest_watch)

# T1629: the passive button dump (mask, edges, scheduler, caps, writer, manifest). Fakes for the capture, a synthetic guest region
# for the real file round trip.
add_executable(test_button_dump tests/c/test_button_dump.c)
target_link_libraries(test_button_dump PRIVATE tsfp_thunk tsfp_host_options)
set_target_properties(test_button_dump PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME button_dump COMMAND test_button_dump)

# T1613: the guarded guest poke (parse, bounds, image sections, read-back, consume). Synthetic guest regions.
add_executable(test_guest_poke tests/c/test_guest_poke.c)
target_link_libraries(test_guest_poke PRIVATE tsfp_thunk tsfp_host_options)
set_target_properties(test_guest_poke PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME guest_poke COMMAND test_guest_poke)

# T1616: route replay (waits, marks, handover, poke triggers). Pure C, no lifted tree.
add_executable(test_route_events tests/c/test_route_events.c)
target_link_libraries(test_route_events PRIVATE tsfp_thunk tsfp_xbox tsfp_gpu tsfp_host_options)
set_target_properties(test_route_events PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME route_events COMMAND test_route_events)

# T1640: closed loop menu navigation (nav grammar, menu table, state machine with a fake menu, route integration). Pure C.
add_executable(test_route_nav tests/c/test_route_nav.c)
target_link_libraries(test_route_nav PRIVATE tsfp_thunk tsfp_xbox tsfp_gpu tsfp_host_options)
set_target_properties(test_route_nav PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME route_nav COMMAND test_route_nav)

add_executable(test_xinput_route tests/c/test_xinput_route.c)
target_link_libraries(test_xinput_route PRIVATE tsfp_thunk tsfp_xbox tsfp_gpu tsfp_host_options)
set_target_properties(test_xinput_route PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xinput_route COMMAND test_xinput_route)

# T1627: host hotkeys (chord grammar, hold time, swallowing, files). Pure C, no lifted tree, no SDL.
add_executable(test_xinput_hotkey tests/c/test_xinput_hotkey.c)
target_link_libraries(test_xinput_hotkey PRIVATE tsfp_host_options)
set_target_properties(test_xinput_hotkey PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xinput_hotkey COMMAND test_xinput_hotkey)

# T821: the spin rule (a pending overlapped read completes when the guest polls it). Synthetic queue and safepoints.
add_executable(test_async_io_spin tests/c/test_async_io_spin.c)
target_link_libraries(test_async_io_spin PRIVATE tsfp_thunk tsfp_xbox)
target_compile_definitions(test_async_io_spin PRIVATE _DEFAULT_SOURCE)
set_target_properties(test_async_io_spin PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME async_io_spin COMMAND test_async_io_spin)

# T1492: the end of the call trace (per-thread tail, recent non-success statuses).
add_executable(test_thunk_trace_tail tests/c/test_thunk_trace_tail.c)
target_link_libraries(test_thunk_trace_tail PRIVATE tsfp_thunk)
set_target_properties(test_thunk_trace_tail PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME thunk_trace_tail COMMAND test_thunk_trace_tail)

add_executable(test_monitor_thunk tests/c/test_monitor_thunk.c)
target_link_libraries(test_monitor_thunk PRIVATE tsfp_thunk)
set_target_properties(test_monitor_thunk PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME monitor_thunk COMMAND test_monitor_thunk)
# The counters are mutex-guarded and shared across guest threads, so a mutation in the
# locking can hang rather than return a wrong answer, and a hung test reports nothing.
set_tests_properties(monitor_thunk PROPERTIES TIMEOUT 120)

add_executable(hle_report src/xbox/hle_report.c)
target_link_libraries(hle_report PRIVATE tsfp_xbox tsfp_loader)
set_target_properties(hle_report PROPERTIES POSITION_INDEPENDENT_CODE ON)

# NOT surface-gated (T405): the report writer takes its rows through the committed
# xdk_registration_row mirror, so this suite builds and runs on a fresh clone.
add_executable(test_xdk_registration_report tests/c/test_xdk_registration_report.c
  src/xbox/xdk_registration_report.c)
target_link_libraries(test_xdk_registration_report PRIVATE tsfp_gpu tsfp_xbox)
set_target_properties(test_xdk_registration_report PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME xdk_registration_report COMMAND test_xdk_registration_report)

if(TSFP_HAVE_XDK_SURFACE)
  add_executable(xdk_report src/xbox/xdk_report.c src/xbox/xdk_registration_report.c)
  target_link_libraries(xdk_report PRIVATE tsfp_gpu tsfp_xbox)
  set_target_properties(xdk_report PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_executable(test_xdk_surface tests/c/test_xdk_surface.c)
  target_link_libraries(test_xdk_surface PRIVATE tsfp_xbox)
  set_target_properties(test_xdk_surface PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_test(NAME xdk_surface COMMAND test_xdk_surface)
else()
  # REGISTERED ANYWAY, as an explicit SKIP. The surface table is generated and gitignored, so
  # in a clean checkout this suite used to vanish from ctest altogether, and the test count
  # then depended on which files happened to exist: that is the measured cause of a 30-versus-
  # 31 discrepancy in a committed badge. A skip is a status somebody can see; an absent test
  # is not.
  add_test(NAME xdk_surface
           COMMAND ${CMAKE_COMMAND} -E echo
                   "SKIP: src/xbox/xdk_surface.c is not generated (run tools/gen_d3d8_surface.py)")
  set_tests_properties(xdk_surface PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP")
endif()

# ---------------------------------------------------------------------------
# The lifted guest code, and the host that runs it.
#
# OPTIONAL BY CONSTRUCTION. The lifted C is 2.56 M lines across 65 chunks, is
# derived from the user's own executable, and is never committed. A fresh clone
# has none of it and must still configure and build cleanly, so this whole
# section is conditional in exactly the way the xdk_surface target above is:
# present when the input is, and a message saying how to produce it when it is
# not. Never a missing-file error.
#
# Regenerate with:
#     uv run python -m tools.lift vendor --source <checkout of xboxrecomp>
#     uv run python -m tools.lift run <your default.xbe>
# ---------------------------------------------------------------------------
set(TSFP_LIFTED_DIR "${CMAKE_SOURCE_DIR}/generated/lifted/gen"
    CACHE PATH "Directory holding the lifted C emitted by tools.lift")

# Optimisation level for the lifted code alone. -O0 because 2.56 M lines is a
# build-time problem before it is a run-time one, and because the differential
# evidence in docs/lifter-evaluation.md was gathered at -O0. Raising it needs
# -fno-strict-aliasing to stay in place: the entire memory model is
# *(volatile T *) type punning, which -O2 is entitled to reorder.
set(TSFP_LIFTED_OPT "-O0" CACHE STRING "Optimisation flag for the lifted chunks")

if(EXISTS "${TSFP_LIFTED_DIR}/recomp_dispatch.c")
  file(GLOB TSFP_LIFTED_SOURCES CONFIGURE_DEPENDS "${TSFP_LIFTED_DIR}/recomp_*.c")
  list(LENGTH TSFP_LIFTED_SOURCES TSFP_LIFTED_COUNT)
  message(STATUS "lifted code: ${TSFP_LIFTED_COUNT} translation units in ${TSFP_LIFTED_DIR}")

  add_library(tsfp_lifted STATIC ${TSFP_LIFTED_SOURCES})
  target_link_libraries(tsfp_lifted PUBLIC tsfp_recomp_copy)
  target_include_directories(tsfp_lifted PUBLIC "${TSFP_LIFTED_DIR}")
  set_target_properties(tsfp_lifted PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # ISOLATE THE GENERATED CODE, DO NOT WEAKEN THE PROJECT.
  #
  # This code is machine-written and will never satisfy -Wconversion or
  # -Wstrict-prototypes; it is scaffolding that gets deleted function by function
  # as hand-written C replaces it. Relaxing the flags for the whole project to
  # accommodate it would trade a permanent loss of signal on 100% of the
  # hand-written code for a temporary convenience on code nobody maintains. So
  # the relaxation is scoped to this one target.
  #
  # -w and -Wno-error are both passed on purpose. Target options come after the
  # directory-level ones, so -w alone would suffice by GCC's last-flag-wins rule,
  # but relying on flag ordering to keep a build green is the kind of thing that
  # breaks silently; -Wno-error says the same thing without depending on order.
  set(TSFP_LIFTED_UNIMPL_POLICY RECOMP_UNIMPL_CONTINUE=1)
  target_compile_definitions(tsfp_lifted PRIVATE ${TSFP_LIFTED_UNIMPL_POLICY})
  file(STRINGS "${TSFP_LIFTED_DIR}/recomp_types.h" TSFP_COOPERATIVE_TEMPLATE
       REGEX "^#define RECOMP_COOPERATIVE_CALL_VERSION 1$")
  if(TSFP_COOPERATIVE_TEMPLATE)
    target_compile_definitions(tsfp_lifted PRIVATE RECOMP_ENABLE_COOPERATIVE_CALLS=1)
  endif()
  target_compile_options(tsfp_lifted PRIVATE
    -w -Wno-error
    # Mandatory, not an optimisation choice. XBOX_PTR/MEM32 reach guest memory
    # through *(volatile uint32_t *) on an address computed as an integer, which
    # is textbook type punning. Without this the compiler may assume those
    # accesses cannot alias and reorder them. See docs/lifter-evaluation.md §6.3.
    -fno-strict-aliasing
    -fno-builtin-memcpy -fno-builtin-memmove
    -include "${CMAKE_CURRENT_SOURCE_DIR}/src/host/recomp_copy_redirect.h"
    ${TSFP_LIFTED_OPT})

  # The lifted code calls into libm (cos, sqrt, fmod) for the x87 instructions it
  # lowers to C double arithmetic.
  find_library(TSFP_LIBM m)
  if(TSFP_LIBM)
    target_link_libraries(tsfp_lifted PUBLIC ${TSFP_LIBM})
  endif()

  # recomp_runtime.c is ours, but it is the one hand-written file that includes
  # the generated recomp_types.h -- deliberately, so a disagreement between our
  # strict-clean view of the ABI and the generated one is a compile error rather
  # than a silent link-time mismatch. The price is that XBOX_PTR() adds a
  # ptrdiff_t to a uintptr_t and trips -Wsign-conversion. Only that one warning
  # class is switched off, and only for this file: everything else, -Werror
  # included, still applies, so this does not become a quiet dumping ground.
  add_library(tsfp_recomp_runtime OBJECT src/host/recomp_runtime.c)
  target_include_directories(tsfp_recomp_runtime PRIVATE src/xbox src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(tsfp_recomp_runtime PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_options(tsfp_recomp_runtime PRIVATE -Wno-conversion -Wno-sign-conversion)
  set_target_properties(tsfp_recomp_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_library(tsfp_recomp_callback OBJECT src/host/recomp_callback.c src/host/xnet_dpc.c)
  target_include_directories(tsfp_recomp_callback PRIVATE src/xbox src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(tsfp_recomp_callback PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_options(tsfp_recomp_callback PRIVATE -Wno-conversion -Wno-sign-conversion)
  set_target_properties(tsfp_recomp_callback PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_executable(test_recomp_callback tests/c/test_recomp_callback.c
    $<TARGET_OBJECTS:tsfp_recomp_runtime> $<TARGET_OBJECTS:tsfp_recomp_callback>)
  target_include_directories(test_recomp_callback PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_options(test_recomp_callback PRIVATE -Wno-conversion -Wno-sign-conversion)
  target_link_libraries(test_recomp_callback PRIVATE tsfp_thunk tsfp_xbox ${TSFP_LIBM})
  add_test(NAME recomp_callback COMMAND test_recomp_callback)


  add_executable(test_recomp_runtime tests/c/test_recomp_runtime.c
    $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_recomp_runtime PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_recomp_runtime PRIVATE _POSIX_C_SOURCE=200809L
                             ${TSFP_LIFTED_UNIMPL_POLICY})
  target_compile_options(test_recomp_runtime PRIVATE -Wno-conversion -Wno-sign-conversion)
  target_link_libraries(test_recomp_runtime PRIVATE tsfp_thunk tsfp_xbox)
  set_target_properties(test_recomp_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_test(NAME recomp_runtime COMMAND test_recomp_runtime)
  set_tests_properties(recomp_runtime PROPERTIES TIMEOUT 60)

  # Exercise the actual generated dispatch table and indirect-call macros when
  # this lift includes independent stop boundaries. Older lifts lack those symbols.
  if(EXISTS "${TSFP_LIFTED_DIR}/recomp_xdk_manual.c")
    file(STRINGS "${TSFP_LIFTED_DIR}/recomp_xdk_manual.c" TSFP_STOP_TRAMPOLINES
         REGEX "xdk_thunk_stop_at\\(")
    if(TSFP_STOP_TRAMPOLINES)
      file(STRINGS "${TSFP_LIFTED_DIR}/recomp_xdk_manual.c" TSFP_BUFFER_STOP_TRAMPOLINES
           REGEX "BufferAddRef")
      file(STRINGS "${TSFP_LIFTED_DIR}/recomp_xdk_manual.c" TSFP_AUDIO_DISPATCH_MARKER
           REGEX "^int recomp_has_dispatch_boundary")
      foreach(route_mode IN ITEMS normal continue)
        set(route_target test_stream_stop_routes)
        if(route_mode STREQUAL "continue")
          set(route_target test_stream_stop_routes_continue)
        endif()
        add_executable(${route_target} tests/c/test_stream_stop_routes.c
          src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
        target_include_directories(${route_target} PRIVATE src/host "${TSFP_LIFTED_DIR}")
        target_compile_definitions(${route_target} PRIVATE _POSIX_C_SOURCE=200809L
          ${TSFP_LIFTED_UNIMPL_POLICY})
        if(TSFP_AUDIO_DISPATCH_MARKER)
          target_compile_definitions(${route_target} PRIVATE TSFP_HAVE_AUDIO_DISPATCH_MARKER=1)
        endif()
        if(TSFP_BUFFER_STOP_TRAMPOLINES)
          target_compile_definitions(${route_target} PRIVATE TSFP_HAVE_BUFFER_STOP_ROUTES=1)
        endif()
        if(route_mode STREQUAL "continue")
          target_compile_definitions(${route_target} PRIVATE RECOMP_ICALL_CONTINUE=1)
        endif()
        target_compile_options(${route_target} PRIVATE -Wno-conversion -Wno-sign-conversion)
        target_link_libraries(${route_target} PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
        string(REGEX REPLACE "^test_" "" route_test "${route_target}")
        add_test(NAME ${route_test} COMMAND ${route_target})
      endforeach()
    endif()
  endif()

  add_executable(test_kernel_clock_thunk tests/c/test_kernel_clock_thunk.c
    src/host/kernel_thunk.c)
  target_include_directories(test_kernel_clock_thunk PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_kernel_clock_thunk PRIVATE TSFP_HAVE_MEASURED_ARITIES=1)
  target_link_libraries(test_kernel_clock_thunk PRIVATE tsfp_thunk tsfp_xbox)
  set_target_properties(test_kernel_clock_thunk PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_test(NAME kernel_clock_thunk COMMAND test_kernel_clock_thunk)
  add_executable(test_kernel_t8c_thunk tests/c/test_kernel_t8c_thunk.c
    src/host/kernel_thunk.c)
  target_include_directories(test_kernel_t8c_thunk PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_kernel_t8c_thunk PRIVATE TSFP_HAVE_MEASURED_ARITIES=1)
  target_link_libraries(test_kernel_t8c_thunk PRIVATE tsfp_thunk tsfp_xbox)
  set_target_properties(test_kernel_t8c_thunk PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_test(NAME kernel_t8c_thunk COMMAND test_kernel_t8c_thunk)
  add_executable(test_kernel_t8e_thunk tests/c/test_kernel_t8e_thunk.c
    src/host/kernel_thunk.c)
  target_include_directories(test_kernel_t8e_thunk PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_kernel_t8e_thunk PRIVATE TSFP_HAVE_MEASURED_ARITIES=1)
  target_link_libraries(test_kernel_t8e_thunk PRIVATE tsfp_thunk tsfp_xbox)
  set_target_properties(test_kernel_t8e_thunk PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_test(NAME kernel_t8e_thunk COMMAND test_kernel_t8e_thunk)

  add_executable(test_kernel_t8f_thunk tests/c/test_kernel_t8f_thunk.c
    src/host/kernel_thunk.c)
  target_include_directories(test_kernel_t8f_thunk PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_kernel_t8f_thunk PRIVATE TSFP_HAVE_MEASURED_ARITIES=1)
  target_link_libraries(test_kernel_t8f_thunk PRIVATE tsfp_thunk tsfp_xbox)
  set_target_properties(test_kernel_t8f_thunk PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_test(NAME kernel_t8f_thunk COMMAND test_kernel_t8f_thunk)
  add_executable(test_kernel_t8d_thunk tests/c/test_kernel_t8d_thunk.c
    src/host/kernel_thunk.c)
  target_include_directories(test_kernel_t8d_thunk PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_kernel_t8d_thunk PRIVATE TSFP_HAVE_MEASURED_ARITIES=1)
  target_link_libraries(test_kernel_t8d_thunk PRIVATE tsfp_thunk tsfp_xbox)
  set_target_properties(test_kernel_t8d_thunk PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_test(NAME kernel_t8d_thunk COMMAND test_kernel_t8d_thunk)
  add_executable(test_kernel_yield_thunk tests/c/test_kernel_yield_thunk.c
    src/host/kernel_thunk.c)
  target_include_directories(test_kernel_yield_thunk PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_kernel_yield_thunk PRIVATE TSFP_HAVE_MEASURED_ARITIES=1)
  target_link_libraries(test_kernel_yield_thunk PRIVATE tsfp_thunk tsfp_xbox)
  add_test(NAME kernel_yield_thunk COMMAND test_kernel_yield_thunk)
  # T94 console identity: published struct bytes, the CRT patch-gate replay, and
  # the import-slot redirect into the window's data annex.
  add_executable(test_kernel_identity tests/c/test_kernel_identity.c src/host/kernel_thunk.c)
  target_include_directories(test_kernel_identity PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_kernel_identity PRIVATE _DEFAULT_SOURCE)
  target_link_libraries(test_kernel_identity PRIVATE tsfp_thunk tsfp_xbox)
  add_test(NAME kernel_identity COMMAND test_kernel_identity)
  add_executable(test_kernel_wait_thunk tests/c/test_kernel_wait_thunk.c src/host/kernel_thunk.c)
  target_include_directories(test_kernel_wait_thunk PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_kernel_wait_thunk PRIVATE TSFP_HAVE_KERNEL_ARITY_INC=1 _DEFAULT_SOURCE)
  target_link_libraries(test_kernel_wait_thunk PRIVATE tsfp_thunk tsfp_xbox)
  add_test(NAME kernel_wait_thunk COMMAND test_kernel_wait_thunk)


  # host_runtime.c is NOT listed here: it is in tsfp_thunk, so the dispatch suite
  # can catch a stop without linking the lifted code. Compiling it into both would
  # give the process two copies of the signal-handler reference count.
  add_executable(tsfp_host
    src/host/main.c
    src/host/kernel_thunk.c
    src/host/recomp_guest_call.c
    src/host/live_render.c
    $<TARGET_OBJECTS:tsfp_recomp_runtime>
    $<TARGET_OBJECTS:tsfp_recomp_callback>)
  target_include_directories(tsfp_host PRIVATE src/host "${TSFP_LIFTED_DIR}")
  # A weak runtime capability query alone cannot extract its new archive member.
  # Retain the actual generated alias chunk when this build supplies it.
  if(EXISTS "${TSFP_LIFTED_DIR}/recomp_xdk_original.c")
    target_link_options(tsfp_host PRIVATE "LINKER:--undefined=recomp_lookup_original")
  endif()
  # Same for the retained original XMV exports (T92/T234, --native-xmv).
  if(EXISTS "${TSFP_LIFTED_DIR}/recomp_xmv_original.c")
    target_link_options(tsfp_host PRIVATE "LINKER:--undefined=recomp_xmv_lookup_original")
  endif()
  # sigsetjmp/sigaction are POSIX and -std=c11 hides them; see src/host/host_runtime.h.
  target_compile_definitions(tsfp_host PRIVATE _POSIX_C_SOURCE=200809L)

  # Measured kernel stack-argument counts. Written by the same tools.lift run that
  # produced the chunks, so it is normally present; guarded anyway because an
  # older output directory will not have it, and the host is correct without it --
  # it simply stops at the first ordinal outside its small hand-written table
  # instead of walking the boot path.
  if(EXISTS "${TSFP_LIFTED_DIR}/kernel_arity.inc")
    target_compile_definitions(tsfp_host PRIVATE TSFP_HAVE_MEASURED_ARITIES=1)
  else()
    message(STATUS
      "no kernel_arity.inc in ${TSFP_LIFTED_DIR} -- tsfp_host will stop at the "
      "first kernel ordinal whose argument count is not hand-recorded. "
      "Re-run: uv run python -m tools.lift run <your default.xbe>")
  endif()
  # Whether main.c can adopt the measured XDK surface at all. The CMake variable
  # already decides whether to COMPILE the generated table; this makes the same
  # fact visible to the C, so the absence is a printed note rather than a boundary
  # that silently reports no traffic.
  if(TSFP_HAVE_XDK_SURFACE)
    target_compile_definitions(tsfp_host PRIVATE TSFP_HAVE_XDK_SURFACE=1)
  endif()
  target_link_libraries(tsfp_host PRIVATE tsfp_controller_sdl tsfp_host_options tsfp_host_report tsfp_host_symbols tsfp_lifted tsfp_recomp_copy tsfp_thunk tsfp_xbox tsfp_loader)
  if(TSFP_SDL3_FOUND)
    # T838: the live renderer's host side (src/host/live_render.c) draws through the T790-T793 libraries
    target_link_libraries(tsfp_host PRIVATE tsfp_live_vk_frame)
  endif()
  # PIE is enforced project-wide above, and this is the one target where it is
  # load-bearing at runtime rather than merely required: a non-PIE tsfp_host
  # loads at 0x400000, inside the guest window, and xbe_map fails with EEXIST.
  set_target_properties(tsfp_host PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # T200: tsfp_host plus link-time wraps, to observe the retained original shader
  # compiler inside the real boot (repeat use, concurrent threads, injected title heap
  # failure). The host itself is untouched: main.c is compiled once more with main renamed.
  # Only built when the private alias chunk is installed, see tests/c/shader_compiler_probe.c.
  if(EXISTS "${TSFP_LIFTED_DIR}/recomp_xdk_original.c")
    add_library(tsfp_host_main_probe OBJECT src/host/main.c)
    target_compile_definitions(tsfp_host_main_probe PRIVATE _POSIX_C_SOURCE=200809L main=tsfp_host_main)
    target_include_directories(tsfp_host_main_probe PRIVATE src/host "${TSFP_LIFTED_DIR}")
    get_target_property(probe_host_libraries tsfp_host LINK_LIBRARIES)
    target_link_libraries(tsfp_host_main_probe PRIVATE ${probe_host_libraries})
    set_target_properties(tsfp_host_main_probe PROPERTIES POSITION_INDEPENDENT_CODE ON)
    get_target_property(probe_host_definitions tsfp_host COMPILE_DEFINITIONS)
    target_compile_definitions(tsfp_host_main_probe PRIVATE ${probe_host_definitions})
    # Everything tsfp_host links except its own main.c, taken from the target so a source
    # added to tsfp_host later cannot leave the probe unlinkable.
    get_target_property(probe_host_sources tsfp_host SOURCES)
    list(REMOVE_ITEM probe_host_sources src/host/main.c)
    get_target_property(probe_host_libraries tsfp_host LINK_LIBRARIES)
    add_executable(tsfp_shader_probe
      tests/c/shader_compiler_probe.c
      $<TARGET_OBJECTS:tsfp_host_main_probe>
      ${probe_host_sources})
    target_include_directories(tsfp_shader_probe PRIVATE src/host "${TSFP_LIFTED_DIR}")
    target_compile_definitions(tsfp_shader_probe PRIVATE ${probe_host_definitions})
    target_link_options(tsfp_shader_probe PRIVATE
      "LINKER:--wrap=recomp_lookup_original"
      "LINKER:--wrap=sub_00383678"
      "LINKER:--wrap=sub_00383DF3"
      "LINKER:--undefined=recomp_lookup_original")
    if(EXISTS "${TSFP_LIFTED_DIR}/recomp_xmv_original.c")
      target_link_options(tsfp_shader_probe PRIVATE "LINKER:--undefined=recomp_xmv_lookup_original")
    endif()
    target_link_libraries(tsfp_shader_probe PRIVATE ${probe_host_libraries})
    set_target_properties(tsfp_shader_probe PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  include(src/game/game.cmake)

  foreach(callback_route_target test_recomp_callback_routes test_recomp_callback_routes_original)
    add_executable(${callback_route_target} tests/c/test_recomp_callback_routes.c
      src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime> $<TARGET_OBJECTS:tsfp_recomp_callback>)
    target_link_libraries(${callback_route_target} PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
    add_test(NAME ${callback_route_target} COMMAND ${callback_route_target})
  endforeach()
  target_sources(test_recomp_callback_routes PRIVATE $<TARGET_OBJECTS:tsfp_game>)
  add_executable(test_xgrph_texture_routes tests/c/test_xgrph_texture_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_xgrph_texture_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_link_libraries(test_xgrph_texture_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME xgrph_texture_routes COMMAND test_xgrph_texture_routes)
  add_executable(test_xgrph_shader_query_routes tests/c/test_xgrph_shader_query_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_xgrph_shader_query_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_link_libraries(test_xgrph_shader_query_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME xgrph_shader_query_routes COMMAND test_xgrph_shader_query_routes)
  add_executable(test_d3d8_texture_lock_routes tests/c/test_d3d8_texture_lock_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_d3d8_texture_lock_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_link_libraries(test_d3d8_texture_lock_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME d3d8_texture_lock_routes COMMAND test_d3d8_texture_lock_routes)
  add_executable(test_d3d8_scissor_routes tests/c/test_d3d8_scissor_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_d3d8_scissor_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_link_libraries(test_d3d8_scissor_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME d3d8_scissor_routes COMMAND test_d3d8_scissor_routes)
  add_executable(test_d3d8_vertex_constants_routes tests/c/test_d3d8_vertex_constants_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_d3d8_vertex_constants_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_link_libraries(test_d3d8_vertex_constants_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME d3d8_vertex_constants_routes COMMAND test_d3d8_vertex_constants_routes)
  add_executable(test_d3d8_vertex_program_routes tests/c/test_d3d8_vertex_program_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_d3d8_vertex_program_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_link_libraries(test_d3d8_vertex_program_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME d3d8_vertex_program_routes COMMAND test_d3d8_vertex_program_routes)
  add_executable(test_d3d8_target_query_routes tests/c/test_d3d8_target_query_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_d3d8_target_query_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_link_libraries(test_d3d8_target_query_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME d3d8_target_query_routes COMMAND test_d3d8_target_query_routes)
  add_executable(test_d3d8_surface_level_routes tests/c/test_d3d8_surface_level_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_d3d8_surface_level_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_link_libraries(test_d3d8_surface_level_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME d3d8_surface_level_routes COMMAND test_d3d8_surface_level_routes)

  add_executable(test_kernel_thread_object_routes tests/c/test_kernel_thread_object_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_kernel_thread_object_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_kernel_thread_object_routes PRIVATE TSFP_HAVE_KERNEL_ARITY_INC=1 _DEFAULT_SOURCE)
  target_link_libraries(test_kernel_thread_object_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME kernel_thread_object_routes COMMAND test_kernel_thread_object_routes)

  add_executable(test_kernel_event_handle_routes tests/c/test_kernel_event_handle_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_kernel_event_handle_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_kernel_event_handle_routes PRIVATE TSFP_HAVE_KERNEL_ARITY_INC=1 _DEFAULT_SOURCE)
  target_link_libraries(test_kernel_event_handle_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME kernel_event_handle_routes COMMAND test_kernel_event_handle_routes)

  add_executable(test_dsound_stream_pause_routes tests/c/test_dsound_stream_pause_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_dsound_stream_pause_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_dsound_stream_pause_routes PRIVATE _DEFAULT_SOURCE)
  target_link_libraries(test_dsound_stream_pause_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME dsound_stream_pause_routes COMMAND test_dsound_stream_pause_routes)

  add_executable(test_dsound_movie_stream_routes tests/c/test_dsound_movie_stream_routes.c
    src/host/kernel_thunk.c src/host/recomp_guest_call.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_dsound_movie_stream_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_dsound_movie_stream_routes PRIVATE _DEFAULT_SOURCE)
  target_link_libraries(test_dsound_movie_stream_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME dsound_movie_stream_routes COMMAND test_dsound_movie_stream_routes)

  add_executable(test_dsound_synch_routes tests/c/test_dsound_synch_routes.c
    src/host/kernel_thunk.c src/host/recomp_guest_call.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_dsound_synch_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_dsound_synch_routes PRIVATE _DEFAULT_SOURCE)
  target_link_libraries(test_dsound_synch_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME dsound_synch_routes COMMAND test_dsound_synch_routes)
  # Exit 77 when the lift has no SynchPlayback stop boundary: a skip, never a pass.
  set_tests_properties(dsound_synch_routes PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(test_d3d8_pixel_constants_routes tests/c/test_d3d8_pixel_constants_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_d3d8_pixel_constants_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_link_libraries(test_d3d8_pixel_constants_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME d3d8_pixel_constants_routes COMMAND test_d3d8_pixel_constants_routes)

  add_executable(test_d3d8_pixel_bind_routes tests/c/test_d3d8_pixel_bind_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_d3d8_pixel_bind_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_link_libraries(test_d3d8_pixel_bind_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME d3d8_pixel_bind_routes COMMAND test_d3d8_pixel_bind_routes)

  foreach(second_route_target test_recomp_second_vblank_routes test_recomp_second_vblank_routes_original)
    add_executable(${second_route_target} tests/c/test_recomp_second_vblank_routes.c
      src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime> $<TARGET_OBJECTS:tsfp_recomp_callback>)
    target_include_directories(${second_route_target} PRIVATE src/host "${TSFP_LIFTED_DIR}")
    target_compile_options(${second_route_target} PRIVATE -Wno-conversion -Wno-sign-conversion)
    target_link_libraries(${second_route_target} PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox ${TSFP_LIBM})
    add_test(NAME ${second_route_target} COMMAND ${second_route_target})
  endforeach()
  target_sources(test_recomp_second_vblank_routes PRIVATE $<TARGET_OBJECTS:tsfp_game>)

  # T676: both tests include the generated, gitignored xdk_surface.h, so without it the whole
  # build failed. Register them as explicit SKIPs then, like xdk_surface above.
  if(TSFP_HAVE_XDK_SURFACE)
    add_executable(test_dsound_stream_virtual_routes tests/c/test_dsound_stream_virtual_routes.c
      src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
    target_include_directories(test_dsound_stream_virtual_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
    target_compile_definitions(test_dsound_stream_virtual_routes PRIVATE _DEFAULT_SOURCE)
    target_link_libraries(test_dsound_stream_virtual_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
    add_test(NAME dsound_stream_virtual_routes COMMAND test_dsound_stream_virtual_routes)

    add_executable(test_dsound_stream_status_routes tests/c/test_dsound_stream_status_routes.c
      src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
    target_include_directories(test_dsound_stream_status_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
    target_compile_definitions(test_dsound_stream_status_routes PRIVATE _DEFAULT_SOURCE)
    target_link_libraries(test_dsound_stream_status_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
    add_test(NAME dsound_stream_status_routes COMMAND test_dsound_stream_status_routes)
  else()
    foreach(skipped_dsound_test dsound_stream_virtual_routes dsound_stream_status_routes)
      add_test(NAME ${skipped_dsound_test}
               COMMAND ${CMAKE_COMMAND} -E echo
                       "SKIP: src/xbox/xdk_surface.h is not generated (run tools/gen_d3d8_surface.py)")
      set_tests_properties(${skipped_dsound_test} PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP")
    endforeach()
  endif()

  add_executable(test_t1245_frequency_routes tests/c/test_t1245_frequency_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_t1245_frequency_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_t1245_frequency_routes PRIVATE _DEFAULT_SOURCE)
  target_link_libraries(test_t1245_frequency_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME t1245_frequency_routes COMMAND test_t1245_frequency_routes)

  if(TSFP_HAVE_XDK_SURFACE)
    add_executable(t1245_registration_policy tests/c/t1245_registration_policy.c
      src/xbox/xdk_registration_report.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
    target_include_directories(t1245_registration_policy PRIVATE src/host)
    target_link_libraries(t1245_registration_policy PRIVATE tsfp_gpu tsfp_thunk tsfp_xbox)
    if(TSFP_XEMU_DSP_SOURCE)
      add_test(NAME t1245_registration_policy COMMAND t1245_registration_policy)
    else()
      add_test(NAME t1245_registration_policy
        COMMAND ${CMAKE_COMMAND} -E echo "SKIP: registration policy audit needs pinned xemu DSP core")
      set_tests_properties(t1245_registration_policy PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP")
    endif()
  else()
    # xdk_surface.h is generated from the user's XBE and gitignored: SKIP, never pass.
    add_test(NAME t1245_registration_policy
      COMMAND ${CMAKE_COMMAND} -E echo "SKIP: registration policy audit needs the generated xdk_surface.h")
    set_tests_properties(t1245_registration_policy PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP")
  endif()

  add_executable(test_dsound_stream_volume_routes tests/c/test_dsound_stream_volume_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_dsound_stream_volume_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_dsound_stream_volume_routes PRIVATE _DEFAULT_SOURCE)
  target_link_libraries(test_dsound_stream_volume_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME dsound_stream_volume_routes COMMAND test_dsound_stream_volume_routes)

  add_executable(test_dsound_listener_commit_routes tests/c/test_dsound_listener_commit_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_dsound_listener_commit_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_dsound_listener_commit_routes PRIVATE _DEFAULT_SOURCE)
  target_link_libraries(test_dsound_listener_commit_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME dsound_listener_commit_routes COMMAND test_dsound_listener_commit_routes)

  add_executable(test_dsound_listener_work_routes tests/c/test_dsound_listener_work_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_dsound_listener_work_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_dsound_listener_work_routes PRIVATE _DEFAULT_SOURCE)
  target_link_libraries(test_dsound_listener_work_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME dsound_listener_work_routes COMMAND test_dsound_listener_work_routes)

  add_executable(test_dsound_stream_flush_routes tests/c/test_dsound_stream_flush_routes.c
    src/host/kernel_thunk.c $<TARGET_OBJECTS:tsfp_recomp_runtime>)
  target_include_directories(test_dsound_stream_flush_routes PRIVATE src/host "${TSFP_LIFTED_DIR}")
  target_compile_definitions(test_dsound_stream_flush_routes PRIVATE _DEFAULT_SOURCE)
  target_link_libraries(test_dsound_stream_flush_routes PRIVATE tsfp_lifted tsfp_thunk tsfp_xbox)
  add_test(NAME dsound_stream_flush_routes COMMAND test_dsound_stream_flush_routes)


else()
  message(STATUS
    "no lifted code at ${TSFP_LIFTED_DIR} -- skipping tsfp_lifted and tsfp_host. "
    "Generate it with:\n"
    "    uv run python -m tools.lift run <your default.xbe>\n"
    "(the lifter is already vendored at third_party/xboxrecomp/, so tools.lift vendor "
    "is only needed when re-vendoring from a fresh upstream checkout)")
endif()

# T1016: real constrained allocation/reuse/lifetime tests, device-free.
add_executable(test_t1016_physical tests/c/test_t1016_physical.c)
target_link_libraries(test_t1016_physical PRIVATE tsfp_xbox)
add_test(NAME t1016_physical COMMAND test_t1016_physical)
set_tests_properties(t1016_physical PROPERTIES TIMEOUT 30)

# T1022 real host allocation failures and stream ordering across growth.
add_executable(test_t1022_recording tests/c/test_t1022_recording.c)
target_link_libraries(test_t1022_recording PRIVATE tsfp_gpu tsfp_xbox)
target_link_options(test_t1022_recording PRIVATE -Wl,--wrap=malloc -Wl,--wrap=realloc)
add_test(NAME t1022_recording COMMAND test_t1022_recording)
set_tests_properties(t1022_recording PROPERTIES TIMEOUT 30)

# T1029: independently retain returned command snapshots across recorder storage changes.
add_executable(test_t1029_recording_snapshot tests/c/test_t1029_recording_snapshot.c)
target_link_libraries(test_t1029_recording_snapshot PRIVATE tsfp_gpu tsfp_xbox)
add_test(NAME t1029_recording_snapshot COMMAND test_t1029_recording_snapshot)
set_tests_properties(t1029_recording_snapshot PROPERTIES TIMEOUT 30)

# T1032 held image/descriptor lifetime across two unsubmitted draws.
if(TARGET tsfp_live_vk_texture)
  add_executable(test_t1032_texture_lifetimes tests/c/test_t1032_texture_lifetimes.c)
  target_link_libraries(test_t1032_texture_lifetimes PRIVATE tsfp_live_vk_texture)
  target_link_options(test_t1032_texture_lifetimes PRIVATE -Wl,--wrap=realloc)
  add_test(NAME t1032_texture_lifetimes COMMAND test_t1032_texture_lifetimes)
  set_tests_properties(t1032_texture_lifetimes PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

  # T1041: no-submit control for a later descriptor-page reset failure.
  add_executable(test_t1041_partial_pool_reset tests/c/test_t1041_partial_pool_reset.c)
  target_link_libraries(test_t1041_partial_pool_reset PRIVATE tsfp_live_vk_texture)
  add_test(NAME t1041_partial_pool_reset COMMAND test_t1041_partial_pool_reset)
  set_tests_properties(t1041_partial_pool_reset PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

  # T1061: independently check multiple successful pool resets, recovery, and batch-local key identity.
  add_executable(test_t1061_descriptor_reset_recovery tests/c/test_t1061_descriptor_reset_recovery.c)
  target_link_libraries(test_t1061_descriptor_reset_recovery PRIVATE tsfp_live_vk_texture)
  add_test(NAME t1061_descriptor_reset_recovery COMMAND test_t1061_descriptor_reset_recovery)
  set_tests_properties(t1061_descriptor_reset_recovery PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
endif()
add_executable(test_gpu_pgraph_visibility tests/c/test_gpu_pgraph_visibility.c)
target_link_libraries(test_gpu_pgraph_visibility PRIVATE tsfp_gpu)
add_test(NAME gpu_pgraph_visibility COMMAND test_gpu_pgraph_visibility)

add_executable(test_d3d8_visibility tests/c/test_d3d8_visibility.c)
target_link_libraries(test_d3d8_visibility PRIVATE tsfp_gpu)
add_test(NAME d3d8_visibility COMMAND test_d3d8_visibility)
# T1246: the report slot waits of the pipelined live renderer, through the production visibility entries.
add_executable(test_live_report_pending tests/c/test_live_report_pending.c)
target_link_libraries(test_live_report_pending PRIVATE tsfp_gpu)
add_test(NAME live_report_pending COMMAND test_live_report_pending)

if(TARGET tsfp_live_vk_texture)
  add_executable(test_t1051_texture_anisotropy tests/c/test_t1051_texture_anisotropy.c)
  target_link_libraries(test_t1051_texture_anisotropy PRIVATE tsfp_live_vk_bind)
  add_test(NAME t1051_texture_anisotropy COMMAND test_t1051_texture_anisotropy)
  set_tests_properties(t1051_texture_anisotropy PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
endif()

# T1015: stale visibility completion must preserve a replacement allocation.
add_executable(test_t1015_visibility_identity tests/c/test_t1015_visibility_identity.c)
target_link_libraries(test_t1015_visibility_identity PRIVATE tsfp_gpu)
add_test(NAME t1015_visibility_identity COMMAND test_t1015_visibility_identity)
# T1039 real live-translator point state and literal Vulkan coverage/feature guards.
if(TARGET tsfp_live_vk_frame)
  find_program(T1039_PYTHON python3 REQUIRED)
  find_program(T1039_GLSLANG glslangValidator)
  if(T1039_GLSLANG)
    set(T1039_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/t1039-shaders)
    add_custom_command(OUTPUT ${T1039_SHADER_DIR}/without_point.spv ${T1039_SHADER_DIR}/with_point.spv
      COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
        ${T1039_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/t1039_point_probe/make_shader.py ${T1039_SHADER_DIR}
      DEPENDS tests/fixtures/t1039_point_probe/make_shader.py tools/nv2a/translate.py tools/nv2a/viewport.py tools/nv2a/isa.py)
    add_custom_target(t1039_point_shaders DEPENDS ${T1039_SHADER_DIR}/without_point.spv ${T1039_SHADER_DIR}/with_point.spv)
    add_executable(test_t1039_point_size tests/c/test_t1039_point_size.c)
    target_link_libraries(test_t1039_point_size PRIVATE tsfp_live_vk_frame)
    add_dependencies(test_t1039_point_size t1039_point_shaders)
    add_test(NAME t1039_point_size COMMAND test_t1039_point_size ${T1039_SHADER_DIR})
    set_tests_properties(t1039_point_size PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
  endif()
endif()

# T1042 independent boundary controls reuse T1039 synthetic shader generation.
if(TARGET t1039_point_shaders)
    add_executable(test_t1042_point_review tests/c/test_t1042_point_review.c)
    target_link_libraries(test_t1042_point_review PRIVATE tsfp_live_vk_frame)
    add_dependencies(test_t1042_point_review t1039_point_shaders)
    add_test(NAME t1042_point_review COMMAND test_t1042_point_review ${T1039_SHADER_DIR})
    set_tests_properties(t1042_point_review PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    add_executable(test_t1042_point_vuid tests/c/test_t1042_point_vuid.c)
    target_link_libraries(test_t1042_point_vuid PRIVATE tsfp_gpu)
endif()

# T1059 original/xemu-discriminated programmable fog factor and real draw pixels.
add_executable(test_t1059_fog tests/c/test_t1059_fog.c)
target_link_libraries(test_t1059_fog PRIVATE tsfp_gpu)
add_test(NAME t1059_fog COMMAND test_t1059_fog)
if(TARGET tsfp_live_vk_frame)
  set(T1059_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/t1059-shaders)
  add_custom_command(OUTPUT ${T1059_SHADER_DIR}/fog.vert.spv ${T1059_SHADER_DIR}/fog.frag.spv ${T1059_SHADER_DIR}/explicit.vert.spv ${T1059_SHADER_DIR}/alpha.frag.spv ${T1059_SHADER_DIR}/names.h
    COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
      python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/t1059_live_fog/make_shader.py ${T1059_SHADER_DIR}
    DEPENDS tests/fixtures/t1059_live_fog/make_shader.py tools/nv2a/fog.py tools/nv2a/translate.py tools/nv2a_combiner/glsl.py)
  add_custom_target(t1059_fog_shaders DEPENDS ${T1059_SHADER_DIR}/fog.vert.spv ${T1059_SHADER_DIR}/fog.frag.spv ${T1059_SHADER_DIR}/explicit.vert.spv ${T1059_SHADER_DIR}/alpha.frag.spv ${T1059_SHADER_DIR}/names.h)
endif()

if(TARGET t1059_fog_shaders)
  add_executable(test_t1059_live_fog tests/c/test_t1059_live_fog.c)
  target_include_directories(test_t1059_live_fog PRIVATE ${T1059_SHADER_DIR})
  target_link_libraries(test_t1059_live_fog PRIVATE tsfp_live_vk_frame)
  add_dependencies(test_t1059_live_fog t1059_fog_shaders)
  add_test(NAME t1059_live_fog COMMAND test_t1059_live_fog ${T1059_SHADER_DIR})
  set_tests_properties(t1059_live_fog PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
endif()

add_executable(test_mu_device tests/c/test_mu_device.c)
target_link_libraries(test_mu_device PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_mu_device PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME mu_device COMMAND test_mu_device)
add_executable(test_t1059_create_fog tests/c/test_t1059_create_fog.c)
target_link_libraries(test_t1059_create_fog PRIVATE tsfp_gpu)
add_test(NAME t1059_create_fog COMMAND test_t1059_create_fog)

add_executable(test_t1103_xonline_title tests/c/test_t1103_xonline_title.c)
target_link_libraries(test_t1103_xonline_title PRIVATE tsfp_thunk)
set_target_properties(test_t1103_xonline_title PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME t1103_xonline_title COMMAND test_t1103_xonline_title)
add_executable(test_t1085_mu_enumeration tests/c/test_t1085_mu_enumeration.c)
target_link_libraries(test_t1085_mu_enumeration PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_t1085_mu_enumeration PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME t1085_mu_enumeration COMMAND test_t1085_mu_enumeration)

add_executable(test_t1085_mu_reset tests/c/test_t1085_mu_reset.c)
target_link_libraries(test_t1085_mu_reset PRIVATE tsfp_xbox tsfp_gpu)
set_target_properties(test_t1085_mu_reset PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME t1085_mu_reset COMMAND test_t1085_mu_reset)

add_executable(test_t1111_xnet_random tests/c/test_t1111_xnet_random.c)
target_link_libraries(test_t1111_xnet_random PRIVATE tsfp_thunk)
target_link_options(test_t1111_xnet_random PRIVATE -Wl,--wrap=getrandom)
add_test(NAME t1111_xnet_random COMMAND test_t1111_xnet_random)
add_executable(test_t1111_options tests/c/test_t1111_options.c)
target_link_libraries(test_t1111_options PRIVATE tsfp_host_options)
add_test(NAME t1111_options COMMAND test_t1111_options)
# T1085 explicit writable-image startup policy; independent of generated surfaces.
target_sources(tsfp_xbox PRIVATE src/input/mu_startup.c)
add_executable(test_t1085_mu_options tests/c/test_t1085_mu_options.c)
target_link_libraries(test_t1085_mu_options PRIVATE tsfp_host_options)
set_target_properties(test_t1085_mu_options PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME t1085_mu_options COMMAND test_t1085_mu_options)

# T1147 exercised real DATA41/42 patching, unavailable NULL, annex and reset.
add_executable(test_t1147_disk_thunk tests/c/t1147_disk_thunk.c src/host/kernel_thunk.c)
target_include_directories(test_t1147_disk_thunk PRIVATE src/host)
target_compile_definitions(test_t1147_disk_thunk PRIVATE _GNU_SOURCE)
target_compile_options(test_t1147_disk_thunk PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_t1147_disk_thunk PRIVATE -Wl,--gc-sections)
target_link_libraries(test_t1147_disk_thunk PRIVATE tsfp_xbox)
add_test(NAME t1147_disk_thunk COMMAND test_t1147_disk_thunk)

# T1091 original-volume comparison runner; driven by Python controls.
add_executable(t1091_config_runner tests/c/t1091_config.c)
target_link_libraries(t1091_config_runner PRIVATE tsfp_xbox ${TSFP_LIBM})

add_executable(t1091_volume_runner tests/c/t1091_volume.c)
target_link_libraries(t1091_volume_runner PRIVATE tsfp_xbox ${TSFP_LIBM})

# T1091 real EEPROM source option, independent of local entropy policy.
add_executable(test_t1091_eeprom_options tests/c/test_t1091_eeprom_options.c)
target_link_libraries(test_t1091_eeprom_options PRIVATE tsfp_host_options)
target_compile_options(test_t1091_eeprom_options PRIVATE -UNDEBUG)
add_test(NAME t1091_eeprom_options COMMAND test_t1091_eeprom_options)

# T1091 authenticated source321/323 complete-byte annex and startup lifecycle.
add_executable(test_t1091_eeprom_thunk tests/c/t1091_eeprom_thunk.c src/host/kernel_thunk.c)
target_include_directories(test_t1091_eeprom_thunk PRIVATE src/host)
target_compile_definitions(test_t1091_eeprom_thunk PRIVATE _GNU_SOURCE)
target_compile_options(test_t1091_eeprom_thunk PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_t1091_eeprom_thunk PRIVATE -Wl,--gc-sections)
target_link_libraries(test_t1091_eeprom_thunk PRIVATE tsfp_xbox)
add_test(NAME t1091_eeprom_thunk COMMAND test_t1091_eeprom_thunk)

# T1091 drain: exact shared-anchor verification only, declared snapshot fixtures.
add_executable(t1091_anchor_repair tests/c/t1091_anchor_repair.c)
target_compile_definitions(t1091_anchor_repair PRIVATE _DEFAULT_SOURCE)
target_link_libraries(t1091_anchor_repair PRIVATE tsfp_xbox tsfp_gpu)
target_link_options(t1091_anchor_repair PRIVATE
    "-Wl,--wrap=dsound_stream_get_snapshot" "-Wl,--wrap=dsound_buffer_get_snapshot")
add_test(NAME t1091_anchor_repair COMMAND t1091_anchor_repair)

# Optional pinned-source private DSP library; no default guest/audio activation.
include(cmake/DSoundDSP.cmake)

add_executable(test_kernel_save_over tests/c/test_kernel_save_over.c)
target_link_libraries(test_kernel_save_over PRIVATE tsfp_xbox)
target_compile_definitions(test_kernel_save_over PRIVATE _DEFAULT_SOURCE)
set_target_properties(test_kernel_save_over PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME kernel_save_over COMMAND test_kernel_save_over)
# T1191 actual mounted-image capability, explicit policy option and DATA annex.
add_executable(test_t1191_ata_backing tests/c/test_t1191_ata_backing.c src/host/kernel_thunk.c)
target_include_directories(test_t1191_ata_backing PRIVATE src/host)
target_compile_definitions(test_t1191_ata_backing PRIVATE _GNU_SOURCE)
target_compile_options(test_t1191_ata_backing PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_t1191_ata_backing PRIVATE -Wl,--gc-sections)
target_link_libraries(test_t1191_ata_backing PRIVATE tsfp_xbox tsfp_host_options)
add_test(NAME t1191_ata_backing COMMAND test_t1191_ata_backing)

# T1245 source-clock consumer also links the original pitch adapter.
target_link_libraries(tsfp_xbox PUBLIC m)

add_executable(test_voice_observer tests/c/test_voice_observer.c src/host/voice_observer.c)
target_include_directories(test_voice_observer PRIVATE src/host src/input)
target_link_libraries(test_voice_observer PRIVATE tsfp_xbox)
target_compile_definitions(test_voice_observer PRIVATE _POSIX_C_SOURCE=200809L)
add_test(NAME voice_observer COMMAND test_voice_observer)
