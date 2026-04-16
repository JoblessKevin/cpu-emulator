# C++ 底層開發環境建置 (WSL2 Ubuntu 24.04)

這是一份從零開始的 C++ 環境建置指南，專為習慣高階語言的後端工程師打造。

## Step 1: 系統更新與安裝編譯器 (Toolchain)
在 Ubuntu 終端機執行以下指令。我們將安裝 `build-essential`（包含 gcc/g++ 與 make）以及 `cmake`（C++ 的建置工具）。
```bash
sudo apt update && sudo apt upgrade -y
sudo apt install build-essential cmake gdb git -y