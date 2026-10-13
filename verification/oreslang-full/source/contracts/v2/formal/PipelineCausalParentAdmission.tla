------------------- MODULE PipelineCausalParentAdmission -------------------
EXTENDS Naturals, FiniteSets, TLC
\* Bounded host-side DATA admission linearization for two independent flows.
\* The production compiler's current SourceActorBootstrap only uses one flow;
\* this abstraction checks the intended per-flow causal admission invariant.
CONSTANT AllowForgedParent
ASSUME AllowForgedParent \in BOOLEAN
Flows == {"a", "b"}
MaxHops == 3
None == MaxHops + 1

VARIABLES phase, last, pending, terminal, committed, forged
vars == <<phase, last, pending, terminal, committed, forged>>

Init ==
    /\ phase = [f \in Flows |-> 0]
    /\ last = [f \in Flows |-> 0]
    /\ pending = [f \in Flows |-> None]
    /\ terminal = [f \in Flows |-> FALSE]
    /\ committed = {}
    /\ forged = FALSE

Reserve(f, parent) ==
    /\ ~terminal[f]
    /\ phase[f] < MaxHops
    /\ pending[f] = None
    /\ parent \in 0..MaxHops
    /\ pending' = [pending EXCEPT ![f] = parent]
    /\ UNCHANGED <<phase, last, terminal, committed, forged>>

Reject(f) ==
    /\ pending[f] # None
    /\ pending' = [pending EXCEPT ![f] = None]
    /\ UNCHANGED <<phase, last, terminal, committed, forged>>

Admit(f) ==
    /\ ~terminal[f]
    /\ pending[f] # None
    /\ phase[f] < MaxHops
    /\ (pending[f] = last[f] \/ AllowForgedParent)
    /\ forged' = IF pending[f] # last[f] THEN TRUE ELSE forged
    /\ phase' = [phase EXCEPT ![f] = @ + 1]
    /\ last' = [last EXCEPT ![f] = phase[f] + 1]
    /\ pending' = [pending EXCEPT ![f] = None]
    /\ committed' = committed \cup {<<f, phase[f] + 1>>}
    /\ UNCHANGED terminal

Cancel(f) ==
    /\ ~terminal[f]
    /\ terminal' = [terminal EXCEPT ![f] = TRUE]
    /\ pending' = [pending EXCEPT ![f] = None]
    /\ UNCHANGED <<phase, last, committed, forged>>

Next ==
    \/ \E f \in Flows, parent \in 0..MaxHops: Reserve(f, parent)
    \/ \E f \in Flows: Reject(f)
    \/ \E f \in Flows: Admit(f)
    \/ \E f \in Flows: Cancel(f)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ phase \in [Flows -> 0..MaxHops]
    /\ last \in [Flows -> 0..MaxHops]
    /\ pending \in [Flows -> 0..None]
    /\ terminal \in [Flows -> BOOLEAN]
    /\ committed \subseteq (Flows \X (1..MaxHops))
    /\ forged \in BOOLEAN

NoForgedCausalAdmission == forged = FALSE
NoDuplicateCompletion ==
    \A f \in Flows:
        phase[f] = Cardinality({i \in 1..MaxHops: <<f,i>> \in committed})
LastTracksCommit ==
    \A f \in Flows: last[f] = phase[f]
CancelledHasNoPending ==
    \A f \in Flows: terminal[f] => pending[f] = None
=============================================================================
