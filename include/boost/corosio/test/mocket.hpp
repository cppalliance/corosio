//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_MOCKET_HPP
#define BOOST_COROSIO_TEST_MOCKET_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_stream.hpp>
#include <boost/system/error_code.hpp>

#include <string>
#include <utility>

namespace boost::capy {
class execution_context;

namespace test {
class fuse;
} // namespace test

} // namespace boost::capy::test

namespace boost::corosio::test {

class mocket_impl;

/** A mock socket for testing I/O operations.

    This class provides a testable socket-like interface where data
    can be staged for reading and expected data can be validated on
    writes. Two mockets are connected together using @ref make_mockets,
    allowing bidirectional communication testing.

    When reading, data comes from the peer mocket's `provide()` buffer.
    When writing, data is validated against this mocket's `expect()` buffer.
    Once both buffers are exhausted, I/O passes through to the underlying
    socket connection.

    @par Thread Safety
    Not thread-safe. All operations must occur on a single thread.
    All coroutines using the mockets must be suspended when calling
    `expect()` or `provide()`.

    @see make_mockets
*/
class BOOST_COROSIO_DECL mocket : public io_stream
{
    friend class mocket_impl;

    mocket_impl* get_impl() const noexcept;

public:
    /** Destructor.
    */
    ~mocket();

    /** Move constructor.
    */
    mocket(mocket&& other) noexcept;

    /** Move assignment.
    */
    mocket& operator=(mocket&& other) noexcept;

    mocket(mocket const&) = delete;
    mocket& operator=(mocket const&) = delete;

    /** Stage data for the peer to read.

        Appends the given string to this mocket's provide buffer.
        When the peer mocket calls `read_some`, it will receive
        this data.

        @param s The data to provide to the peer.

        @pre All coroutines using this mocket must be suspended.
    */
    void provide(std::string s);

    /** Set expected data for writes.

        Appends the given string to this mocket's expect buffer.
        When the caller writes to this mocket, the written data
        must match the expected data. On mismatch, `fuse::fail()`
        is called.

        @param s The expected data.

        @pre All coroutines using this mocket must be suspended.
    */
    void expect(std::string s);

    /** Close the mocket and verify test expectations.

        Closes the underlying socket and verifies that both the
        `expect()` and `provide()` buffers are empty. If either
        buffer contains unconsumed data, returns `test_failure`
        and calls `fuse::fail()`.

        @return An error code indicating success or failure.
            Returns `error::test_failure` if buffers are not empty.
    */
    system::error_code close();

    /** Check if the mocket is open.

        @return `true` if the mocket is open.
    */
    bool is_open() const noexcept;

private:
    friend BOOST_COROSIO_DECL std::pair<mocket, mocket>
    make_mockets(capy::execution_context&, capy::test::fuse&);

    explicit mocket(mocket_impl* impl) noexcept;
};

/** Create a connected pair of mockets.

    Creates two mockets connected via loopback TCP sockets.
    Data written to one mocket can be read from the other.

    The first mocket (m1) has fuse checks enabled via `maybe_fail()`.
    The second mocket (m2) does not call `maybe_fail()`.

    @param ctx The execution context for the mockets.
    @param f The fuse for error injection testing.

    @return A pair of connected mockets.

    @note Mockets are not thread-safe and must be used in a
        single-threaded, deterministic context.
*/
BOOST_COROSIO_DECL
std::pair<mocket, mocket>
make_mockets(capy::execution_context& ctx, capy::test::fuse& f);

} // namespace boost::corosio::test

#endif
