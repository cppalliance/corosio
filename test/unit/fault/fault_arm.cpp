//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// The arm model is the same wherever the hooks are: only the way a
// failing call publishes its error code differs, and that is
// publish_error. Keeping this one copy is what stops the POSIX and
// Windows harnesses from drifting apart on rules a test relies on --
// which arm claims a call, when an arm stays reserved, what a fifth
// scope does.

#include "fault.hpp"
#include "fault_slot.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace boost::corosio::test::fault {

thread_local arm_set tls_arms;
std::atomic<slot*> global_slot{nullptr};

[[noreturn]] void die(char const* msg) noexcept
{
    std::fputs(msg, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

namespace {

// Storage behind the process-wide arm; only one scope may own it.
slot global_storage;

// Single place that claims an arm, shared by every constructor.
// Returns the arm index, or -1 for the process-wide slot.
int arm(slot desired, bool global) noexcept
{
    desired.owned = true;
    desired.armed = true;
    if(!global)
    {
        for(int i = 0; i < arm_set::max_arms; ++i)
        {
            if(tls_arms.arms[i].owned)
                continue;
            tls_arms.arms[i] = desired;
            return i;
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg),
            "fault_scope: all %d fault arms are in use on this thread",
            arm_set::max_arms);
        die(msg);
    }
    if(global_slot.load(std::memory_order_acquire))
        die("fault_scope: a process-wide fault is already armed");
    global_storage = desired;
    global_slot.store(&global_storage, std::memory_order_release);
    return -1;
}

} // namespace

slot* armed_arm(sys which) noexcept
{
    for(auto& s : tls_arms.arms)
    {
        if(s.armed && s.which == which)
            return &s;
    }
    return nullptr;
}

namespace {

// Every arm watching `which` counts the call, so two arms on the same
// symbol can claim different occurrences of it. Only one arm may claim
// a given call: the lowest-indexed one whose count just reached its
// nth. A later arm that reaches its nth on that same call has its
// count rolled back instead of being marked fired, so it stays armed
// and claims the next call rather than being spent on a fault that was
// never delivered. Returns null when the call must forward, and reports
// through `matched` whether this thread watches `which` at all, which
// is what keeps the process-wide slot a fallback rather than a second
// chance.
slot* claim_arm(sys which, bool short_mode, bool& matched) noexcept
{
    slot* won = nullptr;
    for(auto& s : tls_arms.arms)
    {
        if(!s.armed || s.which != which)
            continue;
        matched = true;
        if(s.short_mode != short_mode)
            continue;
        if(++s.seen != s.nth)
            continue;
        if(won)
        {
            --s.seen;
            continue;
        }
        s.armed = false;
        s.fired = true;
        won = &s;
    }
    return won;
}

// The process-wide slot, if it is armed for `which` in this mode.
slot* claim_global(sys which, bool short_mode) noexcept
{
    slot* p = global_slot.load(std::memory_order_acquire);
    if(!p)
        return nullptr;
    auto& s = *p;
    if(!s.armed || s.which != which || s.short_mode != short_mode)
        return nullptr;
    if(++s.seen != s.nth)
        return nullptr;
    s.armed = false;
    s.fired = true;
    return &s;
}

} // namespace

bool should_fail(sys which) noexcept
{
    bool matched = false;
    slot* s = claim_arm(which, false, matched);
    if(!s && !matched)
        s = claim_global(which, false);
    if(!s)
        return false;
    publish_error(s->err);
    return true;
}

bool should_shorten(sys which, std::size_t& count) noexcept
{
    bool matched = false;
    slot* s = claim_arm(which, true, matched);
    if(!s && !matched)
        s = claim_global(which, true);
    if(!s)
        return false;
    count = s->count;
    return true;
}

fault_scope::fault_scope(sys which, int err, unsigned nth)
{
    slot s;
    s.which = which;
    s.err = err;
    s.nth = nth;
    idx_ = arm(s, false);
}

fault_scope::fault_scope(sys which, int err, unsigned nth, any_thread_t)
    : global_(true)
{
    slot s;
    s.which = which;
    s.err = err;
    s.nth = nth;
    idx_ = arm(s, true);
}

fault_scope::fault_scope(short_tag, sys which, std::size_t count,
    unsigned nth, bool global)
    : global_(global)
{
    slot s;
    s.which = which;
    s.count = count;
    s.short_mode = true;
    s.nth = nth;
    idx_ = arm(s, global);
}

fault_scope fault_scope::returning(sys which, std::size_t count, unsigned nth)
{
    return fault_scope(short_tag{}, which, count, nth, false);
}

fault_scope fault_scope::returning_any_thread(sys which, std::size_t count,
    unsigned nth)
{
    return fault_scope(short_tag{}, which, count, nth, true);
}

fault_scope::~fault_scope()
{
    if(global_)
    {
        global_slot.store(nullptr, std::memory_order_release);
        global_storage.armed = false;
        global_storage.owned = false;
        return;
    }
    tls_arms.arms[idx_].armed = false;
    tls_arms.arms[idx_].owned = false;
}

bool fault_scope::fired() const noexcept
{
    return global_ ? global_storage.fired : tls_arms.arms[idx_].fired;
}

unsigned fault_scope::count() const noexcept
{
    return global_ ? global_storage.seen : tls_arms.arms[idx_].seen;
}

} // boost::corosio::test::fault
