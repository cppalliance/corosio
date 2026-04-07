//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_LOCAL_ENDPOINT_HPP
#define BOOST_COROSIO_LOCAL_ENDPOINT_HPP

#include <boost/corosio/detail/config.hpp>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <string_view>
#include <system_error>

namespace boost::corosio {

/** A Unix domain socket endpoint (filesystem path).

    Stores the path in a fixed-size buffer, avoiding heap
    allocation. The object is trivially copyable.

    Abstract sockets (Linux-only) are represented by paths whose
    first character is '\0'. The full path including the leading
    null byte is stored.

    The library does NOT automatically unlink the socket path on
    close — callers are responsible for cleanup.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe.
*/
class BOOST_COROSIO_DECL local_endpoint
{
    // sun_path is 108 on Linux, 104 on macOS/FreeBSD. Use the
    // minimum so local_endpoint is portable across all three.
    char path_[104]{};
    std::uint8_t len_ = 0;

public:
    /// Maximum path length for a Unix domain socket (excluding null terminator).
    static constexpr std::size_t max_path_length = 103;

    /// Default constructor. Creates an empty (unbound) endpoint.
    local_endpoint() noexcept = default;

    /** Construct from a path.

        @param path The filesystem path for the socket.
            Must not exceed @ref max_path_length bytes.

        @throws std::system_error if the path is too long.
    */
    explicit local_endpoint(std::string_view path);

    /** Construct from a path (no-throw).

        @param path The filesystem path for the socket.
        @param ec Set to an error if the path is too long.
    */
    local_endpoint(std::string_view path, std::error_code& ec) noexcept;

    /** Return the socket path.

        For abstract sockets, the returned view includes the
        leading null byte.

        @return A view over the stored path bytes.
    */
    std::string_view path() const noexcept
    {
        return std::string_view(path_, len_);
    }

    /** Check if this is an abstract socket (Linux-only).

        Abstract sockets live in a kernel namespace rather than
        the filesystem. They are identified by a leading null byte
        in the path.

        @return `true` if the path starts with '\\0'.
    */
    bool is_abstract() const noexcept
    {
        return len_ > 0 && path_[0] == '\0';
    }

    /// Return true if the endpoint has no path.
    bool empty() const noexcept
    {
        return len_ == 0;
    }

    /// Compare endpoints for equality.
    friend bool
    operator==(local_endpoint const& a, local_endpoint const& b) noexcept
    {
        return a.len_ == b.len_ &&
            std::memcmp(a.path_, b.path_, a.len_) == 0;
    }

    /** Format the endpoint for stream output.

        Non-abstract paths are printed as-is. Abstract paths
        (leading null byte) are printed as `[abstract:name]`.
        Empty endpoints produce no output.

        @param os The output stream.
        @param ep The endpoint to format.

        @return A reference to @p os.
    */
    friend BOOST_COROSIO_DECL std::ostream&
    operator<<(std::ostream& os, local_endpoint const& ep);

    /// Lexicographic ordering on stored path bytes.
    friend std::strong_ordering
    operator<=>(local_endpoint const& a, local_endpoint const& b) noexcept
    {
        auto common = (std::min)(a.len_, b.len_);
        if (int cmp = std::memcmp(a.path_, b.path_, common); cmp != 0)
            return cmp <=> 0;
        return a.len_ <=> b.len_;
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_LOCAL_ENDPOINT_HPP
