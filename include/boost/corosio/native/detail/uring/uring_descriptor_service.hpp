//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_URING_URING_DESCRIPTOR_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_URING_URING_DESCRIPTOR_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <boost/corosio/detail/descriptor_service.hpp>
#include <boost/corosio/native/detail/uring/uring_descriptor.hpp>
#include <boost/corosio/native/detail/uring/uring_file_service_base.hpp>
#include <boost/corosio/native/detail/uring/uring_scheduler.hpp>
#include <boost/corosio/native/detail/validate_fd.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <system_error>

/* io_uring-backed descriptor_service.

   assign_descriptor is the reactor version minus the registration
   step: io_uring has no adopt-time registration syscall, so adoption
   cannot fail once validation has passed, and a kernel refusal
   surfaces at the first operation instead. It does probe whether the
   kernel can poll the fd, which the reactors learn from registration.

   Lifecycle comes from uring_file_service_base, as asio's file service
   reuses its descriptor service.
*/

namespace boost::corosio::detail {

/// Native io_uring descriptor service. Owns every @ref uring_descriptor
/// the context creates.
class BOOST_COROSIO_DECL uring_descriptor_service final
    : public uring_file_service_base<
          uring_descriptor_service,
          descriptor_service,
          uring_descriptor>
{
    using base_service = uring_file_service_base<
        uring_descriptor_service,
        descriptor_service,
        uring_descriptor>;

public:
    explicit uring_descriptor_service(capy::execution_context& ctx)
        : base_service(ctx.use_service<uring_scheduler>())
    {
    }

    std::error_code assign_descriptor(
        posix_stream_descriptor::implementation& impl_base,
        native_handle_type fd) override
    {
        auto* impl = static_cast<uring_descriptor*>(&impl_base);

        // The public assign() guarantees the object is closed.
        if (auto ec = validate_descriptor_fd(fd))
            return ec;

        impl->set_descriptor(fd, fd_is_pollable(fd));
        return {};
    }
};

inline void
uring_descriptor::retire() noexcept
{
    svc_->pool_.recycle(this);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_URING_URING_DESCRIPTOR_SERVICE_HPP
