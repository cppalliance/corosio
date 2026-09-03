//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Destroying the io_context while io_uring operations are still in
// the ring must drain them without resuming or touching the awaiting
// coroutines. Companion tests drive the multishot acceptor's wait and
// close paths that only exist on the io_uring backend.

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/test/temp_path.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

namespace {

[[maybe_unused]] void
fill_fd(int fd)
{
    char junk[4096] = {};
    ssize_t n;
    do
    {
        n = ::send(fd, junk, sizeof(junk), MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (n > 0);
}

[[maybe_unused]] void
fill_pipe(int fd)
{
    char junk[4096] = {};
    ssize_t n;
    do
    {
        n = ::write(fd, junk, sizeof(junk));
    } while (n > 0);
}

struct temp_file
{
    std::filesystem::path path;

    explicit temp_file(std::string_view contents)
    {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
            ("corosio_teardown_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter++));
        std::ofstream(path) << contents;
    }

    ~temp_file()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    temp_file(temp_file const&)            = delete;
    temp_file& operator=(temp_file const&) = delete;
};

} // namespace

struct io_uring_teardown_test
{
    void testAcceptorWaitAfterClose()
    {
        io_context ioc(io_uring);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(tcp::v4()));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        acc.close();

        std::error_code wec;
        bool done   = false;
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            wec       = ec;
            done      = true;
        };
        capy::run_async(ex)(waiter());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(!!wec);
    }

    void testAcceptorWaitStopRequested()
    {
        io_context ioc(io_uring);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(tcp::v4()));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        std::stop_source ss;
        std::error_code wec;
        bool done   = false;
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            wec       = ec;
            done      = true;
        };
        auto stopper = [&]() -> capy::task<> {
            ss.request_stop();
            co_return;
        };
        capy::run_async(ex, ss.get_token())(waiter());
        capy::run_async(ex)(stopper());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(wec == capy::cond::canceled);
    }

    void testAcceptorCloseWithParkedWait()
    {
        io_context ioc(io_uring);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(tcp::v4()));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        std::error_code wec, aec;
        tcp_socket peer(ioc);
        int done      = 0;
        auto accepter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(peer);
            aec       = ec;
            ++done;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            wec       = ec;
            ++done;
        };
        auto closer = [&]() -> capy::task<> {
            acc.close();
            co_return;
        };
        capy::run_async(ex)(accepter());
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(closer());
        ioc.run();

        BOOST_TEST_EQ(done, 2);
        BOOST_TEST(!!aec);
        BOOST_TEST(!!wec);
    }

#if !COROSIO_TEST_HAS_ASAN
    // These abandon parked coroutine frames by design; see context.hpp.

    void testDestroyWithPendingSocketWrite()
    {
        bool resumed = false;
        {
            io_context ioc(io_uring);
            // The pair is made outside the coroutine (it drains the
            // context internally); the socket moves into the frame so
            // its fd stays open when the frame is abandoned.
            auto [s1, s2] =
                test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);
            fill_fd(static_cast<int>(s1.native_handle()));
            auto keeper = [](tcp_socket s, bool& flag) -> capy::task<> {
                char big[65536] = {};
                std::ignore     = co_await s.write_some(
                    capy::const_buffer(big, sizeof(big)));
                flag = true;
            }(std::move(s1), resumed);
            capy::run_async(ioc.get_executor())(std::move(keeper));
            std::ignore = ioc.run_one();
        }
        BOOST_TEST(!resumed);
    }

    void testDestroyWithPendingDatagramSend()
    {
        bool resumed = false;
        {
            io_context ioc(io_uring);
            local_datagram_socket d1(ioc), d2(ioc);
            if (auto ec = connect_pair(d1, d2))
                throw std::system_error(ec, "connect_pair");
            fill_fd(static_cast<int>(d1.native_handle()));
            auto keeper = [](local_datagram_socket d, bool& flag)
                -> capy::task<> {
                std::ignore = co_await d.send(capy::const_buffer("x", 1));
                flag        = true;
            }(std::move(d1), resumed);
            capy::run_async(ioc.get_executor())(std::move(keeper));
            std::ignore = ioc.run_one();
        }
        BOOST_TEST(!resumed);
    }

    void testDestroyWithPendingFileOps()
    {
        // The counterpart end of each pipe stays raw and open past the
        // context so the drained ops never hit a broken pipe.
        int rp[2], wp[2];
        BOOST_TEST(::pipe2(rp, O_NONBLOCK) == 0);
        BOOST_TEST(::pipe2(wp, O_NONBLOCK) == 0);
        fill_pipe(wp[1]);

        bool read_resumed = false, write_resumed = false;
        {
            io_context ioc(io_uring);
            auto reader = [&]() -> capy::task<> {
                stream_file f(ioc);
                std::ignore = f.assign(static_cast<native_handle_type>(rp[0]));
                char buf[16];
                std::ignore = co_await f.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                read_resumed = true;
            };
            auto writer = [&]() -> capy::task<> {
                stream_file f(ioc);
                std::ignore = f.assign(static_cast<native_handle_type>(wp[1]));
                char big[4096] = {};
                std::ignore    = co_await f.write_some(
                    capy::const_buffer(big, sizeof(big)));
                write_resumed = true;
            };
            capy::run_async(ioc.get_executor())(reader());
            capy::run_async(ioc.get_executor())(writer());
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
        }
        ::close(rp[1]);
        ::close(wp[0]);
        BOOST_TEST(!read_resumed);
        BOOST_TEST(!write_resumed);
    }

    void testDestroyWithSubmittedRandomAccessOps()
    {
        // The ops complete in the ring almost immediately, but nothing
        // processes the completions before teardown, so the drain path
        // must reclaim them.
        temp_file tmp("hello world");
        auto const path = tmp.path;
        bool read_resumed = false, write_resumed = false;
        {
            io_context ioc(io_uring);
            auto reader = [&]() -> capy::task<> {
                random_access_file f(ioc);
                std::ignore = f.open(path, file_base::read_write);
                char buf[8];
                std::ignore = co_await f.read_some_at(
                    0, capy::mutable_buffer(buf, sizeof(buf)));
                read_resumed = true;
            };
            auto writer = [&]() -> capy::task<> {
                random_access_file f(ioc);
                std::ignore = f.open(path, file_base::read_write);
                std::ignore =
                    co_await f.write_some_at(0, capy::const_buffer("x", 1));
                write_resumed = true;
            };
            capy::run_async(ioc.get_executor())(reader());
            capy::run_async(ioc.get_executor())(writer());
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
        }
        BOOST_TEST(!read_resumed);
        BOOST_TEST(!write_resumed);
    }

    // Two completable ops of one kind: three run_one() calls start
    // both and dispatch one completion, leaving the second reaped but
    // undispatched when the context dies, so the drain must run its
    // handler ownerless.

    void testDestroyWithBrokenPipeWriteSurvives()
    {
        // Both ends of one pipe are wrapped, with a write parked on
        // the full pipe. Service shutdown closes the read end first,
        // so the flushed write executes against a broken pipe; the
        // library must absorb the SIGPIPE instead of dying.
        int p[2];
        BOOST_TEST(::pipe2(p, O_NONBLOCK) == 0);
        fill_pipe(p[1]);

        bool read_resumed = false, write_resumed = false;
        {
            io_context ioc(io_uring);
            auto reader = [&]() -> capy::task<> {
                stream_file f(ioc);
                std::ignore = f.assign(static_cast<native_handle_type>(p[0]));
                char buf[16];
                std::ignore = co_await f.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                read_resumed = true;
            };
            auto writer = [&]() -> capy::task<> {
                stream_file f(ioc);
                std::ignore = f.assign(static_cast<native_handle_type>(p[1]));
                char big[4096] = {};
                std::ignore    = co_await f.write_some(
                    capy::const_buffer(big, sizeof(big)));
                write_resumed = true;
            };
            capy::run_async(ioc.get_executor())(reader());
            capy::run_async(ioc.get_executor())(writer());
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
        }
        BOOST_TEST(!read_resumed);
        BOOST_TEST(!write_resumed);
    }


    void testDestroyWithQueuedSocketWrites()
    {
        int resumed = 0;
        {
            io_context ioc(io_uring);
            auto [s1, s2] =
                test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);
            auto [s3, s4] =
                test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);
            fill_fd(static_cast<int>(s1.native_handle()));
            fill_fd(static_cast<int>(s3.native_handle()));
            char big[65536] = {};
            auto writer = [](tcp_socket& s, capy::const_buffer b,
                              int& count) -> capy::task<> {
                std::ignore = co_await s.write_some(b);
                ++count;
            };
            capy::run_async(ioc.get_executor())(
                writer(s1, capy::const_buffer(big, sizeof(big)), resumed));
            capy::run_async(ioc.get_executor())(
                writer(s3, capy::const_buffer(big, sizeof(big)), resumed));
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
            // Raw-drain both peers so the parked writes complete in
            // the ring; one more slice reaps and dispatches one.
            char sink[65536];
            for (auto* peer : {&s2, &s4})
            {
                while (::recv(static_cast<int>(peer->native_handle()), sink,
                           sizeof(sink), MSG_DONTWAIT) > 0)
                {
                }
            }
            std::ignore = ioc.run_one();
        }
        BOOST_TEST_LT(resumed, 2);
    }

    void testDestroyWithQueuedDatagramSends()
    {
        int resumed = 0;
        {
            io_context ioc(io_uring);
            local_datagram_socket d1(ioc), d2(ioc), d3(ioc), d4(ioc);
            if (auto ec = connect_pair(d1, d2))
                throw std::system_error(ec, "connect_pair");
            if (auto ec = connect_pair(d3, d4))
                throw std::system_error(ec, "connect_pair");
            fill_fd(static_cast<int>(d1.native_handle()));
            fill_fd(static_cast<int>(d3.native_handle()));
            auto sender =
                [](local_datagram_socket& d, int& count) -> capy::task<> {
                std::ignore = co_await d.send(capy::const_buffer("x", 1));
                ++count;
            };
            capy::run_async(ioc.get_executor())(sender(d1, resumed));
            capy::run_async(ioc.get_executor())(sender(d3, resumed));
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
            char sink[4096];
            for (auto* peer : {&d2, &d4})
            {
                while (::recv(static_cast<int>(peer->native_handle()), sink,
                           sizeof(sink), MSG_DONTWAIT) > 0)
                {
                }
            }
            std::ignore = ioc.run_one();
        }
        BOOST_TEST_LT(resumed, 2);
    }

    void testDestroyWithQueuedFileOps()
    {
        int rp[2], wp[2];
        BOOST_TEST(::pipe2(rp, O_NONBLOCK) == 0);
        BOOST_TEST(::pipe2(wp, O_NONBLOCK) == 0);
        std::ignore = ::write(rp[1], "seed-data-16byte", 16);

        int resumed = 0;
        {
            io_context ioc(io_uring);
            auto reader = [](io_context& ctx, int fd, int& count)
                -> capy::task<> {
                stream_file f(ctx);
                std::ignore = f.assign(static_cast<native_handle_type>(fd));
                char buf[4];
                std::ignore = co_await f.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                ++count;
                std::ignore = co_await f.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                ++count;
            };
            auto writer = [](io_context& ctx, int fd, int& count)
                -> capy::task<> {
                stream_file f(ctx);
                std::ignore = f.assign(static_cast<native_handle_type>(fd));
                std::ignore =
                    co_await f.write_some(capy::const_buffer("a", 1));
                ++count;
                std::ignore =
                    co_await f.write_some(capy::const_buffer("b", 1));
                ++count;
            };
            capy::run_async(ioc.get_executor())(reader(ioc, rp[0], resumed));
            capy::run_async(ioc.get_executor())(writer(ioc, wp[1], resumed));
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
        }
        ::close(rp[1]);
        ::close(wp[0]);
        BOOST_TEST_LT(resumed, 4);
    }

    void testDestroyWithQueuedRandomAccessOps()
    {
        temp_file tmp("hello world");
        auto const path = tmp.path;
        int resumed     = 0;
        {
            io_context ioc(io_uring);
            auto reader = [](io_context& ctx, std::filesystem::path p,
                              int& count) -> capy::task<> {
                random_access_file f(ctx);
                std::ignore = f.open(p, file_base::read_write);
                char buf[4];
                std::ignore = co_await f.read_some_at(
                    0, capy::mutable_buffer(buf, sizeof(buf)));
                ++count;
                std::ignore = co_await f.read_some_at(
                    1, capy::mutable_buffer(buf, sizeof(buf)));
                ++count;
            };
            auto writer = [](io_context& ctx, std::filesystem::path p,
                              int& count) -> capy::task<> {
                random_access_file f(ctx);
                std::ignore = f.open(p, file_base::read_write);
                std::ignore =
                    co_await f.write_some_at(0, capy::const_buffer("x", 1));
                ++count;
                std::ignore =
                    co_await f.write_some_at(1, capy::const_buffer("y", 1));
                ++count;
            };
            capy::run_async(ioc.get_executor())(reader(ioc, path, resumed));
            capy::run_async(ioc.get_executor())(writer(ioc, path, resumed));
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
        }
        BOOST_TEST_LT(resumed, 4);
    }


    void testDestroyWithParkedLocalAccept()
    {
        bool resumed = false;
        {
            io_context ioc(io_uring);
            auto keeper = [&]() -> capy::task<> {
                test::temp_socket_dir tmp;
                local_stream_acceptor acc(ioc);
                std::ignore = acc.open();
                std::ignore = acc.bind(local_endpoint(tmp.path()));
                std::ignore = acc.listen();
                local_stream_socket peer(ioc);
                std::ignore = co_await acc.accept(peer);
                resumed     = true;
            };
            capy::run_async(ioc.get_executor())(keeper());
            std::ignore = ioc.run_one();
        }
        BOOST_TEST(!resumed);
    }
#endif // !COROSIO_TEST_HAS_ASAN

    void run()
    {
        testAcceptorWaitAfterClose();
        testAcceptorWaitStopRequested();
        testAcceptorCloseWithParkedWait();
#if !COROSIO_TEST_HAS_ASAN
        testDestroyWithPendingSocketWrite();
        testDestroyWithPendingDatagramSend();
        testDestroyWithPendingFileOps();
        testDestroyWithSubmittedRandomAccessOps();
        testDestroyWithBrokenPipeWriteSurvives();
        testDestroyWithQueuedSocketWrites();
        testDestroyWithQueuedDatagramSends();
        testDestroyWithQueuedFileOps();
        testDestroyWithQueuedRandomAccessOps();
        testDestroyWithParkedLocalAccept();
#endif
    }
};

TEST_SUITE(io_uring_teardown_test, "boost.corosio.io_uring_teardown");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IO_URING
