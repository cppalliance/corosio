//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_HANDLE_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_WIN_HANDLE_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/win_object_handle.hpp>
#include <boost/corosio/win_random_access_handle.hpp>
#include <boost/corosio/win_stream_handle.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <system_error>

namespace boost::corosio::detail {

/* Abstract handle service bases. The IOCP backend installs the
   concrete services on first use; the public .cpp files retrieve them
   through use_backend_service under these keys.
*/

/// Abstract service behind @ref win_stream_handle.
class BOOST_COROSIO_DECL stream_handle_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for execution_context lookup.
    using key_type = stream_handle_service;

    /** Adopt an existing handle.

        The implementation is closed. On failure it stays closed and
        the caller retains ownership of @a h.

        @param impl The implementation to assign to.
        @param h The native handle to adopt.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code assign_stream_handle(
        win_stream_handle::implementation& impl, native_handle_type h) = 0;

protected:
    stream_handle_service()           = default;
    ~stream_handle_service() override = default;
};

/// Abstract service behind @ref win_random_access_handle.
class BOOST_COROSIO_DECL random_access_handle_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for execution_context lookup.
    using key_type = random_access_handle_service;

    /** Adopt an existing handle.

        The implementation is closed. On failure it stays closed and
        the caller retains ownership of @a h.

        @param impl The implementation to assign to.
        @param h The native handle to adopt.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code assign_random_access_handle(
        win_random_access_handle::implementation& impl,
        native_handle_type h) = 0;

protected:
    random_access_handle_service()           = default;
    ~random_access_handle_service() override = default;
};

/// Abstract service behind @ref win_object_handle.
class BOOST_COROSIO_DECL object_handle_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for execution_context lookup.
    using key_type = object_handle_service;

    /** Adopt an existing waitable handle.

        The implementation is closed. On failure it stays closed and
        the caller retains ownership of @a h.

        @param impl The implementation to assign to.
        @param h The native handle to adopt.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code assign_object_handle(
        win_object_handle::implementation& impl, native_handle_type h) = 0;

protected:
    object_handle_service()           = default;
    ~object_handle_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif
