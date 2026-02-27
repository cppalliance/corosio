# Physical Structure of Corosio

## 1. Introduction

**Scope**: This document defines the physical structure of the corosio library - the type hierarchy, file layout, and layer boundaries that all code must follow. It is the authoritative reference for where types, headers, and source files belong.

**Goals**:

- Enable users to choose their tradeoff: compilation speed and separate compilation (abstract/concrete) vs maximum runtime performance (native)
- Provide protocol-agnostic vocabulary types (`io_stream`, `io_read_stream`, `io_write_stream`) for generic algorithms
- Provide full protocol-specific APIs (`tcp_socket`, `udp_socket`, etc.) for application code
- Support every major I/O object family: sockets, timers, signals, files, pipes, process, console, serial ports, file watch
- Platform escape hatches: POSIX descriptors, Windows handles

**Why physical structure matters**: The tradeoffs promised by each layer (no platform headers at abstract/concrete, full inlining at native, separate compilation at abstract/concrete) are only delivered if the layer boundaries are maintained precisely. A single misplaced `#include` of a platform header in an abstract-layer file breaks separate compilation for every consumer. A virtual call left in a native-layer awaitable negates the performance benefit. Engineers modifying this codebase must understand which layer they are working in and follow that layer's rules exactly.

The physical structure also serves as an organizational framework for the codebase itself. When every type, header, and source file has a well-defined place in the hierarchy, technical debt stays low - there is no ambiguity about where new code belongs or where to find existing code. Refactorings can be made precisely because the layer boundaries tell you exactly what can change in isolation and what will ripple. When the structure is followed, changes to a platform backend don't touch abstract or concrete headers, changes to a concrete type's API don't touch platform code, and generic algorithms written against the abstract layer remain untouched when new concrete types are added.

This document exists to make those rules unambiguous.

**How users express algorithms**:

- **Templates**: `template<class S> void algo(S& stream)` - full optimization and inlining when passed native objects
- **Concrete sliced bases**: `void algo(tcp_socket& s)` - runtime polymorphism without templates
- **Type-erased streams**: `void algo(io_stream& s)` - ABI stability and no type leakage

**Three layers**:

- **Abstract** - protocol-agnostic, virtual dispatch, separately compilable
- **Concrete** - protocol-specific, virtual dispatch, separately compilable
- **Native** - protocol-specific, template on Backend, fully inlined

## 2. Layer Tradeoffs

| Property             | Abstract                                     | Concrete                                        | Native                                    |
| -------------------- | -------------------------------------------- | ----------------------------------------------- | ----------------------------------------- |
| Compilation speed    | Fastest                                      | Fast                                            | Slowest (platform headers + templates)    |
| Separate compilation | Yes                                          | Yes                                             | No                                        |
| Polymorphism method  | Slicing (base class pointer)                 | Slicing (base class pointer)                    | Template (Backend parameter)              |
| Call penalty         | Virtual dispatch per operation               | Virtual dispatch per operation                  | None (direct / inlined)                   |
| Type-erasure penalty | Awaitables type-erase the impl               | Awaitables type-erase the impl                  | None (awaitables contain impl logic)      |
| API surface          | Protocol-agnostic (bytes only)               | Full protocol-specific API                      | Same as concrete, fully inlined           |
| Use case             | Generic algorithms, library APIs, test mocks | Application code that needs protocol operations | Hot paths, benchmarks, maximum throughput |

## 3. Universal Conventions

Rules that apply at every layer:

- `io_object` is the root base class of every I/O object in the library. It holds the execution context pointer, the implementation pointer, and a cached pointer to the service to avoid needless lookups. All abstract, concrete, and native types derive from `io_object`.
- Every I/O object is managed by a service which owns an `implementation` object whose ownership is tracked by the `handle` RAII type
- Every I/O object type has a nested type called `implementation`
- The `implementation` always inherits from the base class's `implementation`, forming a parallel inheritance chain that mirrors the type hierarchy
- Services construct `implementation` objects at runtime; `static_cast` dispatches through the chain
- Naming: abstract uses `io_` prefix, concrete uses protocol name, native uses `native_` prefix (template) or platform prefix (alias)

## 4. Diamond Hierarchies

Two diamond inheritance patterns exist in the library, one for streams and one for files. They exist because some I/O objects are read-only (pipes, console input), some are write-only (pipes, console output), and some are bidirectional (sockets, serial ports). The diamond lets a `tcp_socket` be passed where any of `io_read_stream&`, `io_write_stream&`, or `io_stream&` is expected.

Stream diamond:

```
           io_object
          /         \
io_read_stream   io_write_stream      <- abstract
          \         /
           io_stream
              |
         tcp_socket                   <- concrete
              |
  native_tcp_socket<Backend>          <- native
```

File diamond:

```
           io_object
          /         \
  io_read_file   io_write_file        <- abstract
          \         /
           io_file
              |
     random_access_file               <- concrete
```

## 5. Abstract Layer

**Purpose**: Protocol-agnostic vocabulary types for generic algorithms. A function taking `io_stream&` works with TCP sockets, Unix stream sockets, TLS streams, pipes, serial ports, or test mocks. The abstract layer exists so byte-stream consumers (protocol parsers, compression layers, TLS wrappers) don't need to know the underlying protocol.

**Shape**:

```cpp
class io_stream : public io_object
{
public:
    // Non-virtual member function returns a type-erasing awaitable
    template< class MutableBufferSequence >
    auto read_some( MutableBufferSequence const& buffers )
    {
        return read_some_awaitable( *this, buffers );
    }

    // Nested implementation with pure virtual functions
    struct implementation : io_object::implementation
    {
        virtual std::coroutine_handle<> read_some(
            std::coroutine_handle<>,
            capy::executor_ref,
            buffer_param,
            std::stop_token,
            std::error_code*,
            std::size_t* ) = 0;
    };

private:
    // static_cast to this layer's implementation type
    implementation& get() const noexcept
    {
        return *static_cast< implementation* >( impl_ );
    }

    template< class MutableBufferSequence >
    struct read_some_awaitable
    {
        io_stream& self_;
        MutableBufferSequence buffers_;
        std::error_code ec_;
        std::size_t n_ = 0;

        auto await_suspend(
            std::coroutine_handle<> h,
            capy::io_env const* env ) -> std::coroutine_handle<>
        {
            // Virtual dispatch through implementation - type-erased
            return self_.get().read_some( h, env->executor,
                buffers_, env->stop_token, &ec_, &n_ );
        }
    };
};
```

**Key structural points**:

- Member functions are non-virtual; they return awaitable objects
- The awaitable's `await_suspend` calls through the `implementation` pointer via `static_cast` from `io_object`'s `impl_`
- The `implementation::read_some` is pure virtual - the call is virtual dispatch, which is the type-erasure cost at this layer
- `implementation` inherits from `io_object::implementation`, maintaining the parallel chain
- No platform headers, no endpoint types, no protocol knowledge

**Types table**:

| Type            | Base(s)                         | Key Operations                             |
| --------------- | ------------------------------- | ------------------------------------------ |
| io_read_stream  | io_object                       | read_some(buffers)                         |
| io_write_stream | io_object                       | write_some(buffers)                        |
| io_stream       | io_read_stream, io_write_stream | read_some, write_some (diamond)            |
| io_read_file    | io_object                       | read_at(offset, buffers)                   |
| io_write_file   | io_object                       | write_at(offset, buffers)                  |
| io_file         | io_read_file, io_write_file     | read_at, write_at (diamond)                |
| io_timer        | io_object                       | wait(duration), cancel()                   |
| io_signal_set   | io_object                       | add(signal), wait(), cancel()              |
| io_file_watch   | io_object                       | watch(path), wait() yielding change events |

**Rules table**:

| Rule                | Detail                                                                                           |
| ------------------- | ------------------------------------------------------------------------------------------------ |
| Header location     | `include/boost/corosio/io/{class}.hpp` (e.g. `io/io_stream.hpp`)                                 |
| Source location     | `src/corosio/src/io/{class}.cpp` (e.g. `io/io_stream.cpp`)                                       |
| Test files          | `test/unit/io/{class}.cpp` (e.g. `io/io_stream.cpp`)                                             |
| Platform OS headers | NEVER included at this layer                                                                     |
| Naming convention   | `io_` prefix                                                                                     |
| Member functions    | Non-virtual; return type-erasing awaitables which use virtual dispatch                           |
| Endpoint types      | Not present (no polymorphic endpoint)                                                            |
| Concepts            | `io_stream` satisfies `capy::Stream`; does not imply kernel socket (TLS, pipes, mockets qualify) |

## 6. Concrete Layer

**Purpose**: Full protocol-specific API. This is where endpoint types, connect(), socket options, and protocol-specific operations live. Each concrete type is a non-template class. The polymorphic service selects the platform implementation at runtime. Most application code uses this layer.

**Shape**:

```cpp
class tcp_socket : public io_stream
{
public:
    // Protocol-specific operation not present at abstract layer
    auto connect( endpoint ep )
    {
        return connect_awaitable( *this, ep );
    }

    // Inherits read_some / write_some from io_stream

    // Nested implementation extends abstract layer's implementation
    struct implementation : io_stream::implementation
    {
        virtual std::coroutine_handle<> connect(
            std::coroutine_handle<>,
            capy::executor_ref,
            endpoint,
            std::stop_token,
            std::error_code* ) = 0;

        virtual std::error_code shutdown( shutdown_type ) noexcept = 0;
        virtual native_handle_type native_handle() const noexcept = 0;
        virtual void cancel() noexcept = 0;

        // Generic socket options (level/name passed through from option type)
        virtual std::error_code set_option(
            int level, int optname,
            void const* data, std::size_t size ) noexcept = 0;
        virtual std::error_code get_option(
            int level, int optname,
            void* data, std::size_t* size ) const noexcept = 0;
    };

private:
    // static_cast to this layer's implementation type
    implementation& get() const noexcept
    {
        return *static_cast< implementation* >( impl_ );
    }

    struct connect_awaitable
    {
        tcp_socket& self_;
        endpoint endpoint_;
        std::error_code ec_;

        auto await_suspend(
            std::coroutine_handle<> h,
            capy::io_env const* env ) -> std::coroutine_handle<>
        {
            // Virtual dispatch through concrete implementation
            return self_.get().connect( h, env->executor,
                endpoint_, env->stop_token, &ec_ );
        }
    };
};
```

**Key structural points**:

- Inherits abstract layer member functions (`read_some`, `write_some`) - those still use virtual dispatch through `io_stream::implementation`
- Adds protocol-specific member functions (`connect`, `shutdown`, socket options) that also use virtual dispatch through `tcp_socket::implementation`
- `tcp_socket::implementation` extends `io_stream::implementation`, adding pure virtual functions for the new operations
- The `static_cast` downcasts `impl_` to `tcp_socket::implementation`, which is one level deeper than the abstract layer's cast
- Endpoint types (`corosio::endpoint`) appear at this layer for the first time
- Still no platform headers - the service and implementation are abstract interfaces

**Types table** - Sockets (TCP):

| Type         | Base(s)   | Key Operations                                                     |
| ------------ | --------- | ------------------------------------------------------------------ |
| tcp_socket   | io_stream | connect(endpoint), shutdown(), TCP options, local/remote_endpoint() |
| tcp_acceptor | io_object | listen(endpoint, backlog), accept(tcp_socket&), local_endpoint()   |
| resolver     | io_object | resolve(host, service), resolve(endpoint), cancel()                |

**Types table** - Sockets (UDP):

| Type       | Base(s)   | Key Operations                                                              |
| ---------- | --------- | --------------------------------------------------------------------------- |
| udp_socket | io_object | send_to(buf, endpoint), recv_from(buf), bind(), connect() for default dest |

**Types table** - Sockets (Unix domain):

| Type                  | Base(s)   | Key Operations                                        |
| --------------------- | --------- | ----------------------------------------------------- |
| unix_stream_socket    | io_stream | connect(path), shutdown()                             |
| unix_datagram_socket  | io_object | send_to(path), recv_from()                            |
| unix_acceptor         | io_object | listen(path), accept(unix_stream_socket&)             |
| unix_seqpacket_socket | io_object | connect(path), send(), recv() with message boundaries |

**Types table** - Sockets (Raw / Generic):

| Type                    | Base(s)   | Key Operations                                        |
| ----------------------- | --------- | ----------------------------------------------------- |
| raw_socket              | io_object | send_to(), recv_from(), IP-layer access (ICMP etc.)   |
| generic_stream_socket   | io_stream | Arbitrary sockaddr, protocol family chosen at runtime |
| generic_datagram_socket | io_object | Arbitrary sockaddr, datagram                          |

**Types table** - Pipes:

| Type           | Base(s)         | Key Operations |
| -------------- | --------------- | -------------- |
| pipe_read_end  | io_read_stream  | read_some()    |
| pipe_write_end | io_write_stream | write_some()   |

**Types table** - Console:

| Type           | Base(s)         | Key Operations |
| -------------- | --------------- | -------------- |
| console_input  | io_read_stream  | read_some()    |
| console_output | io_write_stream | write_some()   |

**Types table** - Files:

| Type               | Base(s)                                        | Key Operations                        |
| ------------------ | ---------------------------------------------- | ------------------------------------- |
| stream_file        | io_stream (or io_read_stream / io_write_stream) | Sequential I/O with implicit position |
| random_access_file | io_file                                        | read_at(offset), write_at(offset)     |

**Types table** - Process, Timers, Signals, Serial, File Watch:

| Type        | Base(s)       | Key Operations                                              |
| ----------- | ------------- | ----------------------------------------------------------- |
| process     | io_object     | spawn(), wait_for_exit(); stdin/stdout/stderr are pipe ends |
| timer       | io_timer      | expires_after(), expires_at(), wait(), cancel()             |
| signal_set  | io_signal_set | add(int), remove(int), wait(), cancel()                     |
| serial_port | io_stream     | Baud rate, parity, flow control options                     |
| file_watch  | io_file_watch | watch(path), wait() yields change events                    |

**Types table** - Platform Escape Hatches:

| Type                     | Base(s)   | Key Operations                                           |
| ------------------------ | --------- | -------------------------------------------------------- |
| posix_descriptor         | io_stream | Wrap any fd into the reactor (POSIX only)                |
| win_stream_handle        | io_stream | Wrap any HANDLE for overlapped stream I/O (Windows only) |
| win_random_access_handle | io_file   | Wrap any HANDLE for overlapped positional I/O (Windows)  |
| win_object_handle        | io_object | WaitForSingleObject on kernel objects (Windows only)     |

**Rules table**:

| Rule                | Detail                                                                       |
| ------------------- | ---------------------------------------------------------------------------- |
| Header location     | `include/boost/corosio/{class}.hpp` (e.g. `tcp_socket.hpp`)                  |
| Source location     | `src/corosio/src/{class}.cpp` (e.g. `tcp_socket.cpp`)                        |
| Test files          | `test/unit/{class}.cpp` (e.g. `tcp_socket.cpp`)                              |
| Platform OS headers | NEVER included at this layer                                                 |
| Naming convention   | Protocol name, no prefix (e.g. `tcp_socket`, `tcp_acceptor`, `timer`)        |
| Member functions    | Inherited from abstract layer + protocol-specific operations                 |
| Endpoint types      | Protocol-specific (e.g. `corosio::endpoint` for TCP/IP); lives at this layer |
| Service interface   | `detail::socket_service`, `detail::acceptor_service` etc. in `src/detail/`   |
| Boilerplate sharing | CRTP mixin for shared socket options (e.g. SO_RCVBUF, SO_SNDBUF, SO_LINGER) |

## 7. Native Layer

**Purpose**: Maximum performance. Platform headers visible, compiler can inline everything including system call wrappers and awaitable suspend logic. The abstract layer's polymorphic interface continues to work via virtual dispatch (slicing works normally). Backend must match the io_context.

**Shape**:

```cpp
template< class Backend >
struct native_tcp_socket : tcp_socket
{
    // Shadows io_stream::read_some - no virtual dispatch
    template< class MutableBufferSequence >
    auto read_some( MutableBufferSequence const& buffers )
    {
        return read_some_awaitable( *this, buffers );
    }

private:
    // static_cast to the known implementation type - direct call
    typename Backend::socket_impl&
    get_impl() noexcept
    {
        return *static_cast< typename Backend::socket_impl* >( impl_ );
    }

    // static_cast to the known service type - direct call
    typename Backend::socket_service&
    get_service() noexcept
    {
        return *static_cast< typename Backend::socket_service* >( svc_ );
    }

    template< class MutableBufferSequence >
    struct read_some_awaitable
    {
        native_tcp_socket& self_;
        MutableBufferSequence buffers_;
        std::error_code ec_;
        std::size_t n_ = 0;

        auto await_suspend(
            std::coroutine_handle<> h,
            capy::io_env const* env ) -> std::coroutine_handle<>
        {
            // Direct call to backend impl - no vtable, inlinable
            return self_.get_impl().read_some( h, env->executor,
                buffers_, env->stop_token, &ec_, &n_ );
        }
    };
};
```

**Member function shadowing**:

- Native types duplicate the member functions found in their base class (e.g. `native_tcp_socket<Backend>` declares its own `read_some` and `write_some` that shadow the versions inherited from `io_stream`)
- Shadowing functions are ordinary (non-virtual), possibly defined inline
- Shadowing functions return awaitables whose member functions may also be defined inline
- Platform-specific structures and includes are fair game in both the shadowing functions and their awaitables
- When calling the service or implementation, the native type uses `static_cast` to downcast to the known backend-specific type
- These downcasted calls are direct (not virtual), possibly inlined, and candidates for link-time optimization
- This is the mechanism that eliminates virtual dispatch - the same implementation chain exists, but the native layer knows the exact types and bypasses the vtable
- The base class versions remain accessible via inheritance - code holding an `io_stream&` or `tcp_socket&` to a native object still calls the inherited virtual-dispatch path
- Only code that uses the native type directly gets the shadowing functions and the performance benefit

**Types table** (one row per concrete type that has a native counterpart):

| Type                           | Base(s)                      | Notes                        |
| ------------------------------ | ---------------------------- | ---------------------------- |
| `native_tcp_socket<Backend>`   | tcp_socket                   | Awaitables inline impl logic |
| `native_tcp_acceptor<Backend>` | tcp_acceptor                 | Awaitables inline impl logic |
| `native_timer<Backend>`        | timer                        | Awaitables inline impl logic |
| `native_signal_set<Backend>`   | signal_set                   | Awaitables inline impl logic |
| (aliases)                      |                              |                              |
| epoll_tcp_socket               | = `native_tcp_socket<epoll>` |                              |
| iocp_tcp_socket                | = `native_tcp_socket<iocp>`  |                              |

**Rules table**:

| Rule                      | Detail                                                                                          |
| ------------------------- | ----------------------------------------------------------------------------------------------- |
| Primary template          | `include/boost/corosio/native/{class}.hpp` (e.g. `native/native_tcp_socket.hpp`)                |
| Backend specializations   | `include/boost/corosio/native/{backend}/{class}.hpp` (e.g. `native/iocp/native_tcp_socket.hpp`) |
| Backend tags              | `include/boost/corosio/native/backends.hpp`                                                     |
| Implementation headers    | `include/boost/corosio/native/{backend}/detail/{class}.hpp` (`detail::{backend}` namespace)     |
| Detail source             | `src/corosio/src/native/{backend}/detail/{class}.cpp`                                           |
| Source location           | `src/corosio/src/native/{backend}/{class}.cpp` (if needed)                                      |
| Test files (primary)      | `test/unit/native/{class}.cpp`                                                                  |
| Test files (backend)      | `test/unit/native/{backend}/{class}.cpp`                                                        |
| Platform OS headers       | YES - visible at this layer                                                                     |
| Naming convention         | Template: `native_` + protocol. Alias: platform + protocol.                                     |
| Member functions          | Return awaitables with NO virtual interface; can contain full implementation logic               |
| Backend constraint        | Must match io_context (e.g. `epoll_tcp_socket` requires `epoll_io_context`)                     |
| Polymorphic compatibility | Abstract layer interface continues to work via virtual dispatch; slicing works normally          |

## 8. Platform Source Layout

Backend source files that do not correspond to public headers live alongside the native implementation sources at `src/corosio/src/{backend}/detail/`. Each backend directory follows the same internal structure.

- `src/corosio/src/timer/` - Shared timer implementation (most timer code is platform-independent)
- `src/corosio/src/epoll/detail/` - Linux: sockets, acceptors, scheduler, ops
- `src/corosio/src/iocp/detail/` - Windows: sockets, acceptors, scheduler, timers, signals, WSA init
- `src/corosio/src/kqueue/detail/` - BSD/macOS: sockets, acceptors, scheduler
- `src/corosio/src/select/detail/` - Fallback: sockets, acceptors, scheduler
- `src/corosio/src/dpdk/detail/` - DPDK: user-space networking, sockets, scheduler
- `src/corosio/src/posix/detail/` - Shared POSIX: resolver, signals

## 9. Special Cases

- Timers: most implementation shared across all platforms
- `io_stream` does not imply kernel socket; TLS wrappers, pipes, console, serial ports, and test mocks also derive from it
- Functions taking `io_stream&` mean "any platform byte stream"; generic algorithms use `template<capy::Stream S>` for non-platform streams
- Datagram sockets, acceptors, and raw sockets derive from `io_object` directly because their APIs require protocol-specific endpoint types
- `io_read_stream` / `io_write_stream` split exists for pipes and console, where one direction is genuinely unavailable
