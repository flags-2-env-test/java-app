------------------------- MODULE SourceActorCapabilityLifetime -------------------------
EXTENDS Integers, FiniteSets, TLC

(*
  Bounded abstraction of SourceActorBootstrap's registry + producer gate.
  One scheduler slot can host three different actor generations.
  Public send resolves the token and pins a producer lease while holding
  the registry lock. Stop closes new admission, waits for old leases, then
  retires the generation. A caller may retain/submit old tokens indefinitely.

  This is a safety model, NOT a C memory-model proof or implementation
  refinement. It does not model scheduler callbacks, pipeline flows, or
  cancellation after a successful DATA enqueue.
*)
CONSTANT AllowStaleAlias
ASSUME AllowStaleAlias \in BOOLEAN
Tokens == 1..3

VARIABLES current, issued, closed, leases, retired, probeToken, probeAccepted

vars == <<current, issued, closed, leases, retired, probeToken, probeAccepted>>

Init ==
    /\ current = 0
    /\ issued = 0
    /\ closed = FALSE
    /\ leases = 0
    /\ retired = {}
    /\ probeToken = 0
    /\ probeAccepted = FALSE

Spawn ==
    /\ current = 0
    /\ issued < 3
    /\ current' = issued + 1
    /\ issued' = issued + 1
    /\ closed' = FALSE
    /\ leases' = 0
    /\ probeToken' = 0
    /\ probeAccepted' = FALSE
    /\ UNCHANGED retired

Pin(t) ==
    /\ t \in Tokens
    /\ current = t
    /\ ~closed
    /\ leases < 2
    /\ leases' = leases + 1
    /\ probeToken' = t
    /\ probeAccepted' = TRUE
    /\ UNCHANGED <<current, issued, closed, retired>>

Reject(t) ==
    /\ t \in Tokens
    /\ (current # t \/ closed)
    /\ probeToken' = t
    /\ probeAccepted' = FALSE
    /\ UNCHANGED <<current, issued, closed, leases, retired>>

Close ==
    /\ current # 0
    /\ ~closed
    /\ closed' = TRUE
    /\ probeToken' = 0
    /\ probeAccepted' = FALSE
    /\ UNCHANGED <<current, issued, leases, retired>>

Leave ==
    /\ leases > 0
    /\ leases' = leases - 1
    /\ probeToken' = 0
    /\ probeAccepted' = FALSE
    /\ UNCHANGED <<current, issued, closed, retired>>

Retire ==
    /\ current # 0
    /\ closed
    /\ leases = 0
    /\ retired' = retired \cup {current}
    /\ current' = 0
    /\ closed' = FALSE
    /\ probeToken' = 0
    /\ probeAccepted' = FALSE
    /\ UNCHANGED <<issued, leases>>

(*
  Deliberate ABA mutant: an old token is mistaken for the next occupant
  of the same slot. TLC must reject it through NoStaleAdmission.
*)
UnsafeSlotOnlySend ==
    /\ AllowStaleAlias
    /\ current # 0
    /\ retired # {}
    /\ \E old \in retired:
         /\ probeToken' = old
         /\ probeAccepted' = TRUE
    /\ UNCHANGED <<current, issued, closed, leases, retired>>

Next ==
    \/ Spawn
    \/ \E t \in Tokens: Pin(t)
    \/ \E t \in Tokens: Reject(t)
    \/ Close
    \/ Leave
    \/ Retire
    \/ UnsafeSlotOnlySend

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ current \in ({0} \cup Tokens)
    /\ issued \in 0..3
    /\ closed \in BOOLEAN
    /\ leases \in 0..2
    /\ retired \subseteq Tokens
    /\ probeToken \in ({0} \cup Tokens)
    /\ probeAccepted \in BOOLEAN

NoFreeWithProducerLease == (leases > 0) => (current # 0)

NoGenerationReissue == (current = 0) \/ ~(current \in retired)

NoStaleAdmission ==
    probeAccepted => (probeToken = current /\ ~(probeToken \in retired) /\ ~closed)

=============================================================================
