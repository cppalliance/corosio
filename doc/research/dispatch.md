# The Dispatch Problem: Symmetric Transfer, Stack Overflow, and Async Mutex Correctness

## Executive Summary

Corosio's `executor_type::dispatch()` returns a `std::coroutine_handle<>`, enabling symmetric transfer from I/O completion paths. This design causes:

1. **Stack overflow** (`STATUS_STACK_BUFFER_OVERRUN` on Windows) when I/O operations complete synchronously in tight loops
2. **Async mutex correctness failures** where coroutine chains holding mutexes break due to improper stack unwinding
3. **Non-returning dispatch calls** when symmetric transfer chains don't terminate properly

The solution is to change `dispatch(coro)` to return `void` and call `h.resume()` as a normal function call when running in the same thread. This aligns with Boost.Asio's proven approach while preserving symmetric transfer for coroutine composition (task-to-task transfers via `final_suspend`).

---

## Table of Contents

1. [Background: Coroutine Resumption Models](#background-coroutine-resumption-models)
2. [How Asio Handles Coroutine Resumption](#how-asio-handles-coroutine-resumption)
3. [How Corosio Gets It Wrong](#how-corosio-gets-it-wrong)
4. [The Stack Overflow Problem](#the-stack-overflow-problem)
5. [The Async Mutex Problem](#the-async-mutex-problem)
6. [The Solution](#the-solution)
7. [Why We Don't Need Asio's Pump](#why-we-dont-need-asios-pump)
8. [Implementation Changes Required](#implementation-changes-required)
9. [Verification Criteria](#verification-criteria)

---

## Background: Coroutine Resumption Models

### The Coroutine Pump

The "pump" is the event loop that drives coroutine execution:

```cpp
// Simplified io_context::run()
while (has_work()) {
    auto completion = dequeue_completion();  // Wait on IOCP/epoll
    completion.handler();                     // Resume suspended coroutine
}
```

When an I/O operation completes, the suspended coroutine must be resumed. The question is *how* that resumption happens.

### Symmetric Transfer

C++20 coroutines support **symmetric transfer**: when `await_suspend` returns a `coroutine_handle`, the compiler generates a tail call to that handle's `resume()`. This avoids stack growth:

```cpp
auto await_suspend(std::coroutine_handle<> h) {
    return other_handle;  // Tail call to other_handle.resume()
}
```

The key property: a tail call **replaces** the current stack frame rather than pushing a new one.

### Normal Function Calls

A normal function call pushes a new stack frame:

```cpp
void dispatch(coro h) {
    h.resume();  // Normal call - pushes frame, will return here
}
```

The call will return when the coroutine suspends (returns `noop_coroutine` from its next `await_suspend`).

---

## How Asio Handles Coroutine Resumption

### Asio's Architecture

Asio uses a **completion token** model where asynchronous operations accept a token that determines how completions are delivered. For coroutines, `use_awaitable` transforms operations into awaitables.

### The `awaitable_thread` and Explicit Frame Stack

Asio maintains an **explicit stack of coroutine frames** in `awaitable_thread`:

```cpp
// From boost/asio/impl/awaitable.hpp
class awaitable_thread {
    awaitable_frame_base* top_of_stack_;
    // ...
    
    void pump() {
        do
            bottom_of_stack_.frame_->top_of_stack_->resume();
        while (bottom_of_stack_.frame_ && bottom_of_stack_.frame_->top_of_stack_);
    }
};
```

Key observations:

1. **`pump()` calls `resume()` as a normal function call** - not symmetric transfer
2. **The loop continues** until the stack is empty or coroutine suspends for I/O
3. **`final_suspend` doesn't transfer** - it just pops the frame and returns

### Asio's `final_suspend`

```cpp
// Asio's awaitable_frame final_suspend
auto await_suspend(coroutine_handle<>) noexcept {
    this->this_->pop_frame();  // Adjust stack pointers
    return noop_coroutine();   // Don't transfer anywhere
}
```

When a child coroutine completes:
1. `final_suspend` pops itself from the explicit stack
2. Returns `noop_coroutine()` (suspend, don't transfer)
3. `resume()` returns to the pump loop
4. Pump loop sees parent is now on top, calls `resume()` on parent

### Why This Works

Asio **never uses symmetric transfer** for I/O completions or coroutine composition. Everything goes through the pump loop as normal function calls. This guarantees:

- Stack always unwinds properly
- No unbounded stack growth
- Nested dispatch calls return correctly
- Async mutex operations work correctly

---

## How Corosio Gets It Wrong

### Corosio's Current `dispatch` Signature

```cpp
// basic_io_context.hpp, executor_type::dispatch
capy::coro dispatch(capy::coro h) const {
    if (running_in_this_thread())
        return h;  // Return handle for symmetric transfer
    ctx_->sched_->post(h);
    return std::noop_coroutine();
}
```

This returns `h` when running in the same thread, enabling the caller to use it for symmetric transfer.

### Usage in I/O Completion Paths

When an I/O operation completes immediately, the completion handler does:

```cpp
// overlapped_op.hpp
std::coroutine_handle<> complete_immediate() {
    // ... setup ...
    return d.dispatch(h);  // Returns h for symmetric transfer
}
```

Or in `await_suspend`:

```cpp
auto await_suspend(std::coroutine_handle<> h) {
    initiate_io(...);
    if (immediate_completion)
        return dispatch(h);  // Symmetric transfer back to h
    return std::noop_coroutine();
}
```

### The Fundamental Problem

When `dispatch` returns `h` and the caller uses it for symmetric transfer, the compiler generates:

```cpp
// What the compiler generates for await_suspend returning h:
goto h.resume();  // Tail call - doesn't push frame, doesn't return
```

This creates several problems detailed below.

---

## The Stack Overflow Problem

### Scenario: Tight Loop with Immediate Completions

```cpp
task<> client(tcp_socket& socket) {
    for (int i = 0; i < 1000000; i++) {
        co_await socket.async_read(...);  // Completes immediately
    }
}
```

### What Happens (Current Implementation)

1. Coroutine does `co_await async_read()`
2. `await_suspend` initiates I/O, completes immediately
3. `await_suspend` returns `dispatch(h)` which returns `h`
4. Compiler generates tail call to `h.resume()`
5. **But if the compiler doesn't generate a proper tail call...**

If the compiler generates a normal call instead of a tail call:

```
coroutine frame
  -> await_suspend returns h
  -> h.resume()  // NOT a tail call - pushes frame!
     -> coroutine continues to next iteration
        -> await_suspend returns h
        -> h.resume()  // Another frame pushed!
           -> next iteration
              -> h.resume()  // Stack grows unboundedly
                 ... STATUS_STACK_BUFFER_OVERRUN
```

### Why Tail Calls Fail

Symmetric transfer requires the compiler to generate an actual tail call. This can fail due to:

1. **Compiler limitations** - older compilers may not optimize correctly
2. **Debug builds** - optimizations disabled
3. **ABI constraints** - calling conventions may prevent tail calls
4. **Inlining decisions** - complex call chains may prevent optimization

### Observed Symptom

On Windows: `STATUS_STACK_BUFFER_OVERRUN` - the /GS security check detects stack corruption when the stack grows into the guard page or overwrites the security cookie.

---

## The Async Mutex Problem

### Scenario: Coroutine Holds Mutex During I/O

```cpp
task<> worker(async_mutex& mutex, tcp_socket& socket) {
    auto lock = co_await mutex.lock();
    co_await socket.async_write(data);  // Completes immediately
    // lock released here
}
```

### What Should Happen

1. Worker A holds mutex
2. Worker B waiting for mutex
3. A's write completes immediately
4. A continues, releases mutex
5. Mutex wakes B
6. A continues to completion
7. B runs

### What Actually Happens (Current Implementation)

1. Worker A holds mutex
2. A's write completes immediately
3. Completion path does `dispatch(A)` returning A's handle
4. Symmetric transfer to A (tail call - no return!)
5. A continues, releases mutex
6. Mutex calls `dispatch(B)` to wake B
7. **dispatch returns B's handle for symmetric transfer**
8. Tail call to B.resume() - **A's stack frame is gone**
9. B runs, but A never gets to continue!

### The Core Issue

When `dispatch(B)` returns B's handle and the caller does symmetric transfer:

```cpp
void async_mutex::unlock() {
    auto next_waiter = waiters_.pop();
    dispatch(next_waiter).resume();  // If dispatch returns handle...
    // This line never executes if .resume() is a tail call!
}
```

The symmetric transfer replaces the current frame. The code after the transfer never runs.

### Current Workaround in Corosio

The codebase has explicit comments about this:

```cpp
// sockets.cpp
// Immediate error - must use post(), not complete_immediate().
// Using symmetric transfer (complete_immediate) here breaks
// coroutine chains that hold async mutexes: the resumed
// coroutine releases its lock and tries to wake the next
// waiter, but the symmetric transfer chain doesn't return
// control to io_context properly.
```

This workaround (always posting) is pessimistic and adds unnecessary latency.

---

## The Solution

### Change `dispatch` to Return `void`

```cpp
// NEW: basic_io_context.hpp, executor_type::dispatch
void dispatch(capy::coro h) const {
    if (running_in_this_thread())
        h.resume();  // Normal function call - will return
    else
        ctx_->sched_->post(h);
}
```

### Why This Works

**Normal function calls return.** When `dispatch` calls `h.resume()`:

1. Coroutine runs until it suspends
2. Coroutine's `await_suspend` returns `noop_coroutine()`
3. `resume()` returns
4. `dispatch()` returns
5. Caller continues

The stack unwinds properly. Nested dispatch calls work correctly:

```cpp
void async_mutex::unlock() {
    auto next_waiter = waiters_.pop();
    dispatch(next_waiter);  // Normal call - returns when waiter suspends
    // This line DOES execute!
}
```

### Stack Overflow Prevention

With immediate completions:

```
io_context::run()
  -> dequeue completion
  -> dispatch(h)
     -> h.resume()  // Normal call
        -> coroutine runs one iteration
        -> co_await next I/O
        -> await_suspend returns noop_coroutine()
     <- h.resume() RETURNS
  <- dispatch() returns
  -> dequeue next completion (or same one if it completed immediately)
  ... stack stays flat
```

Each iteration returns to the run loop. No unbounded stack growth.

---

## Why We Don't Need Asio's Pump

### Asio's Pump Exists Because Asio Doesn't Use Symmetric Transfer

Asio's `awaitable_thread::pump()` maintains an explicit stack because:

1. `final_suspend` doesn't transfer - just pops frame and returns
2. Pump must manually resume the parent
3. Everything goes through the pump loop

### Corosio Can Keep Symmetric Transfer for Task Composition

With dispatch returning void, we can still use symmetric transfer for **task-to-task composition**:

```cpp
// task's final_suspend - UNCHANGED
auto await_suspend(coroutine_handle<>) noexcept {
    return continuation_;  // Symmetric transfer to parent task
}
```

This is safe because:

1. `final_suspend` transfers to exactly one place (the parent)
2. No "wake B AND continue A" scenario
3. Parent continues, may do I/O, returns `noop_coroutine()`
4. The chain terminates

### The Key Insight: `noop_coroutine()` Terminates Chains

When a coroutine's `await_suspend` returns `noop_coroutine()`:

1. Compiler generates "tail call" to `noop_coroutine().resume()`
2. `noop_coroutine().resume()` is a no-op that returns immediately
3. The symmetric transfer chain terminates
4. Control returns to whoever called `resume()` (i.e., dispatch)

Trace:
```
dispatch(parent)
  [1] parent.resume()  // Normal call from dispatch
      [2] parent -> child (symmetric transfer via await_suspend)
          [3] child -> grandchild (symmetric transfer)
              [4] grandchild does I/O, returns noop_coroutine()
              [4] "transfer" to noop - returns immediately
          [3] returns
      [2] returns
  [1] parent.resume() returns
dispatch returns
```

---

## Implementation Changes Required

### 1. Change `executor_type::dispatch` Signature

**File:** `include/boost/corosio/basic_io_context.hpp`

```cpp
// BEFORE
capy::coro dispatch(capy::coro h) const {
    if (running_in_this_thread())
        return h;
    ctx_->sched_->post(h);
    return std::noop_coroutine();
}

// AFTER
void dispatch(capy::coro h) const {
    if (running_in_this_thread())
        h.resume();
    else
        ctx_->sched_->post(h);
}
```

### 2. Update `resume_coro` Helper

**File:** `src/corosio/src/detail/resume_coro.hpp`

The `resume_coro` helper includes a **memory barrier** that must be preserved. This acquire fence ensures that I/O results (buffer contents, error codes, bytes transferred) written by other threads are visible to the resumed coroutine before it continues execution.

```cpp
// BEFORE
inline void
resume_coro(capy::executor_ref d, capy::coro h)
{
    std::atomic_thread_fence(std::memory_order_acquire);  // KEEP THIS
    auto resume_h = d.dispatch(h);
    if (resume_h.address() == h.address())
        resume_h.resume();
}

// AFTER
inline void
resume_coro(capy::executor_ref d, capy::coro h)
{
    std::atomic_thread_fence(std::memory_order_acquire);  // PRESERVED
    d.dispatch(h);  // dispatch now handles resume internally
}
```

**Why the fence matters:**

When an I/O operation completes:
1. The OS (or an internal worker thread) writes results to buffers
2. The completion is signaled to the `io_context` thread
3. `resume_coro` is called to resume the waiting coroutine
4. The coroutine reads the results from those buffers

Without the acquire fence, the coroutine might see stale data due to CPU memory reordering. The fence ensures all writes from step 1 are visible before step 4.

**Note:** The fence is conservative — it always executes even when not strictly necessary (e.g., same-thread immediate completions, or when IOCP/epoll already provides synchronization). This is intentional for safety.

### 3. Update `complete_immediate`

**File:** `src/corosio/src/detail/iocp/overlapped_op.hpp`

```cpp
// BEFORE
std::coroutine_handle<> complete_immediate() {
    // ...
    return d.dispatch(h);
}

// AFTER
void complete_immediate() {
    // ...
    d.dispatch(h);  // Returns void, resumes inline
}
```

### 4. Update All I/O Awaitable `await_suspend` Methods

Any `await_suspend` that currently returns `dispatch(h)` must change to return `noop_coroutine()` and let the completion handler path call `dispatch`.

**Example pattern:**

```cpp
// BEFORE
auto await_suspend(std::coroutine_handle<> h) {
    initiate_io();
    if (immediate_completion)
        return ex_.dispatch(h);
    return std::noop_coroutine();
}

// AFTER
auto await_suspend(std::coroutine_handle<> h) {
    initiate_io();
    // Immediate completions go through completion handler
    // which will call dispatch(h)
    return std::noop_coroutine();
}
```

### 5. Remove Pessimistic `post()` Workarounds

**File:** `src/corosio/src/detail/iocp/sockets.cpp`

Remove comments and code that forces `post()` for immediate completions:

```cpp
// BEFORE (pessimistic)
// Immediate error - must use post(), not complete_immediate()
op->post();

// AFTER (can use dispatch)
op->complete_immediate();  // Now safe
```

### 6. Update Capy's Executor Concept (if applicable)

If Capy defines an executor concept that requires `dispatch` to return a handle, that concept needs updating to allow `void` return.

---

## Verification Criteria

### 1. Stack Overflow Test

```cpp
task<> stack_test(tcp_socket& socket) {
    std::array<char, 1> buf;
    for (int i = 0; i < 1000000; i++) {
        // Use loopback socket that completes immediately
        co_await socket.async_read(buffer(buf));
    }
}
```

**Pass criteria:** No stack overflow, no `STATUS_STACK_BUFFER_OVERRUN`

### 2. Async Mutex Correctness Test

```cpp
async_mutex mutex;
int counter = 0;

task<> increment(async_mutex& m, tcp_socket& s) {
    for (int i = 0; i < 1000; i++) {
        auto lock = co_await m.lock();
        counter++;
        co_await s.async_write(...);  // May complete immediately
    }
}

// Run N concurrent incrementers
// Verify counter == N * 1000
```

**Pass criteria:** Final counter value is exactly N * 1000

### 3. Nested Dispatch Test

```cpp
task<> nested_test() {
    async_mutex m1, m2;
    
    auto lock1 = co_await m1.lock();
    {
        auto lock2 = co_await m2.lock();
        co_await async_op();  // Immediate completion
    }  // lock2 released, may wake waiter
    co_await async_op();
}  // lock1 released
```

**Pass criteria:** All waiters wake correctly, no hangs

### 4. Performance Comparison

Measure latency of immediate completions:
- Before: Always `post()` (queue + context switch overhead)
- After: Inline `resume()` (direct execution)

**Expected improvement:** Significant latency reduction for immediate completions

---

## Summary

| Aspect | Asio | Corosio (Current) | Corosio (Fixed) |
|--------|------|-------------------|-----------------|
| `dispatch` returns | N/A (uses handlers) | `coroutine_handle` | `void` |
| I/O resumption | Handler invocation | Symmetric transfer | Normal call |
| Task composition | Explicit pump | Symmetric transfer | Symmetric transfer |
| Stack behavior | Always unwinds | Can overflow | Always unwinds |
| Async mutex | Works | Broken | Works |
| Immediate completions | Handler path | Can inline (broken) | Can inline (fixed) |
| Memory barrier | In handler path | In `resume_coro` | In `resume_coro` (preserved) |

The fix is conceptually simple: **dispatch must be a normal function call, not an enabler of symmetric transfer.** Symmetric transfer remains available for task-to-task composition via `final_suspend`, where it's safe and efficient.

---

## References

- Boost.Asio source: `boost/asio/impl/awaitable.hpp`
- Lewis Baker: "Understanding Symmetric Transfer"
- P2300: `std::execution` (senders/receivers)
- Corosio source files:
  - `include/boost/corosio/basic_io_context.hpp`
  - `src/corosio/src/detail/resume_coro.hpp`
  - `src/corosio/src/detail/iocp/overlapped_op.hpp`
  - `src/corosio/src/detail/iocp/sockets.cpp`
