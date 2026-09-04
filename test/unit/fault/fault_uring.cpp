//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "fault_slot.hpp"

#include <cerrno>
#include <dlfcn.h>
#include <liburing.h>
#include <tuple>

// liburing 2.6 and later declare their API noexcept in C++; older
// headers leave it unspecified, and a shadow that adds a specification
// the header does not have is ill-formed.
#if !defined(LIBURING_NOEXCEPT)
#define LIBURING_NOEXCEPT
#endif

using namespace boost::corosio::test::fault;

namespace {

// An archive-linked liburing leaves no body behind the shadow: the
// executable's own definition already satisfies the reference, so the
// archive member is never pulled in and RTLD_NEXT has nothing left to
// find. The distro's shared object supplies one. Every ring call in
// the process goes through these shadows, so the ring is still driven
// by a single implementation.
void* uring_real_symbol(char const* name) noexcept
{
    if(void* p = ::dlsym(RTLD_NEXT, name))
        return p;
    static void* const lib = ::dlopen("liburing.so.2", RTLD_NOW | RTLD_LOCAL);
    if(lib)
    {
        if(void* p = ::dlsym(lib, name))
            return p;
    }
    return real_symbol(name);
}

#define COROSIO_FAULT_REAL(name, sig) \
    static auto const real = reinterpret_cast<sig>(uring_real_symbol(#name))

// Fail with a negative errno, liburing style.
bool uring_fail(sys which, int& rc) noexcept
{
    if(!should_fail(which))
        return false;
    rc = -errno;
    return true;
}

slot* sqe_full_armed() noexcept
{
    return armed_arm(sys::uring_sqe_full);
}

// On-demand SQ exhaustion: while an arm is live the ring's user-side
// tail is pushed to capacity so io_uring_get_sqe returns null, and
// io_uring_submit is a no-op so the fake tail never reaches the
// kernel. The first liburing call after the scope dies restores the
// saved tail; a ring torn down while filled is simply forgotten.
struct sq_fill_state
{
    io_uring* ring = nullptr;
    unsigned saved_tail = 0;
    bool filled = false;
};

thread_local sq_fill_state tls_sq_fill;

void apply_sq_fill(io_uring* ring) noexcept
{
    auto& st = tls_sq_fill;
    if(auto* s = armed_arm(sys::uring_sq_fill))
    {
        if(!st.filled)
        {
            st.ring = ring;
            st.saved_tail = ring->sq.sqe_tail;
            ring->sq.sqe_tail += io_uring_sq_space_left(ring);
            st.filled = true;
            s->fired = true;
        }
        return;
    }
    if(st.filled && st.ring == ring)
    {
        ring->sq.sqe_tail = st.saved_tail;
        st = sq_fill_state{};
    }
}

bool sq_filled(io_uring* ring) noexcept
{
    return tls_sq_fill.filled && tls_sq_fill.ring == ring &&
        armed_arm(sys::uring_sq_fill) != nullptr;
}

// Record the user_data of the first pending SQE matching the armed
// fd/opcode. The SQ array is user memory, so this is a plain read.
void scan_pending_sqes(io_uring* ring) noexcept
{
    auto& c = tls_cqe;
    if(!c.armed || c.have_user_data)
        return;
    for(unsigned i = ring->sq.sqe_head; i != ring->sq.sqe_tail; ++i)
    {
        auto const& sqe = ring->sq.sqes[i & ring->sq.ring_mask];
        if(int(sqe.opcode) == c.opcode && (c.fd < 0 || sqe.fd == c.fd))
        {
            c.user_data = sqe.user_data;
            c.have_user_data = true;
            return;
        }
    }
}

// Overwrite `res` on the visible CQE carrying the recorded user_data.
void rewrite_visible_cqes(io_uring* ring) noexcept
{
    auto& c = tls_cqe;
    if(!c.armed || !c.have_user_data)
        return;
    unsigned head;
    io_uring_cqe* cqe;
    io_uring_for_each_cqe(ring, head, cqe)
    {
        if(cqe->user_data == c.user_data)
        {
            cqe->res = c.res;
            cqe->flags &= ~c.flags_clear;
            c.fired = true;
            c.armed = false;
            return;
        }
    }
}

} // namespace

extern "C" int io_uring_queue_init_params(unsigned entries, io_uring* ring,
    io_uring_params* p) LIBURING_NOEXCEPT
{
    COROSIO_FAULT_REAL(io_uring_queue_init_params, int(*)(unsigned, io_uring*, io_uring_params*));
    int rc;
    if(uring_fail(sys::io_uring_queue_init_params, rc))
        return rc;
    if(sqe_full_armed())
        entries = 1;
    int const r = real(entries, ring, p);
    if(r == 0)
        apply_sq_fill(ring);
    return r;
}

extern "C" void io_uring_queue_exit(io_uring* ring) LIBURING_NOEXCEPT
{
    COROSIO_FAULT_REAL(io_uring_queue_exit, void(*)(io_uring*));
    // Cannot fail; counted so a test can assert teardown reached it.
    std::ignore = should_fail(sys::io_uring_queue_exit);
    if(tls_sq_fill.filled && tls_sq_fill.ring == ring)
        tls_sq_fill = sq_fill_state{};
    real(ring);
}

extern "C" int io_uring_submit(io_uring* ring) LIBURING_NOEXCEPT
{
    COROSIO_FAULT_REAL(io_uring_submit, int(*)(io_uring*));
    int rc;
    if(uring_fail(sys::io_uring_submit, rc))
        return rc;
    if(auto* s = sqe_full_armed())
    {
        s->fired = true;
        return 0;
    }
    scan_pending_sqes(ring);
    apply_sq_fill(ring);
    if(sq_filled(ring))
    {
        // The queue reads full, but completions already visible must
        // still be rewritten or a composed arm misses its CQE.
        rewrite_visible_cqes(ring);
        return 0;
    }
    rc = real(ring);
    // A buffered write can complete inside this io_uring_enter, so the
    // CQE the arm is waiting for may already be visible when it
    // returns. Rewriting here as well as in the waiting entry points
    // is what keeps the arm from depending on which call the kernel
    // chose to finish the op in.
    rewrite_visible_cqes(ring);
    return rc;
}

extern "C" int io_uring_submit_and_wait_timeout(io_uring* ring, io_uring_cqe** cqe,
    unsigned wait_nr, __kernel_timespec* ts, sigset_t* sigmask) LIBURING_NOEXCEPT
{
    COROSIO_FAULT_REAL(io_uring_submit_and_wait_timeout,
        int(*)(io_uring*, io_uring_cqe**, unsigned, __kernel_timespec*, sigset_t*));
    int rc;
    if(uring_fail(sys::io_uring_submit_and_wait_timeout, rc))
        return rc;
    scan_pending_sqes(ring);
    apply_sq_fill(ring);
    if(sq_filled(ring))
    {
        rewrite_visible_cqes(ring);
        return 0;
    }
    rc = real(ring, cqe, wait_nr, ts, sigmask);
    rewrite_visible_cqes(ring);
    return rc;
}

extern "C" int io_uring_submit_and_get_events(io_uring* ring) LIBURING_NOEXCEPT
{
    COROSIO_FAULT_REAL(io_uring_submit_and_get_events, int(*)(io_uring*));
    int rc;
    if(uring_fail(sys::io_uring_submit_and_get_events, rc))
        return rc;
    scan_pending_sqes(ring);
    apply_sq_fill(ring);
    if(sq_filled(ring))
    {
        // The queue reads full, but completions already visible must
        // still be rewritten or a composed arm misses its CQE.
        rewrite_visible_cqes(ring);
        return 0;
    }
    rc = real(ring);
    rewrite_visible_cqes(ring);
    return rc;
}

extern "C" int io_uring_wait_cqe_timeout(io_uring* ring, io_uring_cqe** cqe,
    __kernel_timespec* ts) LIBURING_NOEXCEPT
{
    COROSIO_FAULT_REAL(io_uring_wait_cqe_timeout, int(*)(io_uring*, io_uring_cqe**, __kernel_timespec*));
    int rc;
    if(uring_fail(sys::io_uring_wait_cqe_timeout, rc))
        return rc;
    apply_sq_fill(ring);
    rc = real(ring, cqe, ts);
    rewrite_visible_cqes(ring);
    return rc;
}
