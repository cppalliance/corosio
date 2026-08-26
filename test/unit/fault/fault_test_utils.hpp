//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_FAULT_TEST_UTILS_HPP
#define BOOST_COROSIO_TEST_FAULT_TEST_UTILS_HPP

#include "fault.hpp"
#include "test_suite.hpp"

#if defined(__FreeBSD__)
// real_symbol: the descriptor scan below must not spend a live `fcntl`
// arm on its own probing.
#include "fault_slot.hpp"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace boost::corosio::test::fault {

#if defined(_WIN32)

// The handle count is Windows' answer to the descriptor count: a
// socket, a file and a completion port all show up in it, which is
// what the leak assertions need. It moves on its own as the CRT and
// the thread pool come and go, so only differences taken across a
// short failing call mean anything.
inline int open_fds()
{
    DWORD n = 0;
    if(!::GetProcessHandleCount(::GetCurrentProcess(), &n))
        return -1;
    return static_cast<int>(n);
}

// Windows has no fork, so there is no isolated process to run `body`
// in: it runs here. Faults that install process-wide state — the
// signal handlers — therefore have to live in a suite of their own,
// where nothing else has installed that state first.
template<class F>
void in_child(F&& body)
{
    BOOST_TEST(body());
}

// Assert that a repeatedly failing call releases what it creates.
//
// GetProcessHandleCount is process-wide and moves on its own — a
// Winsock provider loading on the first socket of a family, a runtime
// thread coming or going — and it has been observed drifting in both
// directions across a single pair of samples. An exact before/after
// comparison is therefore not a leak signal at all. Instead `fn` runs
// once to absorb the first-use cost, and the count is taken across
// enough repetitions that a per-call leak, which grows the count once
// per call, separates from that ambient noise.
template<class F>
void expect_no_handle_leak(F&& fn)
{
    constexpr int reps = 8;
    fn();
    int const before = open_fds();
    // open_fds() answers -1 when the count cannot be read, which would
    // otherwise satisfy the comparison below on its own.
    BOOST_TEST(before >= 0);
    for(int i = 0; i < reps; ++i)
        fn();
    int const after = open_fds();
    // A -1 here would satisfy the growth comparison on its own.
    BOOST_TEST(after >= 0);
    BOOST_TEST(after - before < reps);
}

// The Win32 and Winsock codes the library hands back unchanged compare
// equal only to themselves: which of them a toolchain's system_category
// also matches to a std::errc condition differs between MSVC and MinGW,
// so a test spells such an expectation as the raw code.
inline std::error_code win_err(DWORD e)
{
    return std::error_code(static_cast<int>(e), std::system_category());
}

inline std::string temp_path(char const* tag)
{
    char dir[MAX_PATH + 1] = {};
    DWORD const n = ::GetTempPathA(sizeof(dir), dir);
    // An unusable TEMP is not worth a fallback that might be
    // unwritable; the test that opens the path reports it.
    std::string base(dir, n);
    return base + "corosio_fault_" + tag + "_" +
        std::to_string(::GetCurrentProcessId());
}

#else

// Count open descriptors so a test can prove the failure path released
// what it created.
inline int open_fds()
{
#if defined(__FreeBSD__)
    // FreeBSD mounts neither /proc nor fdescfs by default, and /dev/fd
    // without fdescfs lists only 0-2, so the table is probed one entry
    // at a time. The probe goes through the real fcntl rather than the
    // shadow: a test that samples the count inside a live `fcntl` arm
    // would otherwise spend that arm on the scan.
    static auto const real_fcntl =
        reinterpret_cast<int(*)(int, int, ...)>(real_symbol("fcntl"));
    long const lim = ::sysconf(_SC_OPEN_MAX);
    // 65536 bounds the scan on a host with an enormous rlimit; nothing
    // here opens anywhere near that many descriptors.
    long const stop = (lim < 0 || lim > 65536) ? 65536 : lim;
    int n = 0;
    for(long fd = 0; fd < stop; ++fd)
    {
        if(real_fcntl(static_cast<int>(fd), F_GETFD) != -1)
            ++n;
    }
    return n;
#else
#if defined(__APPLE__)
    // Darwin has no /proc; /dev/fd is the same per-process listing.
    DIR* d = ::opendir("/dev/fd");
#else
    DIR* d = ::opendir("/proc/self/fd");
#endif
    // -1 rather than 0: an unreadable /proc/self/fd or /dev/fd must
    // break the leak assertions, not satisfy them.
    if(!d)
        return -1;
    int n = 0;
    while(::readdir(d))
        ++n;
    ::closedir(d);
    return n;
#endif
}

// Run `body` in a forked child and assert it returned true. Process-wide
// state that is created once — the signal self-pipe and its sigaction
// handlers — can only be faulted in a fresh process, and installing it
// in this one would silently disarm every other test that faults it.
template<class F>
void in_child(F&& body)
{
    pid_t pid = ::fork();
    BOOST_TEST(pid >= 0);
    if(pid < 0)
        return;
    if(pid == 0)
        std::_Exit(body() ? 0 : 1);
    int status = 0;
    ::waitpid(pid, &status, 0);
    BOOST_TEST(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

inline std::string temp_path(char const* tag)
{
    // A build that confines the process to its own scratch directory
    // sets TMPDIR; /tmp may not even be writable there.
    char const* dir = std::getenv("TMPDIR");
    std::string base = (dir && *dir) ? dir : "/tmp";
    if(base.back() != '/')
        base += '/';
    return base + "corosio_fault_" + tag + "_" + std::to_string(::getpid());
}

#endif

// Return true if the process runs under Valgrind. Valgrind always maps
// its preload library, so scanning the map is enough and does not need
// valgrind.h on the include path.
inline bool running_under_valgrind() noexcept
{
#if defined(_WIN32)
    return false;
#else
    std::FILE* f = std::fopen("/proc/self/maps", "re");
    if(!f)
        return false;
    bool found = false;
    char line[4096];
    while(std::fgets(line, sizeof(line), f))
    {
        if(std::strstr(line, "vgpreload"))
        {
            found = true;
            break;
        }
    }
    std::fclose(f);
    return found;
#endif
}

// Loud-skip a whole fault suite under Valgrind. Valgrind redirects the
// same libc entry points the shadows interpose, so the arms cannot fire
// and would report spurious failures. Interposition-based fault
// injection is not meaningful there, exactly as it is not under the
// sanitizers the Jamfile already excludes. Report the reason so the leg
// passes with a visible cause rather than a silent one.
inline bool skip_under_valgrind() noexcept
{
    if(!running_under_valgrind())
        return false;
    std::fprintf(stderr,
        "fault harness: running under Valgrind, which redirects the libc "
        "entry points the shadows interpose; skipping this fault suite\n");
    return true;
}

// Report a hook that has no import to patch in this executable. A
// silent `return` from a test would read as a pass; name the symbol so
// the log says which coverage the run did not have.
inline void skip_dead_hook(char const* name)
{
    std::fprintf(stderr,
        "fault harness: %s not imported by this executable; skipping the "
        "test that arms it\n", name);
}

// BOOST_TEST_THROWS accepts any std::system_error, which would pass
// even if the library reported an error the fault never injected.
// `Expected` is a std::errc where the library normalizes the code and a
// std::error_code where it hands back the raw platform value.
template<class F, class Expected>
void expect_system_error(F&& fn, Expected expected)
{
    std::error_code caught;
    try
    {
        fn();
    }
    catch(std::system_error const& e)
    {
        caught = e.code();
    }
    BOOST_TEST(caught == expected);
}

} // boost::corosio::test::fault

#endif
