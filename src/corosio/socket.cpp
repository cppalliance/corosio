//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/socket.hpp>

#include <atomic>
#include <optional>
#include <stop_token>

namespace boost {
namespace corosio {

struct socket::ops_state final
{
    struct read_op
        : capy::executor_work
    {
        // Small invocable for stop_callback - avoids std::function overhead
        struct canceller
        {
            read_op* op;
            void operator()() const { op->cancel(); }
        };

        capy::coro h;
        capy::any_dispatcher d;
        std::atomic<bool> cancelled{false};
        std::error_code* ec_out = nullptr;
        std::optional<std::stop_callback<canceller>> stop_cb;

        void operator()() override
        {
            // Clear the stop callback before resuming
            stop_cb.reset();

            // Set error code if cancelled
            if (ec_out && cancelled.load(std::memory_order_acquire))
                *ec_out = std::make_error_code(std::errc::operation_canceled);

            d(h).resume();
        }

        void destroy() override
        {
            stop_cb.reset();
            // do not delete; owned by socket
        }

        void cancel()
        {
            cancelled.store(true, std::memory_order_release);
        }

        void start(std::stop_token token)
        {
            cancelled.store(false, std::memory_order_release);
            stop_cb.reset();

            if (token.stop_possible())
                stop_cb.emplace(token, canceller{this});
        }
    };

    static void deleter(ops_state* p)
    {
        delete p;
    }

    read_op rd;
};

socket::
socket(
    capy::service_provider& sp)
    : reactor_(sp.find_service<platform_reactor>())
    , ops_(new ops_state, ops_state::deleter)
{
    assert(reactor_ != nullptr);
}

void
socket::
cancel() const
{
    ops_->rd.cancel();
}

void
socket::
do_read_some(
    capy::coro h,
    capy::any_dispatcher d,
    std::stop_token token,
    std::error_code* ec)
{
    ++g_io_count;
    ops_->rd.h = h;
    ops_->rd.d = d;
    ops_->rd.ec_out = ec;
    ops_->rd.start(token);
    reactor_->submit(&ops_->rd);
}

} // namespace corosio
} // namespace boost
