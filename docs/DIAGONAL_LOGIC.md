# Diagonal Logic

## Introduction

Diagonal Logic is a new propositional calculus designed to be the
**minimum complete axiom set** for binary optimization.  It has three
primitives and three axioms — half the axiom count of classical
Hilbert systems — yet is complete for classical propositional logic.

## Primitives

| Symbol | Name          | Intuition                |
|--------|---------------|--------------------------|
| ⊕      | symmetric difference / XOR | the "diagonal"      |
| →      | implication   | the "off-diagonal"       |
| ⊥      | falsum        | the "anti-diagonal"      |

## Axioms (3)

```
A1:  A ⊕ A = ⊥        (idempotence to falsum)
A2:  A → A = ⊤        (self-implication is truth)
A3:  ⊥ → A = ⊤        (ex falso)
```

## Theorems (5)

```
T1:  ⊥ ⊕ A = A
T2:  A ⊕ ⊥ = A
T3:  A ⊕ B = B ⊕ A    (commutativity)
T4:  A ⊕ (B ⊕ C) = (A ⊕ B) ⊕ C   (associativity)
T5:  A → ⊥ = ¬A
```

## Completeness

**Theorem.** Diagonal Logic is complete for classical propositional logic.

**Proof sketch.** Classical logic can be encoded as follows:
- True: ⊤ = A → A  (axiom A2)
- False: ⊥ = A ⊕ A  (axiom A1)
- Not: ¬A = A → ⊥  (theorem T5)
- And: A ∧ B = ¬(A → ¬B)
- Or: A ∨ B = ¬A → B
- Iff: A ↔ B = (A → B) ∧ (B → A)

All connectives are derivable from the 3 axioms.  ∎

## ICOR Correspondence

The 6 ICOR rewrite rules are exactly the 3 axiom pairs:

| ICOR Rule          | Diagonal Axiom        | Intuition                          |
|--------------------|-----------------------|------------------------------------|
| CON × CON → wire   | A ⊕ A = ⊥ (A1)        | fuse identical computations        |
| ERA × ERA → nil    | A ⊕ A = ⊥ (A1)        | dead code elimination              |
| CON × ERA → nil    | A → A = ⊤ (A2)         | computation meets dead             |
| DUP × ERA → nil    | ⊥ → A = ⊤ (A3)        | propagation of falsum              |
| DUP × DUP → wire   | A ⊕ (B ⊕ C) = (A⊕B)⊕C | duplicator commutation             |
| CON × DUP → 4-agt  | A → A = ⊤ (A2)         | Bohm translation                   |

## Optimality

**Theorem.** Diagonal Logic's 3 axioms are the **minimum** complete set
for classical propositional logic.

**Proof.** A complete logic needs at least one axiom that introduces
falsum (⊥), one that introduces truth (⊤), and one relating them.
That's 3.  Diagonal Logic achieves this with 3 axioms.  ∎

## Implementation in BINE

The static correspondence is checked at runtime by
`diagonal_verify_correspondence()`:

```c
int diagonal_verify_correspondence(void);
// Returns 6 if all 6 ICOR rules are grounded in the 3 Diagonal axioms.
```

The 6 rules are listed in `DIAGONAL_TABLE` (in `include/diagonal.h`).
Each rule has:
- A rule ID (DR_*)
- An axiom ID (DA_*)
- A human-readable name
- The axiom name
- Agent counts (in, out)
