# Hungry isolated actors: dedicated-thread idle, await, select, I/O, and GC contract

> **Status: proposed design only.** The C++/LLVM actor runtime is still a scaffold. None of the scheduling, GC, actor continuation, or I/O policies below are implemented or benchmark-certified. This is a runtime execution contract for future work, **not** an Oreslang syntax addition.

## Decision in one sentence

A *hungry* isolated actor **retains exclusive ownership of its native carrier thread and (when provisioned) its reserved CPU even while it awaits**; **running** is independent of **ownership**. With no runnable work, the thread normally **parks in the kernel** rather than burning CPU, and resumes on an explicit wake. A hungry actor is *not* automatically a busy-spinning actor.

Long-latency I/O (e.g. 100–200 ms) is performed by an asynchronous I/O subsystem or explicitly bounded blocking-I/O worker, not by holding the actor's realtime thread in an uninterruptible userland wait. Waiting is a legitimate actor state, not a reason to hand the dedicated thread to a different actor.

## Three independent resources

1. **Actor-owned state**: one owner for mutable state, validated ownership/borrowing, mailbox transfers across boundaries. Actor isolation does not by itself imply an OS process boundary.
2. **Carrier thread lease**: a 1:1 lifetime assignment between a hungry actor and its native OS thread. Never run another actor on this thread in the strict dedicated profile. An asynchronous continuation resumes only under the same actor execution lease.
3. **CPU/core reservation**: separately admitted OS placement/affinity and (if requested) isolated physical-core allocation, including SMT siblings. The reservation can persist while the thread sleeps. A pinned but non-isolated CPU may run other tasks during the wait, disturbing caches and the wake tail.

Trust, actor isolation, and compilation strategy remain orthogonal. Exclusive CPU and realtime priority are only permitted to **trusted**, supervisor-admitted actors, never granted by source code.

## Actor state machine

```text
 CREATED ─> ADMITTED ─> WARMING ─> RUNNABLE ─> EXECUTING
                           ^           ^          |
                           |           |          +---- await unresolved / select none ready
                           |           |                        |
                           |           |                    SUSPENDED
                           |           |                        |
                           |           +--- completion/channel/timer/cancel -> READY
                           |                                    |
                           |                                    v
                           |                              RUNNABLE/EXECUTING
                           |
                      IDLE_PARKED  <---- no executable work after a bounded poll
                           |
                fresh event/timeout/cancel (wake sequence changes)
                           |
                      RUNNABLE
 ALL STATES ── stop / fatal fault / supervisor restart ──> STOPPING ──> STOPPED
```

**Actor** suspension and **carrier** parking are separate: `await` suspends a continuation; the carrier parks only when *no eligible actor work, control messages, or completions* are runnable. A pending continuation must not occupy or spin on the thread merely because it exists.

**Fast paths:** `await` on an already-resolved future continues inline; a `select` with a ready case executes that case; neither needs a park/wake transition.

**Blocking select:** means blocking the *actor continuation*, not necessarily issuing a blocking syscall on the dedicated carrier. Timers, channel readiness, I/O completions and cancellation are registered before considering parking. No explicit `await` or `yield` is required in user-level `select`.

## What the OS thread does

| Condition | Strict dedicated profile | Optional adaptive profile |
| --- | --- | --- |
| Runnable actor continuation/messages | Execute under actor's exclusive lease | Same |
| No ready work, expected very short gap | Poll bounded number of iterations or microseconds; check control/cancellation | Same, tuning may differ |
| Pending 100–200 ms network/storage I/O | **Kernel-park** (futex/eventfd/epoll style), retain thread/CPU reservation | Kernel-park; optional supervisor-directed CPU loan with documented tail-latency cost |
| External completion | Producer publishes event using release ordering, increments wake epoch, signals; carrier wakes, acquires event, checks generation | Same |
| Actor-local housekeeping | None on the critical latency core by default | Only preemptible, bounded, opt-in maintenance |
| Shutdown/cancellation | Wake even if no I/O completion; fence stale callbacks and dispose once | Same |
| User tries to perform unbounded blocking FFI/sync I/O | Reject in strict profile or route through bounded I/O service | Offload if permitted; still never silently block/cancel-unsafely |

Linux can put a blocked realtime `SCHED_FIFO` thread to sleep; when it becomes runnable, it rejoins the runnable queue with its configured priority. This is **not** a deterministic wake-latency guarantee. Scheduler policy, affinity, CPU isolation, IRQ/noise, page faults and kernel configuration all matter.

## `await` and mailbox processing: non-reentrancy by default

Default `reentrancy = "serial"`:

- Once an actor handler suspends on a future, another ordinary user handler must **not** run and mutate actor state before the suspended handler completes or cancels. This avoids surprising state interleavings across `await`.
- Incoming mailbox messages may be *enqueued* concurrently through the bounded messaging subsystem, but are not processed by user handlers during that suspension.
- Supervisor control messages, cancellation, fatal errors, and completion signals are processed by the carrier's control plane, not by starting another user handler. Completion/cancellation must be able to wake the actor.
- The actor can hold several completed events queued for later; bounded capacities and backpressure apply.
- This serial model can delay unrelated requests if the current handler awaits a slow operation. That is intentional and must be measured: design high-throughput actors as pipelines or split independent work into additional actors.

Potential future opt-in `reentrancy = "interleave-on-await"` may allow *different* user turns while the first continuation is suspended, but still **never concurrently** run two turns on the actor's thread or access actor-local state from another thread. Interleaving changes observable semantics; require explicit documentation, ownership checks and separate conformance cases before implementing it. This document **does not approve** that behavior yet.

## I/O architecture and a 150 ms request example

```text
 Dedicated hungry actor CPU                         I/O / housekeeping CPUs
 ──────────────────────────                        ──────────────────────────
 (1) build owned request                           async poller/io_uring or
 (2) enqueue bounded I/O descriptor  ────────────> bounded blocking-I/O broker
 (3) save state-machine continuation
 (4) bounded empty-queue poll; park in kernel       I/O pending ~150ms
        [sleep; no actor CPU cycles]                ...
 (5) producer posts completion       <──────────── immutable/owned response
 (6) wake epoch increments; unpark
 (7) verify operation generation and ownership
 (8) resume single actor lease and process data
 ```

The I/O subsystem does not borrow actor-local mutable memory across threads; it accepts owned/pinned, lifetime-validated transfer buffers or copies under a declared cost policy. A timeout or cancellation does not free an in-flight buffer while the kernel/worker can still write to it. On late completion, drop/return the result safely using **operation IDs + generation/epoch fences**. Failure paths must deliver one terminal outcome (completed / canceled / timed-out / faulted), never multiple.

Linux `io_uring`, nonblocking sockets + epoll and completion eventfds are possible adapters. They are **backend choices**, not an Oreslang semantic guarantee: some I/O operations or OS combinations may still use kernel helpers or blocking workers. Avoid a dedicated actor thread issuing unbounded synchronous `read`, `recv`, `connect`, DNS calls, or host FFI in the strict profile.

## The parking/waking correctness rule

A correct waiter protocol must be immune to **lost wakeups** and ABA/reused generation IDs:

1. Each logical operation has a monotonically distinct ID/generation; each carrier has a wake sequence.
2. A completion producer writes the event into a bounded queue, publishes it (release), **then** advances the wake sequence and signals the waiter as necessary.
3. The carrier acquires and drains eligible events, snapshots the wake sequence, checks all queues and stop flags, and only parks with **compare-and-sleep** against that sequence.
4. If a signal occurs during the race between queue-empty and sleeping, the comparison rejects the sleep. On a spurious wake, re-check predicates; do not assume an event exists.
5. Cancellation and supervisor stop use the **same wake path**. The consumer validates operation generation and terminal state before resuming.
6. The callback/publication path must never execute Oreslang user functions or access the private actor heap.

On Linux, a futex wait offers the atomic expected-value comparison and sleep needed for this protocol. The exact primitives and memory-order proof belong in the future C++ implementation; the steps above are requirements, **not runnable pseudocode**.

## Idle strategies (configurable, not new source keywords)

| Policy | Behavior | Use |
| --- | --- | --- |
| `park` | Recheck queues, then kernel sleep immediately | Power-efficient, long waits; simplest safe default |
| `spin_then_park` | Poll for a bounded **duration**, then kernel sleep | **Recommended default** for hungry actors with mixed HPC and I/O |
| `spin` | Busy-poll continuously until runnable/shutdown, while still checking cancellation/control | Explicit opt-in for dedicated CPUs and measured ultra-low latency |
| `elastic_park` | Release/loan **CPU reservation** to other admitted work while actor sleeps | Throughput mode; not permitted when strict exclusive-core latency guarantee is requested |

`spin_then_park` should start with a **measured tunable** short threshold (e.g. 10 µs *as an experiment*, not a universal constant). We do not attempt to predict a 150 ms network wait precisely: after the brief poll, park. Avoid a loop that burns an entire physical core for arbitrary I/O delay.

A strict exclusive-core actor may keep the CPU reservation while parked: the CPU can enter an idle state, but OS/firmware effects and caches are not guaranteed constant. For a workload with exceptionally strict wake deadlines, continuous spinning is a conscious power/thermal tradeoff, not the default meaning of `hungry`.

## Garbage collection / memory maintenance

**Default for strict `no_gc_pauses`**: **never start tracing/compacting GC or arbitrary actor-local maintenance in response to `await`, `select`, or idle parking**. Pausing now does not mean that work will finish before an unpredictable completion wakes the actor.

- Use warm preallocated actor-owned arenas or bounded region allocators, with deterministic disposal, pre-reserved continuation slots and mailbox storage.
- Compile ownership/drop proofs into cleanup operations. Cross-thread reclamation or resource destruction should occur only after safe ownership transfer or an audited epoch/quiescence protocol.
- Cyclic graphs (`Ref<T>`/`WeakRef<T>`) need explicit lifecycle reclamation strategy. No-GC mode cannot silently leak them indefinitely or claim that cycles always disappear.
- Defer costly diagnostics, logging, heap scans and independent cleanup to **non-realtime support threads** after transferring genuinely unshared resources; support threads cannot mutate the live actor heap.
- An actor using tracing GC can offer an **opt-in relaxed profile** with incremental idle collection, but it cannot claim strict no-GC pause behavior, and each step must have a bounded budget and immediate wake/cancel responsiveness.
- A paused continuation is still a live reference root. Never reclaim its reachable state simply because its thread is sleeping.

Thus **no**: the parked strict hungry actor thread does not automatically "do any GC." It does zero actor work until a wake, apart from an explicitly chosen bounded spin window.

## Example deployment policy — proposed TOML, not yet parsed

```toml
[actors.pricing]
trust = "trusted"
isolation = "isolated"
execution = "hungry"
reentrancy = "serial"

[actors.pricing.thread]
lease = "dedicated"
idle_policy = "spin_then_park"
spin_before_park_us = 10          # example to benchmark, not a promised bound

[actors.pricing.cpu]
reservation = "exclusive"        # host admission; retain while parked
release_on_await = false
affinity = [2]
realtime_scheduler = "fifo"
priority = 20

[actors.pricing.memory]
mode = "bounded_arena"
gc = "disabled"
hot_path_allocation = "forbidden"

[actors.pricing.io]
strategy = "async_broker"
unbounded_sync_io = "reject"
max_inflight = 64
queue_capacity = 256
```

These are prospective **deployment settings**, not implemented `.oreslangc.cfg.toml` syntax. Fail admission if mandatory affinity/isolation/RT/memory constraints cannot be met; never silently claim an exclusive core without OS confirmation.

## Verification matrix (future C++ tests)

- **Immediate completion:** `await`/select ready fast path does not park; no lease handoff.
- **100–200 ms delay:** queue an owned async request, actor parks (bounded measured CPU usage), wakes and resumes on same dedicated thread and permitted CPU.
- **Wake race:** stress completion before, during and after parking; no missed or double completion (include spurious wakes).
- **Cancellation/shutdown:** unblock a parked carrier promptly; operation generation fencing, memory safety and idempotent cleanup.
- **Message ordering:** incoming messages enqueue; strict serial handler does not reenter while previous handler awaits.
- **Backpressure:** full request/completion/mailbox queues produce explicit bounded failure or awaitable backpressure; never silently drop replies.
- **Isolation:** I/O worker and notifier cannot dereference private actor objects or concurrently run actor handlers.
- **No-GC strictness:** instrument allocations, allocator slow paths, page faults, GC entry/exit and destructor tails; fail if a forbidden hot-path event occurs.
- **OS scheduling:** verify effective affinity, CPU/SMT isolation, scheduler policy and CPU reservation *before and after sleep*.
- **Tail latency:** record idle-to-wakeup and completion-to-handler distribution (p50/p95/p99/p99.9/max), parked CPU time, wake rate, power, context switches and impact of `spin` vs `park`.
- **Policy interactions:** no starvation of supervisor/cancellation; strict realtime never secretly schedules unrelated actors or GC on the reserved carrier.
- **Kernel / runtime matrix:** Linux versions, PREEMPT_RT versus standard kernels, cpuset and RT throttling, I/O subsystem capabilities, NUMA and C-state changes.

Do not infer hard realtime or a no-GC-pause guarantee from the presence of an actor, `SCHED_FIFO`, `mlockall`, or a successful no-contention microbenchmark.

## References

- Linux realtime scheduler semantics: https://man7.org/linux/man-pages/man7/sched.7.html
- Linux futex compare-and-sleep: https://man7.org/linux/man-pages/man2/FUTEX_WAIT.2const.html
- Linux asynchronous I/O completion queues: https://man7.org/linux/man-pages/man7/io_uring.7.html
- Linux CPU isolation limitations and cpusets: https://www.kernel.org/doc/html/latest/admin-guide/cpu-isolation.html
- [Existing LLVM native runtime scaffold](../src/main/cpp/oreslang/runtime/README.md)
- [Java/LLVM source parity contract](../docs/SOURCE_PARITY.md)

## Hardening notes and executable prototype boundary (2026-10-08)

The [safety audit](HUNGRY_ACTOR_AUDIT.md) is normative for **what must be
proven before enabling** the strict hungry-actor profile. It identifies a
fundamental distinction: a **notification is not a completion**. Publication
of a typed completion/control event must happen before a wake, and the waiter
must acquire/recheck the underlying predicate after waking. A full user
mailbox must **not** block stop/cancel or completion delivery: control and
completion paths need reserved capacity and explicit backpressure.

An experimental Linux `ParkingSignal` notification primitive is available
under `src/main/cpp/oreslang/runtime/`. It uses nonblocking eventfd and poll
rather than the proposed futex epoch. It demonstrates a lost-wake-safe
check-and-park pattern and rejects a second/different waiter thread. The
primitive is **park-only**; it is **not** the actor runtime, an async I/O
backend, spin-then-park, lock-free queue, no-GC allocator, CPU reservation
manager or realtime guarantee. Eventfd performs kernel syscalls and can
internally lock. Its error path currently throws, so it is not certified for
strict no-allocation realtime hot paths.

**Serial-await deadlock rule:** a handler awaiting a response that only the
same suspended actor can produce must not remain silently deadlocked. The
future runtime/compiler should detect the self-request cycle, emit a typed
deadlock/reentrancy error or apply an explicitly approved concurrent handling
policy. This does not authorize implicit reentrancy.

**CPU semantics:** Linux `SCHED_FIFO` priority remains configured across
blocking and waking, but affinity is not physical-core reservation; the CPU
may have kernel/IRQ/firmware interruptions, power-state transitions and
hotplug/cpuset changes. A supervisor must verify effective policy on
admission and after material OS changes. A parked thread continuing to
'own' its CPU is a host resource-management promise, not a kernel guarantee
that no other instruction can execute on that physical core.

**Realtime cancellation:** strict mode cannot depend exclusively on actor
cooperation. The out-of-band supervisor/control channel and watchdog must
make progress even when ordinary handler queues are full, when user code is
CPU-bound, or when an in-flight I/O buffer remains owned by the kernel.
Generation fencing and once-only terminal transitions require implementation
and race tests before code may reuse/release resources.
