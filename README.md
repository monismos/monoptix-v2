# BINE v2 — Binary Interaction Net Executor

**ELF binary superoptimizer** — deterministic, lossless, production-grade.

Takes any x86-64 ELF binary and produces a semantically equivalent binary by applying interaction net reduction (ICOR), probabilistic scheduling (STRAND-P), and binary-level optimization passes — all without source code, ISA manuals, or external data.

## Build

The project builds on Windows and Linux, with and without CUDA.

```bash
# CPU-only (no GPU required)
cmake -S . -B build -DBINE_CPU_ONLY=ON
cmake --build build -j$(nproc)

# CUDA (Blackwell sm_120, requires CUDA Toolkit 13.2+)
cmake -S . -B build -DCMAKE_CUDA_ARCHITECTURES=120
cmake --build build -j$(nproc)
```

On Windows, use the Visual Studio 2022 generator (`-G "Visual Studio 17 2022"`), or
MinGW Makefiles / NMake Makefiles with the toolchain on `PATH`. The CUDA build on
Windows uses the NMake generator with `vcvars64.bat` + `nvcc` (no VS CUDA MSBuild
integration required).

Options:

- `-DBINE_CPU_ONLY=ON` — pure host implementation, no CUDA
- `-DBINE_BUILD_TESTS=ON` (default) — build the test suite

## Usage

```bash
bine run <file.bbf>                  # parse, optimize, execute (GPU runner)
bine optimize <in.bbf> <out.bbf>     # .bbf -> .bbf optimization
bine info <file.bbf>                 # .bbf net summary
bine bench <file.bbf>                # .bbf benchmark
bine from-elf <in.elf> <out.bbf>     # ELF -> .bbf (DFG conversion)
bine elf-optimize <in.elf> <out.elf> # full ELF -> ELF optimization
bine elf-info <in.elf>               # binary self-description
bine elf-bench <in.elf> [out.elf]    # ELF optimization benchmark
bine verify <orig.elf> <opt.elf> [args...]  # semantic equivalence check
bine diag                            # Diagonal Logic properties
```

## Testing

```bash
./build/bine_tests            # all groups (bbf, diagonal, zentropy, kolmogorov,
                              # dfg, pure_binary, elf_pipeline, serialize,
                              # bbf_fuzz, compat, layout, + gpu when CUDA)
./build/bine_tests elf_layout # run a single group by name
BINE_TEST_CORPUS=/path/elfs ./build/bine_tests  # round-trip every *.elf in a dir
```

The suite runs on every platform: synthetic ELF fixtures exercise the full
pipeline (byte-identical roundtrip, dead-code elimination) without needing
Linux binaries, and the `gpu` group runs real CUDA kernels when a device is
present (skips otherwise).

## Pipeline

1. **ELF Parser** — Self-described properties from the binary itself
2. **STRAND Mask Oracle** — Entropy-derived read/write/kill masks
3. **Peephole Optimizer** — NOP removal, dead code elimination
4. **Interaction Net Construction** — Binary → interaction net via formal bijection
5. **ICOR Reduction** — 6 rewrite rules, slot-stable (agent i ↔ instruction i), GPU-parallel where available
6. **STRAND-P Scheduling** — P-bit simulated annealing for optimal instruction order
7. **Offset-Preserving Rewrite** — dead code → equal-length NOPs, provably-safe same-length reorder; `.text` size never changes, so relocations and symbols stay valid
8. **ELF Serializer** — Relocation-preserving output generation

## Requirements

- **Windows 10/11** or **Linux**
- CPU-only builds: any C11 compiler (MinGW GCC, MSVC, GCC/Clang)
- CUDA builds: NVIDIA GPU (sm_120 Blackwell, e.g. RTX 5050) + CUDA Toolkit 13.2+
- Linux ELF targets are optimized in place; synthetic fixtures cover the pipeline on Windows

## Project Structure

```
src/          — C11 + CUDA C++ sources
include/      — public/internal headers
tests/        — test suite + corpus harness
examples/     — .bbf examples
_archive/     — Legacy Rust/Frontend code (separate concerns)
```

## Philosophy

*"Problems ARE their own solutions when we do not assume anything extra, other than solely what's given to us."*

All analysis is self-derived from the binary bytes. No training data. No lookup tables. No ISA manuals.
