/*-------------------------------------------------------------------------
 *
 * llvmjit_wrap.cpp
 *	  LLVM 接口中尚未向 C 暴露的部分。
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/llvm/llvmjit_wrap.cpp
 *
 *-------------------------------------------------------------------------
 */

extern "C"
{
#include "postgres.h"
}

#include <llvm-c/Core.h>

/* 避免与 LLVM 的 C++ 头发生宏冲突 */
#undef Min

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#if LLVM_VERSION_MAJOR < 17
#include <llvm/MC/SubtargetFeature.h>
#endif
#if LLVM_VERSION_MAJOR > 16
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif

#include "jit/llvmjit.h"
#include "jit/llvmjit_backport.h"

#ifdef USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include "jit/SectionMemoryManager.h"
#include <llvm/Support/CBindingWrapping.h>
#endif


/*
 * C-API 扩展。
 */
#if defined(HAVE_DECL_LLVMGETHOSTCPUNAME) && !HAVE_DECL_LLVMGETHOSTCPUNAME
char *LLVMGetHostCPUName(void) {
	return strdup(llvm::sys::getHostCPUName().data());
}
#endif


#if defined(HAVE_DECL_LLVMGETHOSTCPUFEATURES) && !HAVE_DECL_LLVMGETHOSTCPUFEATURES
char *LLVMGetHostCPUFeatures(void) {
	llvm::SubtargetFeatures Features;
	llvm::StringMap<bool> HostFeatures;

	if (llvm::sys::getHostCPUFeatures(HostFeatures))
		for (auto &F : HostFeatures)
			Features.AddFeature(F.first(), F.second);

	return strdup(Features.getString().c_str());
}
#endif

/*
 * 类似 LLVMGetAttributeCountAtIndex()，绕开 LLVM 3.9 的缺陷。
 *
 * LLVM <= 3.9 在某索引无属性时，LLVMGetAttributeCountAtIndex()
 * 会段错误（已在 LLVM 提交 ce9bb1097dc2 修复）。
 */
unsigned
LLVMGetAttributeCountAtIndexPG(LLVMValueRef F, uint32 Idx)
{
	/*
	 * 这更贵，故仅在有问题的 LLVM 版本上做。
	 *
	 */
#if LLVM_VERSION_MAJOR < 4
	if (!llvm::unwrap<llvm::Function>(F)->getAttributes().hasAttributes(Idx))
		return 0;
#endif

	/*
	 * 没有好用的公开 API 能干净地取个数，故一律
	 * 回退到 LLVM 的 C API。
	 */
	return LLVMGetAttributeCountAtIndex(F, Idx);
}

LLVMTypeRef
LLVMGetFunctionReturnType(LLVMValueRef r)
{
	return llvm::wrap(llvm::unwrap<llvm::Function>(r)->getReturnType());
}

LLVMTypeRef
LLVMGetFunctionType(LLVMValueRef r)
{
	return llvm::wrap(llvm::unwrap<llvm::Function>(r)->getFunctionType());
}

#if LLVM_VERSION_MAJOR < 8
LLVMTypeRef
LLVMGlobalGetValueType(LLVMValueRef g)
{
	return llvm::wrap(llvm::unwrap<llvm::GlobalValue>(g)->getValueType());
}
#endif

#ifdef USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(llvm::orc::ExecutionSession, LLVMOrcExecutionSessionRef)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(llvm::orc::ObjectLayer, LLVMOrcObjectLayerRef);

LLVMOrcObjectLayerRef
LLVMOrcCreateRTDyldObjectLinkingLayerWithSafeSectionMemoryManager(LLVMOrcExecutionSessionRef ES)
{
	return wrap(new llvm::orc::RTDyldObjectLinkingLayer(
		*unwrap(ES), [] { return std::make_unique<llvm::backport::SectionMemoryManager>(nullptr, true); }));
}
#endif
