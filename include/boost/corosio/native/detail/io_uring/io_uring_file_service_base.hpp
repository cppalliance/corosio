//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_FILE_SERVICE_BASE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_FILE_SERVICE_BASE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

/*
    Shared lifecycle plumbing for io_uring file services.

    io_uring_stream_file_service and io_uring_random_access_file_service were
    byte-for-byte identical apart from the impl type and open_file's parameter
    type: both make_shared the file impl from the scheduler, track it in an
    intrusive list + raw->shared_ptr map, and close every file on shutdown.
    This base factors that out; the concrete services add only open_file.

    This is a separate base from io_uring_socket_service_base because file
    services differ from socket services in three ways that match the reactor
    socket service instead: they track via an intrusive list + map (sockets:
    map only), they CLOSE files on shutdown (sockets: cancel only), and the
    impl ctor takes just the scheduler (sockets: service + scheduler). See
    tasks/proactor-dedup-decisions.md (#14).

    Requirements on File: derive from enable_shared_from_this<File> and
    intrusive_list<File>::node, a `File(io_uring_scheduler&)` constructor, and
    a `void close_file() noexcept` method (cancel in-flight ops + close fd).

    @tparam Derived     The concrete service (CRTP; unused today but kept for
                        symmetry / future hooks).
    @tparam ServiceBase The abstract service vtable base (file_service,
                        random_access_file_service).
    @tparam File        The concrete io_uring file impl type.
*/

namespace boost::corosio::detail {

template<class Derived, class ServiceBase, class File>
class io_uring_file_service_base : public ServiceBase
{
    friend Derived;

    // Private CRTP ctor: only `Derived` (the concrete service, a friend)
    // constructs the base — prevents inheriting with the wrong Derived
    // (bugprone-crtp-constructor-accessibility).
    explicit io_uring_file_service_base(io_uring_scheduler& sched) noexcept
        : sched_(&sched)
    {
    }

public:
    ~io_uring_file_service_base() override = default;

    io_object::implementation* construct() override
    {
        auto  ptr  = std::make_shared<File>(*sched_);
        auto* impl = ptr.get();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            file_list_.push_back(impl);
            file_ptrs_[impl] = std::move(ptr);
        }
        return impl;
    }

    void destroy(io_object::implementation* p) override
    {
        // close_file() already does cancel_and_flush(fd_) before ::close.
        auto& impl = static_cast<File&>(*p);
        impl.close_file();
        std::lock_guard<std::mutex> lock(mutex_);
        file_list_.remove(&impl);
        file_ptrs_.erase(&impl);
    }

    void close(io_object::handle& h) override
    {
        if (h.get())
            static_cast<File&>(*h.get()).close_file();
    }

    void shutdown() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* impl = file_list_.pop_front(); impl != nullptr;
             impl       = file_list_.pop_front())
        {
            impl->close_file();
        }
        file_ptrs_.clear();
    }

    /// Return the scheduler used by files created by this service.
    io_uring_scheduler& scheduler() noexcept { return *sched_; }

protected:
    io_uring_scheduler*  sched_;
    std::mutex           mutex_;
    intrusive_list<File> file_list_;
    std::unordered_map<File*, std::shared_ptr<File>> file_ptrs_;

private:
    io_uring_file_service_base(io_uring_file_service_base const&) = delete;
    io_uring_file_service_base&
    operator=(io_uring_file_service_base const&) = delete;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_FILE_SERVICE_BASE_HPP
