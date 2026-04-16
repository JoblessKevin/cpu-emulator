#pragma once

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace rv32i {

// ── MemoryFault ───────────────────────────────────────────────────────────────
//
// Thrown whenever an instruction tries to read or write outside the
// allocated memory range.  Carries the faulting address, access width, and
// direction so the caller can emit a meaningful diagnostic.
//
// In a real CPU this would trigger a hardware trap (page fault / access fault).
// For our emulator, a C++ exception is the cleanest equivalent — it unwinds
// the call stack back to whoever called step(), without corrupting state.

class MemoryFault : public std::runtime_error {
public:
    enum class Kind : uint8_t { Read, Write };

    MemoryFault(uint32_t addr, std::size_t width, Kind kind)
        : std::runtime_error{make_msg(addr, width, kind)}
        , addr_{addr}, width_{width}, kind_{kind}
    {}

    [[nodiscard]] uint32_t    addr()  const noexcept { return addr_;  }
    [[nodiscard]] std::size_t width() const noexcept { return width_; }
    [[nodiscard]] Kind        kind()  const noexcept { return kind_;  }

private:
    uint32_t    addr_;
    std::size_t width_;
    Kind        kind_;

    static std::string make_msg(uint32_t addr, std::size_t width, Kind kind) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "MemoryFault: %s addr=0x%08X width=%zu",
                      kind == Kind::Read ? "READ" : "WRITE", addr, width);
        return buf;
    }
};

// ── Constants ────────────────────────────────────────────────────────────────

inline constexpr std::size_t kNumRegisters  = 32;
inline constexpr uint32_t    kResetVector   = 0x0000'0000;  // PC start address
inline constexpr std::size_t kDefaultMemory = 1024 * 1024;  // 1 MiB

// ── Memory ───────────────────────────────────────────────────────────────────

// Java analogue: think of this as a byte[] with explicit bounds checking.
// Unlike Java, going out-of-bounds here is Undefined Behaviour — the CPU won't
// throw a NullPointerException, it'll just corrupt memory silently. Hence the
// explicit check in load/store.
class Memory {
public:
    explicit Memory(std::size_t size = kDefaultMemory) : data_(size, 0) {}

    // Load a program image into memory starting at offset 0.
    // std::span<const uint8_t> is like a non-owning view — no copy, no alloc.
    // Java equivalent: Arrays.copyOf, but without the hidden heap allocation.
    void load_program(std::span<const uint8_t> program) {
        if (program.size() > data_.size()) {
            throw std::runtime_error("Program exceeds memory capacity");
        }
        std::copy(program.begin(), program.end(), data_.begin());
    }

    // 32-bit word read (little-endian, matching RISC-V spec)
    [[nodiscard]] uint32_t read32(uint32_t addr) const {
        bounds_check(addr, 4, MemoryFault::Kind::Read);
        // Reconstruct a uint32_t from 4 consecutive bytes, LSB-first.
        // This avoids undefined behaviour from misaligned pointer casts —
        // in C++, casting a byte* to uint32_t* and dereferencing is UB unless
        // the address is 4-byte aligned. Shift-or is always safe.
        return static_cast<uint32_t>(data_[addr])
             | static_cast<uint32_t>(data_[addr + 1]) << 8
             | static_cast<uint32_t>(data_[addr + 2]) << 16
             | static_cast<uint32_t>(data_[addr + 3]) << 24;
    }

    // 8-bit byte read
    [[nodiscard]] uint8_t read8(uint32_t addr) const {
        bounds_check(addr, 1, MemoryFault::Kind::Read);
        return data_[addr];
    }

    // 32-bit word write (little-endian)
    void write32(uint32_t addr, uint32_t value) {
        bounds_check(addr, 4, MemoryFault::Kind::Write);
        data_[addr]     = static_cast<uint8_t>(value);
        data_[addr + 1] = static_cast<uint8_t>(value >> 8);
        data_[addr + 2] = static_cast<uint8_t>(value >> 16);
        data_[addr + 3] = static_cast<uint8_t>(value >> 24);
    }

    // 8-bit byte write
    void write8(uint32_t addr, uint8_t value) {
        bounds_check(addr, 1, MemoryFault::Kind::Write);
        data_[addr] = value;
    }

    // Bulk write: copy `src` bytes into memory starting at `addr`.
    // Used by the ELF loader to place program segments without per-byte overhead.
    // std::span is a non-owning view — the caller owns the source data.
    void write_bytes(uint32_t addr, std::span<const uint8_t> src) {
        if (src.empty()) return;
        bounds_check(addr, src.size(), MemoryFault::Kind::Write);
        // memcpy is defined behaviour here: both sides are uint8_t buffers,
        // and we have already verified the destination range is in-bounds.
        std::memcpy(data_.data() + addr, src.data(), src.size());
    }

    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

private:
    std::vector<uint8_t> data_;

    void bounds_check(uint32_t addr, std::size_t width, MemoryFault::Kind kind) const {
        // Check for both overflow AND out-of-range.
        // The cast to std::size_t is safe: addr is uint32_t (max 4 GiB),
        // width is at most 4 — their sum fits in size_t on any 32/64-bit platform.
        if (static_cast<std::size_t>(addr) + width > data_.size()) {
            throw MemoryFault{addr, width, kind};
        }
    }
};

// ── Register File ────────────────────────────────────────────────────────────

// x0 is hardwired to 0 in RISC-V — writes are silently discarded.
// We enforce this via write(), not by storing a special value.
// std::array lives on the stack (or inline in the struct), not the heap —
// no pointer indirection, better cache locality than Java's Object[].
class RegisterFile {
public:
    RegisterFile() { regs_.fill(0); }

    [[nodiscard]] uint32_t read(std::size_t idx) const noexcept {
        return regs_[idx];   // x0 is already 0 from fill(0)
    }

    void write(std::size_t idx, uint32_t value) noexcept {
        // Branchless x0 enforcement: multiply by (idx != 0).
        // A branch here would be predicted wrong ~3% of the time on real code.
        // This single expression costs one comparison + cmov, no branch.
        regs_[idx] = value * static_cast<uint32_t>(idx != 0);
    }

    // Convenience: dump all registers (useful for debugging)
    [[nodiscard]] const std::array<uint32_t, kNumRegisters>& raw() const noexcept {
        return regs_;
    }

private:
    std::array<uint32_t, kNumRegisters> regs_{};
};

// ── CPU State ────────────────────────────────────────────────────────────────

// Pure data — no execution logic lives here. This is intentional: keeping
// state and behaviour separate makes the emulator easier to test and snapshot.
struct CpuState {
    RegisterFile regs{};
    uint32_t     pc{kResetVector};
    Memory       mem{};

    // Convenience: advance PC by one word (normal, non-branch instruction)
    void advance_pc() noexcept { pc += 4; }
};

}  // namespace rv32i
