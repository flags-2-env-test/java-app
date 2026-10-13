------------------------- MODULE SourceRootAdmissionCredit -------------------------
EXTENDS Naturals, TLC

(* Bounded single-flow root admission protocol.
   Rejected malformed candidates never consume the sole committed root token.
   A successful DATA publication and token commit share a single lock.
   AllowPreclaim is a deliberately unsafe mutant to test invariant coverage.
   This is a protocol model, not a C11 implementation-refinement proof. *)
CONSTANT AllowPreclaim
ASSUME AllowPreclaim \in BOOLEAN

VARIABLES rootUsed, published, rejects, cancelled

vars == <<rootUsed, published, rejects, cancelled>>

Init ==
    /\ rootUsed = FALSE
    /\ published = 0
    /\ rejects = 0
    /\ cancelled = FALSE

RejectMalformed ==
    /\ rejects < 2
    /\ rejects' = rejects + 1
    /\ rootUsed' = IF AllowPreclaim THEN TRUE ELSE rootUsed
    /\ UNCHANGED <<published, cancelled>>

CommitValid ==
    /\ ~cancelled
    /\ ~rootUsed
    /\ published = 0
    /\ rootUsed' = TRUE
    /\ published' = 1
    /\ UNCHANGED <<rejects, cancelled>>

Cancel ==
    /\ ~cancelled
    /\ cancelled' = TRUE
    /\ UNCHANGED <<rootUsed, published, rejects>>

Next == RejectMalformed \/ CommitValid \/ Cancel
Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ rootUsed \in BOOLEAN
    /\ published \in 0..1
    /\ rejects \in 0..2
    /\ cancelled \in BOOLEAN

NoBurnedRootCredit == rootUsed = (published = 1)
AtMostOneAdmission == published <= 1

=============================================================================
