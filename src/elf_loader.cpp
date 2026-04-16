#include "elf_loader.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace rv32i {

// ── Internal helper ───────────────────────────────────────────────────────────
//
// Seek to `offset` in `file` and read exactly `size` bytes into `dest`.
//
// C++ note: ifstream::read() accepts a `char*` — not because we're reading
// text, but because `char` is the language's canonical "byte" type (along with
// `unsigned char`/`std::byte`).  This is one of char's two special roles in
// the type system: (1) it can alias any other type without violating strict
// aliasing, and (2) it's what stream I/O uses for raw byte buffers.
//
// Reading into a POD struct directly is well-defined here:
//   - Both Elf32_Ehdr and Elf32_Phdr are trivially copyable.
//   - Their sizes match the on-disk layout exactly (verified by static_assert).
//   - The file is opened in binary mode, so no newline translation occurs.
static void read_at(std::ifstream& file,
                    std::streamoff  offset,
                    void*           dest,
                    std::streamsize size)
{
    file.seekg(offset);
    if (!file) {
        throw ElfError{"ELF: seek failed at offset " + std::to_string(offset)};
    }

    file.read(static_cast<char*>(dest), size);

    if (!file || file.gcount() != size) {
        throw ElfError{
            "ELF: short read at offset " + std::to_string(offset) +
            " (wanted " + std::to_string(size) +
            ", got "    + std::to_string(file.gcount()) + ")"
        };
    }
}

// ── load_elf ──────────────────────────────────────────────────────────────────

ElfLoadResult load_elf(const std::string& path, Memory& mem)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw ElfError{"ELF: cannot open '" + path + "'"};
    }

    // ── Step 1: Read and validate the ELF header ──────────────────────────────

    Elf32_Ehdr ehdr{};
    read_at(file, 0, &ehdr, sizeof(ehdr));

    // Magic number check — the four-byte ELF signature
    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr.e_ident[EI_MAG3] != ELFMAG3)
    {
        throw ElfError{"ELF: invalid magic — not an ELF file"};
    }

    // Class check: we only support 32-bit ELF
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS32) {
        throw ElfError{
            "ELF: wrong class (got " +
            std::to_string(ehdr.e_ident[EI_CLASS]) +
            ", expected ELFCLASS32=1)"
        };
    }

    // Endianness check: RISC-V is little-endian; so are our host's uint16_t reads
    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        throw ElfError{"ELF: not little-endian (expected ELFDATA2LSB=1)"};
    }

    // Type check: must be an executable (not a .so or relocatable .o)
    if (ehdr.e_type != ET_EXEC) {
        throw ElfError{
            "ELF: not an executable (e_type=" +
            std::to_string(ehdr.e_type) +
            ", expected ET_EXEC=2)"
        };
    }

    // Machine check: must target RISC-V
    if (ehdr.e_machine != EM_RISCV) {
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "ELF: wrong architecture (e_machine=0x%X, expected EM_RISCV=0xF3)",
                      ehdr.e_machine);
        throw ElfError{buf};
    }

    // Sanity-check the program header entry size reported by the file itself
    if (ehdr.e_phentsize != sizeof(Elf32_Phdr)) {
        throw ElfError{
            "ELF: unexpected e_phentsize=" +
            std::to_string(ehdr.e_phentsize) +
            " (expected " + std::to_string(sizeof(Elf32_Phdr)) + ")"
        };
    }

    if (ehdr.e_phnum == 0) {
        throw ElfError{"ELF: no program headers — nothing to load"};
    }

    // ── Step 2: Iterate program headers, load PT_LOAD segments ───────────────

    std::size_t segments_loaded = 0;

    for (Elf32_Half i = 0; i < ehdr.e_phnum; ++i) {

        Elf32_Phdr phdr{};
        const auto phdr_off =
            static_cast<std::streamoff>(ehdr.e_phoff) +
            static_cast<std::streamoff>(i) * static_cast<std::streamoff>(sizeof(Elf32_Phdr));

        read_at(file, phdr_off, &phdr, sizeof(phdr));

        if (phdr.p_type != PT_LOAD) continue;  // ignore NOTE, DYNAMIC, etc.
        if (phdr.p_memsz == 0)      continue;  // empty segment — nothing to do

        // ── 2a. Copy the file portion (text, data, rodata) ───────────────────
        if (phdr.p_filesz > 0) {
            // Read p_filesz bytes from the ELF file into a temporary buffer,
            // then bulk-copy into Memory.  This is one syscall for the read
            // and one memcpy — significantly faster than byte-by-byte write8().
            std::vector<uint8_t> segment_data(phdr.p_filesz);
            read_at(file,
                    static_cast<std::streamoff>(phdr.p_offset),
                    segment_data.data(),
                    static_cast<std::streamsize>(phdr.p_filesz));

            // Memory::write_bytes handles bounds checking — throws MemoryFault
            // if p_vaddr + p_filesz exceeds the allocated region.
            mem.write_bytes(phdr.p_vaddr,
                            std::span<const uint8_t>{segment_data});
        }

        // ── 2b. Zero-fill the BSS region (p_memsz > p_filesz) ───────────────
        //
        // BSS ("Block Started by Symbol") is uninitialized static data.
        // The linker allocates space for it in memory (p_memsz) but does NOT
        // store it in the ELF file (p_filesz < p_memsz).  The C standard
        // mandates zero-initialization, so we must fill the gap ourselves.
        //
        // Example: a `static int arr[1024]` would contribute 4096 bytes to
        // BSS — p_filesz += 0, p_memsz += 4096.
        if (phdr.p_memsz > phdr.p_filesz) {
            const uint32_t bss_start = phdr.p_vaddr  + phdr.p_filesz;
            const uint32_t bss_size  = phdr.p_memsz  - phdr.p_filesz;

            // Build a zero buffer and write it in one shot.
            // For large BSS regions, a smarter Memory could zero-on-demand,
            // but for a 1 MiB emulator heap this is fine.
            const std::vector<uint8_t> zeros(bss_size, 0u);
            mem.write_bytes(bss_start, std::span<const uint8_t>{zeros});
        }

        ++segments_loaded;
    }

    if (segments_loaded == 0) {
        throw ElfError{"ELF: no PT_LOAD segments found — empty program"};
    }

    return ElfLoadResult{ehdr.e_entry, segments_loaded};
}

}  // namespace rv32i
