# ============================================================================
# surrealdb.cpp — tests
# ============================================================================
#
# These test the C++ surface. A C test used to live here as well, exercising
# the C API through a C++ project; it was removed because surrealdb.c has its
# own suite of 17 groups and duplicating a thin slice of it here only produced
# a second place to forget to update.
#
# No framework: each file is a standalone binary that returns non-zero on
# failure. That keeps them runnable directly from a compiler, which is what
# makes it cheap to sweep them across every supported standard and standard
# library rather than only the one CMake happens to pick.

set(SURREALDB_CPP_TEST_SOURCES
    p0_foundation.cpp   # config / owned / error / result / invoke
    p1_data.cpp         # value / array_view / object
    p2_connection.cpp   # connection / options / transactions
    p3_stream.cpp       # live queries / notifications
    p4_dsl.cpp          # query DSL: fluent + guards
    p7_make.cpp         # value construction: scalars, geometry, bounds, ranges
    p8_connection_full.cpp # call / insert_relation / JSON Patch
    p9_rpc.cpp          # RPC context, sessions, notification stream
    p10_results.cpp     # query results: per-statement status, single()
    p11_geometry.cpp    # geometry and record-id views
    p12_standards.cpp   # feature gates vs the surface they claim
    p13_timeout.cpp     # bounded waits: poll<T>, next_for, try_next
    p14_sessions.cpp    # forked sessions, runtime options, process-wide init
    compat_probe.cpp    # whole-surface breadth probe; also the compat-matrix probe
)

# Compiled but not run: the README's own code, so a snippet cannot rot into
# something that no longer builds. An OBJECT library because there is nothing
# worth linking or executing -- compiling it is the whole test.
add_library(surrealdb_cpp_readme_snippets OBJECT readme_snippets.cpp)
target_link_libraries(surrealdb_cpp_readme_snippets PRIVATE surrealdb::surrealdb_cpp)

# The breadth probe again, through `import surrealdb;`.
#
# Built only when modules are on. A missing export in surrealdb.cppm does not
# break the header build, so nothing else here would notice it -- which is
# exactly how the DSL comparison operators stayed unexported.
if(SURREALDB_USE_MODULES AND TARGET surrealdb::surrealdb_cpp_module)
    add_executable(surrealdb_cpp_module_probe module_probe.cpp)
    target_link_libraries(surrealdb_cpp_module_probe PRIVATE surrealdb::surrealdb_cpp_module)
    target_compile_features(surrealdb_cpp_module_probe PRIVATE cxx_std_20)
    set_target_properties(surrealdb_cpp_module_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
    add_test(NAME surrealdb_cpp_module_probe COMMAND surrealdb_cpp_module_probe)
    set_tests_properties(surrealdb_cpp_module_probe PROPERTIES TIMEOUT 300)
endif()

# Consume the package the way a user does: install it, then `find_package` it
# from a project that knows nothing about this source tree.
#
# Everything else here builds against the sources, so nothing else can see a
# broken install. Skipped under multi-config generators, where `cmake --install`
# needs a configuration this test has no business choosing.
if(NOT CMAKE_CONFIGURATION_TYPES AND UNIX)
    add_test(NAME surrealdb_cpp_consumer
             COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/consumer-test.sh"
                     "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}")
    set_tests_properties(surrealdb_cpp_consumer PROPERTIES TIMEOUT 600)

    # The same again for `import surrealdb;`. Installing a modules build used to
    # produce a package with no module target in it at all -- find_package
    # succeeded and the import was simply unavailable.
    if(SURREALDB_USE_MODULES AND TARGET surrealdb_cpp_module)
        add_test(NAME surrealdb_cpp_consumer_module
                 COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/consumer-test.sh"
                         "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}" consumer_module)
        set_tests_properties(surrealdb_cpp_consumer_module PROPERTIES TIMEOUT 600)
    endif()
endif()

# ============================================================================
# The same suites, at every standard the toolchain supports
# ============================================================================
#
# C++17 is a floor with no ceiling, and the headers change shape as the
# standard rises: gates open, types gain range conformance and span interop,
# `= delete` starts carrying reasons. A suite run only at whatever standard the
# host happened to configure proves one column of that.
#
# This used to be a shell script outside the build -- which meant it was only
# ever as current as whoever remembered to run it, and it could not be part of
# `ctest`. Now the build generates it.
#
# Two levels, because linking is the expensive part: every suite is *compiled*
# at every standard, which is what catches a gate that stops compiling; and the
# two suites whose results can actually differ are *linked and run*. Each linked
# binary carries the whole Rust staticlib, so building 11 of them per standard
# would cost tens of gigabytes to test assertions that do not vary.

option(SURREALDB_CPP_SWEEP_STANDARDS
       "Compile every suite at every supported C++ standard, and run the ones that vary" ON)

# Detected rather than hardcoded: a list with 26 in it would fail on a
# toolchain that has not got there, and the point is to test what this compiler
# actually supports.
set(_sdb_std_candidates 17 20 23 26)
set(SURREALDB_CPP_TEST_STANDARDS "")
foreach(_std IN LISTS _sdb_std_candidates)
    if("cxx_std_${_std}" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        list(APPEND SURREALDB_CPP_TEST_STANDARDS ${_std})
    endif()
endforeach()

# Suites whose behaviour can legitimately differ by standard, so they are run
# rather than merely compiled.
set(SURREALDB_CPP_RUN_PER_STANDARD p12_standards compat_probe)

if(SURREALDB_CPP_SWEEP_STANDARDS AND SURREALDB_CPP_TEST_STANDARDS)
    message(STATUS "Standard sweep: c++${SURREALDB_CPP_TEST_STANDARDS}")

    foreach(_std IN LISTS SURREALDB_CPP_TEST_STANDARDS)
        foreach(_src IN LISTS SURREALDB_CPP_TEST_SOURCES)
            get_filename_component(_name "${_src}" NAME_WE)

            if(_name IN_LIST SURREALDB_CPP_RUN_PER_STANDARD)
                set(_t "surrealdb_cpp_${_name}_cxx${_std}")
                add_executable(${_t} "${_src}")
                target_link_libraries(${_t} PRIVATE surrealdb::surrealdb_cpp)
                set_target_properties(${_t} PROPERTIES
                    CXX_STANDARD ${_std}
                    CXX_STANDARD_REQUIRED ON
                    CXX_EXTENSIONS OFF
                    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
                surrealdb_apply_sanitizers(${_t})
                if(MSVC)
                    target_link_options(${_t} PRIVATE /NODEFAULTLIB:MSVCRT)
                endif()
                add_test(NAME ${_t} COMMAND ${_t})
                set_tests_properties(${_t} PROPERTIES TIMEOUT 300)
                if(SURREALDB_SANITIZE MATCHES "address")
                    set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT
                        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
                endif()
            else()
                # Compile only. An OBJECT library builds the translation unit
                # and stops, which is all that is needed to prove the headers
                # still work at this standard.
                set(_t "surrealdb_cpp_${_name}_cxx${_std}_compile")
                add_library(${_t} OBJECT "${_src}")
                target_link_libraries(${_t} PRIVATE surrealdb::surrealdb_cpp)
                set_target_properties(${_t} PROPERTIES
                    CXX_STANDARD ${_std}
                    CXX_STANDARD_REQUIRED ON
                    CXX_EXTENSIONS OFF)
            endif()
        endforeach()

        # The README's own snippets, at every standard too: a code block that
        # compiles at 17 and not at 23 is still a broken code block.
        set(_t "surrealdb_cpp_readme_snippets_cxx${_std}")
        add_library(${_t} OBJECT readme_snippets.cpp)
        target_link_libraries(${_t} PRIVATE surrealdb::surrealdb_cpp)
        set_target_properties(${_t} PROPERTIES
            CXX_STANDARD ${_std}
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
    endforeach()
endif()

# Code that must NOT compile.
#
# The lifetime guards (`&`-qualified accessors, deleted rvalue overloads) stop
# bugs that used to build cleanly and read freed memory. Nothing in a passing
# suite can tell you when such a guard stops working, because the offending
# code is never supposed to become a program -- so each case is built on its own
# and expected to fail.
#
# Each case is an EXCLUDE_FROM_ALL target and the test drives the build system
# at it, rather than invoking the compiler with hand-assembled flags: that way a
# case is compiled with exactly the include paths, standard and flags the real
# build uses, and cannot start passing because a path went stale.
#
# Case 0 is the control. It must build, which is what stops a typo in the file
# from making every other case "fail to compile" for the wrong reason.
set(SURREALDB_CPP_NEG_CASES 0 1 2 4 5 6 7 8 9)   # 3 unguardable, 10/11 retired; see the file

foreach(_case IN LISTS SURREALDB_CPP_NEG_CASES)
    set(_neg surrealdb_cpp_neg_${_case})
    add_executable(${_neg} EXCLUDE_FROM_ALL negative_compile.cpp)
    target_link_libraries(${_neg} PRIVATE surrealdb::surrealdb_cpp)
    target_compile_definitions(${_neg} PRIVATE SURREALDB_NEG_CASE=${_case})
    set_target_properties(${_neg} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

    add_test(NAME ${_neg}
             COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                     --config "$<CONFIG>" --target ${_neg})
    if(NOT _case EQUAL 0)
        set_tests_properties(${_neg} PROPERTIES WILL_FAIL TRUE)
    endif()
    set_tests_properties(${_neg} PROPERTIES TIMEOUT 300)
endforeach()

foreach(_src IN LISTS SURREALDB_CPP_TEST_SOURCES)
    get_filename_component(_name "${_src}" NAME_WE)
    set(_target "surrealdb_cpp_${_name}")

    add_executable(${_target} "${_src}")

    target_link_libraries(${_target} PRIVATE surrealdb::surrealdb_cpp)

    set_target_properties(${_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
    )

    if(MSVC)
        # The Rust library links the release runtime (/MD); keep the debug
        # runtime from being pulled in alongside it.
        target_link_options(${_target} PRIVATE /NODEFAULTLIB:MSVCRT)
    endif()

    # `-DSURREALDB_SANITIZE=address,undefined` instruments every suite. The
    # option and the function come from surrealdb.c, so one flag covers both
    # halves of the stack. Sanitizer runs used to be a hand-written script
    # outside the build, which meant they were only ever as current as whoever
    # remembered to run them.
    surrealdb_apply_sanitizers(${_target})

    add_test(NAME ${_target} COMMAND ${_target})
    set_tests_properties(${_target} PROPERTIES TIMEOUT 300)

    # The Rust staticlib is not instrumented and leaks at exit by design, so
    # LeakSanitizer needs the suppressions to report only our own frames.
    if(SURREALDB_SANITIZE MATCHES "address")
        set_property(TEST ${_target} APPEND PROPERTY ENVIRONMENT
            "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
    endif()
endforeach()
