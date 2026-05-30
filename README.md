# BINE v2 — Binary Interaction Net Executor

**ELF binary superoptimizer** — deterministic, lossless, production-grade.

Takes any x86-64 ELF binary and produces the smallest, fastest semantically equivalent binary by applying interaction net reduction (ICOR), probabilistic scheduling (STRAND-P), and binary-level optimization passes — all without source code, ISA manuals, or external data.

## Build

```bash
cmake -S . -B build -DCMAKE_CUDA_ARCHITECTURES=120 -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-15
cmake --build build -j$(nproc)
```

## Usage

```bash
./build/bine info    <input.elf>     # Print binary self-description
./build/bine optimize <input.elf> <output.elf>  # Full optimization pipeline
./build/bine verify  <orig.elf> <opt.elf>       # Verify semantic equivalence
./build/bine bench   <input.elf> [output.elf]   # Benchmark optimization
```

## Pipeline

1. **ELF Parser** — Self-described properties from the binary itself
2. **STRAND Mask Oracle** — Entropy-derived read/write/kill masks
3. **Peephole Optimizer** — NOP removal, dead code elimination
4. **Interaction Net Construction** — Binary → interaction net via formal bijection
5. **GPU-ICOR Reduction** — 6 rewrite rules, GPU-parallel, confluence-guaranteed
6. **STRAND-P Scheduling** — P-bit simulated annealing for optimal instruction order
7. **Binary Optimizer** — NOP removal, sequence merging, code reordering
8. **ELF Serializer** — Relocation-preserving output generation

## Requirements

- NVIDIA GPU with Blackwell architecture (sm_120, e.g. RTX 5050)
- CUDA Toolkit 13.2+
- GCC 15+ (CUDA host compiler)
- Linux (ELF target)

## Project Structure

```
src/          — 508KB of source code (C11 + CUDA C++)
include/      — 44KB of headers
tests/        — 36KB of test suite
_archive/     — Legacy Rust/Frontend code (separate concerns)
```

## Philosophy

*"Problems ARE their own solutions when we do not assume anything extra, other than solely what's given to us."*

All analysis is self-derived from the binary bytes. No training data. No lookup tables. No ISA manuals.
