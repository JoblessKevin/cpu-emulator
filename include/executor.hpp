#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "cpu.hpp"
#include "decoder.hpp"

namespace rv32i {

// ── RV32I Base Opcode Constants ───────────────────────────────────────────────
// Only the 7-bit base opcode; funct3/funct7 are resolved at Level 2.
namespace opcode {
    inline constexpr uint32_t LUI    = 0x37;
    inline constexpr uint32_t AUIPC  = 0x17;
    inline constexpr uint32_t JAL    = 0x6F;
    inline constexpr uint32_t JALR   = 0x67;
    inline constexpr uint32_t BRANCH = 0x63;
    inline constexpr uint32_t LOAD   = 0x03;
    inline constexpr uint32_t STORE  = 0x23;
    inline constexpr uint32_t OP_IMM = 0x13;
    inline constexpr uint32_t OP_R   = 0x33;
    inline constexpr uint32_t FENCE  = 0x0F;
    inline constexpr uint32_t SYSTEM = 0x73;
}  // namespace opcode

// ── Executor ──────────────────────────────────────────────────────────────────
//
// Two-level O(1) dispatch.  No switch, no if-else chains, no virtual calls.
//
//  Level 1 — opcode_table_[opcode]          128 entries × 8 B =  1,024 B
//  Level 2 — s_op_imm_table_[funct3]          8 entries × 8 B =     64 B
//           — s_op_r_table_[funct7[5]][funct3] 16 entries × 8 B =    128 B
//             ──────────────────────────────────────────────────────────────
//             Total                                                1,216 B  (~1.2 KiB)
//
// The entire structure fits in a single L1 cache fetch.
// Java analogue: EnumMap<Opcode, InsnHandler>, but without boxing, hashing,
// or heap allocation — every lookup is a direct array index.

class Executor {
public:
    // A raw function pointer: 8 bytes, no heap, no vtable, no std::function
    // overhead.  The calling convention is explicit — every handler receives
    // the full CPU state and the pre-decoded instruction.
    using Handler = void (*)(CpuState&, const DecodedInsn&);

    // Build and populate all dispatch tables.
    Executor() noexcept;

    // Fetch one 32-bit word from cpu.pc, decode it, dispatch it.
    // Each handler is responsible for advancing cpu.pc (either +4 for linear
    // flow, or an arbitrary offset for branches/jumps).
    // This keeps the step() loop minimal — just one load + one indirect call.
    void step(CpuState& cpu) const;

    // ── Inspection (useful for testing) ──────────────────────────────────────
    [[nodiscard]] Handler opcode_handler(uint32_t opcode) const noexcept {
        return opcode_table_[opcode & 0x7Fu];
    }

private:
    // ── Level 1: opcode → handler (128 entries) ───────────────────────────────
    //
    // Slots for valid RV32I opcodes point to leaf handlers or bridge handlers.
    // All 117 remaining slots point to exec_illegal — safe by default.
    std::array<Handler, 128> opcode_table_{};

    // ── Level 2: sub-dispatch tables (static — see note below) ───────────────
    //
    // Raw function pointers cannot capture `this`, so bridge handlers (which
    // ARE stored as raw pointers) need to reach the sub-tables without an
    // object reference.  Making them `inline static` solves this cleanly:
    // they are shared across all Executor instances (in practice: exactly one)
    // and initialised on first construction.
    //
    // C++ note: `inline static` in the class body is a C++17 feature.
    // It satisfies the One Definition Rule without a separate .cpp definition.

    // OP-IMM (0x13): 8 variants keyed by funct3
    //   [000]=ADDI  [001]=SLLI  [010]=SLTI  [011]=SLTIU
    //   [100]=XORI  [101]=SRLI/SRAI  [110]=ORI  [111]=ANDI
    inline static std::array<Handler, 8> s_op_imm_table_{};

    // OP-R (0x33): 8 funct3 variants × 2 funct7[5] variants
    //   s_op_r_table_[0][funct3] — funct7[5]=0: ADD, SLL, SLT, SLTU, XOR, SRL, OR, AND
    //   s_op_r_table_[1][funct3] — funct7[5]=1: SUB, (ill), (ill), (ill), (ill), SRA, (ill), (ill)
    inline static std::array<std::array<Handler, 8>, 2> s_op_r_table_{};

    // BRANCH (0x63): 8 variants keyed by funct3
    //   [000]=BEQ  [001]=BNE  [100]=BLT  [101]=BGE  [110]=BLTU  [111]=BGEU
    inline static std::array<Handler, 8> s_branch_table_{};

    // LOAD (0x03): 8 variants keyed by funct3
    //   [000]=LB  [001]=LH  [010]=LW  [100]=LBU  [101]=LHU
    inline static std::array<Handler, 8> s_load_table_{};

    // STORE (0x23): 8 variants keyed by funct3
    //   [000]=SB  [001]=SH  [010]=SW
    inline static std::array<Handler, 8> s_store_table_{};

    // ── Table initialisation ──────────────────────────────────────────────────
    void init_tables() noexcept;

    // ── Bridge handlers ───────────────────────────────────────────────────────
    // These sit at Level 1 and perform the Level 2 dispatch.
    // They are `static` so they can be stored as raw Handler pointers.

    // OP-IMM: one more array lookup on funct3
    static void h_op_imm (CpuState& cpu, const DecodedInsn& d) noexcept;
    // OP-R: index on funct7[5], then funct3 — still two array reads, O(1)
    static void h_op_r   (CpuState& cpu, const DecodedInsn& d) noexcept;
    // BRANCH / LOAD / STORE: one array lookup on funct3
    static void h_branch (CpuState& cpu, const DecodedInsn& d) noexcept;
    static void h_load   (CpuState& cpu, const DecodedInsn& d);   // may throw MemoryFault
    static void h_store  (CpuState& cpu, const DecodedInsn& d);   // may throw MemoryFault

    // ── Leaf instruction handlers ─────────────────────────────────────────────

    // U-type ──────────────────────────────────────────────────────────────────
    static void exec_lui   (CpuState& cpu, const DecodedInsn& d) noexcept;

    // I-type (OP-IMM family) ───────────────────────────────────────────────────
    static void exec_addi  (CpuState& cpu, const DecodedInsn& d) noexcept;

    // R-type (OP-R family) ────────────────────────────────────────────────────
    static void exec_add   (CpuState& cpu, const DecodedInsn& d) noexcept;
    static void exec_sub   (CpuState& cpu, const DecodedInsn& d) noexcept;

    // J-type ──────────────────────────────────────────────────────────────────
    static void exec_jal   (CpuState& cpu, const DecodedInsn& d) noexcept;

    // B-type (BRANCH family) ───────────────────────────────────────────────────
    // Branchless PC update: taken ? pc += imm_b : pc += 4
    // At -O2 GCC emits a cmov; even at -O0 the *intent* is clear.
    static void exec_beq   (CpuState& cpu, const DecodedInsn& d) noexcept;
    static void exec_bne   (CpuState& cpu, const DecodedInsn& d) noexcept;

    // I-type load / S-type store (may throw MemoryFault) ─────────────────────
    static void exec_lw    (CpuState& cpu, const DecodedInsn& d);
    static void exec_sw    (CpuState& cpu, const DecodedInsn& d);

    // Catch-all ───────────────────────────────────────────────────────────────
    [[noreturn]]
    static void exec_illegal(CpuState& cpu, const DecodedInsn& d);
};

// ═════════════════════════════════════════════════════════════════════════════
// Inline implementations
// Everything below is `inline` to satisfy ODR when this header is included
// in multiple translation units.  In a larger codebase these move to
// executor.cpp, but keeping them here makes the architecture immediately
// visible for review.
// ═════════════════════════════════════════════════════════════════════════════

// ── Constructor ───────────────────────────────────────────────────────────────

inline Executor::Executor() noexcept { init_tables(); }

// ── The hot loop ─────────────────────────────────────────────────────────────

inline void Executor::step(CpuState& cpu) const {
    const DecodedInsn d = decode(cpu.mem.read32(cpu.pc));
    // One array load + one indirect call.  The branch predictor sees a
    // consistent indirect target for common instructions and will predict
    // correctly after the first few executions.
    opcode_table_[d.opcode](cpu, d);
}

// ── Bridge handlers ───────────────────────────────────────────────────────────

inline void Executor::h_op_imm(CpuState& cpu, const DecodedInsn& d) noexcept {
    // Level 2 dispatch: one more array load + indirect call.
    s_op_imm_table_[d.funct3](cpu, d);
}

inline void Executor::h_op_r(CpuState& cpu, const DecodedInsn& d) noexcept {
    // funct7[5] is the single bit that separates ADD↔SUB and SRL↔SRA.
    // Masking it out gives a clean 0 or 1 index — no branch.
    const uint32_t alt = (d.funct7 >> 5u) & 1u;
    s_op_r_table_[alt][d.funct3](cpu, d);
}

inline void Executor::h_branch(CpuState& cpu, const DecodedInsn& d) noexcept {
    s_branch_table_[d.funct3](cpu, d);
}

inline void Executor::h_load(CpuState& cpu, const DecodedInsn& d) {
    s_load_table_[d.funct3](cpu, d);
}

inline void Executor::h_store(CpuState& cpu, const DecodedInsn& d) {
    s_store_table_[d.funct3](cpu, d);
}

// ── Leaf handlers ─────────────────────────────────────────────────────────────
//
// Design rule: every non-branch handler ends with `cpu.pc += 4`.
//              Branch/jump handlers compute the new PC themselves.

inline void Executor::exec_lui(CpuState& cpu, const DecodedInsn& d) noexcept {
    // LUI: rd = imm_u  (upper 20 bits, lower 12 already zero — see decoder.hpp)
    cpu.regs.write(d.rd, d.imm_u);
    cpu.pc += 4;
}

inline void Executor::exec_addi(CpuState& cpu, const DecodedInsn& d) noexcept {
    // ADDI: rd = rs1 + sign_extended_imm12
    //
    // C++ note: adding int32_t to uint32_t promotes the int32_t to uint32_t
    // first (modular/two's-complement), which is exactly what RISC-V specifies
    // for integer arithmetic — overflow wraps silently.
    cpu.regs.write(d.rd, cpu.regs.read(d.rs1) + static_cast<uint32_t>(d.imm_i));
    cpu.pc += 4;
}

inline void Executor::exec_add(CpuState& cpu, const DecodedInsn& d) noexcept {
    // ADD: rd = rs1 + rs2  (unsigned wrap on overflow, per spec)
    cpu.regs.write(d.rd, cpu.regs.read(d.rs1) + cpu.regs.read(d.rs2));
    cpu.pc += 4;
}

inline void Executor::exec_sub(CpuState& cpu, const DecodedInsn& d) noexcept {
    cpu.regs.write(d.rd, cpu.regs.read(d.rs1) - cpu.regs.read(d.rs2));
    cpu.pc += 4;
}

// ── JAL ───────────────────────────────────────────────────────────────────────

inline void Executor::exec_jal(CpuState& cpu, const DecodedInsn& d) noexcept {
    // Save the return address (instruction after JAL) in rd, then jump.
    // The cast of imm_j (int32_t) to uint32_t lets negative offsets wrap
    // correctly in two's-complement — identical to what the hardware does.
    cpu.regs.write(d.rd, cpu.pc + 4u);
    cpu.pc += static_cast<uint32_t>(d.imm_j);
}

// ── Branch instructions ───────────────────────────────────────────────────────
//
// Branchless PC update pattern:
//   cpu.pc += taken ? static_cast<uint32_t>(imm_b) : 4u;
//
// At -O2 GCC turns this into a single CMOVcc — no branch, no misprediction.
// The cast of the signed imm_b to uint32_t is intentional: adding a negative
// two's-complement value as uint32_t produces the correct backward offset.

inline void Executor::exec_beq(CpuState& cpu, const DecodedInsn& d) noexcept {
    const bool taken = (cpu.regs.read(d.rs1) == cpu.regs.read(d.rs2));
    cpu.pc += taken ? static_cast<uint32_t>(d.imm_b) : 4u;
}

inline void Executor::exec_bne(CpuState& cpu, const DecodedInsn& d) noexcept {
    const bool taken = (cpu.regs.read(d.rs1) != cpu.regs.read(d.rs2));
    cpu.pc += taken ? static_cast<uint32_t>(d.imm_b) : 4u;
}

// ── Load / Store ──────────────────────────────────────────────────────────────
//
// Not `noexcept`: Memory::read32 / write32 throw MemoryFault on bad addresses.
// The exception propagates through step() to whoever drives the emulator loop.

inline void Executor::exec_lw(CpuState& cpu, const DecodedInsn& d) {
    // LW: rd = mem32[rs1 + sign_extended_imm12]
    const uint32_t addr = cpu.regs.read(d.rs1) + static_cast<uint32_t>(d.imm_i);
    cpu.regs.write(d.rd, cpu.mem.read32(addr));
    cpu.pc += 4;
}

inline void Executor::exec_sw(CpuState& cpu, const DecodedInsn& d) {
    // SW: mem32[rs1 + sign_extended_imm12] = rs2
    const uint32_t addr = cpu.regs.read(d.rs1) + static_cast<uint32_t>(d.imm_s);
    cpu.mem.write32(addr, cpu.regs.read(d.rs2));
    cpu.pc += 4;
}

[[noreturn]]
inline void Executor::exec_illegal(CpuState& /*cpu*/, const DecodedInsn& d) {
    // In a real CPU this would raise a trap to the OS.
    // For now: loudly crash rather than silently corrupt state.
    throw std::runtime_error(
        "Illegal/unimplemented instruction  opcode=0x"
        + [](uint32_t v) {                           // tiny hex formatter
              char buf[5];
              std::snprintf(buf, sizeof(buf), "%02X", v);
              return std::string{buf};
          }(d.opcode)
    );
}

// ── init_tables ───────────────────────────────────────────────────────────────

inline void Executor::init_tables() noexcept {
    // ── Safe defaults ─────────────────────────────────────────────────────────
    opcode_table_.fill(exec_illegal);
    s_op_imm_table_.fill(exec_illegal);
    s_branch_table_.fill(exec_illegal);
    s_load_table_.fill(exec_illegal);
    s_store_table_.fill(exec_illegal);
    for (auto& row : s_op_r_table_) { row.fill(exec_illegal); }

    // ── Level 1: opcode → handler ─────────────────────────────────────────────
    opcode_table_[opcode::LUI]    = exec_lui;   // U-type leaf
    opcode_table_[opcode::JAL]    = exec_jal;   // J-type leaf
    opcode_table_[opcode::OP_IMM] = h_op_imm;  // bridge → L2 funct3
    opcode_table_[opcode::OP_R]   = h_op_r;    // bridge → L2 funct3 × funct7[5]
    opcode_table_[opcode::BRANCH] = h_branch;  // bridge → L2 funct3
    opcode_table_[opcode::LOAD]   = h_load;    // bridge → L2 funct3
    opcode_table_[opcode::STORE]  = h_store;   // bridge → L2 funct3
    // (AUIPC, JALR, FENCE, SYSTEM — future stages)

    // ── Level 2: OP-IMM funct3 ────────────────────────────────────────────────
    s_op_imm_table_[0x0] = exec_addi;
    // [0x1]=SLLI  [0x2]=SLTI  [0x3]=SLTIU  [0x4]=XORI
    // [0x5]=SRLI/SRAI  [0x6]=ORI  [0x7]=ANDI  — future

    // ── Level 2: OP-R funct3 × funct7[5] ─────────────────────────────────────
    s_op_r_table_[0][0x0] = exec_add;
    s_op_r_table_[1][0x0] = exec_sub;
    // [0][0x1]=SLL  [0][0x2]=SLT  [0][0x3]=SLTU  [0][0x4]=XOR
    // [0][0x5]=SRL  [1][0x5]=SRA  [0][0x6]=OR    [0][0x7]=AND  — future

    // ── Level 2: BRANCH funct3 ────────────────────────────────────────────────
    s_branch_table_[0x0] = exec_beq;
    s_branch_table_[0x1] = exec_bne;
    // [0x4]=BLT  [0x5]=BGE  [0x6]=BLTU  [0x7]=BGEU  — future

    // ── Level 2: LOAD funct3 ──────────────────────────────────────────────────
    s_load_table_[0x2] = exec_lw;
    // [0x0]=LB  [0x1]=LH  [0x4]=LBU  [0x5]=LHU  — future

    // ── Level 2: STORE funct3 ─────────────────────────────────────────────────
    s_store_table_[0x2] = exec_sw;
    // [0x0]=SB  [0x1]=SH  — future
}

}  // namespace rv32i
