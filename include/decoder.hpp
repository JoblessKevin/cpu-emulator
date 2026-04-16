#pragma once

#include <cstdint>

namespace rv32i {

// ── Primitive Bit Helpers ────────────────────────────────────────────────────
//
// These two functions are the *only* tools we need to decode every RV32I
// instruction format — no ifs, no switches, no magic constants scattered
// around the codebase.

// Extract `width` bits starting at position `lo` from `instr`.
//
// Java mental model:  (instr >>> lo) & ((1 << width) - 1)
// C++ note: `instr` is uint32_t, so >> is guaranteed to be a logical
// (zero-filling) shift — no sign-bit surprises. In Java you'd need >>> for
// the same guarantee.
[[nodiscard]] constexpr uint32_t bits(uint32_t instr, uint32_t lo, uint32_t width) noexcept {
    return (instr >> lo) & ((1u << width) - 1u);
}

// Sign-extend a `bit_width`-wide value to a full int32_t.
//
// The trick: slam the sign bit to position 31 with a left-shift, then let
// arithmetic right-shift replicate it back down. C++20 §7.6.7 guarantees
// that right-shifting a signed integer is arithmetic (i.e. sign-fills) —
// this was implementation-defined in C++17 and is now a language guarantee.
//
// Example (12-bit, value = 0xFFF = -1):
//   0xFFF << 20 = 0xFFF0'0000  (sign bit now at bit 31)
//   >> 20       = 0xFFFF'FFFF  (arithmetic fill → -1 in int32_t) ✓
[[nodiscard]] constexpr int32_t sign_extend(uint32_t value, uint32_t bit_width) noexcept {
    const auto shift = static_cast<int32_t>(32u - bit_width);
    return static_cast<int32_t>(value << shift) >> shift;
}

// ── Decoded Instruction ───────────────────────────────────────────────────────
//
// All five immediate variants are decoded in one pass, even though the
// executor will only use one of them per instruction.  The cost is five extra
// ALU operations per decode — negligible compared to a branch-misprediction.
// More importantly, it keeps the executor completely free of bit-wrangling.

struct DecodedInsn {
    uint32_t opcode;   // [6:0]   — identifies the instruction format & class
    uint32_t rd;       // [11:7]  — destination register index
    uint32_t funct3;   // [14:12] — operation sub-type (e.g. ADD vs XOR)
    uint32_t rs1;      // [19:15] — source register 1 index
    uint32_t rs2;      // [24:20] — source register 2 index
    uint32_t funct7;   // [31:25] — further qualification (e.g. ADD vs SUB)

    int32_t  imm_i;    // I-type  signed 12-bit  [31:20]
    int32_t  imm_s;    // S-type  signed 12-bit  [31:25 | 11:7]
    int32_t  imm_b;    // B-type  signed 13-bit  [31 | 7 | 30:25 | 11:8]   LSB=0
    uint32_t imm_u;    // U-type  unsigned 32-bit [31:12]                   low12=0
    int32_t  imm_j;    // J-type  signed 21-bit  [31 | 19:12 | 20 | 30:21] LSB=0
};

// ── Decoder ───────────────────────────────────────────────────────────────────
//
// A pure function: same input always produces same output, no side-effects.
// `constexpr` means the compiler can evaluate this at compile-time for
// constant instructions — useful for future unit tests.

[[nodiscard]] constexpr DecodedInsn decode(uint32_t instr) noexcept {

    // ── Fixed fields (identical position across ALL formats) ─────────────────
    const uint32_t opcode = bits(instr,  0, 7);
    const uint32_t rd     = bits(instr,  7, 5);
    const uint32_t funct3 = bits(instr, 12, 3);
    const uint32_t rs1    = bits(instr, 15, 5);
    const uint32_t rs2    = bits(instr, 20, 5);
    const uint32_t funct7 = bits(instr, 25, 7);

    // ── I-type ────────────────────────────────────────────────────────────────
    //   instr[31:20] → imm[11:0]
    //
    //   31       20 19    15 14  12 11     7 6      0
    //   [ imm[11:0] ][ rs1  ][funct3][  rd  ][ opcode ]
    const int32_t imm_i = sign_extend(bits(instr, 20, 12), 12);

    // ── S-type ────────────────────────────────────────────────────────────────
    //   instr[31:25] → imm[11:5],  instr[11:7] → imm[4:0]
    //
    //   31     25 24   20 19   15 14  12 11     7 6      0
    //   [imm[11:5]][ rs2 ][ rs1  ][funct3][imm[4:0]][ opcode ]
    const uint32_t s_raw = (bits(instr, 25, 7) << 5)   // imm[11:5]
                         |  bits(instr,  7, 5);         // imm[4:0]
    const int32_t imm_s = sign_extend(s_raw, 12);

    // ── B-type ────────────────────────────────────────────────────────────────
    //   The scrambled layout preserves rs1/rs2 in the same bit positions as
    //   R/S-type — a hardware cost-saving trick by the RISC-V designers.
    //
    //   31     25 24   20 19   15 14  12 11     7 6      0
    //   [12|10:5][ rs2 ][ rs1  ][funct3][4:1|11][ opcode ]
    //
    //   imm[12]  ← instr[31]
    //   imm[11]  ← instr[7]
    //   imm[10:5]← instr[30:25]
    //   imm[4:1] ← instr[11:8]    (imm[0] is always 0)
    const uint32_t b_raw = (bits(instr, 31, 1) << 12)   // imm[12]
                         | (bits(instr,  7, 1) << 11)   // imm[11]
                         | (bits(instr, 25, 6) <<  5)   // imm[10:5]
                         | (bits(instr,  8, 4) <<  1);  // imm[4:1]
    const int32_t imm_b = sign_extend(b_raw, 13);

    // ── U-type ────────────────────────────────────────────────────────────────
    //   instr[31:12] → imm[31:12],  imm[11:0] = 0
    //   No sign extension needed — the immediate already spans all 32 bits.
    //   Used by LUI / AUIPC to load large constants.
    const uint32_t imm_u = instr & 0xFFFF'F000u;

    // ── J-type ────────────────────────────────────────────────────────────────
    //   Another scrambled layout (same motive as B-type).
    //
    //   31     12 11  7 6      0
    //   [20|10:1|11|19:12][ rd ][ opcode ]
    //
    //   imm[20]   ← instr[31]
    //   imm[19:12]← instr[19:12]
    //   imm[11]   ← instr[20]
    //   imm[10:1] ← instr[30:21]  (imm[0] is always 0)
    const uint32_t j_raw = (bits(instr, 31,  1) << 20)  // imm[20]
                         | (bits(instr, 12,  8) << 12)  // imm[19:12]
                         | (bits(instr, 20,  1) << 11)  // imm[11]
                         | (bits(instr, 21, 10) <<  1); // imm[10:1]
    const int32_t imm_j = sign_extend(j_raw, 21);

    return DecodedInsn{
        .opcode = opcode,
        .rd     = rd,
        .funct3 = funct3,
        .rs1    = rs1,
        .rs2    = rs2,
        .funct7 = funct7,
        .imm_i  = imm_i,
        .imm_s  = imm_s,
        .imm_b  = imm_b,
        .imm_u  = imm_u,
        .imm_j  = imm_j,
    };
}

}  // namespace rv32i
