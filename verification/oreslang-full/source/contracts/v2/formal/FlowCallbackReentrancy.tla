---------------- MODULE FlowCallbackReentrancy ----------------
EXTENDS TLC
\* Bounded abstraction of two host-owned flow tables. A publisher holding
\* table A's per-flow lock must never invoke ANY table registry operation
\* until its callback returns; otherwise B's nested publisher can reenter
\* A and self-deadlock. This models the callback contract, not C refinement.
CONSTANT AllowNestedTableEntry
ASSUME AllowNestedTableEntry \in BOOLEAN
VARIABLES inCallbackA, enteredB, rejectedB, reenteredA, finished
vars == <<inCallbackA, enteredB, rejectedB, reenteredA, finished>>
Init ==
    /\ inCallbackA = FALSE
    /\ enteredB = FALSE
    /\ rejectedB = FALSE
    /\ reenteredA = FALSE
    /\ finished = FALSE
StartA ==
    /\ ~inCallbackA
    /\ ~finished
    /\ inCallbackA' = TRUE
    /\ UNCHANGED <<enteredB, rejectedB, reenteredA, finished>>
TryB ==
    /\ inCallbackA
    /\ ~enteredB /\ ~rejectedB
    /\ IF AllowNestedTableEntry
       THEN enteredB' = TRUE /\ UNCHANGED rejectedB
       ELSE rejectedB' = TRUE /\ UNCHANGED enteredB
    /\ UNCHANGED <<inCallbackA, reenteredA, finished>>
CycleBackToA ==
    /\ inCallbackA /\ enteredB /\ ~reenteredA
    /\ reenteredA' = TRUE
    /\ UNCHANGED <<inCallbackA, enteredB, rejectedB, finished>>
EndA ==
    /\ inCallbackA /\ rejectedB /\ ~finished
    /\ inCallbackA' = FALSE
    /\ finished' = TRUE
    /\ UNCHANGED <<enteredB, rejectedB, reenteredA>>
Next == StartA \/ TryB \/ CycleBackToA \/ EndA
Spec == Init /\ [][Next]_vars
TypeOK == /\ inCallbackA \in BOOLEAN
          /\ enteredB \in BOOLEAN
          /\ rejectedB \in BOOLEAN
          /\ reenteredA \in BOOLEAN
          /\ finished \in BOOLEAN
NoNestedRegistryEntry == ~enteredB
NoCallbackLockCycle == ~reenteredA
NoFalseAdmission == ~(enteredB /\ rejectedB)
=============================================================================
