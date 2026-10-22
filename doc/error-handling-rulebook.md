# Corosio Error-Handling Rulebook

How corosio reports, classifies, tests, and documents errors.

## 1. One Channel

Every fallible operation reports through exactly one channel, chosen
by classifying its failures:

- **Expected runtime conditions** — anything the environment or a
  peer can cause at runtime: missing file, address in use, descriptor
  exhaustion, disk full, peer disconnect race, unparseable input,
  foreign descriptors unfit for adoption — report through the
  **return value**.
- **Misuse the object can observe** — calling on a closed object,
  or another condition a different party can cause — reports
  through the **return value** like any runtime condition.
- **Contract violations** — breaking a documented precondition on
  the caller's own argument values or call sequence (`to_v4`
  on the wrong family, a `to_buffer` too small, an out-of-range
  option value, `tcp_server` calls out of sequence) — **throw**.
  The address-family conversions throw `std::system_error` carrying
  `address_family_not_supported`; the other violations throw
  `std::logic_error` types.
- **Not actionable** — nothing a caller can act on remains — report
  nothing: **`void noexcept`**. The effect is guaranteed: `close()`
  releases the descriptor even when the OS objects, `cancel()`'s
  request always lands. Everything actionable was available through
  an earlier return-channel operation — durability through
  `sync_data`/`sync_all`, orderly teardown through `shutdown` — and
  cancellation's outcome is inherently asynchronous, surfacing
  through each canceled completion. Reporting a `close()` error
  would be an attractive nuisance: the retry it invites is a
  double-close hazard, because POSIX leaves descriptor release under
  `EINTR` unspecified.
- **No caller at all** — an internal wakeup (`interrupt_reactor()`,
  `wake_self()`) has nobody to report to, but swallowing the failure
  must not swallow every later wake too: a coalescing flag stands for
  a byte a failed write never sent, so the failure path disarms it.
  The cost is then the wakes already in flight rather than every wake
  after them. Never throw from a wake path.
- A service's `shutdown()` is the same shape. Handing process-wide
  state back (`sigaction`/`signal` restored to `SIG_DFL`) has no
  channel and no caller left to act on one, so its result is
  `std::ignore`d deliberately.
- Never both channels for one operation. Never `std::error_code&`
  out-params. Never a throwing/non-throwing overload pair.

## 2. Return Shapes

| Situation | Shape |
|---|---|
| Sync, no payload | `[[nodiscard]] std::error_code f(...) noexcept` |
| Sync, with payload | `[[nodiscard]] capy::results<T> f(...) noexcept` |
| Async | the awaitable completes with `results<...>` — initiators never throw |

`results<T>` is `std::tuple<std::error_code, T...>`, so results
destructure; the sync and async surfaces share one idiom:

```cpp
if (auto ec = sock.open())                        // no payload
    co_return;
auto [ec, pos] = f.seek(0, file_base::seek_end);  // payload
auto [ec2, ep] = make_endpoint("10.0.0.1:80");    // factory
```

The payload always rides in the return — the `make_*` factories
return `results<T>` with a default-constructed payload on failure.
Every awaitable-returning initiator is `[[nodiscard]]`: a discarded
awaitable is a silent no-op that also drops any pre-set completion.

Constructors have no return channel to choose, so they throw only
for root setup for which no code-returning spelling can exist
(`io_context` backend creation, allocation, WSA startup) and for
contract violations (a service not installed, `io_context`'s
`thread_pool_size` and reactor option ranges). The one runtime
exception is the convenience constructors (`tcp_acceptor` and
`local_stream_acceptor` from an endpoint, `signal_set` from a signal
list): each has a code-returning spelling (the piecewise calls) and
throws the code that spelling returns. Building an address or
endpoint from a string has no constructor; it goes through the
`make_*` factories.

Root setup is everything the backend needs — completion port, ring,
wakeup channel, thread pool — and all of it is built during
construction, so a system that refuses any of it throws from the
constructor instead of from the first operation, and the failed
construction leaves nothing open. An initiator may then assume that
infrastructure exists, which is what makes "initiators never throw"
reachable at all. The one piece held back is the thread pool's
workers, which start on the first blocking operation: a thread the
system refuses there is reported through that operation's own channel,
never thrown.

The run loop is the other exception. `run()`, `poll()` and their
siblings throw `std::system_error` when waiting on the event
demultiplexer fails with anything other than an interruption or a
transient condition the next wait clears: no completion can carry
that error, and the context cannot dispatch work past it. On Windows,
`stop()` belongs to the same exception: it throws `std::system_error`
when the wakeup cannot be queued, and so does a run or poll call made
with no outstanding work, which stops the context itself.

## 3. The Classification Test

Ask: **can the caller reliably prevent the failure by checking state
they own, race-free, before the call?**

- **No** → normal error. File existence (TOCTOU), a port's
  availability, a peer's connection state, or whether a string parses
  cannot be pre-checked.
- **Yes** → the caller may pre-check, but a failure the operation
  can observe at runtime still reports through the return value. The
  canonical example: `make_local_endpoint(path)` reports
  `filename_too_long` for an over-long path, and the public
  `max_path_length` constant lets a caller test the same limit
  beforehand — one integer comparison, no grammar, no race. Only an
  operation with no return channel throws: a conversion with no error
  slot is a contract violation (§1), and a convenience constructor
  such as `tcp_acceptor(ctx, ep)` throws `std::system_error` carrying
  the code its code-returning spelling reports (§2).

## 4. The Closed-Object Condition

One condition, one code — `errc::bad_file_descriptor` — delivered
through whichever channel the operation already uses:

- Sync methods (`bind`, `listen`, `shutdown`, `assign`, file
  `resize`/`sync_*`/`seek`, `size`, `release`, `available`,
  `set_option`, `get_option`) **return** it. Closed-ness is
  legitimately observable in async teardown (another coroutine closes
  to cancel), so it is a runtime outcome, not a bug to throw over.
- A `release()` that fails leaves the object owning the handle and
  returns the invalid sentinel `native_handle()` reports when closed,
  never a descriptor value a caller could mistake for a real one. A
  release refused because an operation is in flight
  (`device_or_resource_busy`) has already cancelled the pending
  operations, so none of them can run on a recycled descriptor; the
  caller retries once they complete.
- Async initiators **pre-fail the awaitable**: set `aw.ec_` and
  return it; `await_ready` treats a pre-set `ec_` as
  ready-with-result, so the operation completes immediately without
  dispatching. `connect()`'s auto-open reports an open failure the
  same way. The backends enforce the same contract at their operation
  entries — a closed object reaching a backend read/write/wait
  completes with `bad_file_descriptor` before touching the kernel —
  so the code is deterministic on every path, including the
  devirtualized native facades.

Contract violations unrelated to object state throw `std::logic_error`
types: service not installed, launcher invoked twice, zero-sized thread
pool. The address-family conversions (`to_v4`, `to_v6`) are the
exception: they throw `std::system_error` carrying
`address_family_not_supported`.

## 5. Composite Operations

An operation that performs several fallible steps reports the first
failure through its own single channel — sub-steps never add a
second channel:

- `connect()` opens the socket automatically when needed; an open
  failure surfaces through the connect completion, so callers need no
  explicit `open()` before `connect`.
- The convenience constructors wrap their code-returning spelling
  (`open`+`bind`+`listen`, with `set_option` between for
  `tcp_acceptor`, or `add` per signal) and
  throw the code it returned.
- The free `corosio::connect` walks a candidate range and reports
  through one completion — `no_such_device_or_address` when no
  candidate is viable.
- `tls_context` setters record configuration that is applied when a
  handshake first configures the engine; application failures surface
  through that handshake's completion.
- A failure with no operation to attach to is latched on the object
  and pre-answers the operations that follow, until the step it
  belongs to succeeds again: a multishot arming the ring never took
  reports to every accept until the next arming clears it.
- A completion queued into a scheduler that spends a
  `work_finished()` on everything it dispatches needs a matching
  `work_started()`. An operation nothing counted reports through its
  owner's channel instead of the completion queue.
- Infrastructure an operation needs but does not own — the
  blocking-I/O pool's worker threads, the wait reactor's polling
  thread — is created on the first operation that needs it, and a
  system that refuses it answers through that operation's completion.
  The refusal is that operation's alone: the next one asks again. A
  refusal the initiator learns of on its own thread, before any part of
  the operation is cross-thread, completes there — the same exit the
  closed-object and zero-length contracts take a few lines above it,
  not a completion posted back through the scheduler.

## 6. Attributes and Spelling

- Every error-returning function is `[[nodiscard]]` and, where the
  body permits, `noexcept`. A silently dropped
  `use_certificate_chain_file` failure is a security bug; the
  attribute makes it a warning.
- **`[[nodiscard]]` goes BEFORE `BOOST_COROSIO_DECL`.** The macro
  expands to a visibility/dllexport attribute in shared builds;
  placed after it, `[[nodiscard]]` binds to the return *type* — a
  hard error that static local builds never see. Gate new free
  functions with a `-DBOOST_COROSIO_DYN_LINK -DBOOST_COROSIO_SOURCE`
  syntax check.
- Deliberate discards use `std::ignore = expr;`, never `(void)`
  casts and never `[[maybe_unused]]` on a named result. Reserve them
  for calls whose outcome is asserted downstream (hostile-input tests,
  best-effort bench teardown), whose failure is impossible by
  construction at that site, or which have no channel to report on at
  all (§1's wake and `shutdown()` paths).
- Unused names — parameters kept for signature clarity, structured
  bindings partially consumed, `#if`-gated uses — are declared
  `[[maybe_unused]]`, never silenced with a void cast.

## 7. Code Values and Portability

- **The user vocabulary is `std::errc`**. The one exception is
  `corosio::error::already_open` (`assign()` or `connect_pair()` on
  an open object).
- Codes corosio generates itself are deterministic contracts:
  `invalid_argument` (parsers, signal_set, negative seek),
  `bad_file_descriptor` (closed objects, descriptor validation),
  `wrong_protocol_type` / `address_family_not_supported` (adoption
  rejection), `value_too_large` (off_t guard, truncated hostname),
  `filename_too_long`,
  `device_or_resource_busy` (`release()` on a Windows file, stream
  handle, or random-access handle with an operation in flight, or on a
  file a thread-pool worker is using),
  `no_such_device_or_address` (`corosio::connect` with no viable
  candidate),
  `resource_unavailable_try_again` (io_uring submission queue
  exhausted, for a submitted op, for the signal reader, and for a
  multishot arming that never reached the kernel, latched on the
  object until an arming succeeds; and a polling thread the system
  would not start).
- Portable comparison comes from **normalizing at the boundary**: the
  Windows `make_err` maps the contracted WSA/Win32 codes to
  generic-category `errc` values (`WSAEOPNOTSUPP`, `WSAENOTSOCK`,
  `WSAEAFNOSUPPORT`, `WSAEPROTOTYPE`, `WSAEADDRINUSE`,
  `WSAEADDRNOTAVAIL`, `ERROR_NEGATIVE_SEEK`, `ERROR_MAX_THRDS_REACHED`;
  `iocp_make_err` adds the async condition set and
  `WSAEBADF`/`ERROR_INVALID_HANDLE`). On POSIX, raw errno satisfies
  `errc` comparison with one exception:
  `make_err` normalizes `ENOTSUP` so platforms where it differs from
  `EOPNOTSUPP` still compare equal to
  `errc::operation_not_supported`.
- Kernel codes outside the contracted set go out as raw
  `system_category` values — normalization is for contracts, not a
  laundering of every error. Some rejections stay platform- or even
  runner-dependent (WinSock's handling of IPv6-level options on
  AF_INET sockets; Darwin's `getsockopt(TCP_NODELAY)` on AF_UNIX);
  Windows reports `not_a_socket` where POSIX validation reports
  `EBADF` for garbage (non-sentinel) handles.
- A failing call that leaves a zero last error must not become an
  empty `error_code`: read the last error before anything that can
  clobber it, and substitute rather than report success. Prefer a
  contracted condition to a plausible-looking raw platform value,
  which is indistinguishable from a code the provider really gave.
- Conditions the standard cannot spell come from capy:
  `capy::cond::eof`, `capy::cond::canceled` (a stop token, not
  `errc::operation_canceled`), `capy::cond::timeout` (our deadline,
  not a kernel `ETIMEDOUT`).
- A background thread that dies mid-flight owes one answer, not two:
  the error that killed it, latched, both to the operations it was
  holding and to the ones that arrive afterwards. `canceled` is a
  stop token and belongs only to operations something cancelled — a
  thread that never started is the same rule from the other end: the
  operation that asked for it reports the code the system gave
  (`resource_unavailable_try_again` where the refusal carries no code
  of its own), never `canceled`.

## 8. Testing

- **Lock deterministic codes by equality**: everything corosio
  generates, plus kernel codes with reliable mappings on the platform
  under test. `BOOST_TEST(ec == std::errc::bad_file_descriptor)`.
- **Platform-variant codes assert only that a genuine error arrived**
  (option failures, fsync-on-pipe, resize-read-only), with a comment
  naming the variance.
- **Platform-variant *premises* get gated, not weakened**: if the
  operation legitimately succeeds somewhere (IOCP dual-stack
  acceptors accepting `v6_only`, Darwin's permissive `getsockopt`),
  gate the whole assertion with `#if` and a comment — don't leave an
  assertion that intermittently passes.
- Contracted codes compare by `errc` equality on every platform —
  raw-value pinning is reserved for kernel codes outside the contract
  list.
- Closed-object behavior is tested on **every channel**: returned
  codes and awaited completions
  (`co_await` + `ioc.run()` + a `done` flag so the test cannot pass
  vacuously).
- Error paths count as coverage targets: exercise the reachable ones
  (wrong-direction I/O, huge offsets, bind conflicts, pipe sync);
  document the rest as fault-injection-only. Run the gcc coverage
  build — it doubles as a second-compiler gate.

## 9. Documentation

- Javadocs state the channel: `@return The error code, empty on
  success.` for returns, with the codes named in prose; `@throws`
  only for root setup, contract violations, the convenience
  constructors, and the run loop (including `stop()` on Windows). Never
  a stale `@throws` on a `noexcept` function.
- Examples and doc snippets model checked usage — `if (auto ec = ...)`
  early-return — and never discard a `[[nodiscard]]` result, even
  where a pragma would let it compile. Fragments before `connect`
  simply omit `open()`.
- Example `if` bodies are real statements (`co_return;`), never a
  comment alone — a comment-only body silently swallows the next
  statement.
