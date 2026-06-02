//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Probe file used by build/Jamfile via b2's check-target-builds to detect
// whether a sufficiently recent liburing is installed and linkable. The
// CMake build uses find_package(liburing 2.5); this probe matches that
// requirement by referencing symbols and flags the io_uring backend uses
// that only exist in liburing 2.3+ (multishot accept, cancel-by-fd,
// DEFER_TASKRUN, submit_and_get_events). On Ubuntu 22.04's liburing 2.1
// these are missing and the probe fails, so the io_uring backend is
// correctly disabled.

#include <liburing.h>

int main()
{
    struct io_uring ring;
    struct io_uring_params params{};
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    io_uring_queue_init_params(8, &ring, &params);

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_multishot_accept(sqe, 0, nullptr, nullptr, 0);
    io_uring_prep_cancel_fd(sqe, 0, IORING_ASYNC_CANCEL_ALL);
    io_uring_submit_and_get_events(&ring);

    io_uring_queue_exit(&ring);
    return 0;
}
