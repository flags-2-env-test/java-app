# Actor response contracts: unary, streaming, and transport

Status: language/API proposal; the new actor modifiers and callback types below
are not compiler-admitted yet. Canonical message domains use `define match
pattern`. Java implemented that spelling in PR #684; LLVM pipeline fixtures now
use it too.

## Separate three choices

1. Execution and isolation: serial/shared/private/untrusted actor scheduling.
2. Output contract: fire-and-forget messages, unary request/reply, or a stream.
3. Transport adapter: in-process mailbox, HTTP, TCP, or WebSocket.

Transport does not choose output cardinality. Ordinary message actors have no
implicit response. Stream requests allow zero, one, or many outputs; exactly one
successful response is an explicit unary contract, never the default.

## Unary: a returned value is the response

Proposed source syntax:

```ores
define unary actor Increment as
    receive(int request): int {
        return request + 1;
    }
end
```

The runtime resolves that request's unary reply future exactly once after the
handler's successful completion. Failure/cancellation settles it with a terminal
error, rather than inventing a value. Callers use an explicit unary request API;
plain send does not silently allocate a reply future. No streaming emission is
available through this unary reply capability. A unary result may itself be a
collection: returning five values in an array is still one response.

## Stream: callbacks carry zero or more responses

Proposed source syntax:

```ores
define stream actor Expand as
    receive(int request, StreamReply<int> reply): void {
        await reply.next(request + 1);
        await reply.next(request + 2);
        await reply.next(request + 3);
        await reply.next(request + 4);
        await reply.next(request + 5);
        reply.complete();
        return;
    }
end
```

`StreamReply<T>` is a runtime-issued capability for one response stream, exposing
its callbacks through `next`, `complete`, and `fail`. It is not an arbitrary
closure that can enter another actor's state. A caller registers `on_next(T)`,
`on_complete()`, and `on_error(Error)` with an explicit stream request API. Each
of the five `next` calls dispatches one `on_next` event on the consumer's owning
scheduler lane. FIFO order is per stream; output from separate streams may
interleave. Request correlation is assigned by the runtime and not inferred from
the payload or the immediate upstream actor.

`next` returns an admission future: await provides bounded backpressure, not a
claim that the consumer has already processed the value. Completion closes this
request's output stream, not the actor or socket. It follows all admitted output
in FIFO order. Exactly one terminal event is permitted; emission after terminal
completion rejects. `fail` settles the same stream; caller cancellation fences
later emission and settles blocked producers.

Zero responses are valid: call `complete` without calling `next`. Bare `return;`
only ends handler execution. It does not emit a value or implicitly complete a
response stream. This initial contract requires completing/failing before the
handler finishes; returning with an open reply is a protocol error and fails the
stream. Deferred, long-lived responses require a separately owned reply session
with explicit lifetime/cancellation rules, not a captured callback that escapes
the actor turn.

## Ordinary message actors and pipeline callbacks

An ordinary `define actor` declares its inbox domain and uses
`receive(InboxMessages m): void`. It may explicitly send
zero or more messages to typed actor refs or emit onto an owned output port. It
has no implicit request/reply relationship. An actor-level subscription is a
long-lived event stream, distinct from a response stream associated with one
request; a runtime-issued output-port handle carries its callbacks.

Pipeline callbacks describe routing, not RPC replies.

See the explicit transform role and directional-domain example below.


A handler may call forward/backward zero or many times per input. Forward selects
an authorized outgoing edge; backward follows an authorized ingress/reverse
route. Neither means 'respond to the original caller'. A separate StreamReply
capability is necessary for correlated responses. Source/sink roles restrict
available ports. Finishing one handler never closes the entire pipeline;
end-of-input, port completion, actor stop and graph completion are separate
lifecycle events.

## HTTP, TCP, and WebSocket

- HTTP: one request has one HTTP response, whose body may be a stream of chunks
  (including SSE events). Five callback items may be five chunks/events, not five
  independent HTTP responses. Unary adapts one result to one finite body;
  streaming adapts output to a streaming body and completes it explicitly.
- TCP: bytes have no inherent request/message boundaries. Framing and correlation
  belong to an explicit protocol adapter; a connection does not imply unary RPC.
- WebSocket: bidirectional messages form a session stream. A frame may trigger
  zero or many outgoing frames, and unsolicited events are allowed. Unary RPC
  over WebSocket requires an explicit correlation protocol.

These adapters preserve their transport's semantics while using the same actor
scheduling and output capability rules.

## Existing fifteen-actor fixture

The current LLVM bootstrap receive ABI returns i64. Source `return value` is
stored as `last_result` and observed by `expect_pipeline`, which also checks hop
counts and the terminal result. It is instrumentation/state observation in this
restricted slice, not a reply emitted to another actor. Actual messages are sent
by `self.forward` and `self.backward`. This fixture therefore does not establish
unary return semantics or general streaming callback support.

After void stream callback lowering is available, replace these value returns
with `return;` and move the fixture's state observation into a separate test-only
inspection mechanism. Keep the tested legacy ABI visible until that change is
implemented, rather than presenting unsupported syntax as an executable example.

## Required verification before stream admission

Test zero/one/five outputs for one input, independent simultaneous correlations,
FIFO per stream, bounded queues and suspended admission, consumer cancellation,
actor stop/restart, stale capabilities, duplicate terminal events, emission after
completion and thrown errors. Ensure one terminal event and exact credit release
without entering consumer guest code recursively on the producer's carrier.
Java and LLVM must share the same observable contracts.

## Stateful actors and directional ingress

Use the recent actor state contract as the target source model:

```ores
state { value: int = 0; }
```

This is a fixed typed schema, not an untyped Map. `value` declares a typed key;
there is no `key` keyword in the inspected Java parser. The separate dynamic-key
form is `state: Map<string, int>;`. `self.handle_state(critical |state| -> { ... })`
is the sealed access path. The capability may not escape; copied scalar results
may leave the callback. Do not access a bare persistent `value` in receive or
send/await while relying on a state loan that has escaped its scope. Effect rows
and actor affinity remain subject to the existing state verifier.

A local increment method can return a scalar snapshot without replying to a
message. Local method returns, unary receive returns, and emitted stream outputs
are different operations:

```ores
increment(): int {
    return self.handle_state(critical |state| -> {
        state.value = state.value + 1;
        return state.value;
    });
}
decrement(): int {
    return self.handle_state(critical |state| -> {
        state.value = state.value - 1;
        return state.value;
    });
}
```

For the tested incrementing data pipeline, use the incoming payload:

```ores
update_from(int incoming): int {
    return self.handle_state(critical |state| -> {
        state.value = incoming + 1;
        return state.value;
    });
}
```

Merely incrementing each actor's independent counter ignores the incoming
payload and is a different computation. With update_from, source input 1 becomes
2, the sink reaches 16, and the reverse traversal reaches 30 at the source.

Proposed runtime-dispatched hooks (not compiler-admitted yet):

| Hook | Scheduler-issued ingress | Typical output |
| --- | --- | --- |
| `receive` | Ordinary mailbox/external input | Explicit send, port emission, or an opt-in response |
| `receive_forward` | Authorized upstream edge carrying a forward delivery | Downstream forward emission |
| `receive_backward` | Authorized downstream edge carrying a reverse delivery | Upstream backward emission |

The names describe message travel, not an ambiguous actor 'ahead'. Dispatch
must use runtime routing authority, not a payload tag such as Signal.Result.
A middle actor may implement all three hooks. They share the actor's normal
serial/concurrency admission lane; directional hooks do not introduce parallel
access to state. They are actor entry hooks, not callable public methods that
allow bypassing mailbox and route checks. Different ports may later declare
different nominal input patterns.

For a middle stage, the proposed forward hook would be:

```ores
receive_forward(PipeForwardMessages m): void {
    do match m over {
        on Signal.Data as n {
            const output = self.update_from(n);
            await self.forward(Signal.Data(output));
        }
    }
    return;
}
```

Its reverse hook uses the same scoped state helper, then backward(Signal.Result).
Stage0 receives external Data through receive and returning Result through
receive_backward. The sink receives forward Data, updates its state, and sends
Result backward. These callbacks emit zero or many messages; their void return
only ends execution.

Responding to the request origin remains a separate action. Unconditionally
responding after both the Data and Result arms would emit two responses for the
source's two deliveries; that is valid only if intentionally using a stream
reply contract. For this round-trip example, only the terminal Result handler
emits the final response through reply.next(output), then reply.complete(). A
bare receive hook does not acquire a reply capability merely because it received
a message. self.respond, if retained as a convenience spelling, must require the
same live correlated capability and cannot mean 'send backward'.

The current fifteen-actor native fixture still uses legacy scalar fields and
its restricted i64 receive observation ABI. Migrating that executable fixture to
state callbacks and these directional hooks requires integrating the actual
state source lowering with pipeline dispatch. This proposal is not evidence
that either backend executes the combined source syntax yet.

## Explicit pipe roles and directional message domains

The selected transform declaration is:

```ores
define transform pipe actor Stage1<
    forward_messages=PipeForwardMessages,
    backward_messages=PipeBackwardMessages
> as
    // Scoped state, methods, and both required directional hooks.
end
```

Each `define match pattern` denotes a nominal closed message domain whose
inhabitants are the listed enum variants. A receive parameter uses that domain
name directly: `receive_forward(PipeForwardMessages m): void` and
`receive_backward(PipeBackwardMessages m): void`. No generic `Message` placeholder
or implicit per-actor type substitution appears in new source syntax.

Compiler obligations for a transform pipe actor:

- Both directional domains are declared, known match-pattern types.
- Exactly one receive_forward and exactly one receive_backward hook exist;
  neither can be static. Their parameter domain is exactly the declared domain
  for that direction; their result is void. State helpers may return copied
  scalars without establishing a reply contract.
- Missing, duplicate or mismatched directional hooks are compile errors.
  Ordinary receive cannot satisfy either hook. An ordinary actor is rejected
  as a pipe stage; the role is not inferred merely from method names.
- Match arms are checked against the hook's domain, including exhaustiveness.
  For these definitions, Signal.Result is rejected in the forward hook and
  Signal.Data in the backward hook.
- self.forward checks the forward output domain; self.backward checks the
  backward output domain. Routing methods remain sealed, actor-affine intrinsics.
- Pipeline construction validates role ordering and both directed edges at
  every adjacency: A.forward_output -> B.forward_input, and
  B.backward_output -> A.backward_input. The initial version requires matching
  nominal domains, not merely equal int payload types or a structural wire shape.
- Runtime dispatch is based on pinned routing/ingress authority, never on an
  enum tag supplied by guest code. Ordinary send cannot masquerade as a pipe
  directional delivery.

For a source/readable endpoint there is no upstream pipe ingress; it emits
forward and receives backward, plus optional external receive/on_start input.
For a sink/writable endpoint there is no downstream pipe ingress; it receives
forward and may emit backward. Transform stages require both ingress hooks.
Response streams to a caller remain separate correlated capabilities, not
implicit replies from every directional delivery.

The two-domain shorthand uses the same forward domain for forward input/output
and the same backward domain for backward input/output. It permits transformations
of values and variant selection within those domains. A stage that changes
nominal message types needs four explicit port domains in a subsequent extension:
forward_in, forward_out, backward_in, backward_out. Do not hide this restriction
by allowing untyped Message values or unchecked output coercions.

Implementation status: the native bootstrap's old receive(Message m) was a
parser special case whose domain came from the actor's messages declaration.
Its executable single-domain fixtures now use receive(DeclaredDomain m), and
wrong parameter domains/Message placeholders reject before IR emission. This
removes the hidden alias; it does not implement the proposed role syntax, two
independent domains, scoped-state lowering or directional hook dispatch. Those
require AST, type admission, runtime codec/port tables and compiler lowering to
be integrated across backends. The stateful role example is a design artifact,
not a new executable fifteen-stage test.
