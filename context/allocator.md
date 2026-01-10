# Coroutine Frame Allocation Model

## Problem Statement

Control allocation of every coroutine frame in a coroutine chain, including internal coroutines created by the framework (e.g., `async_run_task` in `async_run`). The API must specify the frame allocator BEFORE the coroutine is created—once the coroutine call expression evaluates, it's too late.

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
4. **Internal coroutines exist** — `async_run_task` (and potentially others) are implementation details
5. **Allocator must be specified BEFORE creation** — Once the coroutine call expression evaluates, it's too late

## Proposed Solution

An execution_context has a default frame allocator

the `io_object` provides the default allocator from its execution_context

`async_run` and `run_on` use the default allocator from the I/O object unless overridden, or no I/O object

