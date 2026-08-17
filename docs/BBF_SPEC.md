# BINE Binary Format v1 Specification

## Overview

A `.bbf` file is a self-describing binary container for interaction nets.
The bytes ARE the program: no text, no assembly, no IR.

## File Layout

```
Offset   Size  Field
------   ----  -----
0x00     4     Magic: "BBF1"  (0x31464242 little-endian)
0x04     4     Version: 0x00010000  (1.0)
0x08     4     Flags: bit 0=has_io, bit 1=is_reduced, ...
0x0C     4     Reserved (must be 0)
0x10     4     num_nodes   (uint32 LE)
0x14     4     num_wires   (uint32 LE)
0x18     4     num_inputs  (uint32 LE)
0x1C     4     num_outputs (uint32 LE)

0x20     ...   Node table  (8 bytes × num_nodes)
0x...    ...   Wire table  (8 bytes × num_wires)
0x...    ...   I/O table   (8 bytes × (num_inputs + num_outputs))
0x...    4     CRC32 of all preceding bytes
```

Total: 36 + 8·(nodes + wires + ios) bytes.

## Node Entry (8 bytes)

| Offset | Size | Field      | Notes                                  |
|--------|------|------------|----------------------------------------|
| 0      | 1    | type       | 0=ERA, 1=DUP, 2-34=CON_*, 0x80=USER   |
| 1      | 1    | flags      | bit 0=is_input, 1=is_output, ...       |
| 2      | 2    | aux_data   | opcode-specific (e.g. imm16, syscall) |
| 4      | 4    | principal_wire | index into wire table, 0xFFFFFFFE=NIL |

## Wire Entry (8 bytes)

| Offset | Size | Field    | Notes                                |
|--------|------|----------|--------------------------------------|
| 0      | 4    | node_a   | node index                           |
| 4      | 2    | port_a   | 0=principal, 1=aux_left, 2=aux_right |
| 6      | 2    | node_b   | node index                           |

The wire connects node_a's port_a to node_b's implicit port:
- If port_a is 0 (principal), node_b's port is 1 (aux_left) by default
- For active pair detection, both endpoints should have principal_wire
  pointing to this wire's index

## I/O Entry (8 bytes)

| Offset | Size | Field     | Notes                                |
|--------|------|-----------|--------------------------------------|
| 0      | 4    | node      | node index                           |
| 4      | 1    | port      | which port of the node               |
| 5      | 1    | direction | 0=IN (CPU→GPU), 1=OUT (GPU→CPU)      |
| 6      | 2    | reserved  | must be 0                            |

## CRC32

Standard IEEE CRC32 (polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF).
The "check value" for input "123456789" is 0xCBF43926.

## Node Types

| Code | Name | Semantics |
|------|------|-----------|
| 0x00 | ERA  | Eraser: deletes connected wire |
| 0x01 | DUP  | Duplicator: forks 1 wire into 2 |
| 0x02 | CON_NAND | Boolean NAND |
| 0x03 | CON_AND  | Boolean AND |
| 0x04 | CON_OR   | Boolean OR |
| 0x05 | CON_XOR  | Boolean XOR |
| 0x06 | CON_NOT  | Boolean NOT |
| 0x07 | CON_ADD  | Integer ADD |
| 0x08 | CON_SUB  | Integer SUB |
| 0x09 | CON_MUL  | Integer MUL |
| 0x0A | CON_DIV  | Integer DIV |
| 0x0B | CON_MOD  | Integer MOD |
| 0x0C | CON_SHL  | Shift left |
| 0x0D | CON_SHR  | Shift right |
| 0x0E-0x10 | CON_CMP_* | Compare |
| 0x11 | CON_MOV     | Register move |
| 0x12 | CON_LOAD    | Memory load |
| 0x13 | CON_STORE   | Memory store |
| 0x14-0x1A | CON_JMP/JZ/JNZ/CALL/RET/PUSH/POP | Control flow |
| 0x1B | CON_SYSCALL | System call (nr in aux_data) |
| 0x1C | CON_NOP     | No-op |
| 0x1D | CON_HALT    | Halt execution |
| 0x1E | CON_MOV_IMM | Move immediate (value in aux_data) |
| 0x1F-0x22 | CON_BIT_* | Bitwise ops |
| 0x80+ | USER   | User-defined (opcode in aux_data) |

## Example: HALT program (44 bytes)

```
42 42 46 31   "BBF1"
00 01 00 00   version 1.0
00 00 00 00   flags
00 00 00 00   reserved
01 00 00 00   1 node
00 00 00 00   0 wires
00 00 00 00   0 inputs
00 00 00 00   0 outputs

1D 00 00 00 FF FF FF FF   node[0] = HALT, no principal wire
                           (1D = CON_HALT, flags=0, aux=0, pw=0xFFFFFFFE)

XX XX XX XX   CRC32 of preceding 44 bytes
```
