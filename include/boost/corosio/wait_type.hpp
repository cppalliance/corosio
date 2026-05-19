//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_WAIT_TYPE_HPP
#define BOOST_COROSIO_WAIT_TYPE_HPP

namespace boost::corosio {

/** Direction selector for socket and acceptor wait() operations.

    Passed to socket::wait() and acceptor::wait() to select which
    readiness condition to await before returning.
*/
enum class wait_type
{
    /// Wait until the descriptor is ready for a non-blocking read.
    read,

    /// Wait until the descriptor is ready for a non-blocking write.
    write,

    /// Wait until an error condition has been reported by the kernel
    /// (e.g. SO_ERROR is non-zero or an exceptional event is pending).
    /// Error events are not buffered across operations: an error that
    /// fires before wait(error) is registered may be lost. Kernel
    /// semantics for what counts as an "error condition" vary by
    /// platform; treat the contract as best-effort.
    error
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_WAIT_TYPE_HPP
