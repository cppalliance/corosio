//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "fault.hpp"
#include "fault_test_utils.hpp"
#include "test_suite.hpp"

#include <boost/corosio/detail/platform.hpp>

#if !defined(_WIN32)

#include <cerrno>
#include <thread>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <string_view>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#endif

#if BOOST_COROSIO_HAVE_LIBURING
#include <liburing.h>
#endif

#if defined(__APPLE__)
// Defined in fault_posix.cpp under this asm name, which is the only
// way to name it: the C++ identifier never reaches the linker.
extern "C" int corosio_fault_select_extsn(
    int, fd_set*, fd_set*, fd_set*, timeval*) __asm__("_select$DARWIN_EXTSN");
#endif

#if defined(__linux__)
// Not in any public header the harness pulls in; only reachable via
// _FORTIFY_SOURCE, which self_test.cpp does not build with.
extern "C" {
ssize_t __read_chk(int, void*, size_t, size_t);
ssize_t __recv_chk(int, void*, size_t, size_t, int);
int __poll_chk(pollfd*, nfds_t, int, size_t);
int __open_2(char const*, int);
int __gethostname_chk(char*, size_t, size_t) noexcept;
}
#endif

namespace boost::corosio::test::fault {

// BOOST_TEST_CSTR_EQ expands to a bare `string_view(...)`, which needs
// this in scope; capy's test_suite.hpp does not pull one in itself.
using std::string_view;

struct self_test
{
    void testFiresOnNth()
    {
        fault_scope f(sys::socket, EMFILE, 2);
        int a = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(a >= 0);
        BOOST_TEST(!f.fired());
        int b = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST_EQ(b, -1);
        BOOST_TEST_EQ(errno, EMFILE);
        BOOST_TEST(f.fired());
        int c = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(c >= 0);
        ::close(a);
        ::close(c);
    }

    void testDisarmsOnScopeExit()
    {
        {
            fault_scope f(sys::socket, EMFILE, 5);
        }
        int a = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(a >= 0);
        ::close(a);
    }

    // should_fail clears `armed` the instant the fault fires; `fired()`
    // must still read true for calls made after that, while the scope
    // that fired is still alive.
    void testFiredScopeStaysFired()
    {
        fault_scope f(sys::socket, EMFILE, 1);
        int a = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST_EQ(a, -1);
        BOOST_TEST_EQ(errno, EMFILE);
        BOOST_TEST(f.fired());
        int b = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(b >= 0);
        BOOST_TEST(f.fired());
        ::close(b);
    }

    // open_fds() backs every leak assertion in the backend suites, and
    // on Darwin it reads a different directory; a build where it
    // returns -1 would satisfy nothing.
    void testOpenFdsProbeWorks()
    {
        BOOST_TEST(open_fds() > 0);
    }

    void testTransparentWhenUnarmed()
    {
        int sv[2];
        BOOST_TEST_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        char const msg[] = "abcdefgh";
        BOOST_TEST_EQ(::write(sv[0], msg, sizeof(msg)), (ssize_t)sizeof(msg));
        char buf[16] = {};
        BOOST_TEST_EQ(::read(sv[1], buf, sizeof(buf)), (ssize_t)sizeof(msg));
        BOOST_TEST_CSTR_EQ(buf, msg);
        ::close(sv[0]);
        ::close(sv[1]);
    }

    void testThreadIsolation()
    {
        fault_scope f(sys::socket, EMFILE);
        int other = -2;
        std::thread t([&]{ other = ::socket(AF_INET, SOCK_STREAM, 0); });
        t.join();
        BOOST_TEST(other >= 0);
        BOOST_TEST(!f.fired());
        ::close(other);
        int mine = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST_EQ(mine, -1);
        BOOST_TEST(f.fired());
    }

    // The library's file I/O and name resolution run on a thread pool,
    // so a scope for those must be visible outside the test thread.
    void testAnyThreadFires()
    {
        fault_scope f(sys::socket, EMFILE, 1, any_thread);
        int other = -2;
        int other_errno = 0;
        std::thread t([&]
        {
            other = ::socket(AF_INET, SOCK_STREAM, 0);
            other_errno = errno;
        });
        t.join();
        BOOST_TEST_EQ(other, -1);
        BOOST_TEST_EQ(other_errno, EMFILE);
        BOOST_TEST(f.fired());
    }

    // A thread that armed its own slot keeps using it; the global is
    // only the fallback.
    void testThreadLocalWinsOverAnyThread()
    {
        fault_scope g(sys::listen, EOPNOTSUPP, 1, any_thread);
        fault_scope f(sys::socket, EMFILE);
        int a = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST_EQ(a, -1);
        BOOST_TEST(f.fired());
        BOOST_TEST(!g.fired());
        // With the thread-local slot spent, the global takes over here.
        int b = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(b >= 0);
        BOOST_TEST_EQ(::listen(b, 1), -1);
        BOOST_TEST_EQ(errno, EOPNOTSUPP);
        BOOST_TEST(g.fired());
        ::close(b);
    }

    void testAnyThreadDisarmsOnScopeExit()
    {
        {
            fault_scope f(sys::socket, EMFILE, 1, any_thread);
        }
        int other = -2;
        std::thread t([&]{ other = ::socket(AF_INET, SOCK_STREAM, 0); });
        t.join();
        BOOST_TEST(other >= 0);
        ::close(other);
    }

    void testAnyThreadShortens()
    {
        int sv[2];
        BOOST_TEST_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        BOOST_TEST_EQ(::write(sv[0], "0123456789", 10), 10);
        ssize_t n = -2;
        {
            auto f = fault_scope::returning_any_thread(sys::read, 3);
            std::thread t([&]
            {
                char buf[16];
                n = ::read(sv[1], buf, sizeof(buf));
            });
            t.join();
            BOOST_TEST(f.fired());
        }
        BOOST_TEST_EQ(n, 3);
        ::close(sv[0]);
        ::close(sv[1]);
    }

    // Deferred-path tests park an operation with one arm and fail its
    // reactor retry with another, so arms must be independent.
    void testTwoArmsCoexist()
    {
        fault_scope f1(sys::socket, EMFILE);
        fault_scope f2(sys::listen, EOPNOTSUPP);
        int a = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST_EQ(a, -1);
        BOOST_TEST_EQ(errno, EMFILE);
        BOOST_TEST(f1.fired());
        BOOST_TEST(!f2.fired());
        int b = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(b >= 0);
        BOOST_TEST_EQ(::listen(b, 1), -1);
        BOOST_TEST_EQ(errno, EOPNOTSUPP);
        BOOST_TEST(f2.fired());
        ::close(b);
    }

    // Two arms on the same symbol each count calls on their own, so
    // nth selects which occurrence a given arm claims.
    void testTwoArmsSameSymbolCountIndependently()
    {
        fault_scope f1(sys::socket, EMFILE, 1);
        fault_scope f2(sys::socket, EACCES, 2);
        int a = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST_EQ(a, -1);
        BOOST_TEST_EQ(errno, EMFILE);
        BOOST_TEST(f1.fired());
        BOOST_TEST(!f2.fired());
        int b = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST_EQ(b, -1);
        BOOST_TEST_EQ(errno, EACCES);
        BOOST_TEST(f2.fired());
        int c = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(c >= 0);
        ::close(c);
    }

    // Two arms can reach their nth on the same call, but only one
    // fault is delivered; the loser must stay armed and keep counting
    // rather than be silently marked fired.
    void testSameNthOnlyOneArmFires()
    {
        fault_scope f1(sys::socket, EMFILE);
        fault_scope f2(sys::socket, EACCES);
        int a = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST_EQ(a, -1);
        BOOST_TEST_EQ(errno, EMFILE);
        BOOST_TEST(f1.fired());
        BOOST_TEST(!f2.fired());
        int b = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST_EQ(b, -1);
        BOOST_TEST_EQ(errno, EACCES);
        BOOST_TEST(f2.fired());
        int c = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(c >= 0);
        ::close(c);
    }

    void testOnlyMatchingSymbolFires()
    {
        fault_scope f(sys::listen, EOPNOTSUPP);
        int a = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(a >= 0);
        BOOST_TEST(!f.fired());
        BOOST_TEST_EQ(::listen(a, 1), -1);
        BOOST_TEST_EQ(errno, EOPNOTSUPP);
        BOOST_TEST(f.fired());
        ::close(a);
    }

    void testEveryCensusSymbolFails()
    {
        // One representative call per shadow, each expected to fail.
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(fd >= 0);
        auto expect = [&](sys s, auto&& call)
        {
            fault_scope f(s, EPERM);
            auto r = call();
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ((long)r, -1L);
            BOOST_TEST_EQ(errno, EPERM);
        };
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        socklen_t len = sizeof(sa);
        int one = 1;
        char buf[4];
        iovec iov{buf, sizeof(buf)};
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        pollfd pfd{fd, POLLIN, 0};
        int pf[2];
        struct stat st;
        struct sigaction sa_old;
        addrinfo* ai = nullptr;
        char host[64];
        timeval tv{0, 0};
        fd_set fds;
        FD_ZERO(&fds);
        int sv[2];

        expect(sys::socketpair, [&]{ return ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv); });
        expect(sys::bind, [&]{ return ::bind(fd, (sockaddr*)&sa, sizeof(sa)); });
        expect(sys::listen, [&]{ return ::listen(fd, 1); });
        expect(sys::accept, [&]{ return ::accept(fd, nullptr, nullptr); });
        expect(sys::connect, [&]{ return ::connect(fd, (sockaddr*)&sa, sizeof(sa)); });
        expect(sys::getsockname, [&]{ return ::getsockname(fd, (sockaddr*)&sa, &len); });
        expect(sys::getpeername, [&]{ return ::getpeername(fd, (sockaddr*)&sa, &len); });
        expect(sys::getsockopt, [&]{ len = sizeof(one); return ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &one, &len); });
        expect(sys::setsockopt, [&]{ return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); });
        expect(sys::shutdown, [&]{ return ::shutdown(fd, SHUT_RD); });
        expect(sys::read, [&]{ return ::read(fd, buf, sizeof(buf)); });
        expect(sys::write, [&]{ return ::write(fd, buf, sizeof(buf)); });
        expect(sys::writev, [&]{ return ::writev(fd, &iov, 1); });
        expect(sys::readv, [&]{ return ::readv(fd, &iov, 1); });
        expect(sys::preadv, [&]{ return ::preadv(fd, &iov, 1, 0); });
        expect(sys::pwritev, [&]{ return ::pwritev(fd, &iov, 1, 0); });
        expect(sys::recv, [&]{ return ::recv(fd, buf, sizeof(buf), 0); });
        expect(sys::send, [&]{ return ::send(fd, buf, sizeof(buf), 0); });
        expect(sys::recvmsg, [&]{ return ::recvmsg(fd, &mh, 0); });
        expect(sys::sendmsg, [&]{ return ::sendmsg(fd, &mh, 0); });
        expect(sys::poll, [&]{ return ::poll(&pfd, 1, 0); });
        expect(sys::pipe, [&]{ return ::pipe(pf); });
        expect(sys::fcntl, [&]{ return ::fcntl(fd, F_GETFL); });
        expect(sys::ioctl, [&]{ return ::ioctl(fd, FIONREAD, &one); });
        expect(sys::open, [&]{ return ::open("/dev/null", O_RDONLY); });
        // fstat is an inline redirect to __fxstat on pre-2.33 glibc, so the
        // "fstat" symbol has no shadow to arm there: the readback marks it
        // not live and the call goes straight to libc. Expect not-live
        // rather than asserting a fire that cannot happen.
        if(hook_is_live(sys::fstat))
            expect(sys::fstat, [&]{ return ::fstat(fd, &st); });
        else
            skip_dead_hook("fstat");
        expect(sys::lseek, [&]{ return (long)::lseek(fd, 0, SEEK_SET); });
        expect(sys::ftruncate, [&]{ return ::ftruncate(fd, 0); });
        expect(sys::fsync, [&]{ return ::fsync(fd); });
        expect(sys::unlink, [&]{ return ::unlink("/nonexistent/x"); });
        expect(sys::sigaction, [&]{ return ::sigaction(SIGUSR1, nullptr, &sa_old); });
        expect(sys::gethostname, [&]{ return ::gethostname(host, sizeof(host)); });
        expect(sys::select, [&]{ return ::select(1, &fds, nullptr, nullptr, &tv); });
#if defined(__linux__) || defined(__FreeBSD__)
        expect(sys::fdatasync, [&]{ return ::fdatasync(fd); });
#endif
#if defined(__linux__)
        expect(sys::accept4, [&]{ return ::accept4(fd, nullptr, nullptr, 0); });
        expect(sys::epoll_create1, [&]{ return ::epoll_create1(0); });
        expect(sys::epoll_ctl, [&]{ return ::epoll_ctl(fd, EPOLL_CTL_DEL, fd, nullptr); });
        expect(sys::epoll_wait, [&]{ epoll_event ev; return ::epoll_wait(fd, &ev, 1, 0); });
        expect(sys::eventfd, [&]{ return ::eventfd(0, 0); });
        expect(sys::timerfd_create, [&]{ return ::timerfd_create(CLOCK_MONOTONIC, 0); });
        expect(sys::timerfd_settime, [&]{ itimerspec its{}; return ::timerfd_settime(fd, 0, &its, nullptr); });
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
        expect(sys::kqueue, [&]{ return ::kqueue(); });
        // An invalid kq would fail anyway; the arm must be what fails it.
        expect(sys::kevent, [&]
        {
            struct kevent ch{};
            return ::kevent(-1, &ch, 1, nullptr, 0, nullptr);
        });
#endif
        expect(sys::close, [&]{ return ::close(fd); });
        // getaddrinfo / getnameinfo return the error, not -1
#if defined(__linux__) || defined(__FreeBSD__)
        {
            fault_scope f(sys::posix_fadvise, EPERM);
            BOOST_TEST_EQ(::posix_fadvise(fd, 0, 0, POSIX_FADV_NORMAL), EPERM);
            BOOST_TEST(f.fired());
        }
#endif
        {
            fault_scope f(sys::getaddrinfo, EAI_FAIL);
            BOOST_TEST_EQ(::getaddrinfo("localhost", nullptr, nullptr, &ai), EAI_FAIL);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::getnameinfo, EAI_FAIL);
            BOOST_TEST_EQ(::getnameinfo((sockaddr*)&sa, sizeof(sa), host, sizeof(host), nullptr, 0, 0), EAI_FAIL);
            BOOST_TEST(f.fired());
        }
        // freeaddrinfo has no failure to inject; the arm only proves
        // the shadow saw the release, and the list is still freed.
        {
            ai = nullptr;
            if(::getaddrinfo("localhost", nullptr, nullptr, &ai) == 0 && ai)
            {
                fault_scope f(sys::freeaddrinfo, EPERM);
                ::freeaddrinfo(ai);
                BOOST_TEST(f.fired());
            }
        }
        ::close(fd);
    }

    void testReturningTruncatesAndForwards()
    {
        int sv[2];
        BOOST_TEST_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        char const msg[] = "0123456789";
        {
            auto f = fault_scope::returning(sys::write, 4);
            BOOST_TEST_EQ(::write(sv[0], msg, 10), 4);
            BOOST_TEST(f.fired());
        }
        char buf[16] = {};
        BOOST_TEST_EQ(::read(sv[1], buf, sizeof(buf)), 4);
        BOOST_TEST_EQ(std::string_view(buf, 4), "0123");

        BOOST_TEST_EQ(::write(sv[0], msg, 10), 10);
        {
            auto f = fault_scope::returning(sys::read, 3);
            BOOST_TEST_EQ(::read(sv[1], buf, sizeof(buf)), 3);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(std::string_view(buf, 3), "012");
        }
        BOOST_TEST_EQ(::read(sv[1], buf, sizeof(buf)), 7);

        // iovec truncation keeps the prefix of the scatter list
        BOOST_TEST_EQ(::write(sv[0], msg, 10), 10);
        char a[4] = {}, b[4] = {};
        iovec iov[2] = {{a, 4}, {b, 4}};
        {
            auto f = fault_scope::returning(sys::readv, 6);
            BOOST_TEST_EQ(::readv(sv[1], iov, 2), 6);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(std::string_view(a, 4), "0123");
            BOOST_TEST_EQ(std::string_view(b, 2), "45");
        }
        BOOST_TEST_EQ(::read(sv[1], buf, sizeof(buf)), 4);

        // zero on the read side is EOF without touching the socket
        BOOST_TEST_EQ(::write(sv[0], msg, 10), 10);
        {
            auto f = fault_scope::returning(sys::recv, 0);
            BOOST_TEST_EQ(::recv(sv[1], buf, sizeof(buf), 0), 0);
            BOOST_TEST(f.fired());
        }
        BOOST_TEST_EQ(::recv(sv[1], buf, sizeof(buf), 0), 10);
        ::close(sv[0]);
        ::close(sv[1]);
    }

#if defined(__APPLE__)
    // <sys/select.h> spells the call select$DARWIN_EXTSN under
    // _DARWIN_C_SOURCE and plain select otherwise. This translation
    // unit gets the plain spelling, so the suffixed shadow is reached
    // by its asm name to prove both land on the same arm.
    void testDarwinSelectAliasReachesHook()
    {
        fd_set fds;
        FD_ZERO(&fds);
        timeval tv{0, 0};
        {
            fault_scope f(sys::select, EIO);
            BOOST_TEST_EQ(::select(1, &fds, nullptr, nullptr, &tv), -1);
            BOOST_TEST_EQ(errno, EIO);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::select, EIO);
            BOOST_TEST_EQ(
                ::corosio_fault_select_extsn(1, &fds, nullptr, nullptr, &tv),
                -1);
            BOOST_TEST_EQ(errno, EIO);
            BOOST_TEST(f.fired());
        }
        // Unarmed, the alias still forwards to the real call.
        FD_ZERO(&fds);
        BOOST_TEST_EQ(
            ::corosio_fault_select_extsn(0, &fds, nullptr, nullptr, &tv), 0);
    }
#endif

#if defined(__linux__)
    void testChkAliasesReachHook()
    {
        int sv[2];
        BOOST_TEST_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        char buf[8];
        {
            fault_scope f(sys::read, EIO);
            BOOST_TEST_EQ(::__read_chk(sv[1], buf, sizeof(buf), sizeof(buf)), -1);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::recv, EIO);
            BOOST_TEST_EQ(::__recv_chk(sv[1], buf, sizeof(buf), sizeof(buf), 0), -1);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::poll, EIO);
            pollfd p{sv[1], POLLIN, 0};
            BOOST_TEST_EQ(::__poll_chk(&p, 1, 0, sizeof(p)), -1);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::open, EIO);
            BOOST_TEST_EQ(::__open_2("/dev/null", O_RDONLY), -1);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::gethostname, EIO);
            char host[64];
            BOOST_TEST_EQ(::__gethostname_chk(host, sizeof(host), sizeof(host)), -1);
            BOOST_TEST(f.fired());
        }
        ::close(sv[0]);
        ::close(sv[1]);
    }
#endif

    // `nth` arithmetic in the backend suites has to reach past calls
    // the library makes on the way in. count() is what lets a test say
    // how many there were instead of hard-coding the number.
    void testCountTracksCalls()
    {
        fault_scope f(sys::socket, EMFILE, 3);
        BOOST_TEST_EQ(f.count(), 0u);
        int a = ::socket(AF_INET, SOCK_STREAM, 0);
        int b = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST_EQ(f.count(), 2u);
        BOOST_TEST(!f.fired());
        BOOST_TEST_EQ(::socket(AF_INET, SOCK_STREAM, 0), -1);
        BOOST_TEST_EQ(f.count(), 3u);
        BOOST_TEST(f.fired());
        // A spent arm stops counting: it no longer claims calls.
        int c = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST_EQ(f.count(), 3u);
        ::close(a);
        ::close(b);
        ::close(c);
    }

    // Every backend suite asserts fired() on the arms it sets. A symbol
    // this platform has no shadow for would fail that assertion with
    // nothing to say why, so a portable test asks first.
    void testHookIsLiveAnswersPerPlatform()
    {
        BOOST_TEST(hook_is_live(sys::socket));
        BOOST_TEST(hook_is_live(sys::getsockopt));
        BOOST_TEST(hook_is_live(sys::select));
        // No POSIX shadow spells a Win32 entry point.
        BOOST_TEST(!hook_is_live(sys::WSASocketW));
        BOOST_TEST(!hook_is_live(sys::CreateIoCompletionPort));
#if defined(__linux__)
        BOOST_TEST(hook_is_live(sys::epoll_ctl));
        BOOST_TEST(hook_is_live(sys::accept4));
        BOOST_TEST(!hook_is_live(sys::kevent));
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
        BOOST_TEST(hook_is_live(sys::kevent));
        BOOST_TEST(!hook_is_live(sys::epoll_ctl));
#endif
#if BOOST_COROSIO_HAVE_LIBURING
        BOOST_TEST(hook_is_live(sys::io_uring_submit));
        BOOST_TEST(hook_is_live(sys::uring_sqe_full));
#else
        BOOST_TEST(!hook_is_live(sys::uring_sqe_full));
#endif
    }

    // The select backend rejects descriptors it cannot represent, and a
    // descriptor number is not something a caller picks: the tests that
    // reach those arms need these two seams to work.
    void testHighFdHelpers()
    {
        int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(fd >= 0);
        int const hi = dup_above_fd_setsize(fd);
        if(hi < 0)
        {
            skip_no_high_fd("the high-descriptor helper self-test");
            ::close(fd);
            return;
        }
        BOOST_TEST(hi >= FD_SETSIZE);
        BOOST_TEST_EQ(::fcntl(hi, F_GETFD) == -1, false);
        ::close(hi);
        ::close(fd);

        {
            fd_wall wall;
            if(!wall.ok())
            {
                skip_no_high_fd("the descriptor-wall self-test");
                return;
            }
            int const walled = ::socket(AF_INET, SOCK_STREAM, 0);
            BOOST_TEST(walled >= FD_SETSIZE);
            ::close(walled);
        }
        // The wall releases the numbers it held, or every later test
        // would run against a table it did not ask for.
        int const freed = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(freed >= 0);
        BOOST_TEST(freed < FD_SETSIZE);
        ::close(freed);
    }

#if BOOST_COROSIO_HAVE_LIBURING
    void testUringSubmitFails()
    {
        ::io_uring ring;
        io_uring_params p{};
        BOOST_TEST_EQ(io_uring_queue_init_params(4, &ring, &p), 0);
        {
            fault_scope f(sys::io_uring_submit, EBADF);
            BOOST_TEST_EQ(io_uring_submit(&ring), -EBADF);
            BOOST_TEST(f.fired());
        }
        BOOST_TEST_EQ(io_uring_submit(&ring), 0);
        io_uring_queue_exit(&ring);
    }

    void testUringSqeFull()
    {
        fault_scope f(sys::uring_sqe_full, 0);
        ::io_uring ring;
        io_uring_params p{};
        BOOST_TEST_EQ(io_uring_queue_init_params(64, &ring, &p), 0);
        // liburing may round the clamped entry count up to its own
        // minimum instead of honoring 1 exactly.
        BOOST_TEST(p.sq_entries <= 2);
        BOOST_TEST(io_uring_get_sqe(&ring) != nullptr);
        BOOST_TEST(io_uring_get_sqe(&ring) == nullptr);
        BOOST_TEST_EQ(io_uring_submit(&ring), 0);
        BOOST_TEST(f.fired());
        BOOST_TEST(io_uring_get_sqe(&ring) == nullptr);
        io_uring_queue_exit(&ring);
    }

    void testCqeRewrite()
    {
        ::io_uring ring;
        io_uring_params p{};
        BOOST_TEST_EQ(io_uring_queue_init_params(4, &ring, &p), 0);
        int sv[2];
        BOOST_TEST_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        BOOST_TEST_EQ(::write(sv[0], "xyz", 3), 3);
        char buf[8];
        cqe_fault_scope c(sv[1], IORING_OP_RECV, -ECONNRESET);
        auto* sqe = io_uring_get_sqe(&ring);
        io_uring_prep_recv(sqe, sv[1], buf, sizeof(buf), 0);
        io_uring_sqe_set_data64(sqe, 42);
        BOOST_TEST_EQ(io_uring_submit(&ring), 1);
        io_uring_cqe* cqe = nullptr;
        BOOST_TEST_EQ(io_uring_wait_cqe_timeout(&ring, &cqe, nullptr), 0);
        BOOST_TEST(c.fired());
        BOOST_TEST_EQ(cqe->user_data, 42u);
        BOOST_TEST_EQ(cqe->res, -ECONNRESET);
        io_uring_cqe_seen(&ring, cqe);
        ::close(sv[0]);
        ::close(sv[1]);
        io_uring_queue_exit(&ring);
    }

    // The multishot re-arm paths key off IORING_CQE_F_MORE, which only
    // the kernel clears. Rewriting `res` alone cannot reach them.
    void testCqeFlagsCleared()
    {
        ::io_uring ring;
        io_uring_params p{};
        BOOST_TEST_EQ(io_uring_queue_init_params(4, &ring, &p), 0);
        int sv[2];
        BOOST_TEST_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        {
            // fd -1: match on the opcode alone, the way a test reaches
            // a poll armed on a descriptor the library never handed out.
            cqe_fault_scope c(-1, IORING_OP_POLL_ADD, POLLIN,
                IORING_CQE_F_MORE);
            auto* sqe = io_uring_get_sqe(&ring);
            io_uring_prep_poll_multishot(sqe, sv[1], POLLIN);
            io_uring_sqe_set_data64(sqe, 7);
            BOOST_TEST_EQ(io_uring_submit(&ring), 1);
            BOOST_TEST_EQ(::write(sv[0], "x", 1), 1);
            io_uring_cqe* cqe = nullptr;
            BOOST_TEST_EQ(
                io_uring_wait_cqe_timeout(&ring, &cqe, nullptr), 0);
            BOOST_TEST(c.fired());
            BOOST_TEST_EQ(cqe->user_data, 7u);
            BOOST_TEST_EQ(cqe->flags & IORING_CQE_F_MORE, 0u);
            io_uring_cqe_seen(&ring, cqe);
        }
        ::close(sv[0]);
        ::close(sv[1]);
        io_uring_queue_exit(&ring);
    }
#endif

    void run()
    {
        if(skip_under_valgrind())
            return;
        testFiresOnNth();
        testDisarmsOnScopeExit();
        testFiredScopeStaysFired();
        testOpenFdsProbeWorks();
        testTransparentWhenUnarmed();
        testThreadIsolation();
        testAnyThreadFires();
        testThreadLocalWinsOverAnyThread();
        testAnyThreadDisarmsOnScopeExit();
        testAnyThreadShortens();
        testTwoArmsCoexist();
        testTwoArmsSameSymbolCountIndependently();
        testSameNthOnlyOneArmFires();
        testOnlyMatchingSymbolFires();
        testEveryCensusSymbolFails();
        testReturningTruncatesAndForwards();
        testCountTracksCalls();
        testHookIsLiveAnswersPerPlatform();
        testHighFdHelpers();
#if defined(__APPLE__)
        testDarwinSelectAliasReachesHook();
#endif
#if defined(__linux__)
        testChkAliasesReachHook();
#endif
#if BOOST_COROSIO_HAVE_LIBURING
        testUringSubmitFails();
        testUringSqeFull();
        testCqeRewrite();
        testCqeFlagsCleared();
#endif
    }
};

TEST_SUITE(self_test, "boost.corosio.fault.self");

} // boost::corosio::test::fault

#else

#include <MSWSock.h>
#include <WS2tcpip.h>
#include <signal.h>

// MinGW's <ws2tcpip.h> stops short of this one; the library declares it
// for itself the same way.
#if defined(__MINGW32__) || defined(__MINGW64__)
extern "C" INT WSAAPI GetAddrInfoExCancel(LPHANDLE lpHandle);
#endif

#include <cstdio>
#include <string_view>
#include <thread>
#include <tuple>

namespace boost::corosio::test::fault {

// BOOST_TEST_CSTR_EQ expands to a bare `string_view(...)`, which needs
// this in scope; capy's test_suite.hpp does not pull one in itself.
using std::string_view;

namespace {

// One arbitrary code, armed everywhere: what matters is that the value
// the arm carries is the value the caller reads back, not which code
// it is.
int constexpr test_err = ERROR_NOT_SUPPORTED;

// Winsock and the kernel share the per-thread error slot, so one read
// covers both families.
bool err_is(int expected) noexcept
{
    return ::GetLastError() == static_cast<DWORD>(expected);
}

// A connected loopback pair. The shortening hooks clamp a real call,
// so the bytes have to move through a real socket; Windows has no
// socketpair and nothing here builds an io_context to borrow
// connect_pair from.
bool make_loopback_pair(SOCKET& a, SOCKET& b)
{
    a = b = INVALID_SOCKET;
    SOCKET acc = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(acc == INVALID_SOCKET)
        return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    int len = static_cast<int>(sizeof(sa));
    bool ok = ::bind(acc, reinterpret_cast<sockaddr*>(&sa), len) == 0 &&
        ::listen(acc, 1) == 0 &&
        ::getsockname(acc, reinterpret_cast<sockaddr*>(&sa), &len) == 0;
    if(ok)
    {
        a = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok = a != INVALID_SOCKET &&
            ::connect(a, reinterpret_cast<sockaddr*>(&sa),
                static_cast<int>(sizeof(sa))) == 0;
    }
    if(ok)
    {
        b = ::accept(acc, nullptr, nullptr);
        ok = b != INVALID_SOCKET;
    }
    std::ignore = ::closesocket(acc);
    return ok;
}

// A stream may deliver a send in pieces. The assertions below are
// about what the hook did to the call it clamped, so the other side
// drains a count it already knows.
int recv_exactly(SOCKET s, char* p, int n)
{
    int got = 0;
    while(got < n)
    {
        int const r = ::recv(s, p + got, n - got, 0);
        if(r <= 0)
            return got;
        got += r;
    }
    return got;
}

// Every entry point this file arms is also called from this
// translation unit, so the program imports all of them: a name with no
// thunk to patch here is census drift rather than a toolchain
// difference, and fails rather than skips.
bool require_hook(sys s, char const* name)
{
    if(hook_is_live(s))
        return true;
    std::fprintf(stderr, "fault harness: %s is not hooked here\n", name);
    BOOST_TEST(false);
    return false;
}

} // namespace

struct self_test
{
    // Winsock has to be up before any socket call in this program;
    // nothing here constructs an io_context to do it.
    struct winsock_guard
    {
        winsock_guard()
        {
            WSADATA data;
            BOOST_TEST_EQ(::WSAStartup(MAKEWORD(2, 2), &data), 0);
        }
        ~winsock_guard() { std::ignore = ::WSACleanup(); }
    };

    void testFiresOnNth()
    {
        fault_scope f(sys::socket, WSAEMFILE, 2);
        SOCKET a = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST(a != INVALID_SOCKET);
        BOOST_TEST(!f.fired());
        SOCKET b = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST(b == INVALID_SOCKET);
        BOOST_TEST(err_is(WSAEMFILE));
        BOOST_TEST(f.fired());
        SOCKET c = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST(c != INVALID_SOCKET);
        std::ignore = ::closesocket(a);
        std::ignore = ::closesocket(c);
    }

    void testDisarmsOnScopeExit()
    {
        {
            fault_scope f(sys::socket, WSAEMFILE, 5);
        }
        SOCKET a = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST(a != INVALID_SOCKET);
        std::ignore = ::closesocket(a);
    }

    // `nth` arithmetic in the backend suites has to reach past calls
    // the library makes on the way in. count() is what lets a test say
    // how many there were instead of hard-coding the number. The arm
    // model is shared with the POSIX harness, so what this pins down
    // is that the Windows hooks feed it.
    void testCountTracksCalls()
    {
        fault_scope f(sys::socket, WSAEMFILE, 3);
        BOOST_TEST_EQ(f.count(), 0u);
        SOCKET a = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        SOCKET b = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST_EQ(f.count(), 2u);
        BOOST_TEST(!f.fired());
        BOOST_TEST(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) ==
            INVALID_SOCKET);
        BOOST_TEST_EQ(f.count(), 3u);
        BOOST_TEST(f.fired());
        // A spent arm stops counting: it no longer claims calls.
        SOCKET c = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST_EQ(f.count(), 3u);
        std::ignore = ::closesocket(a);
        std::ignore = ::closesocket(b);
        std::ignore = ::closesocket(c);
    }

    void testFiredScopeStaysFired()
    {
        fault_scope f(sys::socket, WSAEMFILE);
        BOOST_TEST(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) ==
            INVALID_SOCKET);
        BOOST_TEST(f.fired());
        SOCKET b = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST(b != INVALID_SOCKET);
        BOOST_TEST(f.fired());
        std::ignore = ::closesocket(b);
    }

    void testOpenFdsProbeWorks()
    {
        BOOST_TEST(open_fds() > 0);
    }

    void testTransparentWhenUnarmed()
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST(s != INVALID_SOCKET);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        BOOST_TEST_EQ(::bind(s, reinterpret_cast<sockaddr*>(&sa),
            static_cast<int>(sizeof(sa))), 0);
        BOOST_TEST_EQ(::listen(s, 1), 0);
        std::ignore = ::closesocket(s);
    }

    void testThreadIsolation()
    {
        fault_scope f(sys::socket, WSAEMFILE);
        SOCKET other = INVALID_SOCKET;
        std::thread t([&]
        {
            other = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        });
        t.join();
        BOOST_TEST(other != INVALID_SOCKET);
        BOOST_TEST(!f.fired());
        std::ignore = ::closesocket(other);
        BOOST_TEST(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) ==
            INVALID_SOCKET);
        BOOST_TEST(f.fired());
    }

    // The library resolves names and moves file bytes on a thread
    // pool, where the test thread's arms are never consulted.
    void testAnyThreadFires()
    {
        fault_scope f(sys::socket, WSAEMFILE, 1, any_thread);
        SOCKET other = INVALID_SOCKET;
        DWORD other_err = 0;
        std::thread t([&]
        {
            other = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            other_err = ::GetLastError();
        });
        t.join();
        BOOST_TEST(other == INVALID_SOCKET);
        BOOST_TEST_EQ(other_err, static_cast<DWORD>(WSAEMFILE));
        BOOST_TEST(f.fired());
    }

    void testThreadLocalWinsOverAnyThread()
    {
        fault_scope g(sys::listen, WSAEOPNOTSUPP, 1, any_thread);
        fault_scope f(sys::socket, WSAEMFILE);
        BOOST_TEST(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) ==
            INVALID_SOCKET);
        BOOST_TEST(f.fired());
        BOOST_TEST(!g.fired());
        SOCKET b = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST(b != INVALID_SOCKET);
        BOOST_TEST_EQ(::listen(b, 1), SOCKET_ERROR);
        BOOST_TEST(err_is(WSAEOPNOTSUPP));
        BOOST_TEST(g.fired());
        std::ignore = ::closesocket(b);
    }

    void testTwoArmsCoexist()
    {
        fault_scope f1(sys::socket, WSAEMFILE);
        fault_scope f2(sys::listen, WSAEOPNOTSUPP);
        BOOST_TEST(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) ==
            INVALID_SOCKET);
        BOOST_TEST(f1.fired());
        BOOST_TEST(!f2.fired());
        SOCKET b = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST(b != INVALID_SOCKET);
        BOOST_TEST_EQ(::listen(b, 1), SOCKET_ERROR);
        BOOST_TEST(f2.fired());
        std::ignore = ::closesocket(b);
    }

    // One call per hook, each expected to report its documented
    // failure with the armed code. A hook the toolchain gave no import
    // to patch is named and skipped rather than silently passing.
    void testEveryCensusSymbolFails()
    {
        SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST(fd != INVALID_SOCKET);
        HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr,
            0, 1);
        BOOST_TEST(port != nullptr);

        // This translation unit references every hooked entry point, so
        // the program imports every one of them: a hook with no thunk
        // to patch here is census drift, not a toolchain difference.
        auto expect = [&](sys s, char const* name, auto&& call)
        {
            if(!require_hook(s, name))
                return;
            fault_scope f(s, test_err);
            bool const failed = call();
            // Name the symbol: fifty anonymous BOOST_TEST lines say
            // nothing about which hook came apart.
            if(!failed || !f.fired())
                std::fprintf(stderr,
                    "fault harness: census failed for %s\n", name);
            BOOST_TEST(failed);
            BOOST_TEST(f.fired());
        };

        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        int const salen = static_cast<int>(sizeof(sa));
        int len = salen;
        int one = 1;
        char buf[8] = {};
        WSABUF wbuf{};
        wbuf.len = static_cast<ULONG>(sizeof(buf));
        wbuf.buf = buf;
        DWORD bytes = 0;
        DWORD flags = 0;
        u_long mode = 1;
        WSAPOLLFD pfd{};
        pfd.fd = fd;
        pfd.events = static_cast<SHORT>(POLLRDNORM);
        LARGE_INTEGER big{};
        wchar_t wide[64] = {};
        char narrow[64] = {};
        DWORD widelen = 64;
        ULONG_PTR key = 0;
        LPOVERLAPPED got = nullptr;
        OVERLAPPED ov{};
        HANDLE cancel = nullptr;

        expect(sys::socket, "socket", [&]
            { return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) ==
                INVALID_SOCKET && err_is(test_err); });
        expect(sys::WSASocketW, "WSASocketW", [&]
            { return ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr,
                0, WSA_FLAG_OVERLAPPED) == INVALID_SOCKET &&
                err_is(test_err); });
        expect(sys::bind, "bind", [&]
            { return ::bind(fd, reinterpret_cast<sockaddr*>(&sa), salen)
                == SOCKET_ERROR && err_is(test_err); });
        expect(sys::listen, "listen", [&]
            { return ::listen(fd, 1) == SOCKET_ERROR && err_is(test_err); });
        expect(sys::accept, "accept", [&]
            { return ::accept(fd, nullptr, nullptr) == INVALID_SOCKET &&
                err_is(test_err); });
        expect(sys::connect, "connect", [&]
            { return ::connect(fd, reinterpret_cast<sockaddr*>(&sa),
                salen) == SOCKET_ERROR && err_is(test_err); });
        expect(sys::shutdown, "shutdown", [&]
            { return ::shutdown(fd, SD_RECEIVE) == SOCKET_ERROR &&
                err_is(test_err); });
        expect(sys::ioctlsocket, "ioctlsocket", [&]
            { return ::ioctlsocket(fd, FIONBIO, &mode) == SOCKET_ERROR &&
                err_is(test_err); });
        expect(sys::getsockname, "getsockname", [&]
            { return ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len)
                == SOCKET_ERROR && err_is(test_err); });
        expect(sys::getpeername, "getpeername", [&]
            { return ::getpeername(fd, reinterpret_cast<sockaddr*>(&sa), &len)
                == SOCKET_ERROR && err_is(test_err); });
        expect(sys::getsockopt, "getsockopt", [&]
            { int optlen = sizeof(one);
              return ::getsockopt(fd, SOL_SOCKET, SO_TYPE,
                reinterpret_cast<char*>(&one), &optlen) == SOCKET_ERROR &&
                err_is(test_err); });
        expect(sys::setsockopt, "setsockopt", [&]
            { return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                reinterpret_cast<char const*>(&one), sizeof(one)) ==
                SOCKET_ERROR && err_is(test_err); });
        expect(sys::send, "send", [&]
            { return ::send(fd, buf, sizeof(buf), 0) == SOCKET_ERROR &&
                err_is(test_err); });
        expect(sys::recv, "recv", [&]
            { return ::recv(fd, buf, sizeof(buf), 0) == SOCKET_ERROR &&
                err_is(test_err); });
        expect(sys::WSAConnect, "WSAConnect", [&]
            { return ::WSAConnect(fd, reinterpret_cast<sockaddr*>(&sa),
                salen, nullptr, nullptr, nullptr, nullptr) ==
                SOCKET_ERROR && err_is(test_err); });
        expect(sys::WSARecv, "WSARecv", [&]
            { return ::WSARecv(fd, &wbuf, 1, &bytes, &flags, nullptr,
                nullptr) == SOCKET_ERROR && err_is(test_err); });
        expect(sys::WSASend, "WSASend", [&]
            { return ::WSASend(fd, &wbuf, 1, &bytes, 0, nullptr, nullptr) ==
                SOCKET_ERROR && err_is(test_err); });
        expect(sys::WSARecvFrom, "WSARecvFrom", [&]
            { return ::WSARecvFrom(fd, &wbuf, 1, &bytes, &flags, nullptr,
                nullptr, nullptr, nullptr) == SOCKET_ERROR &&
                err_is(test_err); });
        expect(sys::WSASendTo, "WSASendTo", [&]
            { return ::WSASendTo(fd, &wbuf, 1, &bytes, 0,
                reinterpret_cast<sockaddr*>(&sa), salen, nullptr,
                nullptr) == SOCKET_ERROR && err_is(test_err); });
        expect(sys::WSAPoll, "WSAPoll", [&]
            { return ::WSAPoll(&pfd, 1, 0) == SOCKET_ERROR &&
                err_is(test_err); });
        expect(sys::WSAIoctl, "WSAIoctl", [&]
            { GUID g = WSAID_ACCEPTEX; void* p = nullptr;
              return ::WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &g,
                sizeof(g), &p, sizeof(p), &bytes, nullptr, nullptr) ==
                SOCKET_ERROR && err_is(test_err); });
        expect(sys::closesocket, "closesocket", [&]
            { return ::closesocket(fd) == SOCKET_ERROR && err_is(test_err); });
        expect(sys::WSAStartup, "WSAStartup", [&]
            { WSADATA d; return ::WSAStartup(MAKEWORD(2, 2), &d) ==
                test_err; });
        expect(sys::WSACleanup, "WSACleanup", [&]
            { return ::WSACleanup() == SOCKET_ERROR && err_is(test_err); });
        expect(sys::GetAddrInfoExW, "GetAddrInfoExW", [&]
            { PADDRINFOEXW res = nullptr;
              return ::GetAddrInfoExW(L"localhost", nullptr, NS_DNS, nullptr,
                nullptr, &res, nullptr, nullptr, nullptr, nullptr) ==
                test_err; });
        expect(sys::GetAddrInfoExCancel, "GetAddrInfoExCancel", [&]
            { return ::GetAddrInfoExCancel(&cancel) == test_err; });
        expect(sys::GetNameInfoW, "GetNameInfoW", [&]
            { return ::GetNameInfoW(reinterpret_cast<sockaddr*>(&sa),
                salen, wide, 64, nullptr, 0, 0) == test_err; });
        expect(sys::CreateIoCompletionPort, "CreateIoCompletionPort", [&]
            { return ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr,
                0, 1) == nullptr && err_is(test_err); });
        expect(sys::PostQueuedCompletionStatus, "PostQueuedCompletionStatus",
            [&] { return ::PostQueuedCompletionStatus(port, 0, 0, &ov) ==
                FALSE && err_is(test_err); });
        expect(sys::GetQueuedCompletionStatus, "GetQueuedCompletionStatus",
            [&]
            { got = &ov;
              return ::GetQueuedCompletionStatus(port, &bytes, &key, &got, 0)
                == FALSE && got == nullptr && err_is(test_err); });
        expect(sys::CancelIoEx, "CancelIoEx", [&]
            { return ::CancelIoEx(port, nullptr) == FALSE &&
                err_is(test_err); });
        expect(sys::CreateFileW, "CreateFileW", [&]
            { return ::CreateFileW(L"nul", GENERIC_READ, 0, nullptr,
                OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE &&
                err_is(test_err); });
        expect(sys::ReadFile, "ReadFile", [&]
            { return ::ReadFile(port, buf, sizeof(buf), &bytes, nullptr) ==
                FALSE && err_is(test_err); });
        expect(sys::WriteFile, "WriteFile", [&]
            { return ::WriteFile(port, buf, sizeof(buf), &bytes, nullptr) ==
                FALSE && err_is(test_err); });
        expect(sys::SetFilePointerEx, "SetFilePointerEx", [&]
            { return ::SetFilePointerEx(port, big, nullptr, FILE_BEGIN) ==
                FALSE && err_is(test_err); });
        expect(sys::GetFileSizeEx, "GetFileSizeEx", [&]
            { return ::GetFileSizeEx(port, &big) == FALSE &&
                err_is(test_err); });
        expect(sys::SetEndOfFile, "SetEndOfFile", [&]
            { return ::SetEndOfFile(port) == FALSE && err_is(test_err); });
        expect(sys::FlushFileBuffers, "FlushFileBuffers", [&]
            { return ::FlushFileBuffers(port) == FALSE && err_is(test_err); });
        expect(sys::DeleteFileA, "DeleteFileA", [&]
            { return ::DeleteFileA("nonexistent-corosio-fault") == FALSE &&
                err_is(test_err); });
        expect(sys::CreateWaitableTimerW, "CreateWaitableTimerW", [&]
            { return ::CreateWaitableTimerW(nullptr, TRUE, nullptr) ==
                nullptr && err_is(test_err); });
        expect(sys::SetWaitableTimer, "SetWaitableTimer", [&]
            { return ::SetWaitableTimer(port, &big, 0, nullptr, nullptr,
                FALSE) == FALSE && err_is(test_err); });
        expect(sys::WaitForSingleObject, "WaitForSingleObject", [&]
            { return ::WaitForSingleObject(port, 0) == WAIT_FAILED &&
                err_is(test_err); });
        expect(sys::GetComputerNameExW, "GetComputerNameExW", [&]
            { return ::GetComputerNameExW(ComputerNameDnsHostname, wide,
                &widelen) == FALSE && err_is(test_err); });
        expect(sys::GetModuleHandleA, "GetModuleHandleA", [&]
            { return ::GetModuleHandleA("kernel32") == nullptr &&
                err_is(test_err); });
        expect(sys::GetModuleHandleW, "GetModuleHandleW", [&]
            { return ::GetModuleHandleW(L"kernel32") == nullptr &&
                err_is(test_err); });
        expect(sys::GetProcAddress, "GetProcAddress", [&]
            { return ::GetProcAddress(::GetModuleHandleW(L"kernel32"),
                "CloseHandle") == nullptr && err_is(test_err); });
        expect(sys::MultiByteToWideChar, "MultiByteToWideChar", [&]
            { return ::MultiByteToWideChar(CP_UTF8, 0, "x", 1, wide, 64) == 0
                && err_is(test_err); });
        expect(sys::WideCharToMultiByte, "WideCharToMultiByte", [&]
            { return ::WideCharToMultiByte(CP_UTF8, 0, L"x", 1, narrow, 64,
                nullptr, nullptr) == 0 && err_is(test_err); });
        expect(sys::signal, "signal", [&]
            { return ::signal(SIGINT, SIG_DFL) == SIG_ERR &&
                err_is(test_err); });
        // Last, since an armed CloseHandle leaves the port open and the
        // real close has to follow.
        expect(sys::CloseHandle, "CloseHandle", [&]
            { return ::CloseHandle(port) == FALSE && err_is(test_err); });

        std::ignore = ::CloseHandle(port);
        std::ignore = ::closesocket(fd);
    }

    // The shortening hooks are the one family that forwards a modified
    // call rather than refusing it, and the non-zero clamps are
    // otherwise unreached on this platform.
    void testReturningTruncatesAndForwards()
    {
        SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET;
        if(!make_loopback_pair(a, b))
        {
            BOOST_TEST(false);
            return;
        }
        char const msg[] = "0123456789";
        char buf[16] = {};
        {
            auto f = fault_scope::returning(sys::send, 4);
            BOOST_TEST_EQ(::send(a, msg, 10, 0), 4);
            BOOST_TEST(f.fired());
        }
        BOOST_TEST_EQ(recv_exactly(b, buf, 4), 4);
        BOOST_TEST_EQ(std::string_view(buf, 4), "0123");

        // A two-entry WSABUF array walks truncate_wsabuf's prefix: the
        // first buffer fills and the second takes the remainder.
        BOOST_TEST_EQ(::send(a, msg, 10, 0), 10);
        char p1[4] = {}, p2[4] = {};
        WSABUF wb[2];
        wb[0].buf = p1;
        wb[0].len = 4;
        wb[1].buf = p2;
        wb[1].len = 4;
        {
            auto f = fault_scope::returning(sys::WSARecv, 6);
            DWORD bytes = 0;
            DWORD flags = 0;
            BOOST_TEST_EQ(::WSARecv(b, wb, 2, &bytes, &flags, nullptr,
                nullptr), 0);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(bytes, static_cast<DWORD>(6));
            BOOST_TEST_EQ(std::string_view(p1, 4), "0123");
            BOOST_TEST_EQ(std::string_view(p2, 2), "45");
        }
        BOOST_TEST_EQ(recv_exactly(b, buf, 4), 4);

        // Zero on the read side is EOF without touching the socket.
        BOOST_TEST_EQ(::send(a, msg, 10, 0), 10);
        {
            auto f = fault_scope::returning(sys::recv, 0);
            BOOST_TEST_EQ(::recv(b, buf, sizeof(buf), 0), 0);
            BOOST_TEST(f.fired());
        }
        BOOST_TEST_EQ(recv_exactly(b, buf, 10), 10);
        std::ignore = ::closesocket(a);
        std::ignore = ::closesocket(b);
    }

    // FreeAddrInfoExW has no failure mode, so its arm is a probe that
    // the release ran rather than a fault: the hook forwards either
    // way, since swallowing the call would leak the list.
    void testFreeAddrInfoRunsUnderArm()
    {
        if(!require_hook(sys::FreeAddrInfoExW, "FreeAddrInfoExW"))
            return;
        PADDRINFOEXW res = nullptr;
        if(::GetAddrInfoExW(L"localhost", nullptr, NS_DNS, nullptr, nullptr,
            &res, nullptr, nullptr, nullptr, nullptr) != 0 || !res)
            return;
        fault_scope f(sys::FreeAddrInfoExW, test_err);
        ::FreeAddrInfoExW(res);
        BOOST_TEST(f.fired());
    }

    // The one path to the error branches of the completion handlers:
    // an operation the kernel completed, reported as failed.
    void testCompletionFaultRewrites()
    {
        if(!require_hook(sys::GetQueuedCompletionStatus,
            "GetQueuedCompletionStatus"))
            return;
        HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr,
            0, 1);
        BOOST_TEST(port != nullptr);
        OVERLAPPED ov{};
        BOOST_TEST(::PostQueuedCompletionStatus(port, 7, 99, &ov) != FALSE);
        {
            completion_fault_scope c(ERROR_NETNAME_DELETED);
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            LPOVERLAPPED got = nullptr;
            ::SetLastError(0);
            BOOL const r = ::GetQueuedCompletionStatus(port, &bytes, &key,
                &got, 1000);
            DWORD const err = ::GetLastError();
            BOOST_TEST(r == FALSE);
            BOOST_TEST_EQ(err, static_cast<DWORD>(ERROR_NETNAME_DELETED));
            // The real call ran first, so the packet is still delivered:
            // that is what makes this an error on a real completion.
            BOOST_TEST(got == &ov);
            BOOST_TEST_EQ(key, static_cast<ULONG_PTR>(99));
            BOOST_TEST(c.fired());
        }
        std::ignore = ::CloseHandle(port);
    }

    void testCompletionFaultDisarmsOnScopeExit()
    {
        if(!require_hook(sys::GetQueuedCompletionStatus,
            "GetQueuedCompletionStatus"))
            return;
        HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr,
            0, 1);
        BOOST_TEST(port != nullptr);
        OVERLAPPED ov{};
        BOOST_TEST(::PostQueuedCompletionStatus(port, 7, 99, &ov) != FALSE);
        {
            completion_fault_scope c(ERROR_NETNAME_DELETED, 2);
        }
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED got = nullptr;
        BOOST_TEST(::GetQueuedCompletionStatus(port, &bytes, &key, &got, 1000)
            != FALSE);
        BOOST_TEST(got == &ov);
        std::ignore = ::CloseHandle(port);
    }

    // AcceptEx and ConnectEx are never imported: the library asks
    // WSAIoctl for them. The hook has to hand back a pointer of its own
    // or those two are unreachable.
    void testExtensionPointersAreWrapped()
    {
        if(!require_hook(sys::WSAIoctl, "WSAIoctl"))
            return;
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_TEST(s != INVALID_SOCKET);
        DWORD bytes = 0;
        GUID accept_guid = WSAID_ACCEPTEX;
        LPFN_ACCEPTEX accept_ex = nullptr;
        BOOST_TEST_EQ(::WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &accept_guid, sizeof(accept_guid), &accept_ex, sizeof(accept_ex),
            &bytes, nullptr, nullptr), 0);
        BOOST_TEST(accept_ex != nullptr);

        HMODULE owner = nullptr;
        auto const* addr = reinterpret_cast<void const*>(accept_ex);
        BOOST_TEST(::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(addr), &owner) != FALSE);
        BOOST_TEST(owner == ::GetModuleHandleW(nullptr));

        {
            fault_scope f(sys::AcceptEx, test_err);
            BOOST_TEST(accept_ex(s, s, nullptr, 0, 0, 0, &bytes, nullptr) ==
                FALSE);
            BOOST_TEST(err_is(test_err));
            BOOST_TEST(f.fired());
        }

        GUID connect_guid = WSAID_CONNECTEX;
        LPFN_CONNECTEX connect_ex = nullptr;
        BOOST_TEST_EQ(::WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &connect_guid, sizeof(connect_guid), &connect_ex,
            sizeof(connect_ex), &bytes, nullptr, nullptr), 0);
        BOOST_TEST(connect_ex != nullptr);
        {
            fault_scope f(sys::ConnectEx, test_err);
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
            BOOST_TEST(connect_ex(s, reinterpret_cast<sockaddr*>(&sa),
                static_cast<int>(sizeof(sa)), nullptr, 0, &bytes, nullptr) ==
                FALSE);
            BOOST_TEST(err_is(test_err));
            BOOST_TEST(f.fired());
        }
        std::ignore = ::closesocket(s);
    }

    // The library reaches the two ntdll entry points through
    // GetProcAddress, so the substitution has to survive the same
    // lookup the library performs.
    void testNtPointersAreWrapped()
    {
        if(!require_hook(sys::GetProcAddress, "GetProcAddress"))
            return;
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        BOOST_TEST(ntdll != nullptr);
        using nt_set_fn = LONG(NTAPI*)(HANDLE, ULONG_PTR*, void*, ULONG,
            ULONG);
        auto const fn = reinterpret_cast<nt_set_fn>(
            reinterpret_cast<void (*)()>(
                ::GetProcAddress(ntdll, "NtSetInformationFile")));
        BOOST_TEST(fn != nullptr);
        HMODULE owner = nullptr;
        auto const* addr = reinterpret_cast<void const*>(fn);
        BOOST_TEST(::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(addr), &owner) != FALSE);
        BOOST_TEST(owner == ::GetModuleHandleW(nullptr));
        {
            fault_scope f(sys::NtSetInformationFile, test_err);
            ULONG_PTR iosb[2] = {0, 0};
            void* info[2] = {nullptr, nullptr};
            BOOST_TEST(fn(INVALID_HANDLE_VALUE, iosb, &info,
                static_cast<ULONG>(sizeof(info)), 61) != 0);
            BOOST_TEST(f.fired());
        }

        // The file services resolve this one through the ANSI spelling
        // of the module name, so the substitution has to survive that
        // lookup too.
        using nt_flush_fn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG,
            void*);
        auto const flush = reinterpret_cast<nt_flush_fn>(
            reinterpret_cast<void (*)()>(::GetProcAddress(
                ::GetModuleHandleA("NTDLL"), "NtFlushBuffersFileEx")));
        // Absent before Windows 8; there is nothing to substitute then.
        if(!flush)
            return;
        HMODULE flush_owner = nullptr;
        auto const* flush_addr = reinterpret_cast<void const*>(flush);
        BOOST_TEST(::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(flush_addr), &flush_owner) != FALSE);
        BOOST_TEST(flush_owner == ::GetModuleHandleW(nullptr));
        {
            fault_scope f(sys::NtFlushBuffersFileEx, test_err);
            ULONG_PTR iosb[2] = {0, 0};
            BOOST_TEST(flush(INVALID_HANDLE_VALUE, 1, nullptr, 0, iosb)
                != 0);
            BOOST_TEST(f.fired());
        }
    }

    void run()
    {
        winsock_guard guard;
        testFiresOnNth();
        testDisarmsOnScopeExit();
        testCountTracksCalls();
        testFiredScopeStaysFired();
        testOpenFdsProbeWorks();
        testTransparentWhenUnarmed();
        testThreadIsolation();
        testAnyThreadFires();
        testThreadLocalWinsOverAnyThread();
        testTwoArmsCoexist();
        testEveryCensusSymbolFails();
        testReturningTruncatesAndForwards();
        testFreeAddrInfoRunsUnderArm();
        testCompletionFaultRewrites();
        testCompletionFaultDisarmsOnScopeExit();
        testExtensionPointersAreWrapped();
        testNtPointersAreWrapped();
    }
};

TEST_SUITE(self_test, "boost.corosio.fault.self");

} // boost::corosio::test::fault

#endif
