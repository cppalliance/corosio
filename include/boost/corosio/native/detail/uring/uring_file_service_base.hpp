//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_URING_URING_FILE_SERVICE_BASE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_URING_URING_FILE_SERVICE_BASE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <boost/corosio/detail/object_pool.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/native/detail/uring/uring_scheduler.hpp>

#include <vector>

/*
    Shared lifecycle plumbing for io_uring file services.

    uring_stream_file_service and uring_random_access_file_service were
    byte-for-byte identical apart from the impl type and open_file's parameter
    type: both pop-or-new the file impl from a per-service recycling pool
    and close every file on shutdown. This base factors that out; the
    concrete services add only open_file.

    This is a separate base from uring_socket_service_base because file
    services differ from socket services in two ways that match the reactor
    socket service instead: they CLOSE files on shutdown (sockets: cancel
    only), and the impl ctor takes just the scheduler (sockets: service +
    scheduler). See tasks/proactor-dedup-decisions.md (#14).

    Requirements on File: derive from intrusive_list<File>::node, a
    `File(Derived&, uring_scheduler&)` constructor, a
    `void close_file() noexcept` method (cancel in-flight ops + close fd),
    a `void reuse() noexcept` method, and a `retire()` override that
    recycles through the owning service's private `pool_` member
    (reachable because `File` is a friend). uring_descriptor_service
    reuses it too, as asio's file service reuses its descriptor service.

    @tparam Derived     The concrete service (CRTP; unused today but kept for
                        symmetry / future hooks).
    @tparam ServiceBase The abstract service vtable base (file_service,
                        random_access_file_service).
    @tparam File        The concrete io_uring file impl type.
*/

namespace boost::corosio::detail {

template<class Derived, class ServiceBase, class File>
class uring_file_service_base : public ServiceBase
{
    friend Derived;
    friend File;

    // Private CRTP ctor: only `Derived` (the concrete service, a friend)
    // constructs the base — prevents inheriting with the wrong Derived
    // (bugprone-crtp-constructor-accessibility).
    explicit uring_file_service_base(uring_scheduler& sched) noexcept
        : sched_(&sched)
    {
    }

public:
    ~uring_file_service_base() override = default;

    io_object::implementation* construct() override
    {
        return pool_.acquire(static_cast<Derived&>(*this), *sched_);
    }

    void destroy(io_object::implementation* p) override
    {
        // close_file() closes the fd only once its cancel reached the kernel.
        auto& impl = static_cast<File&>(*p);
        impl.close_file();
        release(&impl);
    }

    void close(io_object::handle& h) override
    {
        if (h.get())
            static_cast<File&>(*h.get()).close_file();
    }

    void shutdown() override
    {
        // See uring_socket_service_base::shutdown(): snapshot under an
        // acquired reference, close without the pool lock held;
        // shutdown() sets shutting-down and takes the snapshot in one
        // critical section, so a close that drops the last ref deletes
        // rather than recycles.
        std::vector<File*> live;
        pool_.shutdown(
            [&](File* f)
            {
                acquire(f);
                live.push_back(f);
            });
        for (auto* f : live)
        {
            f->close_file();
            release(f);
        }
    }

    /// Return the scheduler used by files created by this service.
    uring_scheduler& scheduler() noexcept
    {
        return *sched_;
    }

protected:
    uring_scheduler* sched_;
    object_pool<File> pool_;

private:
    uring_file_service_base(uring_file_service_base const&)            = delete;
    uring_file_service_base& operator=(uring_file_service_base const&) = delete;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_URING_URING_FILE_SERVICE_BASE_HPP
