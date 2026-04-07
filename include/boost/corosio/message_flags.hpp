//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_MESSAGE_FLAGS_HPP
#define BOOST_COROSIO_MESSAGE_FLAGS_HPP

namespace boost::corosio {

/** Flags for datagram send/recv operations.

    Platform-agnostic flag values that are mapped to native
    constants (MSG_PEEK, MSG_OOB, MSG_DONTROUTE) at the
    syscall boundary in the reactor implementation.
*/
enum class message_flags : int
{
    /// No flags set.
    none         = 0,
    /// Peek at incoming data without consuming it (MSG_PEEK).
    peek         = 1,
    /// Send or receive out-of-band data (MSG_OOB).
    out_of_band  = 2,
    /// Bypass routing tables (MSG_DONTROUTE).
    do_not_route = 4
};

/// Combine two flag sets with bitwise OR.
inline constexpr message_flags
operator|(message_flags a, message_flags b) noexcept
{
    return static_cast<message_flags>(
        static_cast<int>(a) | static_cast<int>(b));
}

/// Intersect two flag sets with bitwise AND.
inline constexpr message_flags
operator&(message_flags a, message_flags b) noexcept
{
    return static_cast<message_flags>(
        static_cast<int>(a) & static_cast<int>(b));
}

/// Invert a flag set, masked to the defined flag bits.
inline constexpr message_flags
operator~(message_flags a) noexcept
{
    constexpr int mask =
        static_cast<int>(message_flags::peek) |
        static_cast<int>(message_flags::out_of_band) |
        static_cast<int>(message_flags::do_not_route);
    return static_cast<message_flags>(~static_cast<int>(a) & mask);
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_MESSAGE_FLAGS_HPP
