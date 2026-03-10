#!/usr/bin/env python3
#
# Copyright (c) 2026 Michael Vandeberg
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

"""
Generate CI matrix JSON for GitHub Actions.

Reads compilers.json and outputs a JSON array of matrix entries to stdout.
Each entry has fields matching what the ci.yml build job expects.

Usage:
    python3 generate-matrix.py                    # JSON array
    python3 generate-matrix.py | python3 -m json.tool  # pretty-printed
"""

import json
import os
import sys


def load_compilers(path=None):
    if path is None:
        path = os.path.join(os.path.dirname(__file__), "compilers.json")
    with open(path) as f:
        return json.load(f)


def platform_for_family(compiler_family):
    """Return the platform name for a compiler family."""
    if compiler_family in ("msvc", "clang-cl", "mingw"):
        return "windows"
    elif compiler_family == "apple-clang":
        return "macos"
    return "linux"


def make_entry(compiler_family, spec, **overrides):
    """Build a matrix entry dict from a compiler spec and optional overrides."""
    entry = {
        "compiler": compiler_family,
        "version": spec["version"],
        "cxxstd": spec["cxxstd"],
        "latest-cxxstd": spec["latest_cxxstd"],
        "runs-on": spec["runs_on"],
        "b2-toolset": spec["b2_toolset"],
        "shared": True,
        "build-type": "Release",
        platform_for_family(compiler_family): True,
    }

    if spec.get("container"):
        entry["container"] = spec["container"]
    if spec.get("cxx"):
        entry["cxx"] = spec["cxx"]
    if spec.get("cc"):
        entry["cc"] = spec["cc"]
    if spec.get("generator"):
        entry["generator"] = spec["generator"]
    if spec.get("generator_toolset"):
        entry["generator-toolset"] = spec["generator_toolset"]
    if spec.get("is_latest"):
        entry["is-latest"] = True
    if spec.get("is_earliest"):
        entry["is-earliest"] = True
    if "shared" in spec:
        entry["shared"] = spec["shared"]
    if spec.get("vcpkg_triplet"):
        entry["vcpkg-triplet"] = spec["vcpkg_triplet"]

    # CMake builds only on earliest/latest compilers, unless explicitly disabled
    if spec.get("build_cmake") is False:
        entry["build-cmake"] = False
    elif spec.get("is_latest") or spec.get("is_earliest"):
        entry["build-cmake"] = True
    if spec.get("cmake_cxxstd"):
        entry["cmake-cxxstd"] = spec["cmake_cxxstd"]
    if spec.get("cxxflags"):
        entry["cxxflags"] = spec["cxxflags"]

    entry.update(overrides)
    entry["name"] = generate_name(compiler_family, entry)
    return entry


def generate_name(compiler_family, entry):
    """Generate a human-readable job name from entry fields."""
    name_map = {
        "gcc": "GCC",
        "clang": "Clang",
        "msvc": "MSVC",
        "mingw": "MinGW Clang",
        "clang-cl": "Clang-CL",
        "apple-clang": "Apple-Clang",
    }
    prefix = name_map.get(compiler_family, compiler_family)

    version = entry["version"]
    if version != "*":
        prefix = f"{prefix} {version}"

    standards = entry["cxxstd"].split(",")
    cxxstd = ",".join(f"C++{s}" for s in standards)

    modifiers = []

    runner = entry["runs-on"]
    if "arm" in runner:
        modifiers.append("arm64")
    elif compiler_family == "apple-clang":
        # Extract macOS version from runner name
        macos_ver = runner.replace("macos-", "macOS ")
        modifiers.append(macos_ver)

    if entry.get("tsan"):
        modifiers.append("tsan")
    elif entry.get("asan") and entry.get("ubsan"):
        modifiers.append("asan+ubsan")
    elif entry.get("asan"):
        modifiers.append("asan")
    elif entry.get("ubsan"):
        modifiers.append("ubsan")

    if entry.get("coverage"):
        modifiers.append("coverage")

    if entry.get("x86"):
        modifiers.append("x86")

    if entry.get("clang-tidy"):
        modifiers.append("clang-tidy")

    if entry.get("time-trace"):
        modifiers.append("time-trace")

    if entry.get("superproject-cmake"):
        modifiers.append("superproject CMake")

    if entry.get("shared") is False:
        modifiers.append("static")

    suffix = f" ({', '.join(modifiers)})" if modifiers else ""
    return f"{prefix}: {cxxstd}{suffix}"


def generate_sanitizer_variant(compiler_family, spec):
    """Generate ASAN+UBSAN variant for the latest compiler in a family.

    MSVC does not support UBSAN; only ASAN is enabled for MSVC.
    """
    overrides = {
        "asan": True,
        "build-type": "RelWithDebInfo",
        "shared": True,
        "build-cmake": False,
    }

    # MSVC and Clang-CL only support ASAN, not UBSAN
    if compiler_family not in ("msvc", "clang-cl"):
        overrides["ubsan"] = True

    if compiler_family == "clang":
        overrides["shared"] = False

    return make_entry(compiler_family, spec, **overrides)


def generate_tsan_variant(compiler_family, spec):
    """Generate TSan variant for the latest compiler in a family."""
    overrides = {
        "tsan": True,
        "build-type": "RelWithDebInfo",
        "shared": True,
        "build-cmake": False,
    }

    if compiler_family in ("clang", "apple-clang"):
        overrides["shared"] = False

    return make_entry(compiler_family, spec, **overrides)


def generate_coverage_variant(compiler_family, spec):
    """Generate coverage variant with platform-specific flags.

    Linux/Windows: full gcov flags with atomic profile updates.
    macOS: --coverage only (Apple-Clang uses llvm-cov).
    """
    platform = platform_for_family(compiler_family)

    if platform == "macos":
        cov_flags = "--coverage"
    else:
        cov_flags = ("--coverage -fprofile-arcs -ftest-coverage"
                     " -fprofile-update=atomic")

    overrides = {
        "coverage": True,
        "coverage-flag": platform,
        "shared": False,
        "build-type": "Debug",
        "build-cmake": False,
        "cxxflags": cov_flags,
        "ccflags": cov_flags,
    }

    if platform == "linux":
        overrides["install"] = "lcov wget unzip"

    entry = make_entry(compiler_family, spec, **overrides)
    entry.pop("is-latest", None)
    entry.pop("is-earliest", None)
    return entry


def generate_x86_variant(compiler_family, spec):
    """Generate x86 (32-bit) variant (Clang only)."""
    return make_entry(compiler_family, spec,
        x86=True,
        shared=False,
        install="gcc-multilib g++-multilib")


def generate_arm_entry(compiler_family, spec):
    """Generate ARM64 variant for a compiler spec."""
    arm_runner = spec["runs_on"].replace("ubuntu-24.04", "ubuntu-24.04-arm")
    # ARM runners don't support containers — build a spec copy without container
    arm_spec = {k: v for k, v in spec.items() if k != "container"}
    arm_spec["runs_on"] = arm_runner
    return make_entry(compiler_family, arm_spec)


def generate_time_trace_variant(compiler_family, spec):
    """Generate time-trace variant for compile-time profiling (Clang only)."""
    return make_entry(compiler_family, spec, **{
        "time-trace": True,
        "build-cmake": True,
        "cxxflags": "-ftime-trace",
    })


def generate_superproject_cmake_variant(compiler_family, spec):
    """Generate a single superproject CMake build to verify integration."""
    entry = make_entry(compiler_family, spec, **{
        "superproject-cmake": True,
        "build-cmake": False,
    })
    entry.pop("is-latest", None)
    entry.pop("is-earliest", None)
    return entry


def apply_clang_tidy(entry, spec):
    """Add clang-tidy flag and install package to an entry."""
    entry["clang-tidy"] = True
    entry["build-cmake"] = False
    version = spec["version"]
    existing_install = entry.get("install", "")
    tidy_pkg = f"clang-tidy-{version}"
    entry["install"] = f"{existing_install} {tidy_pkg}".strip()
    entry["name"] = generate_name(entry["compiler"], entry)
    return entry


def main():
    compilers = load_compilers()
    matrix = []

    for family, specs in compilers.items():
        for spec in specs:
            # Base entry (x86_64 / default arch)
            base = make_entry(family, spec)
            if spec.get("clang_tidy"):
                apply_clang_tidy(base, spec)
            matrix.append(base)

            # ARM entry if supported
            if spec.get("arm"):
                matrix.append(generate_arm_entry(family, spec))

            # Variants for the latest compiler in each family
            if spec.get("is_latest"):
                if family != "mingw":
                    matrix.append(generate_sanitizer_variant(family, spec))

                # TSan is incompatible with ASan; separate variant for Linux
                if family in ("gcc", "clang", "apple-clang"):
                    matrix.append(generate_tsan_variant(family, spec))

                # GCC always gets coverage; other families opt in via spec flag
                if family == "gcc" or spec.get("coverage"):
                    matrix.append(generate_coverage_variant(family, spec))

                if family == "gcc":
                    matrix.append(generate_superproject_cmake_variant(family, spec))

                if family == "clang":
                    matrix.append(generate_x86_variant(family, spec))
                    matrix.append(generate_time_trace_variant(family, spec))

    json.dump(matrix, sys.stdout)


if __name__ == "__main__":
    main()
