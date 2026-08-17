# Z-entropy Numbers

## Introduction

A **Z-entropy number** is a quadruple (b, e, z, P) that pairs a value
with a self-information cost.  Encoding a sequence of such numbers
yields the Shannon entropy of the sequence — asymptotically optimal by
Shannon's source-coding theorem.

## Definition

A Z-entropy number n has:

| Field | Type | Meaning |
|-------|------|---------|
| b     | bit  | sign bit (0=positive, 1=negative) |
| e     | uint | exponent (in some encoding) |
| z     | real | self-information = -log₂ P(n) |
| P     | real | empirical probability in the corpus |

The *value* of n is b · 2ᵉ.  The *cost* of transmitting n is z bits.

## Encoding Cost

A sequence (n₁, n₂, ..., nₖ) has total cost:

```
    H = Σᵢ z(nᵢ) = -Σᵢ log₂ P(nᵢ)
```

This is exactly the Shannon entropy of the sequence over the
distribution P.

## Optimality

**Theorem (Shannon's Source-Coding Theorem).** No encoding can achieve
expected length strictly less than H.

**Corollary.** Z-entropy encoding is asymptotically optimal.

## Practical Implementation: Huffman

Z-entropy is implemented via Huffman coding in BINE.  The steps are:

1. **Observe**: count symbol frequencies in a corpus (ls, bash, cat,
   date, echo, true).

2. **Build tree**: combine the two lowest-frequency nodes until one
   root remains.

3. **Assign codes**: walk the tree, assigning 0 to left edges and 1
   to right edges.  More frequent symbols get shorter codes.

4. **Encode**: replace each symbol with its Huffman code.  Total bits
   ≈ H + ε (within 1 bit of entropy).

## In BINE

```c
zentropy_table_t t;
zentropy_table_init(&t);

// Observe frequencies
for (int i = 0; i < 100; i++) zentropy_table_observe(&t, 5);
for (int i = 0; i < 10;  i++) zentropy_table_observe(&t, 10);
for (int i = 0; i < 1;   i++) zentropy_table_observe(&t, 20);

// Build Huffman tree
zentropy_table_finalize(&t);

// Encode a sequence
uint16_t keys[] = {5, 10, 5, 20, 5};
uint8_t *out;
size_t out_bits;
zentropy_encode(&t, keys, 5, &out, &out_bits);

// Decode
uint16_t decoded[5];
zentropy_decode(&t, out, out_bits, decoded, 5);
```

The cost of encoding is `zentropy_encode_cost(&t, keys, n)`, which
returns the total bits used.

## Use in BINE's Pipeline

Z-entropy contributes to the optimality bound in two ways:

1. **As a K(x) proxy.**  The lower bound of the Kolmogorov bound is
   `Σ z(nᵢ)` over the symbols in the program.

2. **As a final compression step.**  After Kolmogorov descent, the
   reduced net can be re-encoded with Z-entropy to further reduce its
   on-disk size.  This is part of `bine optimize`.
