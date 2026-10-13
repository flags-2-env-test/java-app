--------------------- MODULE ConcurrentPipelineFlows ---------------------
EXTENDS Naturals, FiniteSets, TLC
\* Two independently correlated requests share one linear route.
\* Abstracts the native SourcePipelineFlows registry and its lock-protected
\* cancel/commit boundary, not physical delivery or guest actor execution.
CONSTANT AllowLatePublish, AllowDoubleCommit
Flows == {"a", "b"}
MaxHops == 5
ASSUME /\ AllowLatePublish \in BOOLEAN
       /\ AllowDoubleCommit \in BOOLEAN

VARIABLES phase, pending, state, frozen, admitted
vars == <<phase, pending, state, frozen, admitted>>

Init ==
    /\ phase = [f \in Flows |-> 0]
    /\ pending = [f \in Flows |-> FALSE]
    /\ state = [f \in Flows |-> "active"]
    /\ frozen = [f \in Flows |-> 0]
    /\ admitted = {}

Reserve(f) ==
    /\ state[f] = "active"
    /\ ~pending[f]
    /\ phase[f] < MaxHops
    /\ pending' = [pending EXCEPT ![f] = TRUE]
    /\ UNCHANGED <<phase, state, frozen, admitted>>

Publish(f) ==
    /\ state[f] = "active"
    /\ pending[f]
    /\ phase' = [phase EXCEPT ![f] = @ + 1]
    /\ admitted' = admitted \cup {<<f, phase[f]>>}
    /\ pending' = [pending EXCEPT ![f] = FALSE]
    /\ UNCHANGED <<state, frozen>>

Cancel(f) ==
    /\ state[f] = "active"
    /\ state' = [state EXCEPT ![f] = "cancelled"]
    /\ frozen' = [frozen EXCEPT ![f] = phase[f]]
    /\ pending' = [pending EXCEPT ![f] = FALSE]
    /\ UNCHANGED <<phase, admitted>>

Finish(f) ==
    /\ state[f] = "active"
    /\ phase[f] = MaxHops
    /\ ~pending[f]
    /\ state' = [state EXCEPT ![f] = "complete"]
    /\ UNCHANGED <<phase, pending, frozen, admitted>>

\* Deliberately unsafe mutations must be rejected by TLC in their cfgs.
LatePublish(f) ==
    /\ AllowLatePublish
    /\ state[f] = "cancelled"
    /\ phase[f] < MaxHops
    /\ phase' = [phase EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<pending, state, frozen, admitted>>

DoubleCommit(f) ==
    /\ AllowDoubleCommit
    /\ state[f] = "active"
    /\ phase[f] > 0
    /\ ~pending[f]
    /\ phase[f] < MaxHops
    /\ phase' = [phase EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<pending, state, frozen, admitted>>

Next ==
    \/ \E f \in Flows: Reserve(f)
    \/ \E f \in Flows: Publish(f)
    \/ \E f \in Flows: Cancel(f)
    \/ \E f \in Flows: Finish(f)
    \/ \E f \in Flows: LatePublish(f)
    \/ \E f \in Flows: DoubleCommit(f)

Spec == Init /\ [][Next]_vars
TypeOK ==
    /\ phase \in [Flows -> 0..MaxHops]
    /\ pending \in [Flows -> BOOLEAN]
    /\ state \in [Flows -> {"active", "cancelled", "complete"}]
    /\ frozen \in [Flows -> 0..MaxHops]
    /\ admitted \subseteq (Flows \X (0..(MaxHops - 1)))
NoLateCommit ==
    \A f \in Flows: state[f] = "cancelled" => phase[f] = frozen[f]
NoDuplicateCommit ==
    \A f \in Flows:
        phase[f] = Cardinality({i \in 0..(MaxHops - 1): <<f,i>> \in admitted})
TerminalHasNoReservation ==
    \A f \in Flows: state[f] # "active" => ~pending[f]
NoPrematureCompletion ==
    \A f \in Flows: state[f] = "complete" => phase[f] = MaxHops
=============================================================================
