/*-------------------------------------------------------------------------
 *
 * llvmjit_inline.cpp
 *	  适合 postgres JIT 的跨模块内联
 *
 * 内联器遍历传入模块所引用的外部函数并尝试内联。
 * 做法是利用预先建好的、覆盖 postgres 核心与扩展
 * 模块的索引。找到外部函数匹配时——不保证！——用
 * 索引判断其指令数/内联价值。对全部外部函数做完后，
 * 导入所有被引用函数（及其前置依赖）。
 *
 *
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/llvmjit/llvmjit_inline.cpp
 *
 *-------------------------------------------------------------------------
 */

extern "C"
{
#include "postgres.h"
}

#include "jit/llvmjit.h"

extern "C"
{
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/string.h"
#include "miscadmin.h"
#include "storage/fd.h"
}

#include <llvm-c/Core.h>
#include <llvm-c/BitReader.h>

/* 避免与 LLVM 的 C++ 头发生宏冲突 */
#undef Min

#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Analysis/ModuleSummaryAnalysis.h>
#if LLVM_VERSION_MAJOR > 3
#include <llvm/Bitcode/BitcodeReader.h>
#else
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/Support/Error.h>
#endif
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/ModuleSummaryIndex.h>
#include <llvm/Linker/IRMover.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/MemoryBuffer.h>


/*
 * 表示搜索 InlineWorkListItem 主体时所查模块的类型。
 *
 */
typedef llvm::SmallVector<llvm::ModuleSummaryIndex *, 2> InlineSearchPath;

/*
 * 待检查符号队列中的项，以及对应队列。
 */
typedef struct InlineWorkListItem
{
	llvm::StringRef symbolName;
	llvm::SmallVector<llvm::ModuleSummaryIndex *, 2> searchpath;
} InlineWorkListItem;
typedef llvm::SmallVector<InlineWorkListItem, 128> InlineWorkList;

/*
 * 内联期间已处理符号的信息。用于避免重复搜索
 * 并提供附加信息。
 */
typedef struct FunctionInlineState
{
	int costLimit;
	bool processed;
	bool inlined;
	bool allowReconsidering;
} FunctionInlineState;
typedef llvm::StringMap<FunctionInlineState> FunctionInlineStates;

/*
 * 应内联的模块映射，值为待内联符号集合。
 *
 */
typedef llvm::StringMap<llvm::StringSet<> > ImportMapTy;


const float inline_cost_decay_factor = 0.5;
const int inline_initial_cost = 150;

/*
 * 这些是 managed static，好让 LLVM 在 LLVMShutdown()
 * 期间释放，而不是之后（之后会崩溃）。
 */
typedef llvm::StringMap<std::unique_ptr<llvm::Module> > ModuleCache;
llvm::ManagedStatic<ModuleCache> module_cache;
typedef llvm::StringMap<std::unique_ptr<llvm::ModuleSummaryIndex> > SummaryCache;
llvm::ManagedStatic<SummaryCache> summary_cache;


static std::unique_ptr<ImportMapTy> llvm_build_inline_plan(LLVMContextRef lc, llvm::Module *mod);
static void llvm_execute_inline_plan(llvm::Module *mod,
									 ImportMapTy *globalsToInline);

static llvm::Module* load_module_cached(LLVMContextRef c, llvm::StringRef modPath);
static std::unique_ptr<llvm::Module> load_module(LLVMContextRef c, llvm::StringRef Identifier);
static std::unique_ptr<llvm::ModuleSummaryIndex> llvm_load_summary(llvm::StringRef path);


static llvm::Function* create_redirection_function(std::unique_ptr<llvm::Module> &importMod,
												   llvm::Function *F,
												   llvm::StringRef Name);

static bool function_inlinable(llvm::Function &F,
							   int threshold,
							   FunctionInlineStates &functionState,
							   InlineWorkList &worklist,
							   InlineSearchPath &searchpath,
							   llvm::SmallPtrSet<const llvm::Function *, 8> &visitedFunctions,
							   int &running_instcount,
							   llvm::StringSet<> &importVars);
static void function_references(llvm::Function &F,
								int &running_instcount,
								llvm::SmallPtrSet<llvm::GlobalVariable *, 8> &referencedVars,
								llvm::SmallPtrSet<llvm::Function *, 8> &referencedFunctions);

static void add_module_to_inline_search_path(InlineSearchPath& path, llvm::StringRef modpath);
static llvm::SmallVector<llvm::GlobalValueSummary *, 1>
summaries_for_guid(const InlineSearchPath& path, llvm::GlobalValue::GUID guid);

/* 内联器开发用的详细调试 */
/* #define INLINE_DEBUG */
#ifdef INLINE_DEBUG
#define ilog		elog
#else
#define ilog(...)	(void) 0
#endif

/*
 * 重置与内联相关的状态。须在丢弃当前所用
 * LLVMContextRef（并新建）之前调用，否则会留下指向
 * 已删模块的悬空引用。
 */
void
llvm_inline_reset_caches(void)
{
	module_cache->clear();
	summary_cache->clear();
}

/*
 * 基于简单代价分析，对 M 中的外部函数引用做内联。
 *
 */
void
llvm_inline(LLVMModuleRef M)
{
	LLVMContextRef lc = LLVMGetModuleContext(M);
	llvm::Module *mod = llvm::unwrap(M);

	std::unique_ptr<ImportMapTy> globalsToInline = llvm_build_inline_plan(lc, mod);
	if (!globalsToInline)
		return;
	llvm_execute_inline_plan(mod, globalsToInline.get());
}

/*
 * 为在 mod 中内联外部函数引用而构建必要信息。
 *
 */
static std::unique_ptr<ImportMapTy>
llvm_build_inline_plan(LLVMContextRef lc, llvm::Module *mod)
{
	std::unique_ptr<ImportMapTy> globalsToInline(new ImportMapTy());
	FunctionInlineStates functionStates;
	InlineWorkList worklist;

	InlineSearchPath defaultSearchPath;

	/* 尝试把模块加入搜索路径 */
	add_module_to_inline_search_path(defaultSearchPath, "$libdir/postgres");
	/* 若 postgres 不可用，继续也无意义 */
	if (defaultSearchPath.empty())
		return nullptr;

	/*
	 * 从当前对外部函数的引用开始内联，把它们放入内联
	 * 工作队列。内联这些时若又需内联新的外部函数，
	 * 也会放入，但优先级更低。
	 *
	 */
	for (const llvm::Function &funcDecl : mod->functions())
	{
		InlineWorkListItem item = {};
		FunctionInlineState inlineState = {};

		/* 已有定义 */
		if (!funcDecl.isDeclaration())
			continue;

		/* 由 llvm 提供实现 */
		if (funcDecl.isIntrinsic())
			continue;

		item.symbolName = funcDecl.getName();
		item.searchpath = defaultSearchPath;
		worklist.push_back(item);
		inlineState.costLimit = inline_initial_cost;
		inlineState.processed = false;
		inlineState.inlined = false;
		inlineState.allowReconsidering = false;
		functionStates[funcDecl.getName()] = inlineState;
	}

	/*
	 * 遍历挂起工作队列项，在索引中查找，检查应否
	 * 内联。
	 */
	while (!worklist.empty())
	{
		InlineWorkListItem item = worklist.pop_back_val();
		llvm::StringRef symbolName = item.symbolName;
		char *cmodname;
		char *cfuncname;
		FunctionInlineState &inlineState = functionStates[symbolName];
		llvm::GlobalValue::GUID funcGUID;

		llvm_split_symbol_name(symbolName.data(), &cmodname, &cfuncname);

#if LLVM_VERSION_MAJOR >= 21
		funcGUID = llvm::GlobalValue::getGUIDAssumingExternalLinkage(cfuncname);
#else
		funcGUID = llvm::GlobalValue::getGUID(cfuncname);
#endif

		/* 已经处理过 */
		if (inlineState.processed)
			continue;


		if (cmodname)
			add_module_to_inline_search_path(item.searchpath, cmodname);

		/*
		 * 经索引遍历函数的所有已知定义，再查模块，核对
		 * 函数是否真有定义（可能有散列冲突）。
		 *
		 */
		for (const auto &gvs : summaries_for_guid(item.searchpath, funcGUID))
		{
			const llvm::FunctionSummary *fs;
			llvm::StringRef modPath = gvs->modulePath();
			llvm::Module *defMod;
			llvm::Function *funcDef;

			fs = llvm::cast<llvm::FunctionSummary>(gvs);

#if LLVM_VERSION_MAJOR > 3
			if (gvs->notEligibleToImport())
			{
				ilog(DEBUG1, "ineligibile to import %s due to summary",
					 symbolName.data());
				continue;
			}
#endif

			if ((int) fs->instCount() > inlineState.costLimit)
			{
				ilog(DEBUG1, "ineligibile to import %s due to early threshold: %u vs %u",
					 symbolName.data(), fs->instCount(), inlineState.costLimit);
				inlineState.allowReconsidering = true;
				continue;
			}

			defMod = load_module_cached(lc, modPath);
			if (defMod->materializeMetadata())
				elog(FATAL, "failed to materialize metadata");

			funcDef = defMod->getFunction(cfuncname);

			/*
			 * 例如函数名散列冲突时会发生。
			 *
			 */
			if (!funcDef)
				continue;

			if (funcDef->materialize())
				elog(FATAL, "failed to materialize metadata");

			Assert(!funcDef->isDeclaration());
			Assert(funcDef->hasExternalLinkage());

			llvm::StringSet<> importVars;
			llvm::SmallPtrSet<const llvm::Function *, 8> visitedFunctions;
			int running_instcount = 0;

			/*
			 * 检查函数及其依赖对象是否可内联。
			 *
			 */
			if (function_inlinable(*funcDef,
								   inlineState.costLimit,
								   functionStates,
								   worklist,
								   item.searchpath,
								   visitedFunctions,
								   running_instcount,
								   importVars))
			{
				/*
				 * 检查函数及依赖是否太大。已计入将内联
				 * 函数的依赖不再重计。结果略依赖顺序，但
				 * 看不出另一种行为有何意义。
				 *
				 *
				 */
				if (running_instcount > inlineState.costLimit)
				{
					ilog(DEBUG1, "skipping inlining of %s due to late threshold %d vs %d",
						 symbolName.data(), running_instcount, inlineState.costLimit);
					inlineState.allowReconsidering = true;
					continue;
				}

				ilog(DEBUG1, "inline top function %s total_instcount: %d, partial: %d",
					 symbolName.data(), running_instcount, fs->instCount());

				/* 导入被引用函数本身 */
				importVars.insert(symbolName);

				{
					llvm::StringSet<> &modGlobalsToInline = (*globalsToInline)[modPath];
					for (auto& importVar : importVars)
						modGlobalsToInline.insert(importVar.first());
					Assert(modGlobalsToInline.size() > 0);
				}

				/* 标记函数已内联 */
				inlineState.inlined = true;

				/*
				 * 已找到要内联的定义，不再另找。
				 *
				 */
				break;
			}
			else
			{
				ilog(DEBUG1, "had to skip inlining %s",
					 symbolName.data());

				/* 可能还有另一个可内联的定义。 */
			}
		}

		/*
		 * 无论成功（上面 inlined=true）与否，都标明本符号
		 * 已处理完。
		 */
		inlineState.processed = true;
	}

	return globalsToInline;
}

/*
 * 把外部函数（及其依赖）真正内联进 mod。
 *
 */
static void
llvm_execute_inline_plan(llvm::Module *mod, ImportMapTy *globalsToInline)
{
	llvm::IRMover Mover(*mod);

	for (const auto& toInline : *globalsToInline)
	{
		const llvm::StringRef& modPath = toInline.first();
		const llvm::StringSet<>& modGlobalsToInline = toInline.second;
		llvm::SetVector<llvm::GlobalValue *> GlobalsToImport;

		Assert(module_cache->count(modPath));
		std::unique_ptr<llvm::Module> importMod(std::move((*module_cache)[modPath]));
		module_cache->erase(modPath);

		if (modGlobalsToInline.empty())
			continue;

		for (auto &glob: modGlobalsToInline)
		{
			llvm::StringRef SymbolName = glob.first();
			char *modname;
			char *funcname;

			llvm_split_symbol_name(SymbolName.data(), &modname, &funcname);

			llvm::GlobalValue *valueToImport = importMod->getNamedValue(funcname);

			if (!valueToImport)
				elog(FATAL, "didn't refind value %s to import", SymbolName.data());

			/*
			 * 对函数（全局变量仅已是 static 才内联），导入
			 * 变量标成其他函数克隆。a) 避免符号
			 * 冲突 b) 允许优化器做内联。
			 *
			*/
			if (llvm::isa<llvm::Function>(valueToImport))
			{
				llvm::Function *F = llvm::dyn_cast<llvm::Function>(valueToImport);
				typedef llvm::GlobalValue::LinkageTypes LinkageTypes;

				/*
				 * 每函数信息未必已剥掉，剥时模块是惰性
				 * 加载的。
				 */
				llvm::stripDebugInfo(*F);

				/*
				 * 若待导入函数带模块名引用，建只转发调用
				 * 的小内联函数。或以为 GlobalAlias 即可，
				 * 但 a) IRMover 不会用别名覆盖声明（改名），
				 * b) 别名不能是 AvailableExternally。
				 *
				 *
				 */
				if (modname)
				{
					llvm::Function *AF;

					AF = create_redirection_function(importMod, F, SymbolName);

					GlobalsToImport.insert(AF);
					llvm::stripDebugInfo(*AF);
				}

				if (valueToImport->hasExternalLinkage())
				{
					valueToImport->setLinkage(LinkageTypes::AvailableExternallyLinkage);
				}
			}

			GlobalsToImport.insert(valueToImport);
			ilog(DEBUG1, "performing import of %s %s",
				 modPath.data(), SymbolName.data());

		}

#if LLVM_VERSION_MAJOR > 4
#define IRMOVE_PARAMS , /*IsPerformingImport=*/false
#elif LLVM_VERSION_MAJOR > 3
#define IRMOVE_PARAMS , /*LinkModuleInlineAsm=*/false, /*IsPerformingImport=*/false
#else
#define IRMOVE_PARAMS
#endif
		if (Mover.move(std::move(importMod), GlobalsToImport.getArrayRef(),
					   [](llvm::GlobalValue &, llvm::IRMover::ValueAdder) {}
					   IRMOVE_PARAMS))
			elog(FATAL, "function import failed with linker error");
	}
}

/*
 * 返回以 modPath 标识的模块，并缓存在内存中。
 *
 * 注意：未经拷贝不得修改该模块，否则缓存状态会坏。
 *
 */
static llvm::Module*
load_module_cached(LLVMContextRef lc, llvm::StringRef modPath)
{
	auto it = module_cache->find(modPath);
	if (it == module_cache->end())
	{
		it = module_cache->insert(
			std::make_pair(modPath, load_module(lc, modPath))).first;
	}

	return it->second.get();
}

static std::unique_ptr<llvm::Module>
load_module(LLVMContextRef lc, llvm::StringRef Identifier)
{
	LLVMMemoryBufferRef buf;
	LLVMModuleRef mod;
	char path[MAXPGPATH];
	char *msg;

	snprintf(path, MAXPGPATH,"%s/bitcode/%s", pkglib_path, Identifier.data());

	if (LLVMCreateMemoryBufferWithContentsOfFile(path, &buf, &msg))
		elog(FATAL, "failed to open bitcode file \"%s\": %s",
			 path, msg);
	if (LLVMGetBitcodeModuleInContext2(lc, buf, &mod))
		elog(FATAL, "failed to parse bitcode in file \"%s\"", path);

	/*
	 * 目前 JIT 代码用不到更细调试信息。在那之前，
	 * 花内存和周期处理 debuginfo 没多大意义。
	 *
	 */
	llvm::StripDebugInfo(*llvm::unwrap(mod));

	return std::unique_ptr<llvm::Module>(llvm::unwrap(mod));
}

/*
 * 计算函数所引用的变量、函数列表及指令数。
 *
 */
static void
function_references(llvm::Function &F,
					int &running_instcount,
					llvm::SmallPtrSet<llvm::GlobalVariable *, 8> &referencedVars,
					llvm::SmallPtrSet<llvm::Function *, 8> &referencedFunctions)
{
	llvm::SmallPtrSet<const llvm::User *, 32> Visited;

	for (llvm::BasicBlock &BB : F)
	{
		for (llvm::Instruction &I : BB)
		{
			if (llvm::isa<llvm::DbgInfoIntrinsic>(I))
				continue;

			llvm::SmallVector<llvm::User *, 8> Worklist;
			Worklist.push_back(&I);

			running_instcount++;

			while (!Worklist.empty()) {
				llvm::User *U = Worklist.pop_back_val();

				/* 已访问过 */
				if (!Visited.insert(U).second)
					continue;

				for (auto &OI : U->operands()) {
					llvm::User *Operand = llvm::dyn_cast<llvm::User>(OI);
					if (!Operand)
						continue;
					if (llvm::isa<llvm::BlockAddress>(Operand))
						continue;
					if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(Operand)) {
						referencedVars.insert(GV);
						if (GV->hasInitializer())
							Worklist.push_back(GV->getInitializer());
						continue;
					}
					if (auto *CF = llvm::dyn_cast<llvm::Function>(Operand)) {
						referencedFunctions.insert(CF);
						continue;
					}
					Worklist.push_back(Operand);
				}
			}
		}
	}
}

/*
 * 检查函数 F 是否可内联，若可，须导入哪些全局。
 *
 *
 * 被（可能递归）内联的函数对外部函数的引用，加入
 * 传入的工作队列。
 */
static bool
function_inlinable(llvm::Function &F,
				   int threshold,
				   FunctionInlineStates &functionStates,
				   InlineWorkList &worklist,
				   InlineSearchPath &searchpath,
				   llvm::SmallPtrSet<const llvm::Function *, 8> &visitedFunctions,
				   int &running_instcount,
				   llvm::StringSet<> &importVars)
{
	int subThreshold = threshold * inline_cost_decay_factor;
	llvm::SmallPtrSet<llvm::GlobalVariable *, 8> referencedVars;
	llvm::SmallPtrSet<llvm::Function *, 8> referencedFunctions;

	/* 不能依赖可能被内联者 */
	if (F.isInterposable())
		return false;

	/*
	 * 不能依赖函数一定存在。或者我们可以为这些函数
	 * 创建 static 版本？
	 */
	if (F.hasAvailableExternallyLinkage())
		return false;

	ilog(DEBUG1, "checking inlinability of %s", F.getName().data());

	if (F.materialize())
		elog(FATAL, "failed to materialize metadata");

#if LLVM_VERSION_MAJOR < 14
#define hasFnAttr hasFnAttribute
#endif

	if (F.getAttributes().hasFnAttr(llvm::Attribute::NoInline))
	{
		ilog(DEBUG1, "ineligibile to import %s due to noinline",
			 F.getName().data());
		return false;
	}

	function_references(F, running_instcount, referencedVars, referencedFunctions);

	for (llvm::GlobalVariable* rv: referencedVars)
	{
		if (rv->materialize())
			elog(FATAL, "failed to materialize metadata");

		/*
		 * 不内联访问线程局部变量的函数。当前 LLVM 版
		 * 做不到（将来或许可以）。
		 */
		if (rv->isThreadLocal())
		{
			ilog(DEBUG1, "cannot inline %s due to thread-local variable %s",
				 F.getName().data(), rv->getName().data());
			return false;
		}

		/*
		 * 从不内联对外可见变量，引用它们足够便宜。
		 *
		 */
		if (rv->hasExternalLinkage() || rv->hasAvailableExternallyLinkage())
			continue;

		/*
		 * 若变量是文件局部的，须内联它才能内联函数
		 * 若变量可被修改则不能，因为显然会失步。
		 *
		 *
		 * XXX: 目前不是问题，但若 postgres 允许非平凡
		 * 初始化器，会有问题。
		 */
		if (!rv->isConstant())
		{
			ilog(DEBUG1, "cannot inline %s due to uncloneable variable %s",
				 F.getName().data(), rv->getName().data());
			return false;
		}

		ilog(DEBUG1, "memorizing global var %s linkage %d for inlining",
			 rv->getName().data(), (int)rv->getLinkage());

		importVars.insert(rv->getName());
		/* 每个克隆的全局记一小笔代价 */
		running_instcount += 5;
	}

	visitedFunctions.insert(&F);

	/*
	 * 检查被引用函数。静态的查是否可内联，外部
	 * 记下待内联。
	 */
	for (llvm::Function* referencedFunction: referencedFunctions)
	{
		llvm::StringSet<> recImportVars;

		if (referencedFunction->materialize())
			elog(FATAL, "failed to materialize metadata");

		if (referencedFunction->isIntrinsic())
			continue;

		/* 若已访问则跳过，否则记住 */
		if (!visitedFunctions.insert(referencedFunction).second)
			continue;

		/*
		 * 此处不直接内联外部函数，适当时放入工作
		 * 队列，由 llvm_build_inline_plan() 检查。
		 *
		 */
		if (referencedFunction->hasExternalLinkage())
		{
			llvm::StringRef funcName = referencedFunction->getName();

			/*
			 * 剩余代价预算很小时，不必再检查内联。
			 *
			 */
			if (subThreshold < 5)
				continue;

			auto it = functionStates.find(funcName);
			if (it == functionStates.end())
			{
				FunctionInlineState inlineState;

				inlineState.costLimit = subThreshold;
				inlineState.processed = false;
				inlineState.inlined = false;
				inlineState.allowReconsidering = false;

				functionStates[funcName] = inlineState;
				worklist.push_back({funcName, searchpath});

				ilog(DEBUG1,
					 "considering extern function %s at %d for inlining",
					 funcName.data(), subThreshold);
			}
			else if (!it->second.inlined &&
					 (!it->second.processed || it->second.allowReconsidering) &&
					 it->second.costLimit < subThreshold)
			{
				/*
				 * 阈值更高则更新。更低阈值已处理过则
				 * 须重新入队。
				 *
				 */
				if (it->second.processed)
				{
					ilog(DEBUG1,
						 "reconsidering extern function %s at %d for inlining, increasing from %d",
						 funcName.data(), subThreshold, it->second.costLimit);

					it->second.processed = false;
					it->second.allowReconsidering = false;
					worklist.push_back({funcName, searchpath});
				}
				it->second.costLimit = subThreshold;
			}
			continue;
		}

		/* 不能依赖可能被内联者 */
		if (referencedFunction->isInterposable())
			return false;

		if (!function_inlinable(*referencedFunction,
								subThreshold,
								functionStates,
								worklist,
								searchpath,
								visitedFunctions,
								running_instcount,
								recImportVars))
		{
			ilog(DEBUG1,
				 "cannot inline %s due to required function %s not being inlinable",
				 F.getName().data(), referencedFunction->getName().data());
			return false;
		}

		/* 导入被引用函数本身 */
		importVars.insert(referencedFunction->getName());

		/* 导入被引用函数及其依赖 */
		for (auto& recImportVar : recImportVars)
			importVars.insert(recImportVar.first());
	}

	return true;
}

/*
 * 尝试加载 path 处的模块摘要。失败则返回空指针。
 *
 */
static std::unique_ptr<llvm::ModuleSummaryIndex>
llvm_load_summary(llvm::StringRef path)
{
	llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer> > MBOrErr =
		llvm::MemoryBuffer::getFile(path);

	if (std::error_code EC = MBOrErr.getError())
	{
		ilog(DEBUG1, "failed to open %s: %s", path.data(),
			 EC.message().c_str());
	}
	else
	{
		llvm::MemoryBufferRef ref(*MBOrErr.get().get());

#if LLVM_VERSION_MAJOR > 3
		llvm::Expected<std::unique_ptr<llvm::ModuleSummaryIndex> > IndexOrErr =
			llvm::getModuleSummaryIndex(ref);
		if (IndexOrErr)
			return std::move(IndexOrErr.get());
		elog(FATAL, "failed to load summary \"%s\": %s",
			 path.data(),
			 toString(IndexOrErr.takeError()).c_str());
#else
		llvm::ErrorOr<std::unique_ptr<llvm::ModuleSummaryIndex> > IndexOrErr =
			llvm::getModuleSummaryIndex(ref, [](const llvm::DiagnosticInfo &) {});
		if (IndexOrErr)
			return std::move(IndexOrErr.get());
		elog(FATAL, "failed to load summary \"%s\": %s",
			 path.data(),
			 IndexOrErr.getError().message().c_str());
#endif
	}
	return nullptr;
}

/*
 * 尝试把 modpath 加入搜索路径。
 */
static void
add_module_to_inline_search_path(InlineSearchPath& searchpath, llvm::StringRef modpath)
{
	/* 目前仅 libdir 中的扩展可作内联候选 */
#if LLVM_VERSION_MAJOR < 16
#define starts_with startswith
#endif
	if (!modpath.starts_with("$libdir/"))
		return;

	/* 若无匹配，尝试加载 */
	auto it = summary_cache->find(modpath);
	if (it == summary_cache->end())
	{
		std::string path(modpath);
		path = path.replace(0, strlen("$libdir"), std::string(pkglib_path) + "/bitcode");
		path += ".index.bc";
		(*summary_cache)[modpath] = llvm_load_summary(path);
		it = summary_cache->find(modpath);
	}

	Assert(it != summary_cache->end());

	/* 若表项非 NULL，则已有效加载 */
	if (it->second)
		searchpath.push_back(it->second.get());
}

/*
 * 在搜索路径中查找散列到 guid 的全部函数引用，
 * 并按搜索路径顺序返回。
 */
static llvm::SmallVector<llvm::GlobalValueSummary *, 1>
summaries_for_guid(const InlineSearchPath& path, llvm::GlobalValue::GUID guid)
{
	llvm::SmallVector<llvm::GlobalValueSummary *, 1> matches;

	for (auto index : path)
	{
#if LLVM_VERSION_MAJOR > 4
		llvm::ValueInfo funcVI = index->getValueInfo(guid);

		/* 若索引不知该函数，则无函数体，继续 */
		if (funcVI)
			for (auto &gv : funcVI.getSummaryList())
				matches.push_back(gv.get());
#else
		const llvm::const_gvsummary_iterator &I =
			index->findGlobalValueSummaryList(guid);
		if (I != index->end())
		{
			for (auto &gv : I->second)
				matches.push_back(gv.get());
		}
#endif
	}

	return matches;
}

/*
 * 创建名为 Name 的内联包装，把调用转到 F。
 */
static llvm::Function*
create_redirection_function(std::unique_ptr<llvm::Module> &importMod,
							llvm::Function *F,
							llvm::StringRef Name)
{
	typedef llvm::GlobalValue::LinkageTypes LinkageTypes;

	llvm::LLVMContext &Context = F->getContext();
	llvm::IRBuilder<> Builder(Context);
	llvm::Function *AF;
	llvm::BasicBlock *BB;
	llvm::CallInst *fwdcall;
#if LLVM_VERSION_MAJOR < 14
	llvm::Attribute inlineAttribute;
#endif

	AF = llvm::Function::Create(F->getFunctionType(),
								LinkageTypes::AvailableExternallyLinkage,
								Name, importMod.get());
	BB = llvm::BasicBlock::Create(Context, "entry", AF);

	Builder.SetInsertPoint(BB);
	fwdcall = Builder.CreateCall(F, &*AF->arg_begin());
#if LLVM_VERSION_MAJOR < 14
	inlineAttribute = llvm::Attribute::get(Context,
										   llvm::Attribute::AlwaysInline);
	fwdcall->addAttribute(~0U, inlineAttribute);
#else
	fwdcall->addFnAttr(llvm::Attribute::AlwaysInline);
#endif
	Builder.CreateRet(fwdcall);

	return AF;
}
