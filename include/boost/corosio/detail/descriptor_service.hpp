//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_DESCRIPTOR_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_DESCRIPTOR_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/posix_descriptor.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <system_error>

namespace boost::corosio::detail {

/* Abstract descriptor service base class.

   Concrete implementations (epoll, select, kqueue, uring) inherit
   from this class. The three reactor backends register the adopted
   fd with their reactor; uring has no adopt-time registration and
   only takes ownership of it.
   The context constructor installs whichever backend via
   make_service, and posix_descriptor.cpp retrieves it via
   create_handle<descriptor_service>().
*/
class BOOST_COROSIO_DECL descriptor_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for execution_context lookup.
    using key_type = descriptor_service;

    /** Adopt an existing native descriptor.

        Validates before mutating: on failure the implementation
        keeps its previous descriptor and pending operations, and
        the caller retains ownership of @a fd. On success the
        implementation takes ownership and will close it.

        @param impl The descriptor implementation to assign to.
        @param fd The native descriptor to adopt.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code assign_descriptor(
        posix_descriptor::implementation& impl, native_handle_type fd) = 0;

protected:
    descriptor_service()           = default;
    ~descriptor_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif
