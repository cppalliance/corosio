# Use, modification, and distribution are
# subject to the Boost Software License, Version 1.0. (See accompanying
# file LICENSE.txt)
#
# Copyright Rene Rivera 2020.
# Copyright Alan de Freitas 2022.

# For Drone CI we use the Starlark scripting language to reduce duplication.
# As the yaml syntax for Drone CI is rather limited.
#

globalenv = {
    'B2_CI_VERSION': '1',
    'B2_VARIANT': 'debug,release',
    'B2_FLAGS': 'warnings=extra warnings-as-errors=on',
}

def main(ctx):
    # generate() provides: main compiler matrix, asan, ubsan, coverage,
    # and cmake-superproject (linux/latest gcc) by default
    jobs = generate(
        [
            'gcc >=13.0',
            'clang >=17.0',
            'msvc >=14.1',
            'arm64-gcc latest',
            'arm64-clang latest',
            'x86-msvc latest'
        ],
        '>=20',
        docs=False,
        cache_dir='cache')

    # macOS: generate() skips apple-clang when cxx_range='>=20' because
    # ci-automation's compiler_supports() doesn't list C++20 for apple-clang
    jobs += [
        osx_cxx("macOS: Clang 16.2.0", "clang++", packages="",
            buildscript="drone", buildtype="boost",
            xcode_version="16.2.0",
            environment={
                'B2_TOOLSET': 'clang',
                'B2_CXXSTD': '20',
            },
            globalenv=globalenv),

        osx_cxx("macOS: Clang 26.2.0", "clang++", packages="",
            buildscript="drone", buildtype="boost",
            xcode_version="26.2.0",
            environment={
                'B2_TOOLSET': 'clang',
                'B2_CXXSTD': '20',
            },
            globalenv=globalenv),
    ]

    jobs += [
        freebsd_cxx("clang-22", "clang++-22",
            buildscript="drone", buildtype="boost",
            freebsd_version="15.0",
            environment={
                'B2_TOOLSET': 'clang-22',
                'B2_CXXSTD': '20',
            },
            globalenv=globalenv),
    ]

    # Jobs not covered by generate()
    jobs += [
        linux_cxx("Valgrind", "clang++-17", packages="clang-17 libc6-dbg libstdc++-12-dev",
            llvm_os="jammy", llvm_ver="17",
            buildscript="drone", buildtype="valgrind",
            image="cppalliance/droneubuntu2204:1",
            environment={
                'COMMENT': 'valgrind',
                'B2_TOOLSET': 'clang-17',
                'B2_CXXSTD': '20',
                'B2_DEFINES': 'BOOST_NO_STRESS_TEST=1',
                'B2_VARIANT': 'debug',
                'B2_TESTFLAGS': 'testing.launcher=valgrind',
                'VALGRIND_OPTS': '--error-exitcode=1',
            },
            globalenv=globalenv),

        linux_cxx("cmake-mainproject", "g++-13", packages="g++-13",
            image="cppalliance/droneubuntu2404:1",
            buildtype="cmake-mainproject", buildscript="drone",
            environment={
                'COMMENT': 'cmake-mainproject',
                'CXX': 'g++-13',
            },
            globalenv=globalenv),

        linux_cxx("cmake-subdirectory", "g++-13", packages="g++-13",
            image="cppalliance/droneubuntu2404:1",
            buildtype="cmake-subdirectory", buildscript="drone",
            environment={
                'COMMENT': 'cmake-subdirectory',
                'CXX': 'g++-13',
            },
            globalenv=globalenv),

        windows_cxx("msvc-14.3 cmake-superproject", "",
            image="cppalliance/dronevs2022:1",
            buildtype="cmake-superproject", buildscript="drone",
            environment={
                'B2_TOOLSET': 'msvc-14.3',
                'B2_CXXSTD': '20',
            },
            globalenv=globalenv),
    ]

    return jobs


# from https://github.com/cppalliance/ci-automation
load("@ci_automation//ci/drone/:functions.star", "linux_cxx", "windows_cxx", "osx_cxx", "freebsd_cxx", "generate")
