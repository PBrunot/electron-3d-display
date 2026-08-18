# extra_script_ccache.py
#
# Wires ccache into the compile commands that are ACTUALLY executed for this
# project, and points ccache's cache dir at ./.ccache.
#
# Why this is not done via `board_build.cmake_extra_args` /
# CMAKE_C_COMPILER_LAUNCHER (verified 2026-08-18, platform-espressif32@7.0.1):
#
#   PlatformIO's espidf builder runs CMake with the Ninja generator, but it
#   NEVER RUNS NINJA. `build.ninja` is generated only so PlatformIO can read
#   the CMake file-API code model and the linker-fragment info out of it; every
#   object file is then compiled by PlatformIO's own SCons builders
#   (`compile_source_files()` -> `StaticObject()` in the platform's
#   frameworks/espidf.py). Proof in a build tree: no `.ninja_log`/`.ninja_deps`
#   anywhere, `esp-idf/` (the CMake tree) holds zero `.o` files, while
#   `.pio/build/<env>/<component>/**.o` holds all ~1070 of them.
#   CMAKE_*_COMPILER_LAUNCHER only decorates ninja rules, so it was inert:
#   ccache was never invoked once, hence an empty cache and no stats.
#
# The fix is to prefix the SCons compile command strings instead. Notes:
#
#   * $CC/$CXX cannot be used for this: the platform does `env.Replace(CC=...)`
#     inside $BUILD_SCRIPT, which runs AFTER `pre:` extra scripts, so any value
#     set here would be clobbered. $CCCOM/$CXXCOM/$ASPPCOM are set up earlier
#     (by SCons' tools + PlatformIO's `piomaxlen` tool, at DefaultEnvironment
#     construction) and nothing rewrites them afterwards, so prefixing them
#     here survives and is inherited by every `env.Clone()` espidf.py makes.
#
#   * PlatformIO wraps those commands in `${TEMPFILE(...)}` (piomaxlen), which
#     spills the arguments into a response file whenever the command exceeds
#     ~7.7 kB -- which is EVERY compile here (measured: 25-30 kB per command).
#     Prefixing outside the `${TEMPFILE(...)}` keeps the compiler as argv[1],
#     giving `ccache <compiler> @resp.tmp`. Verified on this machine that
#     ccache 4.13.6 expands GCC `@file` response files and gets direct hits
#     through them, so response files do not defeat caching.
#
#   * CCACHE_DIR must go into `env["ENV"]`, not `os.environ`: SCons passes
#     `env["ENV"]` (not the interpreter's environment) to spawned commands.
#     Mutating os.environ here, as this script used to do, did nothing.
#
# Why any of this matters: PlatformIO `rmtree`s the whole `.pio/build/<env>`
# directory whenever platformio.ini changes, PIO Core is upgraded, or a source
# file is added/removed/renamed under src/ or include/ (`clean_build_dir()` +
# `compute_project_checksum()` in platformio/run/helpers.py). Every one of
# those is a from-scratch ~10 min rebuild of the full IDF, so the compiler
# cache is the only thing standing between an edit and 10 idle minutes.
#
# Check that it is working:  ccache -s   (with CCACHE_DIR set to ./.ccache)
import os

Import("env")

ccache_dir = os.path.join(env.subst("$PROJECT_DIR"), ".ccache")
os.makedirs(ccache_dir, exist_ok=True)

if not env.WhereIs("ccache"):
    print("Warning! ccache not found on PATH -- building without a compiler cache.")
else:
    # Seen by every compiler subprocess SCons spawns (os.environ is not).
    env["ENV"]["CCACHE_DIR"] = ccache_dir

    # $ASCOM (plain `as`) is deliberately left alone: ccache does not support
    # the assembler. $ASPPCOM goes through $CC, so it is cacheable.
    for name in ("CCCOM", "CXXCOM", "ASPPCOM"):
        command = env.get(name)
        if command and "ccache" not in command:
            env[name] = "ccache " + command
