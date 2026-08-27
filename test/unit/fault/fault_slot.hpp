//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_FAULT_SLOT_HPP
#define BOOST_COROSIO_TEST_FAULT_SLOT_HPP

#include "fault.hpp"

#include <atomic>
#include <cstddef>

namespace boost::corosio::test::fault {

// Per-thread arm state. Plain aggregate so the hooks touch nothing
// that could allocate or lock.
struct slot
{
    sys which = sys::count_;
    int err = 0;
    std::size_t count = 0;
    unsigned nth = 1;
    unsigned seen = 0;
    bool short_mode = false;
    bool fired = false;
    bool armed = false;
    // Tracks the owning fault_scope's lifetime, independent of `armed`:
    // should_fail/should_shorten clear `armed` the moment the fault
    // fires, but the scope object is still alive and keeps its arm
    // reserved until destroyed.
    bool owned = false;
};

// Four independent arms per thread. A deferred-path test parks an
// operation with one fault and fails its reactor retry with another,
// and every armed arm counts calls on its own, so two arms may target
// different occurrences of the same symbol.
struct arm_set
{
    static constexpr int max_arms = 4;
    slot arms[max_arms];
};

extern thread_local arm_set tls_arms;

// Return the first arm still armed for `which` on this thread, or null.
slot* armed_arm(sys which) noexcept;

// Process-wide fallback, consulted only when the calling thread has no
// arm watching the symbol being called; an arm watching some other
// symbol does not shadow it. Work the library hands to its thread pool
// (file I/O, name resolution) cannot see the test thread's arms, so a
// scope armed with `any_thread` publishes itself here instead.
extern std::atomic<slot*> global_slot;

// Publish `err` where a caller of the failing entry point will read it:
// errno on POSIX, the last-error slots on Windows. The one part of the
// arm model that has to know which harness it was linked into, which
// is why fault_arm.cpp can be shared verbatim.
void publish_error(int err) noexcept;

// Return true when the current call must fail; the error is already
// published.
bool should_fail(sys which) noexcept;

// Return true when the current call must be shortened to `count`.
bool should_shorten(sys which, std::size_t& count) noexcept;

// Print `msg` and abort. Defined in fault_arm.cpp so every hook
// translation unit enforces contract violations the same way.
[[noreturn]] void die(char const* msg) noexcept;

#if !defined(_WIN32)
// Resolve the real entry point behind a shadow. A symbol that resolves
// to null would otherwise only surface as a crash inside the shadow,
// with nothing to say which one; name it instead. The Windows hooks
// keep the real entry point from the import thunk they overwrote and
// never look a symbol up by name.
void* real_symbol(char const* name) noexcept;
#endif

// Second slot for CQE rewriting; independent of `slot` so a test can
// pair an SQ-full fault with a completion rewrite.
struct cqe_slot
{
    // -1 matches any fd: the multishot polls the harness has to reach
    // are armed on descriptors the library never hands out.
    int fd = -1;
    int opcode = -1;
    int res = 0;
    unsigned flags_clear = 0;
    unsigned long long user_data = 0;
    bool have_user_data = false;
    bool fired = false;
    bool armed = false;
    // Tracks the owning scope's lifetime, independent of `armed`: the
    // rewrite clears `armed` the moment it fires, so nesting has to be
    // refused on this instead or a second scope would quietly take
    // over the slot a live one still reads `fired()` from.
    bool owned = false;
};

extern thread_local cqe_slot tls_cqe;

#if defined(_WIN32)
// The IOCP twin of cqe_slot: a completion carries no fd or opcode to
// match on, only its ordinal among the dequeues that produced an
// OVERLAPPED, so the two cannot share one aggregate.
struct completion_slot
{
    unsigned long err = 0;
    unsigned nth = 1;
    unsigned seen = 0;
    bool fired = false;
    bool armed = false;
    // See cqe_slot::owned; the rule is the same.
    bool owned = false;
};

extern thread_local completion_slot tls_completion;

// Return true when the current dequeue must be turned into a failure,
// reporting the armed error through `err`.
bool completion_should_fail(unsigned long& err) noexcept;
#endif

// Claim the one completion-side slot of its kind for a scope, or die
// naming `who`. Shared by cqe_fault_scope and completion_fault_scope,
// whose slots hold different things but reserve them by the same rule.
template<class Slot>
void claim_completion_slot(Slot& s, char const* who) noexcept
{
    if(s.owned)
        die(who);
    s = Slot{};
    s.armed = true;
    s.owned = true;
}

// Release the slot claimed by claim_completion_slot.
template<class Slot>
void release_completion_slot(Slot& s) noexcept
{
    s.armed = false;
    s.owned = false;
}

} // boost::corosio::test::fault

#endif
