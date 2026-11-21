/*
 * 可供回移植 LLVM 代码或 PostgreSQL 代码包含的
 * 小头，用以控制条件编译。
 */
#ifndef LLVMJIT_BACKPORT_H
#define LLVMJIT_BACKPORT_H

#include <llvm/Config/llvm-config.h>

/*
 * LLVM 的 RuntimeDyld 在大内存 ARM 上可能生成会崩溃的
 * 代码，因 llvm::SectionMemoryManager 分配的多块
 * 内存可能相距过远，超出生成代码可达范围。见
 * src/backend/jit/llvm/SectionMemoryManager.cpp 中作为替代的
 * 修补类 llvm::backport::SectionMemoryManager。
 * 本头文件控制是否使用它。
 *
 * 我们已调整其以适配一系列 LLVM 版本，但目前
 * 不早于 12。
 */
#if defined(__aarch64__) && LLVM_VERSION_MAJOR > 11
#define USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER
#endif

#endif
