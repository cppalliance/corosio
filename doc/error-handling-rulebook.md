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
- **Misuse** — violations of a documented precondition the caller
  could have checked: calling on a closed object, invoking twice,
  breaking a stated invariant — **throw**.
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
- Never both channels for one operation. Never `std::error_code&`
  out-params. Never a throwing/non-throwing overload pair.

## 2. Return Shapes

| Situation | Shape |
|---|---|
| Sync, no payload | `[[nodiscard]] std::error_code f(...) noexcept` |
| Sync, with payload | `[[nodiscard]] capy::io_result<T> f(...) noexcept` |
| Async | the awaitable completes with `io_result<...>` — initiators never throw |

`io_result<T>` is `std::tuple<std::error_code, T...>`, so results
destructure; the sync and async surfaces share one idiom:

```cpp
if (auto ec = sock.open())                        // no payload
    co_return;
auto [ec, pos] = f.seek(0, file_base::seek_end);  // payload
auto [ec2, ep] = make_endpoint("10.0.0.1:80");    // factory
```

The payload always rides in the return — the `make_*` factories
return `io_result<T>` with a default-constructed payload on failure.
Every awaitable-returning initiator is `[[nodiscard]]`: a discarded
awaitable is a silent no-op that also drops any pre-set completion.

Constructors have no return channel to choose, so rule 1 admits only
three constructor failures: misuse, guarded by a public pre-check
(`local_endpoint`'s `max_path_length`, `io_context`'s
`thread_pool_size`); the wrapped codes of a code-returning path the
constructor abbreviates (`tcp_acceptor(ctx, ep)` over
`open`+`bind`+`listen`, `endpoint(str)` over `make_endpoint`, thrown
as `std::system_error` carrying the code the piecewise spelling
returns); and root setup for which no code-returning spelling can
exist (`io_context` backend creation, allocation).

Root setup is everything the backend needs — completion port, ring,
wakeup channel, thread pool — and all of it is built during
construction, so a system that refuses any of it throws from the
constructor instead of from the first operation, and the failed
construction leaves nothing open. An initiator may then assume that
infrastructure exists, which is what makes "initiators never throw"
reachable at all.

## 3. The Classification Test

Ask: **can the caller reliably prevent the failure by checking state
they own, race-free, before the call?**

- **No** → normal error. File existence (TOCTOU), a port's
  availability, a peer's connection state, or whether a string parses
  cannot be pre-checked.
- **Yes** → precondition, misuse, throw. The canonical example:
  `local_endpoint(path)` throws on an over-long path because the
  limit is the public `max_path_length` constant — one integer
  comparison, no grammar, no race. The pre-check *is* the
  non-throwing API.

Corollary: an operation with no realistic failure on a valid open
object (`size()`, `release()`, `available()`) stays on the throwing
channel — an `io_result` return would tax every correct call site to
carry a bit that is always "you have a bug".

## 4. The Closed-Object Condition

One condition, one code — `errc::bad_file_descriptor` — delivered
through whichever channel the operation already uses:

- Error-returning sync methods (`bind`, `listen`, `shutdown`,
  `assign`, file `resize`/`sync_*`/`seek`) **return** it.
- Throwing-channel methods (`size`, `release`, `available`,
  `set_option`, `get_option`) **throw**
  `std::system_error(errc::bad_file_descriptor)`. Not `logic_error`:
  closed-ness is legitimately observable in async teardown (another
  coroutine closes to cancel), the platform spells it as a system
  condition (`EBADF`), and one exception type keeps the surface
  coherent.
- Async initiators **pre-fail the awaitable**: set `aw.ec_` and
  return it; `await_ready` treats a pre-set `ec_` as
  ready-with-result, so the operation completes immediately without
  dispatching. `connect()`'s auto-open reports an open failure the
  same way. The backends enforce the same contract at their operation
  entries — a closed object reaching a backend read/write/wait
  completes with `bad_file_descriptor` before touching the kernel —
  so the code is deterministic on every path, including the
  devirtualized native facades.

Genuine logic errors unrelated to object state stay `logic_error`:
service not installed, launcher invoked twice, zero-sized thread pool.

## 5. Composite Operations

An operation that performs several fallible steps reports the first
failure through its own single channel — sub-steps never add a
second channel:

- `connect()` opens the socket automatically when needed; an open
  failure surfaces through the connect completion, so callers need no
  explicit `open()` before `connect`.
- The convenience constructors wrap `open`+`bind`+`listen` and throw
  the code the failing step returned.
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
  casts. Reserve them for calls whose outcome is asserted downstream
  (hostile-input tests, best-effort bench teardown).
- Unused names — parameters kept for signature clarity, structured
  bindings partially consumed, `#if`-gated uses — are declared
  `[[maybe_unused]]`, never silenced with a void cast.

## 7. Code Values and Portability

- **The user vocabulary is `std::errc`** — corosio defines no error
  namespace of its own.
- Codes corosio generates itself are deterministic contracts:
  `invalid_argument` (parsers, signal_set, negative seek),
  `bad_file_descriptor` (closed objects, descriptor validation),
  `wrong_protocol_type` / `address_family_not_supported` (adoption
  rejection), `value_too_large` (off_t guard, truncated hostname),
  `filename_too_long`,
  `already_connected` (`connect_pair` on an open socket),
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
  stop token and belongs only to operations something cancelled.

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
  codes, thrown `system_error`s, and awaited completions
  (`co_await` + `ioc.run()` + a `done` flag so the test cannot pass
  vacuously).
- Error paths count as coverage targets: exercise the reachable ones
  (wrong-direction I/O, huge offsets, bind conflicts, pipe sync);
  document the rest as fault-injection-only. Run the gcc coverage
  build — it doubles as a second-compiler gate.

## 9. Documentation

- Javadocs state the channel: `@return The error code, empty on
  success.` for returns; `@throws std::system_error
  \`errc::bad_file_descriptor\` if ...` for throws. Never a stale
  `@throws` on a `noexcept` function.
- Examples and doc snippets model checked usage — `if (auto ec = ...)`
  early-return — and never discard a `[[nodiscard]]` result, even
  where a pragma would let it compile. Fragments before `connect`
  simply omit `open()`.
- Example `if` bodies are real statements (`co_return;`), never a
  comment alone — a comment-only body silently swallows the next
  statement.
