//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/native/detail/validate_fd.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <filesystem>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#endif

#include "test_suite.hpp"

namespace boost::corosio {

struct validate_fd_test
{
    void testRejectsBadFd()
    {
        BOOST_TEST(
            detail::validate_descriptor_fd(-1) ==
            std::errc::bad_file_descriptor);

        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);
        ::close(fds[0]);
        ::close(fds[1]);
        BOOST_TEST(
            detail::validate_descriptor_fd(fds[0]) ==
            std::errc::bad_file_descriptor);
    }

    void testAcceptsPipe()
    {
        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);
        BOOST_TEST(!detail::validate_descriptor_fd(fds[0]));
        BOOST_TEST(!detail::validate_descriptor_fd(fds[1]));
        ::close(fds[0]);
        ::close(fds[1]);
    }

    void testAcceptsAnonymousInode()
    {
        // The flagship fd kinds (eventfd, timerfd, inotify, pidfd) are
        // anonymous inodes whose st_mode type bits are all zero. This is
        // exactly why the check is a reject-list and not an accept-list.
        //
        // /dev/null stands in for a character device here because it is
        // portable, but note that passing this check is not a promise
        // that assign() will succeed: /dev/null is not pollable, so
        // epoll refuses it with EPERM and kqueue with EINVAL. The
        // validator's job is the file-type policy, not reachability.
        int fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        BOOST_TEST(fd >= 0);
        BOOST_TEST(!detail::validate_descriptor_fd(fd));
        ::close(fd);
    }

    void testRejectsRegularFile()
    {
        // stream_file::assign() / random_access_file::assign() own
        // adoption of regular files; posix_descriptor rejects by policy.
        auto path =
            std::filesystem::temp_directory_path() / "corosio_validate_fd_test";
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        BOOST_TEST(fd >= 0);
        BOOST_TEST(
            detail::validate_descriptor_fd(fd) ==
            std::errc::operation_not_supported);
        ::close(fd);
        std::filesystem::remove(path);
    }

    void testRejectsDirectory()
    {
        int fd = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        BOOST_TEST(fd >= 0);
        BOOST_TEST(
            detail::validate_descriptor_fd(fd) ==
            std::errc::operation_not_supported);
        ::close(fd);
    }

    void testFileFdAcceptListInversion()
    {
        // validate_file_fd is an accept-list, the deliberate inverse of
        // validate_descriptor_fd's reject-list: a regular file is the
        // fd kind a file object exists to adopt.
        auto path = std::filesystem::temp_directory_path() /
            "corosio_validate_file_fd_test";
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        BOOST_TEST(fd >= 0);
        BOOST_TEST(!detail::validate_file_fd(fd));
        ::close(fd);
        std::filesystem::remove(path);

#if defined(__linux__)
        // Head-to-head on the same fd: an anonymous inode (no st_mode
        // type bits) is exactly what validate_descriptor_fd exists to
        // admit and exactly what validate_file_fd -- needing a
        // positionable fd -- must reject.
        int efd = ::eventfd(0, EFD_CLOEXEC);
        BOOST_TEST(efd >= 0);
        BOOST_TEST(!detail::validate_descriptor_fd(efd));
        BOOST_TEST(
            detail::validate_file_fd(efd) ==
            std::errc::operation_not_supported);
        ::close(efd);
#endif
    }

    void testEnsureNonblockingIsIdempotentAndLazy()
    {
        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);

        // Validation alone must never touch the flags.
        BOOST_TEST(!detail::validate_descriptor_fd(fds[0]));
        BOOST_TEST_EQ(::fcntl(fds[0], F_GETFL) & O_NONBLOCK, 0);

        BOOST_TEST(!detail::ensure_nonblocking(fds[0]));
        BOOST_TEST(::fcntl(fds[0], F_GETFL) & O_NONBLOCK);

        // Second call is a no-op and still succeeds.
        BOOST_TEST(!detail::ensure_nonblocking(fds[0]));
        BOOST_TEST(::fcntl(fds[0], F_GETFL) & O_NONBLOCK);

        ::close(fds[0]);
        ::close(fds[1]);

        BOOST_TEST(
            detail::ensure_nonblocking(fds[0]) ==
            std::errc::bad_file_descriptor);
    }

    void run()
    {
        testRejectsBadFd();
        testAcceptsPipe();
        testAcceptsAnonymousInode();
        testRejectsRegularFile();
        testRejectsDirectory();
        testFileFdAcceptListInversion();
        testEnsureNonblockingIsIdempotentAndLazy();
    }
};

TEST_SUITE(validate_fd_test, "boost.corosio.validate_fd");

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX
