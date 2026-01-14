//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_STREAM_HPP
#define BOOST_COROSIO_IO_STREAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/corosio/io_result.hpp>
#include <boost/corosio/any_bufref.hpp>
#include <boost/capy/ex/any_dispatcher.hpp>
#include <boost/system/error_code.hpp>

#include <coroutine>
#include <cstddef>
#include <stop_token>

namespace boost {
namespace corosio {

class BOOST_COROSIO_DECL io_stream : public io_object
{
public:
    /** Initiate an asynchronous read operation.

        Reads available data into the provided buffer sequence. The
        operation completes when at least one byte has been read, or
        an error occurs.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `errc::operation_canceled`.

        @param buffers The buffer sequence to read data into.

        @return An awaitable that completes with a pair of
            `{error_code, bytes_transferred}`. Returns success with the
            number of bytes read, or an error code on failure including:
            - capy::error::eof: End of stream reached
            - operation_canceled: Cancelled via stop_token or cancel()

        @par Preconditions
        The socket must be open and connected.

        @note This function may return fewer bytes than the buffer
            capacity. Use a loop to read an exact amount.
    */
    template<class MutableBufferSequence>
    auto read_some(MutableBufferSequence const& buffers)
    {
        return read_some_awaitable<MutableBufferSequence>(*this, buffers);
    }

    /** Initiate an asynchronous write operation.

        Writes data from the provided buffer sequence. The operation
        completes when at least one byte has been written, or an
        error occurs.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `errc::operation_canceled`.

        @param buffers The buffer sequence containing data to write.

        @return An awaitable that completes with a pair of
            `{error_code, bytes_transferred}`. Returns success with the
            number of bytes written, or an error code on failure including:
            - broken_pipe: Connection closed by peer
            - operation_canceled: Cancelled via stop_token or cancel()

        @par Preconditions
        The socket must be open and connected.

        @note This function may write fewer bytes than the buffer
            contains. Use a loop to write all data.
    */
    template<class ConstBufferSequence>
    auto write_some(ConstBufferSequence const& buffers)
    {
        return write_some_awaitable<ConstBufferSequence>(*this, buffers);
    }

protected:
    template<class MutableBufferSequence>
    struct read_some_awaitable
    {
        io_stream& ios_;
        MutableBufferSequence buffers_;
        std::stop_token token_;
        mutable system::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        read_some_awaitable(
            io_stream& ios,
            MutableBufferSequence buffers) noexcept
            : ios_(ios)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        io_result<std::size_t> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(system::errc::operation_canceled), 0};
            return {ec_, bytes_transferred_};
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d) -> std::coroutine_handle<>
        {
            any_bufref param(buffers_);
            ios_.get().read_some(h, d, param, token_, &ec_, &bytes_transferred_);
            return std::noop_coroutine();
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            any_bufref param(buffers_);
            ios_.get().read_some(h, d, param, token_, &ec_, &bytes_transferred_);
            return std::noop_coroutine();
        }
    };

    template<class ConstBufferSequence>
    struct write_some_awaitable
    {
        io_stream& ios_;
        ConstBufferSequence buffers_;
        std::stop_token token_;
        mutable system::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        write_some_awaitable(
            io_stream& ios,
            ConstBufferSequence buffers) noexcept
            : ios_(ios)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        io_result<std::size_t> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(system::errc::operation_canceled), 0};
            return {ec_, bytes_transferred_};
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d) -> std::coroutine_handle<>
        {
            any_bufref param(buffers_);
            ios_.get().write_some(h, d, param, token_, &ec_, &bytes_transferred_);
            return std::noop_coroutine();
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            any_bufref param(buffers_);
            ios_.get().write_some(h, d, param, token_, &ec_, &bytes_transferred_);
            return std::noop_coroutine();
        }
    };

public:
    struct io_stream_impl : io_object_impl
    {
        virtual void read_some(
            std::coroutine_handle<>,
            capy::any_dispatcher,
            any_bufref&,
            std::stop_token,
            system::error_code*,
            std::size_t*) = 0;

        virtual void write_some(
            std::coroutine_handle<>,
            capy::any_dispatcher,
            any_bufref&,
            std::stop_token,
            system::error_code*,
            std::size_t*) = 0;
    };

protected:
    explicit
    io_stream(
        capy::execution_context& ctx) noexcept
        : io_object(ctx)
    {
    }

private:
    io_stream_impl& get() const noexcept
    {
        return *static_cast<io_stream_impl*>(impl_);
    }
};

} // namespace corosio
} // namespace boost

#endif
