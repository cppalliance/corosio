# Corosio Implementation Lifetime Contract

When an I/O implementation may be touched, by whom, and when its
memory may be reused or freed.

## 1. The Invariant

Every I/O object (socket, acceptor, file, descriptor, Windows handle,
resolver, signal set, timer) is a handle to an *implementation* (`io_object::implementation`) owned
by its service's `object_pool`. The implementation carries an
intrusive count, `refs_`.

> **Nothing touches an implementation after its count reaches zero,
> and nothing raises a count that has reached zero.**

At zero, `retire()` runs and the pool recycles the implementation:
its memory is handed to the next `construct()`. A late access does
not crash; it corrupts an unrelated object. The invariant has no
benign violations.

Debug builds on the reactor backends poison recycled state, so a field
`reuse()` fails to reset reads as garbage. Elsewhere `reuse()` asserts
the fields the contract guarantees. Release builds rely on the
contract alone.

## 2. Who Holds a Reference

Anything that can reach an implementation holds an `object_ref` for
exactly as long as it can reach it. The holders are:

| Holder | Taken | Released |
|---|---|---|
| The handle | `construct()` (the count starts at 1) | `destroy()` |
| An in-flight operation | before the op is handed to the kernel, a thread or a queue | by its completion, after the last touch of the impl |
| A reactor registration | when the descriptor can produce events | when no reactor pass can still deliver one (§5.1) |
| A kernel-owned multishot op | when armed | on its terminal completion (§5.2) |
| A teardown pin | `object_pool::shutdown()` | after every visitor has run, outside the pool lock |
| A cross-thread work item (resolver, file) | before the hand-off | moved into the completion op, never dropped on the worker |
| An accept's peer | `construct()`: in the initiator before anything is armed (IOCP), or in the completion once the kernel returns the descriptor (reactor, io_uring) | passed to the caller's handle on success, `destroy()`ed on failure or drain |

An op that is merely embedded in the implementation (`rd_`, `wr_`,
`conn_`) is part of it, not a separate lifetime. The *reference* it
carries while in flight is what matters.

## 3. Acquiring a Reference

A reference may be created in exactly two ways:

1. **From a held reference.** Copying an `object_ref`, or
   `object_ref(this)` inside a member reached through a handle or an
   op that already holds one. The count is at least one and cannot
   reach zero concurrently.
2. **From a raw pointer, with `try_acquire`.** A context that reached
   the implementation through a raw pointer holds no reference, and
   the count may already be zero. That covers a registry walk, a
   kernel completion naming it, or an OS callback. Such a context
   must use `try_acquire`, which refuses to revive a zero count, and
   must give up that path when it fails.

`object_ref(p)` from a raw-pointer context is the one way to break
the invariant's second half. Every revival bug found so far was this
pattern.

## 4. Releasing a Reference

- The last touch of an implementation precedes the release that may
  retire it. Read what you need (scheduler pointer, continuation,
  executor) into locals under the lock, then release.
- A release that may reach zero runs with no lock held that
  `retire()` → `recycle()` could need. In practice that means no
  lock at all: recycle takes the pool mutex, and pool shutdown takes
  service and scheduler locks after it.
- A completion must not write implementation state that close resets
  (endpoints, `fd`) once the implementation was closed underneath it.
  Close, cancel and release flag every op they reach before touching
  the descriptor, and a flagged completion writes nothing. That covers
  calls serialized with the completion; a close concurrent with one is
  outside the objects' thread-safety contract.

## 5. Raw Pointers Outside the Process's Control

Some pointers are held where an `object_ref` cannot live: in the
kernel, in an OS thread pool, in an executor's queue. Each needs an
explicit owner of the reference that stands in for it.

### 5.1 Reactor registrations (epoll, kqueue, select)

`epoll_event.data.ptr` and the select snapshot name a descriptor
state with no reference. A reactor pass that has already read an
event can deliver it after another thread closes the socket.

Rule: **the reactor holds a reference for as long as a pass can
deliver an event for the descriptor.** While the descriptor is
registered, the handle's reference serves: every path that releases a
handle's last reference deregisters first. Deregistration passes a
reference to the scheduler. If a pass is running, the scheduler parks
it until that pass ends. Then, or at once if no pass is running, it
moves to the queued invocation if the descriptor is queued, and is
released otherwise.

Registration publishes the descriptor to the kernel only after its
state (fd, scheduler, mutex mode) is initialized. Fields a queued
invocation reads change only under the descriptor mutex.

Sockets, acceptors and posix descriptors share one implementation of
this protocol (`reactor_io_core`), so the rule is stated once.

### 5.2 io_uring operations

`user_data` names an op. Ordinary ops carry their impl reference
until their completion handler runs.

Rule for multishot ops: **a multishot op is owned by the kernel
while armed.** It holds the implementation reference. Its terminal
completion (no `IORING_CQE_F_MORE`) is dispatched like any other op,
and its handler either re-arms it or returns it to the owner's free
list and drops that reference. The owner remembers the live arming by
address only (`armed_`); deliveries from any other arming are stale
and closed on arrival. Close disowns the arming, then cancels it. An
arming is admitted under the ring mutex, re-checking that the owner is
not closing, so it either precedes the cancel in the submission queue
or is refused.

### 5.3 IOCP and OS callbacks

An `OVERLAPPED` inside an op is held by the kernel until its packet is
dequeued; the op's reference covers that. Callbacks on OS threads
(`GetAddrInfoExW`) take their own reference and are counted, so that
teardown can wait for them (§6).

Once an initiator hands a pending op to the scheduler
(`on_pending`), another thread may complete it and recycle the
implementation, so every touch of the op or the implementation,
including the post-submit cancellation re-check, comes first.

The object handle's thread-pool wait callback reaches its state by raw
pointer but only claims an armed wait, whose op holds the reference;
every rundown drains queued callbacks before claiming for
cancellation, so no callback outlives the reference that covers it.
A wait engages its stop callback before it is published as armed.

### 5.4 Executor queues

A continuation handed to an executor is held by reference until the
executor dequeues it. It must live in storage that outlives that
window. The awaiting coroutine's frame does; an op's embedded storage
does not, because the op is consumed when its completion is.

So every awaitable owns its continuation and passes it to the
implementation; ops only point at it. The continuation carries the
owner of the awaiting coroutine's chain (`env->owner`), so a holder
that gives it up at teardown passes it to `capy::abandon`, which
unwinds the chain from its owner. An op embedded in an
implementation that is queued in the scheduler (rather than handed to
the kernel) still holds a reference, like any other in-flight op.

### 5.5 Descriptor numbers

A descriptor number outlives the object that closed it only as a
number, and the process may hand it to the next open. Anything that
names an implementation's descriptor by number after a close could
reach an unrelated file:

- **Pool workers** (posix files) enter a gate around each system call;
  close defers the `::close` to the last worker out.
- **io_uring** cancels an object's ops by their user data, never by
  descriptor, so the cancel needs no number. A close or release first
  flushes the submission queue so no queued SQE still names the
  descriptor; if the flush fails, the queued SQEs that name the
  descriptor become NOPs and their ops complete cancelled. A cancel that cannot be queued is
  deferred by op and queued ahead of the next op's SQE, which may reuse
  the address.
- **`SO_ERROR` probes** for error waits are skipped once the op is
  flagged cancelled on io_uring and the IOCP wait reactor, since
  reading the option clears it. The IOCP wait reactor probes under the
  lock close takes before `closesocket`. The reactor backends probe
  under the descriptor mutex, and close clears the descriptor's number
  under that mutex before closing it, so a probe never reads a reused
  number.

## 6. Teardown

`io_context` destruction runs every service's `shutdown()` newest
first, then deletes the services in the same order. Services created
after the scheduler come before it; the timer service, which the
scheduler's constructor creates, comes after it. The contract for that
sequence:

0. **Timers first.** Every scheduler shuts the timer service down
   before it drains, so work a timer's teardown posts is drained too.
1. **Cancel.** Each service cancels or closes every live
   implementation. Its pool's `shutdown()` pins each one with
   `try_acquire` and switches the pool to delete-on-zero.
2. **Quiesce.** Before any service is destroyed, every agent outside
   the process's control must have stopped reaching implementations:
   - io_uring: cancel everything in the kernel and wait until every
     submitted op has completed;
   - IOCP: drain every outstanding packet (`pending_io_`), stop the
     wait reactor, and wait for counted OS callbacks;
   - reactors: no pass is running, which holds because no thread is
     inside `run()` when the context is destroyed, and every queued
     descriptor has been drained;
   - thread pools: joined. A queued item that owns a suspended
     coroutine is not run; its `destroy_` abandons the coroutine. An
     item without one runs its `func_` on the shutting-down thread.
3. **Drain.** A reactor scheduler first settles the registration
   references parked by `retire_descriptor`. The scheduler then
   destroys every queued op without resuming it, which releases the
   references those ops hold. Destroying an op
   abandons the coroutine it holds, and the owners those abandons post
   land in the same queue. So every chain started on this context is
   unwound during the drain, and the handles in its frames release
   their references.
4. **Free.** `~object_pool` deletes whatever remains. After step 3 a
   remaining reference can belong only to a handle the application
   still holds, or to a frame of a chain started on another context.
   The contract excludes both: I/O objects are destroyed before their
   context, and work that crosses contexts is finished before either
   is destroyed. The count is pinned before each delete, so references
   an implementation holds on itself cannot re-enter `retire()`.
5. **Late work.** Work posted after the drain, for example from a
   service's destructor, is destroyed when the scheduler is deleted,
   after every service created after it.

A step that cannot complete must not be skipped silently. For
example, a cancel that cannot be submitted is the one case where
io_uring does not wait, because nothing could end the wait.

## 7. Recycling

- `reuse()` restores every field the next owner could observe. On the
  io_uring, file and IOCP backends, fields the contract guarantees are
  already reset (descriptor closed, no op in flight, not queued) are
  asserted rather than reset, so a contract violation fails in debug
  instead of being masked. The reactor backends reset the descriptor
  fields and assert only the reference count and the queued flag.
- State that a late completion can legitimately write (§4, last
  bullet) is reset, not asserted.
- Free lists owned by an implementation (op pools, accept nodes) may
  survive recycling. Their entries hold no reference.

## 8. Conformance

How the code meets each rule:

| Rule | Status |
|---|---|
| §2 op references | Conforms on all backends, including a signal set's queued completion. |
| §3 raw-pointer acquisition | Conforms. `object_pool::shutdown` uses `try_acquire`; the uring re-arm keeps the reference its arming already held. |
| §4 release ordering | Conforms after the review fixes (`invoke_deferred_io`, posts after unlock). |
| §4 completion writes | Conforms for serialized close on all backends: close, cancel and release flag the connect op, and `reuse()` resets the endpoints. |
| §5.1 reactor | Conforms, sockets, acceptors and posix descriptors alike (`reactor_io_core::abandon_all`). Close deregisters, then hands its reference to `retire_descriptor`; whether the descriptor is queued is consulted only in `hand_off`. |
| §5.2 multishot | Conforms. The fault harness still fakes terminal completions, so it swallows the real one that follows. |
| §5.3 IOCP | Conforms: socket and acceptor initiators re-check cancellation before `on_pending`, as the handle initiators do. IOCP is checked only by Windows CI. |
| §5.5 descriptor numbers | Conforms: posix file gate, uring flush before close or release, cancelled waits skip the probe on uring and the IOCP wait reactor. |
| §5.4 executor queues | Conforms: every awaitable owns its continuation; no op embeds one. |
| §6 teardown | Conforms on all backends, timers first everywhere; implemented per backend with no shared statement. |
| §7 recycling | Mixed. Some `reuse()` methods assert fields others reset. |
