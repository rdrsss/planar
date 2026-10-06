# cmake/llvm-toolchain.cmake — DISCOVER the pinned LLVM instead of
# hardcoding one machine's path (plan 996, task 6755, decision 1123).
#
# `CMakePresets.json` used to carry four absolute `/opt/homebrew/opt/llvm`
# paths in its `base` preset. That did not make the tree macOS-OPTIMIZED,
# it made it macOS-ONLY, silently, at configure time — while CLAUDE.md
# claims "macOS + Linux parity first" and task 6054 had already made the
# LINT toolchain discoverable. Half the toolchain was portable and half
# was not; `make cpp-lint` worked on Linux with an explicit prefix, but
# `cmake --preset debug` could not configure there, so there was no build
# for that working lint path to lint.
#
# THIS FILE CHANGES THE PATH, NOT THE CONTRACT. The pin documented in
# docs/toolchain-parity.md is unchanged and is still enforced: C++26
# modules and `import std` need an LLVM of the pinned major with a
# modules-enabled libc++, and discovery REFUSES anything else rather than
# falling through to a system clang that cannot build this tree.
#
# Resolution order (mirroring the lint path's, Makefile § LLVM_PREFIX):
#
#   1. An explicit `-DPLANAR_LLVM_PREFIX=<path>` (or the same name in the
#      environment) ALWAYS wins, unconditionally. Discovery is a fallback,
#      never an override.
#   2. `brew --prefix llvm` — Homebrew's LLVM is keg-only, so it is never
#      on PATH and this is the only way to find it.
#   3. The prefix of a PATH-resolved `clang++` (and the versioned
#      `clang++-<major>` names apt.llvm.org installs), plus the
#      `/usr/lib/llvm-<major>` prefix that same repository uses.
#
# Every candidate is VALIDATED before it is accepted (see
# `_planar_llvm_validate`), and a failure names the override that fixes
# it rather than dying deep inside a compile with a confusing diagnostic.

include_guard(GLOBAL)

# The pinned LLVM major. D10 targets "latest LLVM (23+)";
# docs/toolchain-parity.md § Toolchain pins carries the exact version and
# the rationale. Discovery accepts this major or newer -- never older,
# because `import std` support is what the floor is protecting.
set(PLANAR_LLVM_MAJOR_FLOOR 23)

option(PLANAR_PORTABLE "Link the pinned C++ runtime statically for release bundles" OFF)

# --- validation --------------------------------------------------------
#
# A prefix is usable only if it provides ALL of: the two compilers, a
# libc++ header tree, and a `libc++.modules.json` (the artifact that means
# this libc++ was actually built with module support -- the single most
# load-bearing check here, and the one a stock system clang fails). The
# modules.json also LOCATES the libc++ library directory, which differs
# between Homebrew (`lib/c++`) and apt.llvm.org
# (`lib/<triple>`), so it is resolved rather than assumed.
function(_planar_llvm_validate prefix out_ok out_libdir out_json out_reason)
  set(${out_ok} FALSE PARENT_SCOPE)
  set(${out_libdir} "" PARENT_SCOPE)
  set(${out_json} "" PARENT_SCOPE)

  if(NOT EXISTS "${prefix}/bin/clang++")
    set(${out_reason} "no clang++ at ${prefix}/bin/clang++" PARENT_SCOPE)
    return()
  endif()
  if(NOT EXISTS "${prefix}/bin/clang")
    set(${out_reason} "no clang at ${prefix}/bin/clang" PARENT_SCOPE)
    return()
  endif()
  if(NOT IS_DIRECTORY "${prefix}/include/c++/v1")
    set(${out_reason} "no libc++ headers at ${prefix}/include/c++/v1" PARENT_SCOPE)
    return()
  endif()

  # Three layouts, because the two that existed here only covered Homebrew.
  # Debian/Ubuntu's apt.llvm.org packages put the manifest DIRECTLY under
  # `lib/` (`/usr/lib/llvm-23/lib/libc++.modules.json`), which neither
  # `lib/c++/...` nor `lib/*/...` matches -- the latter needs an intervening
  # directory. The result was that this function rejected a perfectly
  # modules-enabled libc++ with "was built without module support", on the
  # exact toolchain its own failure message tells the reader to install
  # ("Debian/Ubuntu: apt.llvm.org's llvm-toolchain-<codename>-23"). Measured
  # against `debian:trixie-slim` + `clang-23`/`libc++-23-dev` while building
  # the Linux gate (Dockerfile, task 6936).
  file(GLOB _json
       "${prefix}/lib/c++/libc++.modules.json"
       "${prefix}/lib/libc++.modules.json"
       "${prefix}/lib/*/libc++.modules.json")
  if(NOT _json)
    set(${out_reason}
        "no libc++.modules.json under ${prefix}/lib -- this libc++ was built without module support, so `import std` cannot work"
        PARENT_SCOPE)
    return()
  endif()
  list(GET _json 0 _json_first)
  get_filename_component(_libdir "${_json_first}" DIRECTORY)

  execute_process(
    COMMAND "${prefix}/bin/clang++" --version
    OUTPUT_VARIABLE _ver_out
    ERROR_VARIABLE _ver_err
    RESULT_VARIABLE _ver_rc
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _ver_rc EQUAL 0)
    set(${out_reason} "${prefix}/bin/clang++ --version failed (exit ${_ver_rc}): ${_ver_err}" PARENT_SCOPE)
    return()
  endif()
  if(NOT _ver_out MATCHES "clang version ([0-9]+)\\.")
    set(${out_reason} "cannot parse a version out of `${prefix}/bin/clang++ --version`" PARENT_SCOPE)
    return()
  endif()
  if(CMAKE_MATCH_1 LESS PLANAR_LLVM_MAJOR_FLOOR)
    set(${out_reason}
        "clang ${CMAKE_MATCH_1} is below the pinned floor ${PLANAR_LLVM_MAJOR_FLOOR} (docs/toolchain-parity.md)"
        PARENT_SCOPE)
    return()
  endif()

  set(${out_ok} TRUE PARENT_SCOPE)
  set(${out_libdir} "${_libdir}" PARENT_SCOPE)
  set(${out_json} "${_json_first}" PARENT_SCOPE)
  set(${out_reason} "" PARENT_SCOPE)
endfunction()

# --- resolution --------------------------------------------------------

if(NOT DEFINED PLANAR_LLVM_PREFIX AND DEFINED ENV{PLANAR_LLVM_PREFIX})
  set(PLANAR_LLVM_PREFIX "$ENV{PLANAR_LLVM_PREFIX}" CACHE PATH "LLVM prefix used to build Planar")
endif()

set(_planar_llvm_explicit FALSE)
if(DEFINED PLANAR_LLVM_PREFIX AND NOT PLANAR_LLVM_PREFIX STREQUAL "")
  set(_planar_llvm_explicit TRUE)
  set(_planar_llvm_candidates "${PLANAR_LLVM_PREFIX}")
else()
  set(_planar_llvm_candidates "")

  # Homebrew: keg-only, never on PATH, so `brew --prefix` is the only lookup.
  find_program(_planar_brew NAMES brew)
  if(_planar_brew)
    execute_process(
      COMMAND "${_planar_brew}" --prefix llvm
      OUTPUT_VARIABLE _brew_prefix
      ERROR_QUIET
      RESULT_VARIABLE _brew_rc
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_brew_rc EQUAL 0 AND _brew_prefix)
      list(APPEND _planar_llvm_candidates "${_brew_prefix}")
    endif()
  endif()

  # apt.llvm.org's versioned prefixes, NEWEST FIRST (the lists are built
  # ascending and reversed -- `foreach(RANGE)` takes a positive step only).
  set(_planar_llvm_versioned "")
  set(_planar_clang_versioned "")
  foreach(_major RANGE ${PLANAR_LLVM_MAJOR_FLOOR} 30)
    if(IS_DIRECTORY "/usr/lib/llvm-${_major}")
      list(APPEND _planar_llvm_versioned "/usr/lib/llvm-${_major}")
    endif()
    list(APPEND _planar_clang_versioned "clang++-${_major}")
  endforeach()
  list(REVERSE _planar_llvm_versioned)
  list(REVERSE _planar_clang_versioned)
  list(APPEND _planar_llvm_candidates ${_planar_llvm_versioned})

  set(_planar_clang_names clang++ ${_planar_clang_versioned})
  find_program(_planar_path_clangxx NAMES ${_planar_clang_names})
  if(_planar_path_clangxx)
    get_filename_component(_bindir "${_planar_path_clangxx}" DIRECTORY)
    get_filename_component(_prefix "${_bindir}" DIRECTORY)
    list(APPEND _planar_llvm_candidates "${_prefix}")
  endif()

  list(REMOVE_DUPLICATES _planar_llvm_candidates)
endif()

set(_planar_llvm_report "")
set(_planar_llvm_found "")
foreach(_cand IN LISTS _planar_llvm_candidates)
  _planar_llvm_validate("${_cand}" _ok _libdir _json _reason)
  if(_ok)
    set(_planar_llvm_found "${_cand}")
    set(_planar_llvm_libdir "${_libdir}")
    set(_planar_llvm_json "${_json}")
    break()
  endif()
  string(APPEND _planar_llvm_report "\n    ${_cand}: ${_reason}")
endforeach()

if(NOT _planar_llvm_found)
  if(_planar_llvm_explicit)
    message(FATAL_ERROR
      "PLANAR_LLVM_PREFIX='${PLANAR_LLVM_PREFIX}' is not a usable LLVM for this tree:${_planar_llvm_report}\n"
      "  Planar needs LLVM ${PLANAR_LLVM_MAJOR_FLOOR}+ with a modules-enabled libc++ (C++26 modules and `import std`).\n"
      "  See docs/toolchain-parity.md.")
  endif()
  if(NOT _planar_llvm_candidates)
    set(_planar_llvm_report "\n    (no candidate prefixes found at all)")
  endif()
  message(FATAL_ERROR
    "No usable LLVM found. Planar needs LLVM ${PLANAR_LLVM_MAJOR_FLOOR}+ with a modules-enabled "
    "libc++ (C++26 modules and `import std`); see docs/toolchain-parity.md.\n"
    "  Candidates tried:${_planar_llvm_report}\n"
    "  Install the pinned LLVM (macOS: `brew install llvm`; Debian/Ubuntu: apt.llvm.org's "
    "llvm-toolchain-<codename>-${PLANAR_LLVM_MAJOR_FLOOR}), or point at it explicitly:\n"
    "      cmake --preset debug -DPLANAR_LLVM_PREFIX=/path/to/llvm")
endif()

set(PLANAR_LLVM_PREFIX "${_planar_llvm_found}" CACHE PATH "LLVM prefix used to build Planar" FORCE)

# --- the pinned flag set, now expressed against the resolved prefix -----
#
# Byte-for-byte what CMakePresets.json's `base` preset carried, with
# `/opt/homebrew/opt/llvm` replaced by the resolved prefix and the libc++
# library directory resolved from libc++.modules.json rather than assumed
# to be `lib/c++`. See docs/toolchain-parity.md § Derived import-std /
# embed flag set.
# These land in the CACHE, exactly as the preset's `cacheVariables` block
# did, rather than as `*_INIT` normal variables. Two reasons, both
# load-bearing:
#   - `make cpp-lint` resolves its LLVM_PREFIX from this build directory's
#     `CMAKE_CXX_COMPILER` cache entry (Makefile § LLVM_PREFIX). That is
#     the binding that keeps the formatter and the compiler from drifting
#     apart, and a toolchain file that sets only normal variables would
#     silently remove the entry it reads.
#   - `*_INIT` flags get platform additions appended by CMake; the cache
#     form reproduces the pinned flag set byte-for-byte
#     (docs/toolchain-parity.md § Derived import-std / embed flag set).
set(CMAKE_C_COMPILER "${PLANAR_LLVM_PREFIX}/bin/clang" CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${PLANAR_LLVM_PREFIX}/bin/clang++" CACHE FILEPATH "C++ compiler" FORCE)
# `-Wno-unused-command-line-argument` on non-Apple UNIX only. With
# `-nostdinc++ -isystem .../c++/v1` supplied, `-stdlib=libc++` has no
# COMPILE-time effect on Linux (it still matters at link, where it also
# appears), so clang reports it as an unused argument, which first-party
# targets' `-Werror` turns into a build failure. Apple's driver consumes it,
# which is why this never fires on macOS. Scoped to a driver-level nit: it
# silences no diagnostic about the code itself.
set(_planar_llvm_extra_cxx_flags "")
if(UNIX AND NOT APPLE)
  set(_planar_llvm_extra_cxx_flags " -Wno-unused-command-line-argument")
endif()
set(CMAKE_CXX_FLAGS "-stdlib=libc++ -nostdinc++ -isystem ${PLANAR_LLVM_PREFIX}/include/c++/v1${_planar_llvm_extra_cxx_flags}"
    CACHE STRING "C++ flags" FORCE)
if(PLANAR_PORTABLE)
  # Keep archives after objects and target libraries: Linux linkers resolve
  # static archive references in command-line order. -nostdlib++ prevents
  # clang from adding a shared libc++ after our explicit runtime archives.
  # CMake first reads a native toolchain before initializing SYSTEM_NAME.
  # Honor an explicit cross target; otherwise use the native host on that read.
  set(_planar_runtime_system "${CMAKE_SYSTEM_NAME}")
  if(NOT _planar_runtime_system)
    set(_planar_runtime_system "${CMAKE_HOST_SYSTEM_NAME}")
  endif()
  set(_planar_runtime_archives libc++.a libc++abi.a)
  set(_planar_runtime_unwind_flag "")
  if(_planar_runtime_system STREQUAL "Linux")
    list(APPEND _planar_runtime_archives libunwind.a)
    # Supply the pinned archive ourselves, rather than clang's default
    # libgcc_s unwinder. Compiler builtins and startup objects remain enabled.
    set(_planar_runtime_unwind_flag " --unwindlib=none")
  endif()
  set(_planar_runtime_libraries "")
  foreach(_archive IN LISTS _planar_runtime_archives)
    set(_path "${_planar_llvm_libdir}/${_archive}")
    if(NOT EXISTS "${_path}")
      message(FATAL_ERROR
        "PLANAR_PORTABLE requires ${_path}; install the pinned LLVM static runtime archives or configure with -DPLANAR_PORTABLE=OFF.")
    endif()
    if(APPLE)
      # System frameworks load Apple's libc++ transitively. Hide our archive
      # globals so its locale/ABI state cannot interpose with the pinned
      # runtime (a plain static link aborts in locale::~locale on startup).
      string(APPEND _planar_runtime_libraries " -Wl,-load_hidden,\"${_path}\"")
    else()
      string(APPEND _planar_runtime_libraries " \"${_path}\"")
    endif()
  endforeach()
  string(STRIP "${_planar_runtime_libraries}" _planar_runtime_libraries)
  set(CMAKE_EXE_LINKER_FLAGS "-stdlib=libc++ -nostdlib++${_planar_runtime_unwind_flag}"
      CACHE STRING "Executable linker flags" FORCE)
  set(CMAKE_CXX_STANDARD_LIBRARIES "${_planar_runtime_libraries}"
      CACHE STRING "C++ runtime libraries" FORCE)
else()
  # Preserve the developer link: Apple folds the ABI into libc++; Linux
  # needs the separate shared ABI library explicitly on its link line.
  set(_planar_llvm_abi_flag "")
  if(UNIX AND NOT APPLE)
    set(_planar_llvm_abi_flag " -lc++abi")
  endif()
  set(CMAKE_EXE_LINKER_FLAGS "-stdlib=libc++ -L${_planar_llvm_libdir} -Wl,-rpath,${_planar_llvm_libdir}${_planar_llvm_abi_flag}"
      CACHE STRING "Executable linker flags" FORCE)
  # Clear archives if an existing build tree switches back to shared mode.
  set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "C++ runtime libraries" FORCE)
endif()
set(CMAKE_CXX_STDLIB_MODULES_JSON "${_planar_llvm_json}" CACHE FILEPATH "libc++ import-std module manifest" FORCE)
