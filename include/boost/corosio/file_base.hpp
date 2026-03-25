//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_FILE_BASE_HPP
#define BOOST_COROSIO_FILE_BASE_HPP

#include <boost/corosio/detail/config.hpp>

namespace boost::corosio {

/** Common definitions for file I/O objects.

    Provides open flags and seek origin constants shared
    by @ref stream_file and @ref random_access_file.
*/
struct file_base
{
    /** Bitmask flags for opening a file.

        Flags are combined with bitwise OR to specify the
        desired access mode and creation behavior.
    */
    enum flags : unsigned
    {
        /// Open for reading only.
        read_only = 1,

        /// Open for writing only.
        write_only = 2,

        /// Open for reading and writing.
        read_write = read_only | write_only,

        /// Append to the end of the file on each write.
        append = 4,

        /// Create the file if it does not exist.
        create = 8,

        /// Fail if the file already exists (requires @ref create).
        exclusive = 16,

        /// Truncate the file to zero length on open.
        truncate = 32,

        /// Synchronize data to disk on each write.
        sync_all_on_write = 64
    };

    /** Origin for seek operations. */
    enum seek_basis
    {
        /// Seek relative to the beginning of the file.
        seek_set,

        /// Seek relative to the current position.
        seek_cur,

        /// Seek relative to the end of the file.
        seek_end
    };

    friend constexpr flags operator|(flags a, flags b) noexcept
    {
        return static_cast<flags>(
            static_cast<unsigned>(a) | static_cast<unsigned>(b));
    }

    friend constexpr flags operator&(flags a, flags b) noexcept
    {
        return static_cast<flags>(
            static_cast<unsigned>(a) & static_cast<unsigned>(b));
    }

    friend constexpr flags& operator|=(flags& a, flags b) noexcept
    {
        return a = a | b;
    }

    friend constexpr flags& operator&=(flags& a, flags b) noexcept
    {
        return a = a & b;
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_FILE_BASE_HPP
