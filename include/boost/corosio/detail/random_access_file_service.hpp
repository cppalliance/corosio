//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_RANDOM_ACCESS_FILE_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_RANDOM_ACCESS_FILE_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <filesystem>
#include <system_error>

namespace boost::corosio::detail {

/** Abstract random-access file service base class.

    Concrete implementations (posix, IOCP) inherit from
    this class and provide platform-specific file operations.
*/
class BOOST_COROSIO_DECL random_access_file_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = random_access_file_service;

    /** Open a file.

        @param impl The file implementation to initialize.
        @param path The filesystem path to open.
        @param mode Bitmask of file_base::flags.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code open_file(
        random_access_file::implementation& impl,
        std::filesystem::path const& path,
        file_base::flags mode) = 0;

protected:
    random_access_file_service() = default;
    ~random_access_file_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_RANDOM_ACCESS_FILE_SERVICE_HPP
