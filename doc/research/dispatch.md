# Dispatch Design: Symmetric Transfer for Coroutine Resumption

## Principle

Every coroutine resumption must go through either symmetric transfer or the scheduler queue -- never through an inline `resume()` or `dispatch()` that creates a frame below the resumed coroutine.

## Design

`dispatch` returns `std::coroutine_handle<>`:

```cpp
std::coroutine_handle<>
dispatch(std::coroutine_handle<> h) const
{
    if (running_in_this_thread())
        return h;              // symmetric transfer
    post(h);
    return std::noop_coroutine();
}
```

- Same thread: returns `h` for symmetric transfer
- Different thread: posts to queue, returns `std::noop_coroutine()`
- Never calls `h.resume()` internally

`post` returns `void` -- it always queues.

## Call Site Patterns

### From coroutine machinery (await_suspend, final_suspend)

Return the handle for symmetric transfer:

```cpp
std::coroutine_handle<>
await_suspend(std::coroutine_handle<> h) noexcept
{
    // ...
    return caller_env.executor.dispatch(cont);
}
```

### From the event loop pump (scheduler/reactor handlers)

The one place where `.resume()` is called directly:

```cpp
// In scheduler completion handler
dispatch_coro(ex, h).resume();
```

### Launching concurrent work (when_all, when_any)

Use `post` instead of `dispatch` since you cannot symmetric-transfer to multiple handles:

```cpp
// Launch all runners via post
for (auto& handle : runner_handles)
    caller_env.executor.post(handle);
```

## dispatch_coro Helper

Corosio provides `dispatch_coro` as an optimized wrapper that skips executor dispatch overhead for the native `io_context` executor:

```cpp
inline std::coroutine_handle<>
dispatch_coro(
    capy::executor_ref ex,
    std::coroutine_handle<> h)
{
    if (&ex.type_id() == &capy::detail::type_id<
            basic_io_context::executor_type>())
        return h;
    return ex.dispatch(h);
}
```

## Audience

Ordinary users writing coroutine tasks do not interact with `dispatch` and `post` directly. These operations are used by authors of coroutine machinery -- `promise_type` implementations, awaitables, `await_transform` -- to implement asynchronous algorithms such as `when_all`, `when_any`, `async_mutex`, channels, and similar primitives.
