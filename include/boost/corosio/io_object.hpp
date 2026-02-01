//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_OBJECT_HPP
#define BOOST_COROSIO_IO_OBJECT_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>

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

    @note Intended as a protected base class. The implementation
        pointer `impl_` is accessible to derived classes.

    @see io_stream, tcp_socket, tcp_acceptor
*/
class BOOST_COROSIO_DECL io_object
{
public:
    /// Forward declaration for platform-specific implementation.
    struct implementation;

    class handle;

    /** Service interface for I/O object lifecycle management.

        Platform backends implement this interface to manage the
        creation, opening, closing, and destruction of I/O object
        implementations.
    */
    struct io_service
    {
        /// Open the I/O object for use.
        virtual void open(handle&) = 0;

        /// Close the I/O object, releasing kernel resources.
        virtual void close(handle&) = 0;

        /// Destroy the implementation, freeing memory.
        virtual void destroy(implementation*) = 0;

        /// Construct a new implementation instance.
        virtual implementation* construct() = 0;
    };

    /** RAII wrapper for I/O object implementation lifetime.

        Manages ownership of the platform-specific implementation,
        automatically destroying it when the handle goes out of scope.
    */
    class handle
    {
        capy::execution_context* ctx_ = nullptr;
        io_service* svc_ = nullptr;
        implementation* impl_ = nullptr;

    public:
        /// Destroy the handle and its implementation.
        ~handle()
        {
            if(impl_)
                svc_->destroy(impl_);
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
        handle(handle&& other)
            : ctx_(std::exchange(other.ctx_, nullptr))
            , svc_(std::exchange(other.svc_, nullptr))
            , impl_(std::exchange(other.impl_, nullptr))
        {
        }

        /// Move assign from another handle.
        handle& operator=(handle&& other) noexcept
        {
            ctx_ = std::exchange(other.ctx_, nullptr);
            svc_ = std::exchange(other.svc_, nullptr);
            impl_ = std::exchange(other.impl_, nullptr);
            return *this;
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
        implementation& get() const noexcept
        {
            return *impl_;
        }
    };

    /** Base interface for platform I/O implementations.

        Derived classes provide platform-specific operation dispatch.
    */
    struct io_object_impl
    {
        virtual ~io_object_impl() = default;

        /// Release associated resources without closing.
        virtual void release() = 0;
    };

    /// Return the execution context.
    capy::execution_context&
    context() const noexcept
    {
        return *ctx_;
    }

protected:
    virtual ~io_object() = default;

    /// Construct an I/O object bound to the given context.
    explicit
    io_object(
        capy::execution_context& ctx) noexcept
        : ctx_(&ctx)
    {
    }

    capy::execution_context* ctx_ = nullptr;
    io_object_impl* impl_ = nullptr;
};

} // namespace boost::corosio

#endif
