# I/O Object Lifecycle Management

This document analyzes implementation object and file descriptor lifecycle strategies across platform reactors (epoll, kqueue, IOCP, io_uring) and recommends a per-context configurable policy design.

## Overview

Every I/O object (socket, acceptor, timer) has two resources to manage:

1. **Implementation object** — platform-specific state (reactor registration, pending operations, cached endpoints)
2. **File descriptor / handle** — OS-level resource (socket fd, HANDLE)

The timing of allocation and deallocation for these resources has significant performance and correctness implications that vary by platform.

## Platform Reactor Characteristics

### epoll (Linux)

- **Model**: Reactor (readiness notification)
- **Registration**: `epoll_ctl()` adds/modifies/removes fds
- **Behavior on close**: Auto-removes fd from epoll set
- **Cancellation**: Deregister fd, pending waiters see EPOLLERR or get no event
- **Lifetime requirements**: None special — ops don't "own" completion state

### kqueue (BSD/macOS)

- **Model**: Reactor (readiness notification)  
- **Registration**: `kevent()` with EV_ADD/EV_DELETE
- **Behavior on close**: Auto-removes fd from kqueue
- **Cancellation**: EV_DELETE optional before close
- **Lifetime requirements**: Same as epoll

### IOCP (Windows)

- **Model**: Proactor (completion notification)
- **Registration**: `CreateIoCompletionPort()` associates handle once
- **Behavior on close**: `closesocket()` before ops complete → **undefined behavior**
- **Cancellation**: `CancelIoEx()` + drain completions required before close
- **Lifetime requirements**: **Implementation must outlive in-flight operations**

Key IOCP behaviors:
- Operations are "posted and forgotten" — kernel owns the OVERLAPPED until completion
- MSDN claims `closesocket()` returns ERROR_OPERATION_ABORTED; reality returns ERROR_NETNAME_DELETED (indistinguishable from peer hard-close)

### io_uring (Linux)

- **Model**: Hybrid (submission/completion queues)
- **Registration**: Optional `IORING_REGISTER_FILES` for fixed-file operations
- **Behavior on close**: Regular fds behave like epoll; fixed-file slots require explicit management
- **Cancellation**: `IORING_OP_ASYNC_CANCEL` or close fd
- **Lifetime requirements**: Consider extension for coroutine suspension; fixed-file slots are limited (~32K)

Fixed-file trade-offs:
- `IOSQE_FIXED_FILE` operations avoid fd table lookup (faster)
- Registration is a batch operation with limited slots
- Pooling maximizes fixed-file utilization

## Lifecycle Strategies

### Strategy A: Eager

```
Impl: allocated on socket construction
fd:   allocated on socket construction
```

| Aspect | Analysis |
|--------|----------|
| Mental model | Simplest — resource exists from construction |
| Null checks | None required in operations |
| Pre-configuration | fd available for setsockopt before connect |
| Resource usage | fd/memory consumed even if socket never connects |
| Address family | Must guess AF_INET vs AF_INET6 at construction |
| fd exhaustion | Hits earlier in application lifecycle |
| Reopen support | Awkward — must close and recreate |

**Best for**: Connection pools, pre-allocated socket arrays, low-latency paths requiring zero allocation during connect.

### Strategy B: Lazy

```
Impl: allocated on socket construction  
fd:   allocated in open() or connect()
```

| Aspect | Analysis |
|--------|----------|
| Mental model | Two-phase: construct object, then open |
| Null checks | Required in every operation (is_open validation) |
| Pre-configuration | Limited until open() called |
| Resource usage | Minimal until actually needed |
| Address family | Known at open() time |
| fd exhaustion | Deferred until connection attempt |
| Reopen support | Natural — close() + open() cycle |

**Best for**: Clients that may or may not connect, applications with many dormant socket objects, memory-constrained environments.

### Strategy C: Pooled

```
Impl: allocated on first open, cached until context shutdown
fd:   allocated in open(), closed in close() (impl survives)
```

| Aspect | Analysis |
|--------|----------|
| Mental model | acquire/release from pool |
| Null checks | Same as lazy |
| Pre-configuration | Same as lazy |
| Resource usage | Memory grows monotonically (bounded by pool max) |
| Address family | Known at open() time |
| fd exhaustion | Same as lazy |
| Reopen support | Excellent — impl reused across connections |
| State management | Must clear all state on reuse |
| Shutdown | Must drain pool |

**Best for**: High-throughput servers, accept loops, connection-per-request workloads.

## Platform Suitability Matrix

| Strategy | epoll | kqueue | IOCP | io_uring |
|----------|-------|--------|------|----------|
| **Eager** | Works | Works | Works | Enables fixed-file registration |
| **Lazy** | Natural fit | Natural fit | Works | Works (no fixed files) |
| **Pooled** | Minor benefit | Minor benefit | Minor benefit | **Best fit** |

### Why IOCP Favors Pooling

1. Overlapped structures and per-socket state can be pre-allocated and reused
2. Avoids repeated memory allocation for socket impl structures
3. The handle-to-IOCP association cost (`CreateIoCompletionPort`) is trivial — not a real optimization target

Note: While `DisconnectEx` with `TF_REUSE_SOCKET` theoretically allows kernel socket reuse, this is **not recommended** — see "Why Not TF_REUSE_SOCKET" below.

### Why io_uring Favors Pooling

1. Fixed-file slots are limited (typically 32K)
2. Registration/unregistration is a batch operation with overhead
3. Pooling preserves fixed-file slots across connections

### Why Not TF_REUSE_SOCKET (Windows)

Windows offers `DisconnectEx` with `TF_REUSE_SOCKET` to recycle kernel socket objects without closing the handle. This is **not recommended** for production use:

**No evidence of benefit:**
- No published benchmarks show meaningful throughput improvement over close-and-recreate
- A connected socket uses ~2 KB non-paged pool; allocation cost is trivial on modern hardware
- Neither Boost.Asio nor libuv use this feature; major IOCP frameworks just close and recreate

**Substantial documented problems:**

1. **TIME_WAIT blocking** — `DisconnectEx` may block for up to 2×MSL (typically 4 minutes) waiting on TIME_WAIT state, defeating the purpose of reuse

2. **Only works for passive closer** — The socket enters TIME_WAIT on the side that initiates termination. If the server initiates close, subsequent `ConnectEx` fails with error 52. This only works when the client disconnects first — a scenario servers don't control

3. **AcceptEx/ConnectEx socket confusion** — Sockets used with `AcceptEx` cannot be reused with `ConnectEx` or vice versa after `DisconnectEx`; mixing them causes silent `WSAEINVAL` failures

4. **Partially undocumented behavior** — MSDN doesn't document `WSARecv()`, `WSASend()`, or `DisconnectEx()` among functions supported on reused sockets

5. **Pool management complexity** — Requires distinguishing between connection close and pool shutdown, managing pending `DisconnectEx` completions

**Recommendation:** Just close the socket and create a new one. The implementation cost and edge cases of `TF_REUSE_SOCKET` far outweigh any theoretical benefit.

## Address Family and the Eager Strategy

Creating a socket fd requires specifying the address family upfront:

```cpp
int fd = socket(AF_INET, SOCK_STREAM, 0);   // IPv4 only
int fd = socket(AF_INET6, SOCK_STREAM, 0);  // IPv6 (possibly dual-stack)
```

With eager allocation, the fd is created at construction time — before the user specifies what address to bind or connect to. This creates a fundamental mismatch for acceptors.

### The Problem for Acceptors

```cpp
// Eager: fd created with AF_INET at construction
tcp_acceptor acc(ctx);  // internally: socket(AF_INET, ...)

// User tries to bind to IPv6 — fails
acc.bind(endpoint("[::]:8080"));  // Can't bind IPv6 address to AF_INET socket
```

The acceptor cannot know the correct address family until `bind()` is called.

### Workarounds for Eager Acceptors

**Option 1: Require protocol at construction**

```cpp
tcp_acceptor acc(ctx, tcp::v4());  // Creates AF_INET immediately
tcp_acceptor acc(ctx, tcp::v6());  // Creates AF_INET6 immediately
```

Forces the user to decide upfront, reducing API flexibility.

**Option 2: Default to dual-stack IPv6**

An AF_INET6 socket with `IPV6_V6ONLY=0` accepts both IPv4 and IPv6:

```cpp
int fd = socket(AF_INET6, SOCK_STREAM, 0);
int off = 0;
setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
bind(fd, "::");  // Accepts IPv4 (as ::ffff:x.x.x.x) and IPv6
```

Issues with this approach:
- Not all platforms support dual-stack or enable it by default
- Some deployments explicitly require IPv4-only or IPv6-only
- IPv4-mapped addresses (`::ffff:192.168.1.1`) can be confusing
- Binding to a specific IPv4 address still fails

**Option 3: Eager impl, lazy fd (hybrid)**

Allocate the implementation structure eagerly, but defer fd creation:

```cpp
tcp_acceptor acc(ctx);       // impl allocated, fd = -1
acc.bind("[::]:8080");       // NOW creates AF_INET6, binds, listens
```

This is effectively the lazy strategy for fd allocation.

### Strategy Suitability by I/O Object Type

| I/O Object | Eager | Lazy | Pooled |
|------------|-------|------|--------|
| **Client socket** | Works (can default AF or require protocol) | Natural fit | Natural fit |
| **Acceptor** | Problematic (address family unknown) | **Natural fit** | Rarely needed |
| **Timer** | Works | Works | Rarely needed |
| **UDP socket** | Same issues as acceptor | Natural fit | Natural fit |

### Recommendation

- **Client sockets**: All strategies viable; eager requires protocol parameter or AF default
- **Acceptors**: Use lazy fd allocation regardless of context policy
- **Timers**: No fd involved; strategy applies only to impl lifetime

The eager strategy is best suited for client socket pools where the application controls the address family. For acceptors, lazy fd allocation is the only practical approach unless the protocol is specified at construction.

## Comparison with Asio

Asio uses a different fundamental approach:

| Aspect | Asio | corosio |
|--------|------|---------|
| Impl storage | Embedded struct (value semantics) | `shared_ptr<impl>` (heap) |
| Heap allocation | Zero per socket | One per socket |
| Cache locality | Impl adjacent to socket object | Impl elsewhere in heap |
| Move cost | Copy ~64-128 bytes | Swap pointers |
| Polymorphism | Compile-time (templates) | Runtime (virtual) |
| Op lifetime safety | Small `cancel_token_` shared_ptr (IOCP only) | `shared_from_this()` in all ops |

Asio achieves zero heap allocation by embedding the impl struct directly in the socket object. The trade-off is compile-time polymorphism (templates throughout) versus corosio's runtime polymorphism (cleaner public API, virtual dispatch).

For coroutine libraries, `shared_ptr` lifetime extension is natural — operations capture `shared_from_this()` to survive across suspension points. The overhead is minimal compared to coroutine frame allocation.

## Recommended Design: Per-Context Configuration

### Configuration Interface

```cpp
enum class socket_lifecycle {
    lazy,      // Default: impl on ctor, fd on open()
    eager,     // impl + fd on ctor  
    pooled     // impl cached, fd on open(), impl survives close()
};

struct context_options {
    socket_lifecycle lifecycle = socket_lifecycle::lazy;
    
    // Pooled strategy options:
    std::size_t pool_initial_size = 0;    // 0 = grow on demand
    std::size_t pool_max_size = 0;        // 0 = unlimited
};
```

### Usage

```cpp
// Default: lazy strategy
io_context ctx;

// Server workload: pooled strategy  
io_context ctx(context_options{
    .lifecycle = socket_lifecycle::pooled,
    .pool_initial_size = 256,
    .pool_max_size = 4096
});

// Low-latency client: eager strategy
io_context ctx(context_options{
    .lifecycle = socket_lifecycle::eager
});
```

### Service Method Semantics Per Policy

| Method | Lazy | Eager | Pooled |
|--------|------|-------|--------|
| `construct()` | Allocate impl, fd = -1 | Allocate impl + fd | Acquire from pool (or allocate) |
| `open()` | Create fd, register | No-op (fd exists) | Create fd, register |
| `close()` | Deregister, close fd | Deregister, close fd | Deregister, close fd (impl survives) |
| `destroy()` | Close if open, free impl | Close if open, free impl | Return impl to pool |

**Note**: For acceptors, `construct()` should always defer fd creation (lazy behavior) regardless of the context policy, since the address family is unknown until `bind()`. The eager strategy applies fully only to client sockets where the protocol can be specified or defaulted.

### Implementation Structure

The existing `io_service` abstraction supports this directly:

```cpp
struct io_service {
    virtual implementation* construct() = 0;
    virtual void open(handle&) = 0;
    virtual void close(handle&) = 0;
    virtual void destroy(implementation*) = 0;
};
```

Each strategy implements these four methods differently. The socket class remains unchanged — it simply calls service methods at the appropriate times.

### Platform-Specific Optimizations

**io_uring pooled mode** preserves fixed-file slots:

```cpp
implementation* construct() {
    auto impl = pool_acquire();
    if (impl->fixed_slot_ < 0 && fixed_slots_available()) {
        impl->fixed_slot_ = allocate_fixed_slot();
    }
    return impl;
}
```

## User Profile Recommendations

| Profile | Recommended Strategy | Rationale |
|---------|---------------------|-----------|
| Simple client | Lazy (default) | Minimal resource use, intuitive API |
| HTTP/WebSocket server | Pooled | High connection throughput for accepted sockets |
| Game server | Eager or Pooled | Deterministic latency, pre-allocation |
| Embedded system | Lazy | Minimal memory footprint |
| Financial/HFT | Eager | Zero allocation on hot path |
| General purpose | Lazy | Safe default, works everywhere |

**Note**: These recommendations apply to client sockets and accepted connections. Acceptors themselves always use lazy fd allocation internally due to the address family constraint — the context policy affects only the acceptor's impl lifetime, not its fd timing.

## Implementation Effort

| Component | Effort |
|-----------|--------|
| Lazy strategy | Already implemented (current behavior) |
| Eager strategy | Small — change `construct()` to also create fd |
| Pooled strategy | Moderate — free list, state reset, shutdown drain |
| io_uring fixed-file | Optional optimization within pooled |

## Summary

- **Default to lazy** — safest, most intuitive, minimal resource usage
- **Offer pooled** for server workloads — amortizes allocation, enables platform optimizations
- **Offer eager** for latency-sensitive paths — zero allocation during operations
- **Configure per-context** — keeps type system simple, policy invisible to socket users
- **Leverage existing abstractions** — `io_service` virtual interface already supports all strategies
- **Acceptors are special** — always use lazy fd allocation due to address family constraints; context policy affects only impl lifetime
