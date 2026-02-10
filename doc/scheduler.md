# Scheduler Architecture

This document describes the architectural differences between Boost.Asio and Corosio's coroutine scheduling mechanisms, and outlines the implementation approach for Corosio.

## Overview

Asio and Corosio take fundamentally different approaches to coroutine management:

| Aspect | Asio | Corosio |
|--------|------|---------|
| Symmetric transfer | Simulated via `pump()` loop | Native language mechanism |
| Type system | Closed (`asio::awaitable` only) | Open (`IoAwaitable` concept) |
| `await_suspend` return | `void` | `coroutine_handle` |
| I/O initiation timing | `after_suspend_fn_` callback | TBD |

## Symmetric Transfer

### The Problem

When coroutine A awaits coroutine B, naive implementations create stack growth:

```
A.resume()
  └─> B.resume()
        └─> C.resume()
              └─> ...  // unbounded stack growth
```

C++20 symmetric transfer solves this by allowing `await_suspend` to return a `coroutine_handle`, which the compiler tail-calls instead of returning to the caller.

### Asio's Approach: Manual Simulation

Asio's `await_suspend` returns `void`, forfeiting language-based symmetric transfer:

```cpp
// asio::awaitable - await_suspend returns void
template <class U>
void await_suspend(
    detail::coroutine_handle<detail::awaitable_frame<U, Executor>> h)
{
    frame_->push_frame(&h.promise());  // builds linked list
}
```

Instead, Asio maintains a manual stack of frames and uses `pump()` to simulate symmetric transfer:

```cpp
void pump()
{
    do
      bottom_of_stack_.frame_->top_of_stack_->resume();
    while (bottom_of_stack_.frame_ && bottom_of_stack_.frame_->top_of_stack_);
    // ...
}
```

The `pump()` loop repeatedly calls `resume()` on the top frame until the stack empties or transfers to another thread. This achieves the same bounded-stack behavior but through explicit frame management.

### Corosio's Approach: Native Symmetric Transfer

Corosio's `task<T>` returns `coroutine_handle` from `await_suspend`, enabling compiler-optimized tail calls:

```cpp
// task<T>::await_suspend - returns coroutine_handle
coro await_suspend(coro cont, io_env const& env)
{
    h_.promise().set_continuation(cont, env.executor);
    h_.promise().set_environment(env);
    return h_;  // compiler tail-calls this handle
}
```

Similarly, `final_suspend` returns the continuation handle:

```cpp
auto final_suspend() noexcept
{
    struct awaiter
    {
        coro await_suspend(coro) const noexcept
        {
            return p_->complete();  // returns continuation
        }
        // ...
    };
    return awaiter{this};
}
```

This means task-to-task awaits have zero overhead beyond what the language provides. No pump loop needed for non-I/O transitions.

## Type System

### Asio's Closed System

Asio's `await_suspend` only accepts handles to `awaitable_frame`:

```cpp
void await_suspend(
    detail::coroutine_handle<detail::awaitable_frame<U, Executor>> h)
```

This creates a closed type system where only `asio::awaitable<T>` coroutines can participate in I/O chains. User-defined coroutine types with different promise types cannot `co_await` an `asio::awaitable`.

### Corosio's Open System

Corosio uses the `IoAwaitable` concept, allowing any conforming type to participate:

```cpp
template<class Awaitable>
auto transform_awaitable(Awaitable&& a)
{
    using A = std::decay_t<Awaitable>;
    if constexpr (IoAwaitable<A>)
    {
        return transform_awaiter<Awaitable>{
            std::forward<Awaitable>(a), this};
    }
    // ...
}
```

The `await_suspend` signature accepts the execution environment:

```cpp
coro await_suspend(coro cont, io_env const& env)
```

This design allows third-party awaitable types to integrate with Corosio's I/O system by satisfying the `IoAwaitable` concept.

## I/O Initiation Timing

### The Suspension Race Problem

A critical issue in coroutine-based I/O is ensuring the I/O operation isn't initiated until the coroutine is fully suspended. If the completion handler fires before suspension completes, the coroutine may be resumed while still in the middle of suspending—undefined behavior.

### Asio's Solution: `after_suspend_fn_`

Asio solves this with a deferred callback mechanism:

```cpp
struct resume_context
{
    void (*after_suspend_fn_)(void*) = nullptr;
    void *after_suspend_arg_ = nullptr;
};

void resume()
{
    resume_context context;
    resume_context_ = &context;
    coro_.resume();  // coroutine runs until it suspends
    if (context.after_suspend_fn_)
      context.after_suspend_fn_(context.after_suspend_arg_);  // NOW safe to initiate I/O
}
```

Within `await_suspend`, true I/O operations register their initiation function:

```cpp
// awaitable_async_op::await_suspend
void await_suspend(coroutine_handle<void>)
{
    frame_->after_suspend(
        [](void* arg)
        {
            awaitable_async_op* self = static_cast<awaitable_async_op*>(arg);
            // Actually initiate the I/O operation here
            std::forward<Op&&>(self->op_)(
                handler_type(self->frame_->detach_thread(), self->result_));
        }, this);
}
```

Key distinction:
- **Task-to-task awaits**: Use `push_frame()`, no `after_suspend_fn_` set
- **True I/O awaits**: Set `after_suspend_fn_` to defer initiation

### Corosio's Approach

TBD - Document Corosio's mechanism for safe I/O initiation timing.

## Scheduler Implementation

TBD - Document:
- Event loop design
- Platform-specific backends (epoll, IOCP)
- Threading model
- Work stealing / distribution

## Implementation Plan

TBD - To be developed after gathering additional facts about:
- Corosio's I/O initiation mechanism
- Scheduler event loop design
- Platform-specific details
- Threading model
