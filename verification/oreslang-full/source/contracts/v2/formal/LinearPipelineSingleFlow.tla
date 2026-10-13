------------------------- MODULE LinearPipelineSingleFlow -------------------------
EXTENDS Integers, FiniteSets, TLC

(*
  Finite safety specification of the first Java/LLVM 3-actor pipeline:
  root -> source -> transform -> sink -> transform -> source.
  Phase 6 is successful source completion; -1 is terminal admission failure.
  The model separates reservation from mailbox admission. It does not model
  arbitrary scheduling, crash recovery, multi-flow concurrency or a proof of
  implementation refinement.
*)
CONSTANT AllowReplay
ASSUME AllowReplay \in BOOLEAN

VARIABLES phase, reservation, admissions

vars == <<phase, reservation, admissions>>

Init ==
    /\ phase = 0
    /\ reservation = "none"
    /\ admissions = 0

ReserveRoot ==
    /\ phase = 0
    /\ reservation = "none"
    /\ phase' = 1
    /\ reservation' = "root"
    /\ UNCHANGED admissions

ReserveHop(expected, actor) ==
    /\ phase = expected - 1
    /\ admissions = expected - 1
    /\ reservation = "none"
    /\ phase' = expected
    /\ reservation' = actor
    /\ UNCHANGED admissions

Commit ==
    /\ reservation # "none"
    /\ admissions' = admissions + 1
    /\ reservation' = "none"
    /\ UNCHANGED phase

Reject ==
    /\ reservation # "none"
    /\ phase' = -1
    /\ reservation' = "none"
    /\ UNCHANGED admissions

FinishSource ==
    /\ phase = 5
    /\ reservation = "none"
    /\ admissions = 5
    /\ phase' = 6
    /\ UNCHANGED <<reservation, admissions>>

(*
  Deliberately unsafe mutant: a callback applies one admission twice
  without a matching pipeline reservation. TLC must produce a counterexample
  when AllowReplay is TRUE.
*)
DuplicateCompletion ==
    /\ AllowReplay
    /\ phase = 2
    /\ reservation = "none"
    /\ admissions = 2
    /\ admissions' = admissions + 1
    /\ UNCHANGED <<phase, reservation>>

Next ==
    \/ ReserveRoot
    \/ ReserveHop(2, "source")
    \/ ReserveHop(3, "transform")
    \/ ReserveHop(4, "sink")
    \/ ReserveHop(5, "backward")
    \/ Commit
    \/ Reject
    \/ FinishSource
    \/ DuplicateCompletion

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ phase \in ({-1} \cup 0..6)
    /\ reservation \in {"none", "root", "source", "transform",
                       "sink", "backward"}
    /\ admissions \in 0..5

NoDoubleAdmission ==
    (phase \in 0..5) =>
       (admissions + (IF reservation = "none" THEN 0 ELSE 1) = phase)

NoPrematureCompletion ==
    (phase = 6) => (admissions = 5 /\ reservation = "none")

TerminalHasNoReservation ==
    (phase = -1) => (reservation = "none")

=============================================================================
