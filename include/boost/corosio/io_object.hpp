//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_OBJECT_HPP
#define BOOST_COROSIO_IO_OBJECT_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <utility>

namespace boost::corosio {

/** Base class for platform I/O objects.

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

    /** Base interface for platform I/O implementations.

        Derived classes provide platform-specific operation dispatch.
    */
    struct io_object_impl
    {
        virtual ~io_object_impl() = default;

        /// Release associated resources without closing.
        virtual void release() {}
    };

    /** Service interface for I/O object lifecycle management.

        Platform backends implement this interface to manage the
        creation, opening, closing, and destruction of I/O object
        implementations.
    */
    struct io_service
    {
        virtual ~io_service() = default;

        /// Construct a new implementation instance.
        virtual io_object_impl* construct() = 0;

        /// Destroy the implementation, closing kernel resources and freeing memory.
        virtual void destroy(io_object_impl*) = 0;

        /// Open the I/O object, creating the kernel resource.
        virtual void open(handle&) = 0;

        /// Close the I/O object, releasing kernel resources without deallocating.
        virtual void close(handle&) = 0;
    };

    /** RAII wrapper for I/O object implementation lifetime.

        Manages ownership of the platform-specific implementation,
        automatically destroying it when the handle goes out of scope.
    */
    class handle
    {
        capy::execution_context* ctx_ = nullptr;
        io_service* svc_ = nullptr;
        io_object_impl* impl_ = nullptr;

    public:
        /// Destroy the handle and its implementation.
        ~handle()
        {
            if(impl_)
            {
                svc_->close(*this);
                svc_->destroy(impl_);
            }
        }

        /// Construct an empty handle.
        handle() = default;

        /// Construct a handle bound to a context and service.
        handle(
            capy::execution_context& ctx,
            io_service& svc)
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
                ctx_ = std::exchange(other.ctx_, nullptr);
                svc_ = std::exchange(other.svc_, nullptr);
                impl_ = std::exchange(other.impl_, nullptr);
            }
            return *this;
        }

        handle(handle const&) = delete;
        handle& operator=(handle const&) = delete;

        /// Return true if the handle owns an implementation.
        explicit operator bool() const noexcept
        {
            return impl_ != nullptr;
        }

        /// Return the execution context.
        capy::execution_context& context() const noexcept
        {
            return *ctx_;
        }

        /// Return the associated I/O service.
        io_service& service() const noexcept
        {
            return *svc_;
        }

        /// Return the platform implementation.
        io_object_impl* get() const noexcept
        {
            return impl_;
        }

        /** Release ownership of the implementation without destroying.

            The caller is responsible for eventually passing the
            returned pointer to the service's destroy() method.

            @return The implementation pointer, or nullptr if empty.
        */
        io_object_impl* release() noexcept
        {
            return std::exchange(impl_, nullptr);
        }

        /** Replace the implementation, destroying the old one.

            @param p The new implementation to own. May be nullptr.
        */
        void reset(io_object_impl* p) noexcept
        {
            if (impl_)
                svc_->destroy(impl_);
            impl_ = p;
        }
    };

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

    /// Return the execution context.
    capy::execution_context&
    context() const noexcept
    {
        return h_.context();
    }

protected:
    virtual ~io_object() = default;

    /// Construct an I/O object from a handle.
    explicit
    io_object(handle h) noexcept
        : h_(std::move(h))
    {
    }

    /// Move construct from another I/O object.
    io_object(io_object&& other) noexcept
        : h_(std::move(other.h_))
    {
    }

    /// Move assign from another I/O object.
    io_object& operator=(io_object&& other) noexcept
    {
        if (this != &other)
            h_ = std::move(other.h_);
        return *this;
    }

    io_object(io_object const&) = delete;
    io_object& operator=(io_object const&) = delete;

    handle h_;
};

} // namespace boost::corosio

#endif
