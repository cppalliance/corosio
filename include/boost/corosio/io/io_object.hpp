//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_IO_OBJECT_HPP
#define BOOST_COROSIO_IO_IO_OBJECT_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <atomic>
#include <cstddef>
#include <utility>

namespace boost::corosio {

/** Owns the platform-specific handle and execution context that a derived
    socket, timer, signal handler, or acceptor type uses to dispatch
    operations.

    Provides common infrastructure for I/O objects that wrap kernel
    resources (sockets, timers, signal handlers, acceptors). Derived
    classes dispatch operations through a platform-specific vtable
    (IOCP, epoll, kqueue, io_uring).

    @par Semantics
    Only concrete platform I/O types should inherit from `io_object`.
    Test mocks, decorators, and stream adapters must not inherit from
    this class. Use concepts or templates for generic I/O algorithms.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe. All operations on a single I/O object
    must be serialized.

    @note Intended as a protected base class. The handle member
        `h_` is accessible to derived classes.

    @see io_stream, tcp_socket, tcp_acceptor
*/
class BOOST_COROSIO_DECL io_object
{
public:
    class handle;

    /** Derived types dispatch platform-specific I/O operations through it.

        Reference-counted: the owning service holds one reference
        (`refs_` starts at 1); in-flight operations hold additional
        references through `detail::object_ref`. When the count reaches
        zero, `retire()` runs — services recycle the impl into
        their free-list there.
    */
    struct implementation
    {
        /// Destroy the implementation; called only through @ref io_service.
        virtual ~implementation() = default;

        /** Called when the reference count reaches zero.

            @note Transitional default is a no-op while backends are
            migrated; it becomes pure virtual at the end of the
            migration.
        */
        virtual void retire() noexcept {}

        /// In-flight + service references; starts at the service's 1.
        std::atomic<std::size_t> refs_{1};
    };

    /** Constructs, closes, and destroys platform implementations on
        behalf of an I/O object. Platform backends implement this
        interface.
    */
    struct BOOST_COROSIO_DECL io_service
    {
        /// Destroy the service; the execution context outlives it.
        virtual ~io_service() = default;

        /** Construct a new implementation instance.

            May return a recycled implementation taken from the
            service's free-list (populated by `implementation::retire`)
            instead of allocating new storage.
        */
        virtual implementation* construct() = 0;

        /** Close kernel resources and release the service's reference.

            Called whenever a handle relinquishes its current
            implementation, not only on destruction: handle
            destruction, move-assignment onto a handle that already
            owns one (the replaced implementation is destroyed, the
            incoming one is not), and `handle::reset()` all invoke
            this. Closes the underlying descriptor and drops the
            service's own reference; in-flight operations may still
            hold references of their own, so the implementation is
            not necessarily recycled or freed here — that happens in
            `implementation::retire` whenever the refcount
            actually reaches zero.
        */
        virtual void destroy(implementation* impl) = 0;

        /// Close the I/O object, releasing kernel resources without deallocating.
        virtual void close([[maybe_unused]] handle& h) {}
    };

    /** Owns a platform-specific I/O implementation and destroys it
        when the handle goes out of scope.
    */
    class handle
    {
        capy::execution_context* ctx_ = nullptr;
        io_service* svc_              = nullptr;
        implementation* impl_         = nullptr;

    public:
        /// Destroy the handle and its implementation.
        ~handle()
        {
            if (impl_)
            {
                svc_->close(*this);
                svc_->destroy(impl_);
            }
        }

        /// Construct an empty handle.
        handle() = default;

        /// Construct a handle bound to a context and service.
        handle(capy::execution_context& ctx, io_service& svc)
            : ctx_(&ctx)
            , svc_(&svc)
            , impl_(svc_->construct())
        {
        }

        /// Move construct from another handle.
        handle(handle&& other) noexcept
            : ctx_(std::exchange(other.ctx_, nullptr))
            , svc_(std::exchange(other.svc_, nullptr))
            , impl_(std::exchange(other.impl_, nullptr))
        {
        }

        /// Move assign from another handle.
        handle& operator=(handle&& other) noexcept
        {
            if (this != &other)
            {
                if (impl_)
                {
                    svc_->close(*this);
                    svc_->destroy(impl_);
                }
                ctx_  = std::exchange(other.ctx_, nullptr);
                svc_  = std::exchange(other.svc_, nullptr);
                impl_ = std::exchange(other.impl_, nullptr);
            }
            return *this;
        }

        /// Copy construction is disabled; the implementation is uniquely owned.
        handle(handle const&) = delete;
        /// Copy assignment is disabled; the implementation is uniquely owned.
        handle& operator=(handle const&) = delete;

        /// Return true if the handle owns an implementation.
        explicit operator bool() const noexcept
        {
            return impl_ != nullptr;
        }

        /// Return the associated I/O service.
        io_service& service() const noexcept
        {
            return *svc_;
        }

        /// Return the platform implementation.
        implementation* get() const noexcept
        {
            return impl_;
        }

        /** Replace the implementation, destroying the old one.

            @pre The handle is already bound to a service (constructed
                via the service-taking constructor, not the default
                one) — the old implementation is destroyed through
                that service.
            @pre @p p, if non-null, was constructed by this handle's
                own service: the service that destroys it later must
                be the one that knows how to close it.

            @param p The new implementation to own. May be nullptr.
        */
        void reset(implementation* p) noexcept
        {
            if (impl_)
            {
                svc_->close(*this);
                svc_->destroy(impl_);
            }
            impl_ = p;
        }

        /// Return the execution context.
        capy::execution_context& context() const noexcept
        {
            return *ctx_;
        }
    };

    /// Return the execution context.
    capy::execution_context& context() const noexcept
    {
        return h_.context();
    }

protected:
    /// Destroy the object; protected, so only a derived type destroys one.
    virtual ~io_object() = default;

    /// Default construct for virtual base initialization.
    io_object() noexcept = default;

    /** Create a handle bound to a service found in the context.

        @tparam Service The service type whose key_type is used for lookup.
        @param ctx The execution context to search for the service.

        @return A handle owning a freshly constructed implementation.

        @throws std::logic_error if the service is not installed.
    */
    template<class Service>
    static handle create_handle(capy::execution_context& ctx)
    {
        auto* svc = ctx.find_service<Service>();
        if (!svc)
            detail::throw_logic_error(
                "io_object::create_handle: service not installed");
        return handle(ctx, *svc);
    }

    /// Construct an I/O object from a handle.
    explicit io_object(handle h) noexcept : h_(std::move(h)) {}

    /// Move construct from another I/O object.
    io_object(io_object&& other) noexcept : h_(std::move(other.h_)) {}

    /// Move assign from another I/O object.
    io_object& operator=(io_object&& other) noexcept
    {
        if (this != &other)
            h_ = std::move(other.h_);
        return *this;
    }

    /// Copy construction is disabled; the handle is uniquely owned.
    io_object(io_object const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    io_object& operator=(io_object const&) = delete;

    /// The platform I/O handle owned by this object.
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251)
    handle h_;
    BOOST_COROSIO_MSVC_WARNING_POP
};

} // namespace boost::corosio

#endif
