## Asio Event Loop Architecture

### Core Abstraction

```
io_context
    └── scheduler (owns work queue, coordinates threads)
            └── reactor (optional, lazy, handles I/O readiness)
```

---

### Platform Implementations

**Linux (epoll)**
```
io_context
    └── scheduler
            ├── op_queue_ (posted work)
            └── epoll_reactor (service, lazy)
                    ├── epoll_fd_
                    ├── eventfd_ (wake-up)
                    └── timer_queue_
```
- Reactor model: "fd is ready, now you call recv()"
- Scheduler and reactor are separate services
- `epoll_wait()` blocks, wakes on fd events or eventfd poke

**macOS/BSD (kqueue)**
```
io_context
    └── scheduler
            └── kqueue_reactor (service, lazy)
                    ├── kqueue_fd_
                    └── timer_queue_
```
- Same reactor model as epoll
- kqueue handles more event types natively (timers, signals, vnodes)
- Otherwise identical architecture to Linux

**Windows (IOCP)**
```
io_context
    └── win_iocp_io_context (scheduler + IOCP combined)
            ├── iocp_ (HANDLE)
            └── select_reactor (fallback, rare)
```
- Proactor model: "OS completed the read, here's your data"
- No separate scheduler — IOCP *is* the work queue
- `PostQueuedCompletionStatus()` for post, `GetQueuedCompletionStatus()` for run
- `select_reactor` exists for edge cases only

---

### Scheduler Responsibilities

1. Own the work queue (`op_queue_`)
2. Track outstanding work for `run()` exit
3. Coordinate threads calling `run()`
4. Integrate reactor (or be the reactor on Windows)
5. Handle stop/restart

---

### Reactor Responsibilities

1. Wrap OS multiplexing API
2. Track fd → callback registrations
3. Block waiting for events
4. Enqueue completions back to scheduler
5. Provide wake-up mechanism for `post()`

---

### Event Loop Comparison

| | Linux | macOS | Windows |
|---|---|---|---|
| Mechanism | epoll | kqueue | IOCP |
| Model | Reactor | Reactor | Proactor |
| Scheduler | Separate class | Separate class | Combined |
| I/O syscall | After notification | After notification | Before notification |
| Wake-up | eventfd | kqueue user event | PostQueuedCompletionStatus |
| Timer handling | timerfd or internal queue | kqueue native | Internal queue |

---

### Lifecycle

```cpp
io_context ioc;              // scheduler created (IOCP created on Windows)
                             // reactor NOT created yet on Linux/macOS

tcp::socket s(ioc);          // use_service<reactor>() → epoll_create

ioc.run();                   // scheduler::run() → epoll_wait or GQCS
```

---

### run() Unified Model

Regardless of platform, `run()` does:

```
while (!stopped) {
    if (work in queue)
        execute it
    else if (reactor)
        block in reactor, harvest completions
    else
        condvar wait for post()
}
```

The abstraction hides whether you're on a reactor (Linux/macOS) or proactor (Windows) — same `async_read_some()` call, different underlying mechanics.