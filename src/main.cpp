#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cpu.hpp"
#include "decoder.hpp"
#include "elf_loader.hpp"
#include "executor.hpp"

// ── Helpers (smoke test only, not part of the emulator) ──────────────────────

static void print_separator(const char* title) {
    std::printf("\n── %s ", title);
    const int dashes = 40 - static_cast<int>(std::strlen(title));
    for (int i = 0; i < dashes; ++i) { std::putchar('-'); }
    std::putchar('\n');
}

static void print_registers(const rv32i::RegisterFile& regs) {
    for (std::size_t i = 0; i < rv32i::kNumRegisters; ++i) {
        std::printf("  x%-2zu = 0x%08X (%d)\n", i, regs.read(i),
                    static_cast<int32_t>(regs.read(i)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────

// ── ELF execution mode ────────────────────────────────────────────────────────
//
// If a path argument is supplied, load and run the ELF.
// The emulator steps until it executes an illegal/unimplemented instruction,
// hits a MemoryFault, or the PC lands at address 0xFFFF'FFFF (sentinel halt).
//
// Usage:  ./rv_emulator                  (smoke tests)
//         ./rv_emulator my_program.elf   (run ELF)

static int run_elf(const std::string& path)
{
    rv32i::CpuState  cpu{};
    rv32i::Executor  executor{};

    // ── Load ─────────────────────────────────────────────────────────────────
    rv32i::ElfLoadResult result{};
    try {
        result = rv32i::load_elf(path, cpu.mem);
    } catch (const rv32i::ElfError& e) {
        std::printf("[ELF ERROR] %s\n", e.what());
        return 1;
    }

    std::printf("Loaded '%s'\n", path.c_str());
    std::printf("  Segments : %zu\n",   result.segments_loaded);
    std::printf("  Entry PC : 0x%08X\n", result.entry);

    cpu.pc = result.entry;

    // ── Execute ───────────────────────────────────────────────────────────────
    // Sentinel halt address: RISC-V bare-metal programs often spin on a
    // self-jump (`j .` = `jal x0, 0`) to signal the end of execution.
    // For safety we also cap at a maximum step count.
    constexpr uint32_t HALT_ADDR   = 0xFFFF'FFFFu;
    constexpr uint64_t MAX_STEPS   = 10'000'000;

    uint64_t steps = 0;
    while (cpu.pc != HALT_ADDR && steps < MAX_STEPS) {
        try {
            executor.step(cpu);
        } catch (const rv32i::MemoryFault& e) {
            std::printf("\n[FAULT @ step %llu] %s\n",
                        static_cast<unsigned long long>(steps), e.what());
            return 1;
        } catch (const std::runtime_error& e) {
            // Illegal/unimplemented instruction — treat as normal halt for now
            std::printf("\n[HALT  @ step %llu, PC=0x%08X] %s\n",
                        static_cast<unsigned long long>(steps), cpu.pc, e.what());
            break;
        }
        ++steps;
    }

    std::printf("\nExecution complete: %llu steps\n",
                static_cast<unsigned long long>(steps));

    // ── Register dump ─────────────────────────────────────────────────────────
    std::printf("\nFinal register state:\n");
    for (std::size_t i = 0; i < rv32i::kNumRegisters; ++i) {
        const uint32_t v = cpu.regs.read(i);
        if (v != 0) {  // only print non-zero registers to keep output tidy
            std::printf("  x%-2zu = 0x%08X (%d)\n", i, v, static_cast<int32_t>(v));
        }
    }
    std::printf("  PC  = 0x%08X\n", cpu.pc);

    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {

    // ── ELF mode: argument provided ───────────────────────────────────────────
    if (argc == 2) {
        return run_elf(argv[1]);
    }
    if (argc > 2) {
        std::printf("Usage: %s [path/to/program.elf]\n", argv[0]);
        return 1;
    }

    // ── Smoke test mode ───────────────────────────────────────────────────────
    std::printf("=== RV32I Emulator — Smoke Test ===\n");

    // ── Stage 1: CPU State ────────────────────────────────────────────────────

    rv32i::CpuState cpu{};

    print_separator("Initial Register State");
    print_registers(cpu.regs);

    print_separator("Initial PC");
    std::printf("  PC = 0x%08X\n", cpu.pc);

    // ── Memory: write & read back ─────────────────────────────────────────────
    print_separator("Memory Write/Read");

    cpu.mem.write32(0x00, 0xDEAD'BEEF);
    cpu.mem.write32(0x04, 0xCAFE'BABE);
    cpu.mem.write8 (0x08, 0xAB);

    const uint32_t word0 = cpu.mem.read32(0x00);
    const uint32_t word1 = cpu.mem.read32(0x04);
    const uint8_t  byte0 = cpu.mem.read8 (0x08);

    std::printf("  [0x00] write 0xDEADBEEF → read 0x%08X  %s\n",
                word0, word0 == 0xDEAD'BEEF ? "OK" : "FAIL");
    std::printf("  [0x04] write 0xCAFEBABE → read 0x%08X  %s\n",
                word1, word1 == 0xCAFE'BABE ? "OK" : "FAIL");
    std::printf("  [0x08] write 0xAB       → read 0x%02X        %s\n",
                byte0, byte0 == 0xAB        ? "OK" : "FAIL");

    // ── x0 hardwired-zero enforcement ────────────────────────────────────────
    print_separator("x0 Hardwired-Zero");

    cpu.regs.write(0, 0xFFFF'FFFF);  // should be silently discarded
    cpu.regs.write(1, 0xFFFF'FFFF);  // x1 should change

    std::printf("  write 0xFFFFFFFF to x0 → x0 = 0x%08X  %s\n",
                cpu.regs.read(0), cpu.regs.read(0) == 0 ? "OK (hardwired 0)" : "FAIL");
    std::printf("  write 0xFFFFFFFF to x1 → x1 = 0x%08X  %s\n",
                cpu.regs.read(1), cpu.regs.read(1) == 0xFFFF'FFFF ? "OK" : "FAIL");

    // ── Stage 2: Instruction Decode ───────────────────────────────────────────
    //
    // Test vectors validated with the RISC-V ISA specification:
    //
    //   addi  x1, x0,  5      I-type  0x00500093  imm_i = +5
    //   sw    x2, 12(x1)      S-type  0x0020A623  imm_s = +12
    //   beq   x1, x2, +8      B-type  0x00208463  imm_b = +8
    //   beq   x1, x2, -8      B-type  0xFE208CE3  imm_b = -8  (sign-extension test)
    //   lui   x3, 0xABCDE     U-type  0xABCDE1B7  imm_u = 0xABCDE000
    //   jal   x1, +8          J-type  0x008000EF  imm_j = +8

    struct TestCase {
        const char* name;
        uint32_t    instr;
        int32_t     expected_imm;
        char        imm_type;   // 'I','S','B','U','J'
    };

    constexpr TestCase cases[] = {
        { "addi  x1, x0, +5    (I-type)", 0x00500093,          5, 'I' },
        { "sw    x2, 12(x1)    (S-type)", 0x0020A623,         12, 'S' },
        { "beq   x1, x2, +8    (B-type)", 0x00208463,          8, 'B' },
        { "beq   x1, x2, -8    (B-type)", 0xFE208CE3,         -8, 'B' },
        { "lui   x3, 0xABCDE   (U-type)", 0xABCDE1B7, static_cast<int32_t>(0xABCDE000), 'U' },
        { "jal   x1, +8        (J-type)", 0x008000EF,          8, 'J' },
    };

    print_separator("Instruction Decode");
    bool all_pass = true;

    for (const auto& tc : cases) {
        const rv32i::DecodedInsn d = rv32i::decode(tc.instr);

        int32_t got{};
        switch (tc.imm_type) {
            case 'I': got = d.imm_i;                              break;
            case 'S': got = d.imm_s;                              break;
            case 'B': got = d.imm_b;                              break;
            case 'U': got = static_cast<int32_t>(d.imm_u);        break;
            case 'J': got = d.imm_j;                              break;
            default:  got = 0;
        }

        const bool pass = (got == tc.expected_imm);
        all_pass &= pass;
        std::printf("  %-38s imm=%-6d  %s\n",
                    tc.name, got, pass ? "OK" : "FAIL");
    }

    // ── Stage 3: Executor / Dispatch Table ───────────────────────────────────
    //
    // We encode a tiny 3-instruction program directly into memory, then run
    // the executor's fetch-decode-dispatch loop over it.
    //
    //   addi  x1, x0,  10    → x1 = 10        (I-type, OP-IMM)
    //   addi  x2, x0,  32    → x2 = 32        (I-type, OP-IMM)
    //   add   x3, x1,  x2    → x3 = 42        (R-type, OP-R)
    //   lui   x4, 0xABCDE    → x4 = 0xABCDE000  (U-type)
    //
    // Encodings verified against the RV32I spec (and our own decoder tests above).
    constexpr uint32_t prog[] = {
        0x00A00093u,   // addi  x1, x0, 10
        0x02000113u,   // addi  x2, x0, 32
        0x002081B3u,   // add   x3, x1, x2
        0xABCDE237u,   // lui   x4, 0xABCDE
    };

    // Load program into a fresh CPU and executor.
    // std::as_bytes gives us a std::span<const std::byte>; memcpy is the
    // correct zero-cost way to reinterpret the bit pattern as uint8_t[].
    rv32i::CpuState ecpu{};
    {
        std::vector<uint8_t> image(sizeof(prog));
        std::memcpy(image.data(), prog, sizeof(prog));
        ecpu.mem.load_program(image);
    }

    rv32i::Executor executor{};

    print_separator("Executor — Dispatch Table");

    struct ExecTest {
        const char* desc;
        uint32_t    reg;
        uint32_t    expected;
    };

    constexpr ExecTest exec_cases[] = {
        { "addi x1, x0, 10   → x1",  1,  10          },
        { "addi x2, x0, 32   → x2",  2,  32          },
        { "add  x3, x1, x2   → x3",  3,  42          },
        { "lui  x4, 0xABCDE  → x4",  4,  0xABCDE000u },
    };

    bool exec_pass = true;
    for (const auto& tc : exec_cases) {
        try {
            executor.step(ecpu);
        } catch (const std::exception& e) {
            std::printf("  EXCEPTION: %s\n", e.what());
            exec_pass = false;
            continue;
        }
        const uint32_t got = ecpu.regs.read(tc.reg);
        const bool     ok  = (got == tc.expected);
        exec_pass &= ok;
        std::printf("  %-36s = 0x%08X  %s\n", tc.desc, got, ok ? "OK" : "FAIL");
    }
    std::printf("  PC after 4 instructions: 0x%08X  %s\n",
                ecpu.pc, ecpu.pc == 16 ? "OK" : "FAIL");
    exec_pass &= (ecpu.pc == 16);

    all_pass &= exec_pass;

    // ── Stage 4a: Countdown loop (ADDI + BNE) ────────────────────────────────
    //
    // Assembler listing:
    //   0x00  addi x1, x0,  5     # x1 = 5  (init counter)
    //   0x04  addi x1, x1, -1     # x1 -= 1  ← loop top
    //   0x08  bne  x1, x0, -4     # if x1 != 0, jump to 0x04
    //         (imm_b = 0x04 - 0x08 = -4)
    //
    // Expected outcome: x1 == 0, PC == 0x0C (5 iterations exactly)
    constexpr uint32_t loop_prog[] = {
        0x00500093u,  // addi x1, x0, 5
        0xFFF08093u,  // addi x1, x1, -1
        0xFE009EE3u,  // bne  x1, x0, -4
    };

    rv32i::CpuState lcpu{};
    {
        std::vector<uint8_t> img(sizeof(loop_prog));
        std::memcpy(img.data(), loop_prog, sizeof(loop_prog));
        lcpu.mem.load_program(img);
    }

    // Step 1: init (addi x1 = 5)
    executor.step(lcpu);

    // Step the loop body until PC exits (at most 100 steps as safety guard)
    int iterations = 0;
    while (lcpu.pc == 0x04 || lcpu.pc == 0x08) {
        executor.step(lcpu);
        if (lcpu.pc == 0x04) ++iterations;
        if (++iterations > 100) break;
    }

    print_separator("Loop: BNE countdown x1=5..0");
    const bool loop_ok = (lcpu.regs.read(1) == 0) && (lcpu.pc == 0x0C);
    std::printf("  x1 = %u  (expect 0)   %s\n", lcpu.regs.read(1),
                lcpu.regs.read(1) == 0 ? "OK" : "FAIL");
    std::printf("  PC = 0x%08X  (expect 0x0C)  %s\n", lcpu.pc,
                lcpu.pc == 0x0C ? "OK" : "FAIL");
    all_pass &= loop_ok;

    // ── Stage 4b: SW + LW round-trip ─────────────────────────────────────────
    //
    //   0x00  addi x2, x0, 0xAB   # x2 = 0xAB
    //   0x04  sw   x2, 0x100(x0)  # mem[0x100] = 0xAB
    //   0x08  lw   x3, 0x100(x0)  # x3 = mem[0x100]
    constexpr uint32_t mem_prog[] = {
        0x0AB00113u,  // addi x2, x0, 0xAB
        0x10202023u,  // sw   x2, 0x100(x0)
        0x10002183u,  // lw   x3, 0x100(x0)
    };

    rv32i::CpuState mcpu{};
    {
        std::vector<uint8_t> img(sizeof(mem_prog));
        std::memcpy(img.data(), mem_prog, sizeof(mem_prog));
        mcpu.mem.load_program(img);
    }

    print_separator("SW + LW round-trip");
    bool mem_ok = true;
    try {
        executor.step(mcpu);  // addi
        executor.step(mcpu);  // sw
        executor.step(mcpu);  // lw
        const uint32_t stored = mcpu.mem.read32(0x100);
        const uint32_t loaded = mcpu.regs.read(3);
        const bool sw_ok = (stored == 0xAB);
        const bool lw_ok = (loaded == 0xAB);
        mem_ok = sw_ok && lw_ok;
        std::printf("  mem[0x100] after SW  = 0x%08X  %s\n", stored, sw_ok ? "OK" : "FAIL");
        std::printf("  x3        after LW   = 0x%08X  %s\n", loaded, lw_ok ? "OK" : "FAIL");
    } catch (const rv32i::MemoryFault& e) {
        std::printf("  UNEXPECTED MemoryFault: %s\n", e.what());
        mem_ok = false;
    }
    all_pass &= mem_ok;

    // ── Stage 4c: MemoryFault guard ──────────────────────────────────────────
    print_separator("MemoryFault guard");
    bool fault_caught = false;
    try {
        static_cast<void>(mcpu.mem.read32(0xFFFF'FF00u));  // only testing the throw
    } catch (const rv32i::MemoryFault& e) {
        fault_caught = true;
        std::printf("  Caught: %s\n", e.what());
        std::printf("  addr=0x%08X  width=%zu  kind=%s  OK\n",
                    e.addr(), e.width(),
                    e.kind() == rv32i::MemoryFault::Kind::Read ? "READ" : "WRITE");
    }
    if (!fault_caught) {
        std::printf("  ERROR: out-of-bounds read did NOT throw!\n");
    }
    all_pass &= fault_caught;

    // ── Summary ───────────────────────────────────────────────────────────────
    print_separator("Result");
    std::printf("  %s\n\n", all_pass ? "All tests passed." : "*** FAILURES DETECTED ***");

    return all_pass ? 0 : 1;
}
