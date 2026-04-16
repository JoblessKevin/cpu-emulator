#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

#include "cpu.hpp"

namespace rv32i {

// ═════════════════════════════════════════════════════════════════════════════
// ELF32 Type Aliases
// Names follow the System V ABI specification exactly — this makes it easy
// to cross-reference the spec while reading the code.
// ═════════════════════════════════════════════════════════════════════════════

using Elf32_Addr  = uint32_t;   // Program/virtual addresses
using Elf32_Off   = uint32_t;   // File offsets
using Elf32_Half  = uint16_t;   // 16-bit integer fields
using Elf32_Word  = uint32_t;   // 32-bit integer fields
using Elf32_Sword = int32_t;    // 32-bit signed integer fields

// ── ELF Identification (e_ident) ─────────────────────────────────────────────

inline constexpr std::size_t EI_NIDENT  = 16;   // size of e_ident[]
inline constexpr std::size_t EI_MAG0    =  0;   // '\x7f'
inline constexpr std::size_t EI_MAG1    =  1;   // 'E'
inline constexpr std::size_t EI_MAG2    =  2;   // 'L'
inline constexpr std::size_t EI_MAG3    =  3;   // 'F'
inline constexpr std::size_t EI_CLASS   =  4;   // object file class
inline constexpr std::size_t EI_DATA    =  5;   // data encoding
inline constexpr std::size_t EI_VERSION =  6;   // ELF version

inline constexpr uint8_t ELFMAG0      = 0x7F;
inline constexpr uint8_t ELFMAG1      = 'E';
inline constexpr uint8_t ELFMAG2      = 'L';
inline constexpr uint8_t ELFMAG3      = 'F';
inline constexpr uint8_t ELFCLASS32   = 1;      // 32-bit object
inline constexpr uint8_t ELFDATA2LSB  = 1;      // little-endian data encoding
inline constexpr uint8_t EV_CURRENT   = 1;      // current ELF version

// ── Object File Types (e_type) ────────────────────────────────────────────────

inline constexpr Elf32_Half ET_EXEC = 2;         // Executable file

// ── Machine Architectures (e_machine) ────────────────────────────────────────

inline constexpr Elf32_Half EM_RISCV = 0xF3;    // RISC-V (= 243 decimal)

// ── Segment Types (p_type) ────────────────────────────────────────────────────

inline constexpr Elf32_Word PT_LOAD  = 1;        // Loadable program segment
inline constexpr Elf32_Word PT_NULL  = 0;        // Program header table entry unused

// ── Segment Permission Flags (p_flags) ───────────────────────────────────────

inline constexpr Elf32_Word PF_X = 1u;           // Execute
inline constexpr Elf32_Word PF_W = 2u;           // Write
inline constexpr Elf32_Word PF_R = 4u;           // Read

// ═════════════════════════════════════════════════════════════════════════════
// ELF32 Header — Elf32_Ehdr (52 bytes)
//
// Layout (verified against System V ABI — zero compiler padding):
//
//   Offset  Size  Field
//   ──────  ────  ──────────────────────────────────────────────────────────
//   +0      16    e_ident   ['\x7f','E','L','F', class, data, ver, ...]
//   +16      2    e_type    object file type
//   +18      2    e_machine target ISA (0xF3 = RISC-V)
//   +20      4    e_version (must be EV_CURRENT = 1)
//   +24      4    e_entry   virtual address of _start / entry point
//   +28      4    e_phoff   byte offset of program header table in file
//   +32      4    e_shoff   byte offset of section header table in file
//   +36      4    e_flags   architecture-specific flags (RVC, float ABI…)
//   +40      2    e_ehsize  size of THIS header (must be 52 for ELF32)
//   +42      2    e_phentsize  size of one program header entry (must be 32)
//   +44      2    e_phnum   number of program header entries
//   +46      2    e_shentsize  size of one section header entry
//   +48      2    e_shnum   number of section header entries
//   +50      2    e_shstrndx   section name string table index
//   ──────  ────
//   total   52
//
// No padding is inserted because:
//   - e_ident ends at offset 16 (even), so uint16_t fields are 2-byte aligned ✓
//   - After the two uint16_t fields (4 bytes), we reach offset 20, which is
//     4-byte aligned — all subsequent uint32_t fields stay aligned ✓
// ═════════════════════════════════════════════════════════════════════════════

struct Elf32_Ehdr {
    uint8_t    e_ident[EI_NIDENT];   // +0   Magic, class, data encoding, version
    Elf32_Half e_type;                // +16  Object file type
    Elf32_Half e_machine;             // +18  Target architecture
    Elf32_Word e_version;             // +20  ELF format version (must be 1)
    Elf32_Addr e_entry;               // +24  Virtual address of entry point
    Elf32_Off  e_phoff;               // +28  File offset of program header table
    Elf32_Off  e_shoff;               // +32  File offset of section header table
    Elf32_Word e_flags;               // +36  Processor-specific flags
    Elf32_Half e_ehsize;              // +40  Size of this ELF header (52)
    Elf32_Half e_phentsize;           // +42  Size of one program header entry (32)
    Elf32_Half e_phnum;               // +44  Number of program header entries
    Elf32_Half e_shentsize;           // +46  Size of one section header entry
    Elf32_Half e_shnum;               // +48  Number of section header entries
    Elf32_Half e_shstrndx;            // +50  Section name string table index
};

// If either of these fires, the compiler inserted unexpected padding.
// Fix: add #pragma pack(push, 1) / #pragma pack(pop) around the struct.
static_assert(sizeof(Elf32_Ehdr) == 52,
    "Elf32_Ehdr layout broken — compiler inserted padding");
static_assert(alignof(Elf32_Ehdr) <= 4,
    "Elf32_Ehdr over-aligned — may misread from raw file bytes");

// ═════════════════════════════════════════════════════════════════════════════
// ELF32 Program Header — Elf32_Phdr (32 bytes)
//
// Each entry describes one *segment* — a contiguous region loaded into memory.
// We care about PT_LOAD entries: they tell us what to put where.
//
// Key fields for a bare-metal RISC-V loader:
//
//   p_type   == PT_LOAD  → must process this entry
//   p_offset            → where the segment data starts IN THE ELF FILE
//   p_vaddr             → where to place the data IN MEMORY
//   p_filesz            → how many bytes to copy from the file
//   p_memsz             → how many bytes the segment occupies in memory
//                         (p_memsz >= p_filesz;  extra bytes = BSS, zero-filled)
//   p_flags             → PF_R | PF_W | PF_X  (ignored on bare-metal emulator)
//
// Layout (all uint32_t → zero padding, trivially verified):
//   +0  p_type   +4  p_offset  +8  p_vaddr  +12  p_paddr
//   +16 p_filesz +20 p_memsz   +24 p_flags  +28  p_align
// ═════════════════════════════════════════════════════════════════════════════

struct Elf32_Phdr {
    Elf32_Word p_type;    // +0   Segment type (PT_LOAD = 1 → must load)
    Elf32_Off  p_offset;  // +4   Offset of segment data within the ELF file
    Elf32_Addr p_vaddr;   // +8   Target virtual address in memory
    Elf32_Addr p_paddr;   // +12  Physical address (= vaddr on bare-metal)
    Elf32_Word p_filesz;  // +16  Bytes in file  (0 for pure BSS segment)
    Elf32_Word p_memsz;   // +20  Bytes in memory (BSS = memsz − filesz, zero-filled)
    Elf32_Word p_flags;   // +24  Permission flags (PF_R / PF_W / PF_X)
    Elf32_Word p_align;   // +28  Alignment requirement (power of 2)
};

static_assert(sizeof(Elf32_Phdr) == 32,
    "Elf32_Phdr layout broken — compiler inserted padding");
static_assert(alignof(Elf32_Phdr) <= 4,
    "Elf32_Phdr over-aligned — may misread from raw file bytes");

// ═════════════════════════════════════════════════════════════════════════════
// ELF Loader Interface
// ═════════════════════════════════════════════════════════════════════════════

// Thrown when the ELF file is malformed, wrong architecture, or cannot be
// opened.  Distinct from MemoryFault (which fires when vaddr is out of bounds).
class ElfError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Result returned by load_elf on success.
struct ElfLoadResult {
    uint32_t    entry;            // ELF e_entry — set cpu.pc to this
    std::size_t segments_loaded;  // Number of PT_LOAD segments processed
};

// ── load_elf ─────────────────────────────────────────────────────────────────
//
// Parse an ELF32/RISC-V little-endian executable at `path`.
// For every PT_LOAD program header:
//   1. Copy p_filesz bytes from file offset p_offset → mem at p_vaddr.
//   2. Zero-fill the remaining (p_memsz − p_filesz) bytes (BSS region).
//
// Preconditions:
//   - `mem` must have been constructed with sufficient capacity for all
//     virtual addresses used by the ELF (default 1 MiB is fine for our tests).
//
// Throws:
//   - ElfError      — bad magic, wrong class/machine/endianness, I/O error
//   - MemoryFault   — a segment's vaddr + size exceeds mem.size()
//
// Usage:
//   rv32i::CpuState cpu{};
//   auto [entry, n] = rv32i::load_elf("hello.elf", cpu.mem);
//   cpu.pc = entry;
ElfLoadResult load_elf(const std::string& path, Memory& mem);

}  // namespace rv32i
