//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_FILE_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_FILE_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <filesystem>
#include <system_error>

namespace boost::corosio::detail {

/** Abstract stream file service base class.

    Concrete implementations (posix, IOCP) inherit from
    this class and provide platform-specific file operations.
    The context constructor installs whichever backend via
    `make_service`, and `stream_file.cpp` retrieves it via
    `use_service<file_service>()`.
*/
class BOOST_COROSIO_DECL file_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = file_service;

    /** Open a file.

        Opens the file at the given path with the specified flags
        and associates it with the platform I/O mechanism.

        @param impl The file implementation to initialize.
        @param path The filesystem path to open.
        @param mode Bitmask of file_base::flags.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code open_file(
        stream_file::implementation& impl,
        std::filesystem::path const& path,
        file_base::flags mode) = 0;

protected:
    file_service() = default;
    ~file_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_FILE_SERVICE_HPP
