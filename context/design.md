# Coroutine-First I/O Execution Model

Optimized for coroutines-first, other models secondary

**Definition:** An `io_object` reflects operating-system level asynchronous operation which completes through a platform-specific reactor

**Requirement:** A coroutine signals execution affinity by participating in the affine awaitable protocol

**Requirement:** An `io_object` resumes the calling coroutine by using its dispatcher, provided during await_suspend

**Implication:** Because the `io_object` always resumes through the dispatcher, a coroutine at `final_suspend` is guaranteed to be executing on its own executor's context. When returning to a caller with the same executor (`caller_ex_ == ex_`), symmetric transfer can proceed without a `running_in_this_thread()` check—pointer equality is sufficient. When executors differ, `final_suspend` must dispatch through the caller's executor.

## Flow Diagrams

A _flow diagram_ signifies a composed asynchronous call chain reified as a series of co_awaits.

* Coroutines are indicated by `c`, `c1`, `c2`, ...
* `io_object` are represented as `io`, `io1`, `io2`, ...
* Foreign awaitable contexts are represented as `f`, `f1`, `f2`, ...
* `co_await` leading to an `io_object` are represented by arrows `->`
* `co_await` to a foreign context are represented by `\`
* Executors are annotated `ex`, `ex1`, `ex2`, ...

For example, this call chain:
```
c -> io
```
reflects (ordinals are left out when singular):
```
task c(io_object& io) { co_await io.op(); }
```


While this call chain:
```
c1 -> c2 -> io
```
reflects:
```
task c1(io_object& io) { co_await c2(io); }
task c2(io_object& io) { co_await io.op(); }
```

A coroutine can await a foreign context, to send work elsewhere:
```
c1 -> c2 -> io
        \
         f
```
for example a CPU-bound task to not block the io thread:
```
task c1(io_object& io) { co_await c2(io); }
task c2(io_object& io) {
    co_await f();
    co_await io.op();
}
```

A coroutine can be launched on an executor:
```
run_on( ex, c() );

task c(io_object& io) { co_await io.op(); }
```
A coroutine launched this way has _executor affinity_, or just _affinity_. It is a lazy coroutine, and its call to resume is dispatched through `ex`. The execution model offers an invariant: the later call to resume `c` happens through the executor `ex`, obtained through `co_await io.op()` (affine awaitable protocol).

Affinity propagates forward through the affine awaitable protocol. The initial coroutine in the chain captures the typed executor by value, and propagates it by reference forward. A coroutine created in this fashion captures the type-erased affine dispatcher and propagates it forward. This continues to the `io_object` where it is stored and used later when the operation completes.

The first coroutine in a flow diagram representing I/O execution has affinity unless otherwise noted. Subsequent coroutines in a flow diagram representing I/O execution
have inherited affinity unless annotated with an exclamation point indicating that
their affinity has changed:
```
c1 -> c2! -> io
```
represents
```
task c1(io_object& io) {
     co_await run_on( ex2, c2(io) );
}
task c2(io_object& io) {
    co_await io.op();
}
```
The `run_on` function is notional, representing an awaitable means of awaiting a new coroutine on the same `io_object`, on a different executor `ex2`. What must happen here is the following:
- `c1` is launched on its implied executor `ex`
- `c2` is launched on `ex2` explicitly
- `io` captures the type-erased `ex2`
- When the I/O completes, `c2` is resumed through `ex2`
- When `c2` returns, `c1` is resumed through `ex1`
