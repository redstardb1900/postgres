/*
 * This file is from https://github.com/llvm/llvm-project/pull/71968
 * with minor modifications to avoid name clash and work with older
 * LLVM versions.  The llvm::backport::SectionMemoryManager class is a
 * drop-in replacement for llvm::SectionMemoryManager, for use with
 * llvm::RuntimeDyld.  It fixes a memory layout bug on large memory
 * ARM systems (see pull request for details).  If the LLVM project
 * eventually commits the change, we may need to resynchronize our
 * copy with any further modifications, but they would be unlikely to
 * backport it into the LLVM versions that we target so we would still
 * need this copy.
 *
 * In the future we will switch to using JITLink instead of
 * RuntimeDyld where possible, and later remove this code (.cpp, .h,
 * .LICENSE) after all LLVM versions that we target allow it.
 *
 * This file is a modified copy of a part of the LLVM source code that
 * we would normally access from the LLVM library.  It is therefore
 * covered by the license at https://llvm.org/LICENSE.txt, reproduced
 * verbatim in SectionMemoryManager.LICENSE in fulfillment of clause
 * 4a.  The bugfix changes from the pull request are also covered, per
 * clause 5.
 */

//===- SectionMemoryManager.cpp - Memory manager for MCJIT/RtDyld *- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// 本文件实现 MCJIT 执行引擎与 RuntimeDyld 所用的
// 基于节的内存管理器
//
//===----------------------------------------------------------------------===//

#include "jit/llvmjit_backport.h"

#ifdef USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER

#include "jit/SectionMemoryManager.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Process.h"

namespace llvm {
namespace backport {

bool SectionMemoryManager::hasSpace(const MemoryGroup &MemGroup,
                                    uintptr_t Size) const {
  for (const FreeMemBlock &FreeMB : MemGroup.FreeMem) {
    if (FreeMB.Free.allocatedSize() >= Size)
      return true;
  }
  return false;
}

#if LLVM_VERSION_MAJOR < 16
void SectionMemoryManager::reserveAllocationSpace(uintptr_t CodeSize,
                                                  uint32_t CodeAlign_i,
                                                  uintptr_t RODataSize,
                                                  uint32_t RODataAlign_i,
                                                  uintptr_t RWDataSize,
                                                  uint32_t RWDataAlign_i) {
  Align CodeAlign(CodeAlign_i);
  Align RODataAlign(RODataAlign_i);
  Align RWDataAlign(RWDataAlign_i);
#else
void SectionMemoryManager::reserveAllocationSpace(
    uintptr_t CodeSize, Align CodeAlign, uintptr_t RODataSize,
    Align RODataAlign, uintptr_t RWDataSize, Align RWDataAlign) {
#endif
  if (CodeSize == 0 && RODataSize == 0 && RWDataSize == 0)
    return;

  static const size_t PageSize = sys::Process::getPageSizeEstimate();

  // 代码对齐至少须达到 stub 对齐——但此处不易取得
  // 该值，故权宜假定为 8，这是我在各平台上见到的
  // 最大值。
  constexpr uint64_t StubAlign = 8;
  CodeAlign = Align(std::max(CodeAlign.value(), StubAlign));
  RODataAlign = Align(std::max(RODataAlign.value(), StubAlign));
  RWDataAlign = Align(std::max(RWDataAlign.value(), StubAlign));

  // 取每节所需空间。计算与 allocateSection 相同，
  // 因为必须能满足它。
  uint64_t RequiredCodeSize = alignTo(CodeSize, CodeAlign) + CodeAlign.value();
  uint64_t RequiredRODataSize =
      alignTo(RODataSize, RODataAlign) + RODataAlign.value();
  uint64_t RequiredRWDataSize =
      alignTo(RWDataSize, RWDataAlign) + RWDataAlign.value();

  if (hasSpace(CodeMem, RequiredCodeSize) &&
      hasSpace(RODataMem, RequiredRODataSize) &&
      hasSpace(RWDataMem, RequiredRWDataSize)) {
    // 连续块中已有足够空间。
    return;
  }

  // MemoryManager 没有在分配后再释放内存的函数。
  // 通常它会尽量用因页对齐而多分配的多余块，但若
  // 空闲内存不够本次请求，就会分到相距过远的内存，
  // 从而违反 ARM ABI。清空空闲内存，只用新分配，但
  // 不释放已分配内存，因其可能仍在使用。
  // 
  CodeMem.FreeMem.clear();
  RODataMem.FreeMem.clear();
  RWDataMem.FreeMem.clear();

  // 上取整到最近页大小。块必须按页对齐。
  RequiredCodeSize = alignTo(RequiredCodeSize, PageSize);
  RequiredRODataSize = alignTo(RequiredRODataSize, PageSize);
  RequiredRWDataSize = alignTo(RequiredRWDataSize, PageSize);
  uint64_t RequiredSize =
      RequiredCodeSize + RequiredRODataSize + RequiredRWDataSize;

  std::error_code ec;
  sys::MemoryBlock MB = MMapper->allocateMappedMemory(
      AllocationPurpose::RWData, RequiredSize, nullptr,
      sys::Memory::MF_READ | sys::Memory::MF_WRITE, ec);
  if (ec) {
    return;
  }
  // CodeMem 将任意地拥有此 MemoryBlock 以便清理。
  CodeMem.AllocatedMem.push_back(MB);
  uintptr_t Addr = (uintptr_t)MB.base();
  FreeMemBlock FreeMB;
  FreeMB.PendingPrefixIndex = (unsigned)-1;

  if (CodeSize > 0) {
    assert(isAddrAligned(CodeAlign, (void *)Addr));
    FreeMB.Free = sys::MemoryBlock((void *)Addr, RequiredCodeSize);
    CodeMem.FreeMem.push_back(FreeMB);
    Addr += RequiredCodeSize;
  }

  if (RODataSize > 0) {
    assert(isAddrAligned(RODataAlign, (void *)Addr));
    FreeMB.Free = sys::MemoryBlock((void *)Addr, RequiredRODataSize);
    RODataMem.FreeMem.push_back(FreeMB);
    Addr += RequiredRODataSize;
  }

  if (RWDataSize > 0) {
    assert(isAddrAligned(RWDataAlign, (void *)Addr));
    FreeMB.Free = sys::MemoryBlock((void *)Addr, RequiredRWDataSize);
    RWDataMem.FreeMem.push_back(FreeMB);
  }
}

uint8_t *SectionMemoryManager::allocateDataSection(uintptr_t Size,
                                                   unsigned Alignment,
                                                   unsigned SectionID,
                                                   StringRef SectionName,
                                                   bool IsReadOnly) {
  if (IsReadOnly)
    return allocateSection(SectionMemoryManager::AllocationPurpose::ROData,
                           Size, Alignment);
  return allocateSection(SectionMemoryManager::AllocationPurpose::RWData, Size,
                         Alignment);
}

uint8_t *SectionMemoryManager::allocateCodeSection(uintptr_t Size,
                                                   unsigned Alignment,
                                                   unsigned SectionID,
                                                   StringRef SectionName) {
  return allocateSection(SectionMemoryManager::AllocationPurpose::Code, Size,
                         Alignment);
}

uint8_t *SectionMemoryManager::allocateSection(
    SectionMemoryManager::AllocationPurpose Purpose, uintptr_t Size,
    unsigned Alignment) {
  if (!Alignment)
    Alignment = 16;

  assert(!(Alignment & (Alignment - 1)) && "Alignment must be a power of two.");

  uintptr_t RequiredSize = Alignment * ((Size + Alignment - 1) / Alignment + 1);
  uintptr_t Addr = 0;

  MemoryGroup &MemGroup = [&]() -> MemoryGroup & {
    switch (Purpose) {
    case AllocationPurpose::Code:
      return CodeMem;
    case AllocationPurpose::ROData:
      return RODataMem;
    case AllocationPurpose::RWData:
      return RWDataMem;
    }
    llvm_unreachable("Unknown SectionMemoryManager::AllocationPurpose");
  }();

  // 在空闲内存区列表中查找，若有一块够用就用。
  // 
  for (FreeMemBlock &FreeMB : MemGroup.FreeMem) {
    if (FreeMB.Free.allocatedSize() >= RequiredSize) {
      Addr = (uintptr_t)FreeMB.Free.base();
      uintptr_t EndOfBlock = Addr + FreeMB.Free.allocatedSize();
      // 对齐该地址。
      Addr = (Addr + Alignment - 1) & ~(uintptr_t)(Alignment - 1);

      if (FreeMB.PendingPrefixIndex == (unsigned)-1) {
        // 交给用户的那部分块现在处于 pending
        MemGroup.PendingMem.push_back(sys::MemoryBlock((void *)Addr, Size));

        // 记住此 pending 块，使后续分配只需改它而不必
        // 新建一个
        FreeMB.PendingPrefixIndex = MemGroup.PendingMem.size() - 1;
      } else {
        sys::MemoryBlock &PendingMB =
            MemGroup.PendingMem[FreeMB.PendingPrefixIndex];
        PendingMB = sys::MemoryBlock(PendingMB.base(),
                                     Addr + Size - (uintptr_t)PendingMB.base());
      }

      // 记住此块现在还剩多少空闲空间
      FreeMB.Free =
          sys::MemoryBlock((void *)(Addr + Size), EndOfBlock - Addr - Size);
      return (uint8_t *)Addr;
    }
  }

  // 没有够大的预分配空闲块。分配新的内存区域。
  // 注意所有节都按读写分配。权限稍后按内存组
  // 再更新。
  //
  // FIXME: 定义默认分配大小（或作构造参数）会有用，
  // 以减少分配次数。
  //
  // FIXME: 初始化各内存组的 Near 成员以免交错。
  // 
  std::error_code ec;
  sys::MemoryBlock MB = MMapper->allocateMappedMemory(
      Purpose, RequiredSize, &MemGroup.Near,
      sys::Memory::MF_READ | sys::Memory::MF_WRITE, ec);
  if (ec) {
    // FIXME: 给接口加上错误传播。
    return nullptr;
  }

  // 保存此地址作为下次请求的基准
  MemGroup.Near = MB;

  // 若其他组尚未初始化，把该地址拷过去。
  // 
  if (CodeMem.Near.base() == nullptr)
    CodeMem.Near = MB;
  if (RODataMem.Near.base() == nullptr)
    RODataMem.Near = MB;
  if (RWDataMem.Near.base() == nullptr)
    RWDataMem.Near = MB;

  // 记住我们分配了这块内存
  MemGroup.AllocatedMem.push_back(MB);
  Addr = (uintptr_t)MB.base();
  uintptr_t EndOfBlock = Addr + MB.allocatedSize();

  // 对齐该地址。
  Addr = (Addr + Alignment - 1) & ~(uintptr_t)(Alignment - 1);

  // 交给用户的那部分块现在处于 pending
  MemGroup.PendingMem.push_back(sys::MemoryBlock((void *)Addr, Size));

  // allocateMappedMemory 可能分配远超所需的内存。此时
  // 把未用内存存成空闲块。
  unsigned FreeSize = EndOfBlock - Addr - Size;
  if (FreeSize > 16) {
    FreeMemBlock FreeMB;
    FreeMB.Free = sys::MemoryBlock((void *)(Addr + Size), FreeSize);
    FreeMB.PendingPrefixIndex = (unsigned)-1;
    MemGroup.FreeMem.push_back(FreeMB);
  }

  // 返回对齐地址
  return (uint8_t *)Addr;
}

bool SectionMemoryManager::finalizeMemory(std::string *ErrMsg) {
  // FIXME: 出错时是否应撤销进行中的权限更改？
  std::error_code ec;

  // 使代码内存可执行。
  ec = applyMemoryGroupPermissions(CodeMem,
                                   sys::Memory::MF_READ | sys::Memory::MF_EXEC);
  if (ec) {
    if (ErrMsg) {
      *ErrMsg = ec.message();
    }
    return true;
  }

  // 使只读数据内存变为只读。
  ec = applyMemoryGroupPermissions(RODataMem, sys::Memory::MF_READ);
  if (ec) {
    if (ErrMsg) {
      *ErrMsg = ec.message();
    }
    return true;
  }

  // 读写数据内存权限已经正确

  // 有些平台数据缓存与指令缓存分离，须刷新缓存，
  // 否则 JIT 代码改动（如已解析的重定位）只进数据
  // 缓存而进不了指令缓存。
  invalidateInstructionCache();

  return false;
}

static sys::MemoryBlock trimBlockToPageSize(sys::MemoryBlock M) {
  static const size_t PageSize = sys::Process::getPageSizeEstimate();

  size_t StartOverlap =
      (PageSize - ((uintptr_t)M.base() % PageSize)) % PageSize;

  size_t TrimmedSize = M.allocatedSize();
  TrimmedSize -= StartOverlap;
  TrimmedSize -= TrimmedSize % PageSize;

  sys::MemoryBlock Trimmed((void *)((uintptr_t)M.base() + StartOverlap),
                           TrimmedSize);

  assert(((uintptr_t)Trimmed.base() % PageSize) == 0);
  assert((Trimmed.allocatedSize() % PageSize) == 0);
  assert(M.base() <= Trimmed.base() &&
         Trimmed.allocatedSize() <= M.allocatedSize());

  return Trimmed;
}

std::error_code
SectionMemoryManager::applyMemoryGroupPermissions(MemoryGroup &MemGroup,
                                                  unsigned Permissions) {
  for (sys::MemoryBlock &MB : MemGroup.PendingMem)
    if (std::error_code EC = MMapper->protectMappedMemory(MB, Permissions))
      return EC;

  MemGroup.PendingMem.clear();

  // 再扫空闲块，裁掉那些未覆盖整页的，因为某个
  // pending 块可能与之重叠。
  for (FreeMemBlock &FreeMB : MemGroup.FreeMem) {
    FreeMB.Free = trimBlockToPageSize(FreeMB.Free);
    // 已清空 PendingMem 列表，故这些指针现在都无效
    FreeMB.PendingPrefixIndex = (unsigned)-1;
  }

  // 去掉现已为空的块
  erase_if(MemGroup.FreeMem, [](FreeMemBlock &FreeMB) {
    return FreeMB.Free.allocatedSize() == 0;
  });

  return std::error_code();
}

void SectionMemoryManager::invalidateInstructionCache() {
  for (sys::MemoryBlock &Block : CodeMem.PendingMem)
    sys::Memory::InvalidateInstructionCache(Block.base(),
                                            Block.allocatedSize());
}

SectionMemoryManager::~SectionMemoryManager() {
  for (MemoryGroup *Group : {&CodeMem, &RWDataMem, &RODataMem}) {
    for (sys::MemoryBlock &Block : Group->AllocatedMem)
      MMapper->releaseMappedMemory(Block);
  }
}

SectionMemoryManager::MemoryMapper::~MemoryMapper() = default;

void SectionMemoryManager::anchor() {}

namespace {
// SectionMemoryManager::MemoryMapper 的平凡实现，只调用
// sys::Memory。
class DefaultMMapper final : public SectionMemoryManager::MemoryMapper {
public:
  sys::MemoryBlock
  allocateMappedMemory(SectionMemoryManager::AllocationPurpose Purpose,
                       size_t NumBytes, const sys::MemoryBlock *const NearBlock,
                       unsigned Flags, std::error_code &EC) override {
    return sys::Memory::allocateMappedMemory(NumBytes, NearBlock, Flags, EC);
  }

  std::error_code protectMappedMemory(const sys::MemoryBlock &Block,
                                      unsigned Flags) override {
    return sys::Memory::protectMappedMemory(Block, Flags);
  }

  std::error_code releaseMappedMemory(sys::MemoryBlock &M) override {
    return sys::Memory::releaseMappedMemory(M);
  }
};
} // namespace

SectionMemoryManager::SectionMemoryManager(MemoryMapper *UnownedMM,
                                           bool ReserveAlloc)
    : MMapper(UnownedMM), OwnedMMapper(nullptr),
      ReserveAllocation(ReserveAlloc) {
  if (!MMapper) {
    OwnedMMapper = std::make_unique<DefaultMMapper>();
    MMapper = OwnedMMapper.get();
  }
}

} // namespace backport
} // namespace llvm

#endif
