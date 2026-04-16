# cpu-emulator
A high-performance RISC-V (RV32I) CPU emulator written in modern C++20, featuring O(1) branchless instruction dispatch and bitwise-optimized decoding.

# RV32I C++ Emulator: From Java to Silicon

這是一個由 Java 後端工程師發起，旨在探索底層運算、Bitwise 操作與 O(1) 效能極致的 RISC-V 32-bit (RV32I) 指令集模擬器。

## 技術棧
* **語言**: C++20
* **建置系統**: CMake
* **開發環境**: WSL2 (Ubuntu 24.04) + AntiGravity IDE

## 架構與里程碑 (Milestones)

- [ ] **Stage 0: 基礎設施**
  - 建立 CMake 專案結構。
  - 定義 `CMakeLists.txt`。
- [ ] **Stage 1: CPU 狀態機 (State & Memory)**
  - 實作 32 個通用暫存器 (`std::array<uint32_t, 32>`)。
  - 實作 Program Counter (PC)。
  - 實作線性記憶體佈局 (`std::vector<uint8_t>`)。
- [ ] **Stage 2: 取指與解碼 (Fetch & Decode)**
  - 精準的 Bitwise 操作，拆解 32-bit 指令 (Opcode, Registers, Immediates)。
- [ ] **Stage 3: O(1) 指令分發 (Execute & Dispatch)**
  - 捨棄龐大的 `switch-case`。
  - 實作基於函式指標陣列的 Branchless Dispatcher。
- [ ] **Stage 4: 系統整合與測試**
  - 載入並執行第一支 RISC-V 組合語言二進位檔 (e.g., 簡單的加法迴圈)。

## 啟動方式 (Build Instructions)
```bash
mkdir build && cd build
cmake ..
make
./rv_emulator