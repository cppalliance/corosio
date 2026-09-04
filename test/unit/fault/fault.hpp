//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_FAULT_HPP
#define BOOST_COROSIO_TEST_FAULT_HPP

#include <cstddef>

namespace boost::corosio::test::fault {

/** OS entry points the harness can fail.

    Names equal the libc / liburing / Win32 symbol. Every enumerator
    exists on every platform so a portable test can name one behind a
    `#if`; arming a symbol the running platform has no shadow for
    simply never fires. The Berkeley-socket names (`socket`, `bind`,
    `listen`, `accept`, `connect`, `shutdown`, `getsockname`,
    `getpeername`, `getsockopt`, `setsockopt`, `send`, `recv`) name the
    ws2_32 entry point of the same name on Windows. The
    `uring_sqe_full` enumerator is not a symbol: arming it clamps the
    next ring to one SQE and turns `io_uring_submit` into a no-op so
    `io_uring_get_sqe` returns null on the second acquisition.
    `kevent_register` is not a symbol either: it names the subset of
    `kevent` calls that add a descriptor to the kqueue, so a test can
    reach a registration without counting the waits the run loop makes
    on its way there. An arm on `kevent` still counts every call,
    registrations included. `cpp_new` is not an OS symbol: it names the
    global allocation functions the fault target replaces, so arming it
    makes the nth `new` on the armed thread report `bad_alloc` (the
    nothrow forms return null). `pthread_create` names thread creation
    wherever it happens: the libc symbol on POSIX, and on Windows both
    the winpthreads `pthread_create` and the CRT `_beginthreadex` that
    MSVC's `std::thread` reaches through msvcp's import table.
*/
enum class sys
{
    socket, socketpair, bind, listen, accept, accept4, connect,
    getsockname, getpeername, getsockopt, setsockopt, shutdown, close,
    read, write, writev, readv, preadv, pwritev, recv, send, recvmsg,
    sendmsg,
    poll, pipe, fcntl, ioctl, open, fstat, lseek, ftruncate, fsync,
    fdatasync, posix_fadvise, unlink, sigaction, pthread_create,
    getaddrinfo, freeaddrinfo, getnameinfo, gethostname,
    epoll_create1, epoll_ctl, epoll_wait, eventfd, timerfd_create,
    timerfd_settime, select, kqueue, kevent, kevent_register,
    io_uring_queue_init_params, io_uring_queue_exit, io_uring_submit,
    io_uring_submit_and_wait_timeout, io_uring_submit_and_get_events,
    io_uring_wait_cqe_timeout, uring_sqe_full, cpp_new,
    // OpenSSL entry points the TLS engine drives; live only when the
    // process loads libssl/libcrypto.
    BIO_new_mem_buf, BIO_new_bio_pair, BIO_read, BIO_nwrite0,
    SSL_CTX_new, SSL_new, SSL_clear, SSL_set_session, SSL_get0_param,
    X509_STORE_add_cert, X509_dup, X509_VERIFY_PARAM_set1_host,
    WSASocketW, WSAConnect, WSARecv, WSASend, WSARecvFrom, WSASendTo,
    WSAPoll, WSAIoctl, WSAStartup, WSACleanup, closesocket, ioctlsocket,
    GetAddrInfoExW, GetAddrInfoExCancel, FreeAddrInfoExW, GetNameInfoW,
    CreateIoCompletionPort, GetQueuedCompletionStatus,
    PostQueuedCompletionStatus, CancelIoEx, CloseHandle, CreateFileW,
    ReadFile, WriteFile, SetFilePointerEx, GetFileSizeEx, SetEndOfFile,
    FlushFileBuffers, DeleteFileA, CreateWaitableTimerW, SetWaitableTimer,
    WaitForSingleObject, GetComputerNameExW, GetModuleHandleA,
    GetModuleHandleW, GetProcAddress, MultiByteToWideChar,
    WideCharToMultiByte, signal,
    // Reached through a pointer the OS hands out rather than through an
    // import: the WSAIoctl and GetProcAddress hooks substitute a wrapper
    // for the pointer the library caches.
    AcceptEx, ConnectEx, NtSetInformationFile, NtFlushBuffersFileEx,
    count_
};

/** Tag selecting the process-wide arm mode.

    A thread-local arm watching the same symbol shadows the
    process-wide one for the thread that holds it, in either mode: that
    thread's calls are counted and claimed by its own arm and never
    reach the process-wide arm, even after the thread-local one has
    fired or when its `nth` is out of reach.

    @see fault_scope
*/
inline constexpr struct any_thread_t {} any_thread{};

/** Fail one OS call, by default on the current thread.

    While the scope is alive the `nth` call to `which` returns its
    documented failure value with `errno` set to `err` (liburing
    shadows return `-err`; on Windows `err` is a `WSA*`/`ERROR_*` code
    published through `SetLastError`, and the handful of entry points
    that report through their return value return it directly). Calls
    before the nth and all calls after it forward to the real
    function. The arm is thread-local unless
    the scope is created with the `any_thread` overload, so by
    default other threads never observe the fault.

    Up to four thread-local scopes may be alive at once, each with its
    own counter: a deferred-path test parks an operation with one and
    fails its reactor retry with another. Every live arm watching a
    symbol counts each call to it, so two arms on the same symbol are
    distinguished by `nth`.

    @par Preconditions
    Fewer than four `fault_scope` objects are alive on this thread;
    a fifth aborts. A scope must be constructed and destroyed on the
    same thread, since it owns one of that thread's arms by index: a
    scope held across a `co_await` therefore requires an
    `io_context` run by a single thread, or the destructor releases
    an arm belonging to some other thread. A process-wide scope has
    its own one-at-a-time rule, described on the `any_thread`
    overload.

    @par Example
    @code
    fault_scope f(sys::epoll_ctl, EPERM);
    tcp_socket s(ioc);
    auto ec = s.open(tcp::v4());
    BOOST_TEST(f.fired());
    BOOST_TEST(ec == std::errc::operation_not_permitted);
    @endcode
*/
class fault_scope
{
public:
    /// Disarm, even if the fault never fired.
    ~fault_scope();

    /// Arm `which` to fail with `err` on its `nth` call.
    fault_scope(sys which, int err, unsigned nth = 1);

    /** Arm `which` on every thread rather than just this one.

        The library runs file I/O and name resolution on a thread
        pool, where the thread-local arms are never consulted. This
        overload publishes the arm process-wide; a thread with an arm
        of its own watching `which` still uses that one.

        @par Preconditions
        No other process-wide scope is alive; nesting aborts. The test
        must keep at most one call to `which` in flight at a time, or
        the shared counter races.
    */
    fault_scope(sys which, int err, unsigned nth, any_thread_t);

    /** Create a scope that shortens the `nth` call instead of failing it.

        For the byte-moving calls (`read`, `write`, `writev`, `readv`,
        `preadv`, `pwritev`, `recv`, `send`, `recvmsg`, `sendmsg`) the real
        function is invoked with its length clamped to `count`, so
        bytes genuinely move. `count == 0` forwards nothing and returns
        0, which reads as EOF on the read side.
    */
    static fault_scope returning(sys which, std::size_t count,
        unsigned nth = 1);

    /** Create a `returning` scope armed on every thread.

        @par Preconditions
        No other process-wide scope is alive, and at most one call to
        `which` is in flight at a time.
    */
    static fault_scope returning_any_thread(sys which, std::size_t count,
        unsigned nth = 1);

    /// Return true once the armed call has been intercepted.
    bool fired() const noexcept;

    /** Return how many calls to the armed symbol this arm has counted.

        Every live arm watching a symbol counts each call to it, so this
        is the arm's own view of the call ordinal an `nth` selects. A
        test that has to reach past calls the library makes on the way
        in can arm a counting scope first and read the number off it
        instead of hard-coding one.

        @return Calls seen since the scope was constructed, including
            the one that fired.
    */
    unsigned count() const noexcept;

    fault_scope(fault_scope const&) = delete;
    fault_scope& operator=(fault_scope const&) = delete;

private:
    struct short_tag {};
    fault_scope(short_tag, sys which, std::size_t count, unsigned nth,
        bool global);

    bool global_ = false;
    // Index of the claimed thread-local arm; -1 for a process-wide scope.
    int idx_ = -1;
};

/** Rewrite one io_uring completion before corosio sees it.

    Matches the first unsubmitted SQE whose `fd` and `opcode`
    (`IORING_OP_*`) equal the arguments, remembers its `user_data`,
    and overwrites `res` on the CQE carrying that `user_data` when it
    becomes visible. An `fd` of -1 matches on the opcode alone, for
    the polls the library arms on descriptors it never hands out.
    Only meaningful with the io_uring backend; the scope is inert on
    the reactor backends.
*/
class cqe_fault_scope
{
public:
    /// Destroy the scope, releasing the CQE arm it claimed.
    ~cqe_fault_scope();

    /** Construct a scope that rewrites the matched CQE's `res`.

        @param fd The descriptor the SQE was prepared on, or -1 to
            match on `opcode` alone.
        @param opcode The `IORING_OP_*` the SQE carries.
        @param res The value to write into the CQE's `res`.
    */
    cqe_fault_scope(int fd, int opcode, int res);

    /** Construct a scope that also clears `flags_to_clear` on the CQE.

        The multishot re-arm paths key off `IORING_CQE_F_MORE`, which
        the kernel clears only when it terminates the multishot; there
        is no way to provoke that from userspace, so the bit is cleared
        here instead.

        @param fd The descriptor the SQE was prepared on, or -1 to
            match on `opcode` alone.
        @param opcode The `IORING_OP_*` the SQE carries.
        @param res The value to write into the CQE's `res`.
        @param flags_to_clear Bits to clear in the CQE's `flags`.
    */
    cqe_fault_scope(int fd, int opcode, int res, unsigned flags_to_clear);

    /// Return true once a CQE has been rewritten.
    bool fired() const noexcept;

    cqe_fault_scope(cqe_fault_scope const&) = delete;
    cqe_fault_scope& operator=(cqe_fault_scope const&) = delete;
};

/** Fail one IOCP completion before corosio sees it.

    The `GetQueuedCompletionStatus` hook forwards, and for the `nth`
    dequeue that yields a non-null `OVERLAPPED` reports failure with
    `GetLastError()` set to `err`. That is the one way to reach the
    error branches of the completion handlers: the kernel result of an
    overlapped operation cannot be armed at the call that started it.
    Windows only; on other platforms nothing defines this scope.

    @par Preconditions
    No other completion fault is armed on this thread.
*/
class completion_fault_scope
{
public:
    /// Destroy the scope, disarming the completion fault.
    ~completion_fault_scope();

    /** Construct a scope that fails one dequeued completion.

        @param err The Win32 error the dequeue reports.
        @param nth Which completion carrying an `OVERLAPPED` to fail.
    */
    completion_fault_scope(unsigned long err, unsigned nth = 1);

    /// Return true once a completion has been failed.
    bool fired() const noexcept;

    completion_fault_scope(completion_fault_scope const&) = delete;
    completion_fault_scope& operator=(completion_fault_scope const&) = delete;
};

/** Return true if arming `which` can still fire.

    Windows reaches its entry points through an import table, and what
    a program imports is decided when it is linked: a name no module
    references has no thunk to patch and no arm on it will ever fire.
    The harness reports those at startup; a test asks here rather than
    driving a hook that cannot fire.

    On POSIX the answer comes from the census: `which` has a shadow on
    this platform, and — in a shared build — the readback found the
    loader binding the library's call to it. A symbol this platform
    does not spell (`kevent` on Linux, `accept4` on Darwin) answers
    false, which is what lets one portable test skip loudly instead of
    asserting `fired()` on an arm that can never fire.

    @param which The entry point to ask about. On Windows the four
        reached through a pointer the OS hands out (`AcceptEx`,
        `ConnectEx`, `NtSetInformationFile`, `NtFlushBuffersFileEx`)
        answer for the hook that substitutes that pointer;
        `uring_sqe_full` is not a symbol and answers for the liburing
        shadows it works through.

    @return `true` if a hook for `which` is installed and reachable.
*/
bool hook_is_live(sys which) noexcept;

/// Return true if the executable links corosio as a shared library.
bool corosio_is_shared() noexcept;

} // boost::corosio::test::fault

#endif
