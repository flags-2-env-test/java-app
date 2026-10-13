# Hungry actor idle, I/O, and realtime safety audit

**Scope:** Native C++/LLVM hungry isolated actor proposal and one Linux notification/parking primitive. **Status:** partial proof-of-concept; not a production actor scheduler. This audit does not certify GC freedom, hard realtime, CPU exclusivity, lock-free mailboxes or async actor continuations.

## Audit findings and dispositions

| Severity | Finding | Hardening / acceptance requirement |
| --- | --- | --- |
| Critical | Notification can race with deciding to sleep | Linux nonblocking `eventfd` retains readiness until read; caller checks published ready/stop predicates before and after poll. Tested before-wait and competing notifications. A completion queue is **still required**. |
| Critical | A wake is not a completion payload | Notification is a **hint** only. Producer publishes an owned result (release) before signaling, consumer reads it (acquire). Spurious notifications cannot run user code. Tests cover false wake. |
| Critical | Ordinary bounded mailbox full can prevent stop/completion | Control, cancellation, and continuation-completion traffic need **reserved capacity / separate queues**; a full user mailbox must not prevent supervisor stop. Not implemented; design gate. |
| Critical | Cancellation vs I/O completion can double-resume/free in-flight buffers | Operation-specific generation/epoch and single terminal CAS state, kernel/worker buffer ownership until acknowledged, cancellation wake and late-completion drop. Not implemented; future race test. |
| High | Two waiters can consume one another's eventfd notifications | `ParkingSignal` rejects concurrent waiting and a later call from a different `std::thread::id`. The supervisor must own thread lifetime; thread ID alone is not a secure never-reused generation token. |
| High | Reusing actor thread while suspended breaks isolation/serial semantics | Strict dedicated carrier never executes another actor; suspended handler blocks normal actor turns by default. Runtime still needs single-execution lease and self-await/cycle deadlock detection. |
| High | Confusing CPU pinning with physical-core exclusivity | Affinity is **not** isolation or a guarantee that CPU cache is untouched. Check effective cpuset/SMT siblings, active isolation partition, IRQ and OS housekeeping, hotplug, cgroup changes and RT policy. |
| High | `SCHED_FIFO` busy spinning can starve stop/control | Use bounded spin-then-park by default; control checks within spin and compute checkpoints; RT watchdog/emergency out-of-band supervisor. Untrusted actors cannot request exclusive realtime privileges. |
| High | GC during an arbitrary idle gap races with completion | Strict profile never performs implicit tracing/compaction on parked carrier; paused continuation retains roots. Actual allocator, cycle reclamation and ownership proofs remain unimplemented. |
| High | Blocking syscall / FFI can steal realtime native thread | No unbounded blocking FFI in strict profile. Use async I/O broker and cancellation-safe owned/pinned buffers; some `io_uring` operations can still use helpers/block. |
| Medium | Thread parking while queue is full or notifier fails | Notification failure must be a visible infrastructure failure; no silent swallowed `EBADF`/`EIO`. `eventfd` `EAGAIN` is safe only because its counter already indicates readability. |
| Medium | Realtime path can allocate/throw despite no-GC intent | Current prototype constructs eventfd on startup but **throws exceptions on error**. Not realtime-certified; replace throwing hot paths with explicitly accounted statuses, preallocation and fail-closed supervisor before integration. |
| Medium | 32-bit futex wake sequences can wrap in high-throughput long-lived actors | This prototype uses 64-bit eventfd counter/readiness instead. Eventfd still isn't a message log and must never serve as an operation ID. Future futex design must prove rollover handling. |
| Medium | Wall-time timeout can be inaccurate | Wait timeout uses `steady_clock`, rounds sub-ms waits upward with `poll`, rechecks predicates on EINTR. No microsecond precision or hard bound is claimed. |
| Medium | Destroying signal while any producer/waiter is active leads to FD reuse and races | Only destroy after quiescing publishers and joining the owner thread. Explicitly documented lifetime contract; future managed handle needs a shutdown barrier and ref-counted ownership. |

## Current executable scope

`src/main/cpp/oreslang/runtime/ParkingSignal.{hpp,cpp}` is one Linux eventfd-based **notification** primitive, with:
- A single dedicated waiter claim guarded by an atomic flag; concurrent notifiers permitted.
- Nonblocking eventfd writes and poll sleeps; failed kernel syscalls fail explicitly.
- Release/acquire predicate synchronization required of the caller, not silently inferred from notification.
- Monotonic timeout, stop-first arbitration, EINTR/spurious-wake rechecks.
- Test cases for ready-before-wait, signal-before-park, a 150 ms simulated I/O completion, spurious notify, cancellation wake, thread migration rejection, timeout, and 4-producer notifications across repeated runs.
- CTest integration only on Linux; this does **not** modify the LLVM compiler or the actual application actor runtime (not implemented).

## Core concurrency invariants to prove next

1. **Linearized completion:** each (actor generation, operation generation) reaches exactly one terminal state: completed, timed out, canceled or faulted. Competing paths cannot resume it twice.
2. **No completion drops:** bounded queues must reserve control/completion capacity and expose backpressure. No silent overwrites or dropped wakeups when mailbox is full.
3. **No use-after-cancel:** outstanding I/O buffer remains owned/pinned until asynchronous kernel/user callback releases it; actor cancellation alone does not free it.
4. **No cross-thread heap access:** I/O workers and notifier threads cannot read or mutate actor-local Oreslang objects; transfer only typed owned immutable/copy buffers.
5. **No accidental reentrancy:** default serial handler continues before processing next normal turn. Actor self-request awaiting its own queued response must fail/cancel or invoke a separately approved reentrancy mechanism.
6. **Stop always deliverable:** watchdog, supervisor stop and cancellation must have an out-of-band unblocked path independent of user mailbox pressure and actor code cooperation.
7. **Hot path accounting:** no heap growth, tracing GC, loader/JIT, blocking FFI or unbounded finalizer on the strict carrier. Deterministic drops can still have latency and require budgets.
8. **No premature realtime claims:** warm-up, prefaulting, CPU/SMT topology, actual cpuset state and effective scheduler policy must be *observed* at runtime; a successfully called `pthread_setaffinity_np` alone is not sufficient.

## Additional risk considerations

- **Core occupancy:** when parked the core may enter idle power states or be reused by kernel/firmware interrupts. Exclusive allocation means administrative admission, not a cryptographic CPU lock. If a fully reserved sleeping CPU is desired, the host must keep other runnable processes away; do not assume `SCHED_FIFO` alone accomplishes that.
- **Latency tradeoff:** a `spin_then_park` threshold (10 µs in earlier design) is a benchmark hypothesis. The current primitive implements **park only**, not spin/affinity/RT admission.
- **Kernel and tooling:** Linux eventfd uses internal kernel serialization; the primitive is not a blanket lock-free runtime guarantee. Each wake incurs syscalls; benchmark busy-poll and futex alternatives later, accounting for lost wake/rollover proofs.
- **GC claims:** `gc=disabled` in a TOML sketch is not enforcement. Verify guest/native allocations, deferred drop latency, cyclic reference strategy, paused stack roots, and realtime page faults separately.
- **Cgroup and OS dynamics:** online CPU set, partition validity, IRQ affinity, RT throttling, SMT and NUMA can change over time; the supervisor must detect admission loss and either fail safely or downgrade only with explicit permission.

## Verification and non-claims

Local development may compile `ParkingSignal` with `-fsanitize=address,undefined`; GitHub CTest still must execute at an immutable Git tree SHA. A no-runner-allocated Actions failure is not test evidence.

**Does not exist yet:** actor scheduler integration, private heaps, native borrow checker, hot-path allocation bounds, a bounded lock-free mailbox, completion generations, spin-then-park policy, I/O broker, CPU exclusivity admission, realtime watchdog, native trusted actor AOT, or real GC strategy.

Detailed proposed semantics: [HUNGRY_ACTOR_IDLE_IO.md](HUNGRY_ACTOR_IDLE_IO.md).
Linux references: [eventfd(2)](https://man7.org/linux/man-pages/man2/eventfd.2.html), [poll(2)](https://man7.org/linux/man-pages/man2/poll.2.html), [sched(7)](https://man7.org/linux/man-pages/man7/sched.7.html), and [kernel CPU isolation](https://www.kernel.org/doc/html/latest/admin-guide/cpu-isolation.html).

## Incremental dedicated-thread carrier prototype (2026-10-08)

A subsequent implementation branch adds a C++-only `HungryCarrier` with:
- Preallocated 64-element MPSC mailbox (producers use CAS, one carrier reads),
  explicit backpressure instead of dropping messages.
- Strict serial handler ownership: an `await_io` step suspends the active turn,
  and queued ordinary messages cannot reenter before that turn resumes.
- A separate I/O broker worker and 2-slot completion channel. Exactly one
  operation is in flight per actor continuation. Completion carries a
  monotonically increasing generation and only resumes the matching turn.
- Dedicated stop/cancel notification that bypasses user-mailbox saturation;
  a canceled suspended turn cannot run its user continuation.
- Optional logical CPU pinning with effective affinity verification,
  rejecting unsupported/invalid requested CPUs rather than silently downgrading.
- Bounded actor-local POD arena with explicit exhaustion (no GC).
- Native C++ tests for a simulated 150-ms I/O wait, actual socket I/O
  performed off the carrier, saturated user mailbox, four concurrent producers,
  thread identity and failed CPU admission.

**Still not implemented**: parsing/lowering `define actor` into the carrier
ABI, full actor heap/borrow safety, kernel-cancelable asynchronous storage/
network transport, lifetime-safe cancellation of arbitrary blocking FFI,
multi-op broker dispatch, shared actors, physical core/cpuset/IRQ reservation,
`SCHED_FIFO` admission, hard realtime certification, deterministic cycle
reclamation or full fuzz/differential test coverage. The model allows one
in-flight IO operation at a time and deliberately exposes the limits of
serial non-reentrancy instead of claiming arbitrary concurrent async support.

The carrier's `BrokerOperation` callback is host-trusted and *not* a
sandbox for arbitrary untrusted source. Blocking callback cancellation
remains dependent on the callback returning. Do not promote this experiment
to a `no_gc_pauses`, `exclusive_core`, or `hard_realtime` production
profile until independent allocation, OS topology, buffer ownership and
latency evidence exists.

## Cancellation/completion terminal-state hardening

The C++ carrier now uses a single atomic `OperationTerminal` state with
`pending -> completed` **or** `pending -> canceled` CAS resolution. There is
only one in-flight operation for this serial actor prototype. A completion
is delivered only when the operation generation matches and the terminal
state is `completed`. Stop can win while the I/O worker is waiting, but
the worker must still quiesce before any broker-owned data is destroyed.

The implementation intentionally **rejects** requested production guarantees
`require_exclusive_core`, `require_strict_no_gc` and
`require_hard_realtime`, because physical CPU reservation, compiler-proved
GC freedom and bounded OS scheduling have NOT been implemented. Ordinary
logical CPU affinity remains optional and verified against the effective
thread mask. Non-lock-free atomic index hosts are also rejected rather than
silently implementing the supposed lock-free ring with a hidden mutex.

The CI workflow now builds and executes the native actor test under
AddressSanitizer and UndefinedBehaviorSanitizer in addition to regular CTest,
including repeated stop/completion interleavings. Sanitizer success is not a
formal proof of linearizability, lock-freedom, or memory isolation.

Remaining: complete ownership/pinned-buffer protocol for arbitrary
`io_uring`/epoll operations, external-callback lifetime/cancellation,
guest-code lowering and native actor source syntax, multi-inflight completion
ordering, no-GC deterministic drops and cyclic graphs, supervisor watchdog,
physical core/cgroup/IRQ admission, cold-to-warm latency and tail jitter
measurement, fault injection and long soak/TSan testing.

## Shutdown/send and ring-head readiness audit (2026-10-08)

New race review found three hazards that did not have explicit regression gates:

1. **In-flight `try_send` versus shutdown:** a producer can successfully
   enqueue just before another thread requests stop. The carrier may exit
   without executing that accepted turn; a lifecycle owner must not
   destruct the mailbox while entered send calls can still notify it.
   The native carrier now counts in-flight send operations; `join`
   quiesces those already-entered calls after stopping its workers.
   `CarrierStats.abandoned_on_stop` accounts for accepted turns not
   completed after joining. A successful `try_send` means ownership
   transfer, **not** execution guaranteed after concurrent stop.
   As in any raw-pointer C++ API, the caller must prevent **new**
   method invocations after it begins destroying the object; a counter
   inside an already-destroyed object cannot prevent use-after-free.
2. **Published queue count is not head readiness:** an MPSC producer can
   reserve the head and be preempted before publishing its slot. A later
   producer may publish its own slot. Looking only at an aggregate
   queued-count can keep the hungry carrier spinning despite the head
   being unreadable. The consumer now checks the **head sequence**
   with `has_ready()` before parking/waking; no independent queued
   counters are used for correctness. The ring remains *lock-free*,
   **not wait-free**: a stalled producer can still hold the head and
   delay later messages. A strict hard realtime design needs a bounded
   producer progress contract or a different queue architecture.
3. **Join or destruction from a worker:** skipping the self-`join`
   previously could destroy eventfds and queue storage while that
   same worker still used them. A self-join or self-destruction now
   terminates rather than continuing into a use-after-free.
   Lifecycle remains the supervisor's responsibility.

Tests now cover 100,000 ring recycle operations, FIFO and full-queue
boundaries, idle `join` without prior stop, and repeated concurrent
multi-producer stop/join races with terminal `accepted = completed +
abandoned_on_stop` accounting. Thread sanitizer, host resource
admission, real guest actor syntax, bounded kernel I/O cancellation and
a formal MPSC memory-order proof remain separate gates.


## Independent producer-handle lifetime gate

The C++ carrier now offers a copyable \`producer_handle()\` for producer
threads that can outlive the carrier object. Each handle owns a
\`shared_ptr\` to an independent control block with a **single atomic admission word** combining a closing bit and an
in-flight lease count. The send obtains an active lease using CAS only
while the closing bit is clear. The supervisor atomically sets the close
bit before stopping workers and waits for all pre-close leases before
freeing queues or eventfds. The common atomic gives a linearizable
admission-versus-close ordering point.
An already-copied handle can safely reject messages after destruction.

The old direct \`HungryCarrier::try_send()\` method still exists for the
internal native test harness and requires callers to maintain the raw
carrier's lifetime. Production callers should use preissued handles.
Creating the initial gate and copying handles involves ref-counting;
it is not certified for strict no-GC/hard-realtime hot paths. The
\`try_send\` operation still uses the preallocated MPSC queue after
entering the independent gate.

**Bounded progress limitation:** if an admitted producer stalls
indefinitely inside \`try_send\`, destruction intentionally waits for its
lease. A future supervisor needs stall detection and a bounded drain/
leak-safe quarantine rather than freeing referenced actor storage.
The lifetime gate prevents UAF; it cannot guarantee bounded shutdown
under arbitrary native-thread failure.

Regression tests copy handles to five producer threads and destroy
the actor while they are sending; after destruction every handle
must return false without touching freed memory. Verification must
include native CTest, ASan/UBSan and TSan.


## Isolated SPSC producer lanes: head-of-line stall mitigation

A strict low-latency actor may now use **one fixed-capacity SPSC ring
per registered producer**, alongside the existing MPSC ingress. The actor
polls lanes round-robin, and each producer publishes with a release tail
store after writing the slot. Other lanes make progress independently
if a producer is interrupted halfway through its own publication.
No unfinished slot may be consumed.

- Registration requires one supervisor before \`start\`; the mutex is never
  taken by the carrier's message processing loop. Reject registration
  after start and more than four lanes.
- Each lane has a move-only shared-lifetime gated handle and rejects
  *simultaneous* calls from multiple producers. Thread migration between
  sequential calls requires external synchronization.
- Fair rotation avoids always preferring the legacy MPSC lane under
  steady MPSC pressure, but **does not** promise a fixed bounded latency:
  OS preemption, handler execution time and wait/sleep jitter still apply.
- SPSC messages from different lanes have no single agreed global FIFO
  order; any stable cross-lane ordering contract would require a separate
  sequencer, not implied by this implementation.
- Deterministic test instrumentation interrupts lane A between slot write
  and tail publication; lane B must publish/consume while A is stalled.
  The normal \`try_push\` compiles with an empty inline hook.
- Producer-handle shutdown uses the same atomic close/lease arbitration
  already verified in PR #22. Carrier destruction remains supervisor-owned.

**Remaining hard-realtime blockers:** native actor codegen and borrow
checks, a trusted no-GC guest memory path, CPU/SMT/IRQ admission,
completion buffer ownership, bounded callbacks, physical load isolation,
and measured tail-latency proof.


## Enforce producer-gated API exclusively

The earlier native prototype retained a public \`HungryCarrier::try_send()\`
escape hatch requiring raw C++ object lifetime discipline. Although the
safe producer-handle path was tested, a caller could bypass it and race
destruction. That escape hatch is now private (\`enqueue_mpsc\`), and
the only public ingress paths are lifetime-gated \`ProducerHandle\` and
\`DedicatedProducer\`. All native tests use the gated API, and a compile-
time negative test rejects reintroduction of the raw public send method.

The supervisor must still own start/join/destroy and must not
concurrently invoke its methods while destroying the actor. Gate leases
ensure producers that were already admitted cannot access freed queue
storage even when producer handles outlive the carrier.
