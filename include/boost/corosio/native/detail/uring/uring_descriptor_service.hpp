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
#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/native/detail/uring/uring_descriptor.hpp>
#include <boost/corosio/native/detail/uring/uring_scheduler.hpp>
#include <boost/corosio/native/detail/validate_fd.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

/* io_uring-backed descriptor_service.

   assign_descriptor is the validate-before-mutate core the public
   assign() contract rests on. It is the reactor version minus the
   registration step: io_uring has no adopt-time registration syscall,
   so adoption cannot fail once validation has passed, and a kernel
   refusal surfaces at the first operation instead.

   Neither uring_socket_service_base nor uring_file_service_base fits:
   the first constructs impls with a (service&, scheduler&) ctor and
   only cancels on shutdown, the second names its teardown hook
   close_file(). The lifecycle here is small enough to carry directly.

   PARALLEL COPY: construct, destroy, close and shutdown here mirror the
   same four members of reactor_descriptor_service.hpp, which this
   cannot reuse because that template is rooted in the reactor's
   scheduler and service state. A fix to the descriptor service
   lifecycle -- the construct/destroy bookkeeping, or shutdown's
   deliberate retention of the impl map so impls outlive the
   scheduler's drain -- belongs in both files.
*/

namespace boost::corosio::detail {

/// Native io_uring descriptor service. Owns every @ref uring_descriptor
/// the context creates.
class BOOST_COROSIO_DECL uring_descriptor_service final
    : public descriptor_service
{
    uring_scheduler* sched_ = nullptr;
    std::mutex mutex_;
    std::unordered_map<uring_descriptor*, std::shared_ptr<uring_descriptor>>
        impls_;

public:
    explicit uring_descriptor_service(capy::execution_context& ctx)
        : sched_(&ctx.use_service<uring_scheduler>())
    {
    }

    std::error_code assign_descriptor(
        posix_descriptor::implementation& impl_base,
        native_handle_type fd) override
    {
        auto* impl = static_cast<uring_descriptor*>(&impl_base);

        // fd >= 0 guard: an unset impl reports native_handle() == -1, and
        // a caller-supplied -1 must fail as a bad fd, not a self-assign.
        if (fd >= 0 && fd == impl->native_handle())
            return std::make_error_code(std::errc::invalid_argument);

        // Validate before touching the held descriptor: a failed assign
        // must leave the object unchanged and the caller owning the fd.
        if (auto ec = validate_descriptor_fd(fd))
            return ec;

        impl->close_descriptor();
        impl->set_descriptor(fd);
        return {};
    }

    io_object::implementation* construct() override
    {
        auto p    = std::make_shared<uring_descriptor>(*sched_);
        auto* raw = p.get();
        std::lock_guard lock(mutex_);
        impls_.emplace(raw, std::move(p));
        return raw;
    }

    void destroy(io_object::implementation* p) override
    {
        if (!p)
            return;
        auto* impl = static_cast<uring_descriptor*>(p);
        impl->close_descriptor();
        std::lock_guard lock(mutex_);
        impls_.erase(impl);
    }

    void close(io_object::handle& h) override
    {
        if (auto* impl = static_cast<uring_descriptor*>(h.get()))
            impl->close_descriptor();
    }

    void shutdown() override
    {
        // Snapshot, then close without the lock held. impls_ is
        // deliberately not cleared: the scheduler shuts down after this
        // service and drains its completed ops, so every impl must
        // outlive that drain.
        std::vector<std::shared_ptr<uring_descriptor>> live;
        {
            std::lock_guard lock(mutex_);
            live.reserve(impls_.size());
            for (auto& [raw, p] : impls_)
                live.push_back(p);
        }
        for (auto& p : live)
            p->close_descriptor();
    }

private:
    uring_descriptor_service(uring_descriptor_service const&) = delete;
    uring_descriptor_service&
    operator=(uring_descriptor_service const&) = delete;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_URING_URING_DESCRIPTOR_SERVICE_HPP
