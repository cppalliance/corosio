# Coroutine Frame Allocation Model

## Problem Statement

Control allocation of every coroutine frame in a coroutine chain, including internal coroutines created by the framework (e.g., `root_task` in `async_run`). The API must specify the frame allocator BEFORE the coroutine is created—once the coroutine call expression evaluates, it's too late.

## Requirements

The system must be:
- invisible to the user unless they ask
- give great performance by default
- give execution contexts a way to override the default
- give the user a way to force a specific allocator
- ergonomic to use

## System Properties & Constraints

1. **Allocation happens at call site** — Coroutine frame is allocated when the coroutine function is *called*, not when awaited
2. **Promise type controls allocation** — `operator new` overloads in the promise type are the only customization point
3. **Arguments are inspected** — The promise's `operator new` receives copies of the coroutine arguments
4. **Internal coroutines exist** — `root_task` (and potentially others) are implementation details
5. **Allocator must be specified BEFORE creation** — Once the coroutine call expression evaluates, it's too late



## Current Implementation

The `promise_allocator` mixin detects `has_frame_allocator` on first or second parameter:

```cpp
template<has_frame_allocator Arg0, class... ArgN>
static void* operator new(std::size_t size, Arg0& arg0, ArgN&...)
{
    return allocate_with(size, arg0.get_frame_allocator());
}

template<class Arg0, has_frame_allocator Arg1, class... ArgN>
static void* operator new(std::size_t size, Arg0&, Arg1& arg1, ArgN&...)
    requires(!has_frame_allocator<Arg0>)
{
    return allocate_with(size, arg1.get_frame_allocator());
}
```

### The Problem: Internal Coroutines

`async_run` creates an internal `root_task` coroutine:

```cpp
root_task<Dispatcher, T, Handler>
make_root_task(Dispatcher, Handler handler, task<T> t)
{
    co_return co_await std::move(t);
}
```

Arguments are `(Dispatcher, Handler, task<T>)` — none carry a frame allocator.



## Design Approaches

### Approach 1: Allocator Threading via Dispatcher

Thread the allocator through the dispatcher type.

```cpp
template<frame_allocator A, dispatcher D>
struct allocating_dispatcher
{
    D inner_;
    A* alloc_;
    
    auto& get_frame_allocator() { return *alloc_; }
    coro operator()(coro h) const { return inner_(h); }
};
```

**Pros:**
- Natural extension of existing infrastructure
- Dispatcher is already first argument to internal coroutines
- Single injection point (`async_run`) propagates everywhere

**Cons:**
- Pollutes dispatcher concept with allocation concerns
- Dispatcher is for *where to resume*, not *how to allocate*

### Approach 2: Allocator Argument Convention

Always pass allocator as first argument using `std::allocator_arg_t` tag.

```cpp
template<typename Allocator, typename... Args>
void* operator new(std::size_t size, std::allocator_arg_t, Allocator alloc, Args&&...)
{
    return allocate_with(size, alloc);
}
```

Internal coroutines propagate:
```cpp
root_task<Dispatcher, T, Handler, Allocator>
make_root_task(std::allocator_arg_t, Allocator alloc, Dispatcher d, Handler handler, task<T> t)
{
    co_return co_await std::move(t);
}
```

**Pros:**
- Standard C++ convention (std library uses this)
- Explicit and composable
- No concept pollution

**Cons:**
- Requires modifying every internal coroutine signature
- Allocator must be propagated manually through call chain

### Approach 3: Thread-Local Allocator Context

Establish an allocator context before launching, inherit via thread-local.

```cpp
class allocator_context
{
    static thread_local frame_allocator* current_;
public:
    struct scope {
        frame_allocator* prev_;
        scope(frame_allocator& a) : prev_(current_) { current_ = &a; }
        ~scope() { current_ = prev_; }
    };
    
    static frame_allocator& current() { 
        return current_ ? *current_ : frame_pool::shared(); 
    }
};
```

Promise uses it:
```cpp
static void* operator new(std::size_t size)
{
    return allocate_with(size, allocator_context::current());
}
```

**Pros:**
- Zero signature changes to any coroutine
- All coroutines in scope automatically use the allocator
- Works for internal coroutines transparently

**Cons:**
- Thread-local state is implicit
- Doesn't work across thread boundaries (I/O completion)
- Harder to reason about which allocator is active

### Approach 4: Executor-Carried Allocator (Recommended)

The executor/dispatcher carries the allocator. The promise inspects the propagated executor.

Key insight: **the executor is already propagated through `any_dispatcher`**. Make the executor carry allocation strategy.

```cpp
template<executor E, frame_allocator A>
struct allocating_executor
{
    E inner_;
    A alloc_;
    
    // Satisfies executor concept
    // ...
    
    // Satisfies has_frame_allocator
    auto& get_frame_allocator() { return alloc_; }
};
```



## Recommended Design: Allocator-Carrying Executor

### 1. Define `allocating_executor` wrapper

```cpp
template<executor E, frame_allocator A>
class allocating_executor
{
    E inner_;
    A alloc_;
    
public:
    allocating_executor(E ex, A alloc)
        : inner_(std::move(ex)), alloc_(std::move(alloc)) {}
    
    // Executor requirements
    template<typename F>
    void execute(F&& f) const { inner_.execute(std::forward<F>(f)); }
    
    bool operator==(allocating_executor const&) const = default;
    
    // Frame allocator access
    A& get_frame_allocator() { return alloc_; }
    A const& get_frame_allocator() const { return alloc_; }
    
    // Access inner executor for comparison
    E const& inner() const { return inner_; }
};

// Factory function
template<executor E, frame_allocator A>
auto with_allocator(E ex, A alloc) {
    return allocating_executor<E, A>{std::move(ex), std::move(alloc)};
}
```

### 2. Extend promise_allocator to detect dispatcher

```cpp
// Existing overloads...

// NEW: First arg is dispatcher with allocator
template<class Arg0, class... ArgN>
    requires (dispatcher<Arg0> && has_frame_allocator<Arg0>)
static void* operator new(std::size_t size, Arg0& arg0, ArgN&...)
{
    return allocate_with(size, arg0.get_frame_allocator());
}
```

### 3. Update `make_root_task` signature

The first argument must be the dispatcher (by reference, non-const for allocator access):

```cpp
template<dispatcher Dispatcher, typename T, typename Handler>
root_task<Dispatcher, T, Handler>
make_root_task(Dispatcher& d, Handler handler, task<T> t)
{
    // d's allocator used for root_task frame
    if constexpr (std::is_void_v<T>)
        co_await std::move(t);
    else
        co_return co_await std::move(t);
}
```

### 4. Usage Example

```cpp
io_context ioc;
my_frame_allocator alloc;

// All coroutines (including internal root_task) use alloc
async_run(
    with_allocator(ioc.get_executor(), alloc),
    my_coroutine()
);
```

### 5. Type-Erased Allocator Dispatcher

For `any_dispatcher` (type-erased), carry a type-erased allocator:

```cpp
class any_dispatcher
{
    void const* d_ = nullptr;
    coro(*f_)(void const*, coro);
    
    // NEW: type-erased allocator
    void* alloc_ = nullptr;
    void* (*alloc_fn_)(void*, std::size_t) = nullptr;
    void (*dealloc_fn_)(void*, void*, std::size_t) = nullptr;
    
public:
    // ... existing ...
    
    // Frame allocator interface (returns *this as allocator proxy)
    void* allocate(std::size_t n) {
        if (alloc_fn_) return alloc_fn_(alloc_, n);
        return frame_pool::shared().allocate(n);
    }
    
    void deallocate(void* p, std::size_t n) {
        if (dealloc_fn_) dealloc_fn_(alloc_, p, n);
        else frame_pool::shared().deallocate(p, n);
    }
    
    any_dispatcher& get_frame_allocator() { return *this; }
};
```



## Design Tradeoffs Summary

| Approach | Pros | Cons |
|----------|------|------|
| **Allocator-Carrying Executor** | Natural composition; propagates automatically; explicit | Executor type changes |
| **std::allocator_arg Convention** | Standard; explicit | Manual propagation; signature changes |
| **Thread-Local Context** | Zero signature changes | Implicit; doesn't cross threads |
| **Separate Allocator Argument** | Simple | Doesn't help internal coroutines |



## Additional Considerations

### HALO Elision

When HALO (Heap Allocation Elision Optimization) applies, the allocator is never called. This is fine—it's an optimization. The allocator exists for when elision doesn't happen.

### Allocator Lifetime

The allocator must outlive all coroutine frames it allocates. This is ensured by:
1. `allocating_executor` stores by value (copied into root)
2. Frames deallocate before returning to caller
3. Type-erased variant must use reference semantics carefully

### Internal Coroutines Checklist

For each internal coroutine, verify:
- [ ] `root_task` in `async_run` — first arg is dispatcher
- [ ] Any future adapters — propagate allocator via dispatcher
