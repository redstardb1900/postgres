/*-------------------------------------------------------------------------
 *
 * llvmjit_deform.c
 *	  为堆元组变形生成代码。
 *
 * 相对未 JIT 的变形，本实现能利用编译期
 * 对元组描述符的知识：固定列宽、NOT NULL 等
 * 均可利用。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit_deform.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <llvm-c/Core.h>

#include "access/htup_details.h"
#include "access/tupdesc_details.h"
#include "executor/tuptable.h"
#include "jit/llvmjit.h"
#include "jit/llvmjit_emit.h"


/*
 * 创建函数，把类型为 desc 的元组变形到 natts 列。
 */
LLVMValueRef
slot_compile_deform(LLVMJitContext *context, TupleDesc desc,
					const TupleTableSlotOps *ops, int natts)
{
	char	   *funcname;

	LLVMModuleRef mod;
	LLVMContextRef lc;
	LLVMBuilderRef b;

	LLVMTypeRef deform_sig;
	LLVMValueRef v_deform_fn;

	LLVMBasicBlockRef b_entry;
	LLVMBasicBlockRef b_adjust_unavail_cols;
	LLVMBasicBlockRef b_find_start;

	LLVMBasicBlockRef b_out;
	LLVMBasicBlockRef b_dead;
	LLVMBasicBlockRef *attcheckattnoblocks;
	LLVMBasicBlockRef *attstartblocks;
	LLVMBasicBlockRef *attisnullblocks;
	LLVMBasicBlockRef *attcheckalignblocks;
	LLVMBasicBlockRef *attalignblocks;
	LLVMBasicBlockRef *attstoreblocks;

	LLVMValueRef v_offp;

	LLVMValueRef v_tupdata_base;
	LLVMValueRef v_tts_values;
	LLVMValueRef v_tts_nulls;
	LLVMValueRef v_slotoffp;
	LLVMValueRef v_flagsp;
	LLVMValueRef v_nvalidp;
	LLVMValueRef v_nvalid;
	LLVMValueRef v_maxatt;

	LLVMValueRef v_slot;

	LLVMValueRef v_tupleheaderp;
	LLVMValueRef v_tuplep;
	LLVMValueRef v_infomask1;
	LLVMValueRef v_infomask2;
	LLVMValueRef v_bits;

	LLVMValueRef v_hoff;

	LLVMValueRef v_hasnulls;

	/* 保证存在的最后一列（0 起算） */
	int			guaranteed_column_number = -1;

	/* 当前已知对齐 */
	int			known_alignment = 0;

	/* 若为真，known_alignment 即该列的确定偏移 */
	bool		attguaranteedalign = true;

	int			attnum;

	/* 虚元组无需变形，故不生成代码 */
	if (ops == &TTSOpsVirtual)
		return NULL;

	/* 未知如何处理的槽类型不做 JIT */
	if (ops != &TTSOpsHeapTuple && ops != &TTSOpsBufferHeapTuple &&
		ops != &TTSOpsMinimalTuple)
		return NULL;

	mod = llvm_mutable_module(context);
	lc = LLVMGetModuleContext(mod);

	funcname = llvm_expand_funcname(context, "deform");

	/*
	 * 检查哪些列必须存在，以免不必要地检查行的
	 * natts。
	 */
	for (attnum = 0; attnum < desc->natts; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, attnum);

		/*
		 * 若列声明为 NOT NULL，则每条元组都必须有它，
		 * 除非有 missing 项可为其提供非空值。
		 * 这进而保证 NULL 位图（若有可空列）至少
		 * 长到能覆盖至 attnum 的列。
		 *
		 *
		 * 再偏执地检查 !attisdropped，尽管
		 * attisdropped && attnotnull 组合本不应存在。
		 *
		 */
		if (att->attnotnull &&
			!att->atthasmissing &&
			!att->attisdropped)
			guaranteed_column_number = attnum;
	}

	/* 创建签名与函数 */
	{
		LLVMTypeRef param_types[1];

		param_types[0] = l_ptr(StructTupleTableSlot);

		deform_sig = LLVMFunctionType(LLVMVoidTypeInContext(lc),
									  param_types, lengthof(param_types), 0);
	}
	v_deform_fn = LLVMAddFunction(mod, funcname, deform_sig);
	LLVMSetLinkage(v_deform_fn, LLVMInternalLinkage);
	LLVMSetParamAlignment(LLVMGetParam(v_deform_fn, 0), MAXIMUM_ALIGNOF);
	llvm_copy_attributes(AttributeTemplate, v_deform_fn);

	b_entry =
		LLVMAppendBasicBlockInContext(lc, v_deform_fn, "entry");
	b_adjust_unavail_cols =
		LLVMAppendBasicBlockInContext(lc, v_deform_fn, "adjust_unavail_cols");
	b_find_start =
		LLVMAppendBasicBlockInContext(lc, v_deform_fn, "find_startblock");
	b_out =
		LLVMAppendBasicBlockInContext(lc, v_deform_fn, "outblock");
	b_dead =
		LLVMAppendBasicBlockInContext(lc, v_deform_fn, "deadblock");

	b = LLVMCreateBuilderInContext(lc);

	attcheckattnoblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attstartblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attisnullblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attcheckalignblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attalignblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attstoreblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);

	known_alignment = 0;

	LLVMPositionBuilderAtEnd(b, b_entry);

	/* 先做 alloca，llvm 只把这些转成寄存器 */
	v_offp = LLVMBuildAlloca(b, TypeSizeT, "v_offp");

	v_slot = LLVMGetParam(v_deform_fn, 0);

	v_tts_values =
		l_load_struct_gep(b, StructTupleTableSlot, v_slot, FIELDNO_TUPLETABLESLOT_VALUES,
						  "tts_values");
	v_tts_nulls =
		l_load_struct_gep(b, StructTupleTableSlot, v_slot, FIELDNO_TUPLETABLESLOT_ISNULL,
						  "tts_ISNULL");
	v_flagsp = l_struct_gep(b, StructTupleTableSlot, v_slot, FIELDNO_TUPLETABLESLOT_FLAGS, "");
	v_nvalidp = l_struct_gep(b, StructTupleTableSlot, v_slot, FIELDNO_TUPLETABLESLOT_NVALID, "");

	if (ops == &TTSOpsHeapTuple || ops == &TTSOpsBufferHeapTuple)
	{
		LLVMValueRef v_heapslot;

		v_heapslot =
			LLVMBuildBitCast(b,
							 v_slot,
							 l_ptr(StructHeapTupleTableSlot),
							 "heapslot");
		v_slotoffp = l_struct_gep(b, StructHeapTupleTableSlot, v_heapslot, FIELDNO_HEAPTUPLETABLESLOT_OFF, "");
		v_tupleheaderp =
			l_load_struct_gep(b, StructHeapTupleTableSlot, v_heapslot, FIELDNO_HEAPTUPLETABLESLOT_TUPLE,
							  "tupleheader");
	}
	else if (ops == &TTSOpsMinimalTuple)
	{
		LLVMValueRef v_minimalslot;

		v_minimalslot =
			LLVMBuildBitCast(b,
							 v_slot,
							 l_ptr(StructMinimalTupleTableSlot),
							 "minimalslot");
		v_slotoffp = l_struct_gep(b,
								  StructMinimalTupleTableSlot,
								  v_minimalslot,
								  FIELDNO_MINIMALTUPLETABLESLOT_OFF, "");
		v_tupleheaderp =
			l_load_struct_gep(b,
							  StructMinimalTupleTableSlot,
							  v_minimalslot,
							  FIELDNO_MINIMALTUPLETABLESLOT_TUPLE,
							  "tupleheader");
	}
	else
	{
		/* 本应在函数开头就返回了 */
		pg_unreachable();
	}

	v_tuplep =
		l_load_struct_gep(b,
						  StructHeapTupleData,
						  v_tupleheaderp,
						  FIELDNO_HEAPTUPLEDATA_DATA,
						  "tuple");
	v_bits =
		LLVMBuildBitCast(b,
						 l_struct_gep(b,
									  StructHeapTupleHeaderData,
									  v_tuplep,
									  FIELDNO_HEAPTUPLEHEADERDATA_BITS,
									  ""),
						 l_ptr(LLVMInt8TypeInContext(lc)),
						 "t_bits");
	v_infomask1 =
		l_load_struct_gep(b,
						  StructHeapTupleHeaderData,
						  v_tuplep,
						  FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK,
						  "infomask1");
	v_infomask2 =
		l_load_struct_gep(b,
						  StructHeapTupleHeaderData,
						  v_tuplep, FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK2,
						  "infomask2");

	/* t_infomask & HEAP_HASNULL */
	v_hasnulls =
		LLVMBuildICmp(b, LLVMIntNE,
					  LLVMBuildAnd(b,
								   l_int16_const(lc, HEAP_HASNULL),
								   v_infomask1, ""),
					  l_int16_const(lc, 0),
					  "hasnulls");

	/* t_infomask2 & HEAP_NATTS_MASK */
	v_maxatt = LLVMBuildAnd(b,
							l_int16_const(lc, HEAP_NATTS_MASK),
							v_infomask2,
							"maxatt");

	/*
	 * 须 zext，否则 getelementptr 会把 hoff 当有符号 8 位
	 * 整数，t_hoff > 127 时会得到负偏移。
	 */
	v_hoff =
		LLVMBuildZExt(b,
					  l_load_struct_gep(b,
										StructHeapTupleHeaderData,
										v_tuplep,
										FIELDNO_HEAPTUPLEHEADERDATA_HOFF,
										""),
					  LLVMInt32TypeInContext(lc), "t_hoff");

	v_tupdata_base = l_gep(b,
						   LLVMInt8TypeInContext(lc),
						   LLVMBuildBitCast(b,
											v_tuplep,
											l_ptr(LLVMInt8TypeInContext(lc)),
											""),
						   &v_hoff, 1,
						   "v_tupdata_base");

	/*
	 * 从槽加载元组起始偏移。若槽中尚无已变形列，
	 * 下面会重置。
	 */
	{
		LLVMValueRef v_off_start;

		v_off_start = l_load(b, LLVMInt32TypeInContext(lc), v_slotoffp, "v_slot_off");
		v_off_start = LLVMBuildZExt(b, v_off_start, TypeSizeT, "");
		LLVMBuildStore(b, v_off_start, v_offp);
	}

	/* 为每属性建基本块，后面要当跳转目标 */
	for (attnum = 0; attnum < natts; attnum++)
	{
		attcheckattnoblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.attcheckattno", attnum);
		attstartblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.start", attnum);
		attisnullblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.attisnull", attnum);
		attcheckalignblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.attcheckalign", attnum);
		attalignblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.align", attnum);
		attstoreblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.store", attnum);
	}

	/*
	 * 检查是否保证所需属性都在元组中（仍可能
	 * 为 NULL）：或因待变形的最后一列是 NOT NULL，
	 * 或因这里未访问的后续列是 NOT NULL。若不保证，
	 * 就要检查元组头的 natts，缺失属性可能还须
	 * 用 slot_getmissingattrs() 去取。
	 *
	 */
	if ((natts - 1) <= guaranteed_column_number)
	{
		/* 只跳过不需要的块 */
		LLVMBuildBr(b, b_adjust_unavail_cols);
		LLVMPositionBuilderAtEnd(b, b_adjust_unavail_cols);
		LLVMBuildBr(b, b_find_start);
	}
	else
	{
		LLVMValueRef v_params[3];
		LLVMValueRef f;

		/* 并非所有列都可用则分支 */
		LLVMBuildCondBr(b,
						LLVMBuildICmp(b, LLVMIntULT,
									  v_maxatt,
									  l_int16_const(lc, natts),
									  ""),
						b_adjust_unavail_cols,
						b_find_start);

		/* 若否，把相关列的 tts_isnull 置 true */
		LLVMPositionBuilderAtEnd(b, b_adjust_unavail_cols);

		v_params[0] = v_slot;
		v_params[1] = LLVMBuildZExt(b, v_maxatt, LLVMInt32TypeInContext(lc), "");
		v_params[2] = l_int32_const(lc, natts);
		f = llvm_pg_func(mod, "slot_getmissingattrs");
		l_call(b,
			   LLVMGetFunctionType(f), f,
			   v_params, lengthof(v_params), "");
		LLVMBuildBr(b, b_find_start);
	}

	LLVMPositionBuilderAtEnd(b, b_find_start);

	v_nvalid = l_load(b, LLVMInt16TypeInContext(lc), v_nvalidp, "");

	/*
	 * 建 switch，从 nvalid 跳到正确 startblock。调用方
	 * 目前还没有这知识，但若已知槽为空（如扫描
	 * 节点），跳过此检查对性能更好。
	 *
	 */
	if (true)
	{
		LLVMValueRef v_switch = LLVMBuildSwitch(b, v_nvalid,
												b_dead, natts);

		for (attnum = 0; attnum < natts; attnum++)
		{
			LLVMValueRef v_attno = l_int16_const(lc, attnum);

			LLVMAddCase(v_switch, v_attno, attcheckattnoblocks[attnum]);
		}
	}
	else
	{
		/* 从入口块跳到第一块 */
		LLVMBuildBr(b, attcheckattnoblocks[0]);
	}

	LLVMPositionBuilderAtEnd(b, b_dead);
	LLVMBuildUnreachable(b);

	/*
	 * 遍历每个待变形属性，生成变形它的代码。
	 *
	 */
	for (attnum = 0; attnum < natts; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, attnum);
		LLVMValueRef v_incby;
		int			alignto;
		LLVMValueRef l_attno = l_int16_const(lc, attnum);
		LLVMValueRef v_attdatap;
		LLVMValueRef v_resultp;

		/* 建块：检查是否已做完所有必要属性 */
		LLVMPositionBuilderAtEnd(b, attcheckattnoblocks[attnum]);

		/*
		 * 若是第一属性，则 slot->tts_nvalid 曾为 0。因此
		 * 也把偏移重置为 0，它可能来自上次执行。
		 */
		if (attnum == 0)
		{
			LLVMBuildStore(b, l_sizet_const(0), v_offp);
		}

		/*
		 * 检查列是否可用（即元组是否存了那么多列）。
		 * 若已知后面有 NOT NULL 列，可免此分支。
		 *
		 */
		if (attnum <= guaranteed_column_number)
		{
			LLVMBuildBr(b, attstartblocks[attnum]);
		}
		else
		{
			LLVMValueRef v_islast;

			v_islast = LLVMBuildICmp(b, LLVMIntUGE,
									 l_attno,
									 v_maxatt,
									 "heap_natts");
			LLVMBuildCondBr(b, v_islast, b_out, attstartblocks[attnum]);
		}
		LLVMPositionBuilderAtEnd(b, attstartblocks[attnum]);

		/*
		 * 必要时检查空值。不必考虑 missing 属性：
		 * 若有，堆元组的 natts 会表明需要
		 * slot_getmissingattrs()。
		 */
		if (!att->attnotnull)
		{
			LLVMBasicBlockRef b_ifnotnull;
			LLVMBasicBlockRef b_ifnull;
			LLVMBasicBlockRef b_next;
			LLVMValueRef v_attisnull;
			LLVMValueRef v_nullbyteno;
			LLVMValueRef v_nullbytemask;
			LLVMValueRef v_nullbyte;
			LLVMValueRef v_nullbit;

			b_ifnotnull = attcheckalignblocks[attnum];
			b_ifnull = attisnullblocks[attnum];

			if (attnum + 1 == natts)
				b_next = b_out;
			else
				b_next = attcheckattnoblocks[attnum + 1];

			v_nullbyteno = l_int32_const(lc, attnum >> 3);
			v_nullbytemask = l_int8_const(lc, 1 << ((attnum) & 0x07));
			v_nullbyte = l_load_gep1(b, LLVMInt8TypeInContext(lc), v_bits, v_nullbyteno, "attnullbyte");

			v_nullbit = LLVMBuildICmp(b,
									  LLVMIntEQ,
									  LLVMBuildAnd(b, v_nullbyte, v_nullbytemask, ""),
									  l_int8_const(lc, 0),
									  "attisnull");

			v_attisnull = LLVMBuildAnd(b, v_hasnulls, v_nullbit, "");

			LLVMBuildCondBr(b, v_attisnull, b_ifnull, b_ifnotnull);

			LLVMPositionBuilderAtEnd(b, b_ifnull);

			/* 存空值字节 */
			LLVMBuildStore(b,
						   l_int8_const(lc, 1),
						   l_gep(b, LLVMInt8TypeInContext(lc), v_tts_nulls, &l_attno, 1, ""));
			/* 存零 Datum */
			LLVMBuildStore(b,
						   l_sizet_const(0),
						   l_gep(b, TypeSizeT, v_tts_values, &l_attno, 1, ""));

			LLVMBuildBr(b, b_next);
			attguaranteedalign = false;
		}
		else
		{
			/* 无事可做 */
			LLVMBuildBr(b, attcheckalignblocks[attnum]);
			LLVMPositionBuilderAtEnd(b, attisnullblocks[attnum]);
			LLVMBuildBr(b, attcheckalignblocks[attnum]);
		}
		LLVMPositionBuilderAtEnd(b, attcheckalignblocks[attnum]);

		/* 判定所需对齐 */
		if (att->attalign == TYPALIGN_INT)
			alignto = ALIGNOF_INT;
		else if (att->attalign == TYPALIGN_CHAR)
			alignto = 1;
		else if (att->attalign == TYPALIGN_DOUBLE)
			alignto = ALIGNOF_DOUBLE;
		else if (att->attalign == TYPALIGN_SHORT)
			alignto = ALIGNOF_SHORT;
		else
		{
			elog(ERROR, "unknown alignment");
			alignto = 0;
		}

		/* ------
		 * 即使需要对齐，在可证明不必时也可跳过：
		 *
		 * - 第一列保证已对齐
		 * - NOT NULL 定宽 datum 之后的列已知对齐，
		 *   若该已知对齐与当前列兼容，可跳过对齐计算。
		 *
		 * ------
		 */
		if (alignto > 1 &&
			(known_alignment < 0 || known_alignment != TYPEALIGN(alignto, known_alignment)))
		{
			/*
			 * 访问 varlena 字段时须"窥视"：是填充字节，
			 * 还是 1 字节头 datum 的首字节。零字节是填充，
			 * 或已对齐的 4 字节长度字首字节；两种
			 * 情况都可安全对齐。非零字节是 1 字节长度字，
			 * 或已正确对齐的 4 字节长度字的首字节；两种
			 * 情况都不必对齐。
			 *
			 */
			if (att->attlen == -1)
			{
				LLVMValueRef v_possible_padbyte;
				LLVMValueRef v_ispad;
				LLVMValueRef v_off;

				/* 尚不知是否短 varlena */
				attguaranteedalign = false;

				v_off = l_load(b, TypeSizeT, v_offp, "");

				v_possible_padbyte =
					l_load_gep1(b, LLVMInt8TypeInContext(lc), v_tupdata_base,
								v_off, "padbyte");
				v_ispad =
					LLVMBuildICmp(b, LLVMIntEQ,
								  v_possible_padbyte, l_int8_const(lc, 0),
								  "ispadbyte");
				LLVMBuildCondBr(b, v_ispad,
								attalignblocks[attnum],
								attstoreblocks[attnum]);
			}
			else
			{
				LLVMBuildBr(b, attalignblocks[attnum]);
			}

			LLVMPositionBuilderAtEnd(b, attalignblocks[attnum]);

			/* 对齐代码的翻译（参见 TYPEALIGN()） */
			{
				LLVMValueRef v_off_aligned;
				LLVMValueRef v_off = l_load(b, TypeSizeT, v_offp, "");

				/* ((ALIGNVAL) - 1) */
				LLVMValueRef v_alignval = l_sizet_const(alignto - 1);

				/* ((uintptr_t) (LEN) + ((ALIGNVAL) - 1)) */
				LLVMValueRef v_lh = LLVMBuildAdd(b, v_off, v_alignval, "");

				/* ~((uintptr_t) ((ALIGNVAL) - 1)) */
				LLVMValueRef v_rh = l_sizet_const(~(alignto - 1));

				v_off_aligned = LLVMBuildAnd(b, v_lh, v_rh, "aligned_offset");

				LLVMBuildStore(b, v_off_aligned, v_offp);
			}

			/*
			 * 对齐要么本不必要，要么已做完，故现在知道
			 * 当前对齐。仅因 varlena 与可空列不用此值，
			 * 这样做才安全。
			 */
			if (known_alignment >= 0)
			{
				Assert(known_alignment != 0);
				known_alignment = TYPEALIGN(alignto, known_alignment);
			}

			LLVMBuildBr(b, attstoreblocks[attnum]);
			LLVMPositionBuilderAtEnd(b, attstoreblocks[attnum]);
		}
		else
		{
			LLVMPositionBuilderAtEnd(b, attcheckalignblocks[attnum]);
			LLVMBuildBr(b, attalignblocks[attnum]);
			LLVMPositionBuilderAtEnd(b, attalignblocks[attnum]);
			LLVMBuildBr(b, attstoreblocks[attnum]);
		}
		LLVMPositionBuilderAtEnd(b, attstoreblocks[attnum]);

		/*
		 * 若偏移已知为常量则存下来。这让 LLVM 能生成
		 * 更好的代码。否则 LLVM 看不出偏移可能是常量，
		 * 因为先前已解码列的跳转会挡住分析。
		 *
		 */
		if (attguaranteedalign)
		{
			Assert(known_alignment >= 0);
			LLVMBuildStore(b, l_sizet_const(known_alignment), v_offp);
		}

		/* 计算后续列对齐到何处 */
		if (att->attlen < 0)
		{
			/* 变长字段之后无法再保证任何对齐 */
			known_alignment = -1;
			attguaranteedalign = false;
		}
		else if (att->attnotnull && attguaranteedalign && known_alignment >= 0)
		{
			/*
			 * 若该列偏移先前已知，则 NOT NULL 且定宽的列
			 * 保证对齐就是先前对齐加上列宽。
			 *
			 */
			Assert(att->attlen > 0);
			known_alignment += att->attlen;
		}
		else if (att->attnotnull && (att->attlen % alignto) == 0)
		{
			/*
			 * NOT NULL 定宽列若长度为其对齐的整数倍，
			 * 则下一列至少对齐到当前列的对齐。
			 *
			 */
			Assert(att->attlen > 0);
			known_alignment = alignto;
			Assert(known_alignment > 0);
			attguaranteedalign = false;
		}
		else
		{
			known_alignment = -1;
			attguaranteedalign = false;
		}


		/* 计算加载数据的地址 */
		{
			LLVMValueRef v_off = l_load(b, TypeSizeT, v_offp, "");

			v_attdatap =
				l_gep(b, LLVMInt8TypeInContext(lc), v_tupdata_base, &v_off, 1, "");
		}

		/* 计算存值的地址 */
		v_resultp = l_gep(b, TypeSizeT, v_tts_values, &l_attno, 1, "");

		/* 存空字节(false) */
		LLVMBuildStore(b, l_int8_const(lc, 0),
					   l_gep(b, TypeStorageBool, v_tts_nulls, &l_attno, 1, ""));

		/*
		 * 存 Datum。byval：复制值、扩展到 Datum 宽后存储。
		 * byref：存指向数据的指针。
		 */
		if (att->attbyval)
		{
			LLVMValueRef v_tmp_loaddata;
			LLVMTypeRef vartype = LLVMIntTypeInContext(lc, att->attlen * 8);
			LLVMTypeRef vartypep = LLVMPointerType(vartype, 0);

			v_tmp_loaddata =
				LLVMBuildPointerCast(b, v_attdatap, vartypep, "");
			v_tmp_loaddata = l_load(b, vartype, v_tmp_loaddata, "attr_byval");
			v_tmp_loaddata = LLVMBuildSExt(b, v_tmp_loaddata, TypeSizeT, "");

			LLVMBuildStore(b, v_tmp_loaddata, v_resultp);
		}
		else
		{
			LLVMValueRef v_tmp_loaddata;

			/* 存指针 */
			v_tmp_loaddata =
				LLVMBuildPtrToInt(b,
								  v_attdatap,
								  TypeSizeT,
								  "attr_ptr");
			LLVMBuildStore(b, v_tmp_loaddata, v_resultp);
		}

		/* 递增数据指针 */
		if (att->attlen > 0)
		{
			v_incby = l_sizet_const(att->attlen);
		}
		else if (att->attlen == -1)
		{
			v_incby = l_call(b,
							 llvm_pg_var_func_type("varsize_any"),
							 llvm_pg_func(mod, "varsize_any"),
							 &v_attdatap, 1,
							 "varsize_any");
			l_callsite_ro(v_incby);
			l_callsite_alwaysinline(v_incby);
		}
		else if (att->attlen == -2)
		{
			v_incby = l_call(b,
							 llvm_pg_var_func_type("strlen"),
							 llvm_pg_func(mod, "strlen"),
							 &v_attdatap, 1, "strlen");

			l_callsite_ro(v_incby);

			/* +1 为 NUL 字节 */
			v_incby = LLVMBuildAdd(b, v_incby, l_sizet_const(1), "");
		}
		else
		{
			Assert(false);
			v_incby = NULL;		/* 让编译器静 */
		}

		if (attguaranteedalign)
		{
			Assert(known_alignment >= 0);
			LLVMBuildStore(b, l_sizet_const(known_alignment), v_offp);
		}
		else
		{
			LLVMValueRef v_off = l_load(b, TypeSizeT, v_offp, "");

			v_off = LLVMBuildAdd(b, v_off, v_incby, "increment_offset");
			LLVMBuildStore(b, v_off, v_offp);
		}

		/*
		 * 跳到下一块，除非已是最后可能列，或想要的
		 * 可用属性都已取到。
		 */
		if (attnum + 1 == natts)
		{
			/* 跳出 */
			LLVMBuildBr(b, b_out);
		}
		else
		{
			LLVMBuildBr(b, attcheckattnoblocks[attnum + 1]);
		}
	}


	/* 建返回用的块 */
	LLVMPositionBuilderAtEnd(b, b_out);

	{
		LLVMValueRef v_off = l_load(b, TypeSizeT, v_offp, "");
		LLVMValueRef v_flags;

		LLVMBuildStore(b, l_int16_const(lc, natts), v_nvalidp);
		v_off = LLVMBuildTrunc(b, v_off, LLVMInt32TypeInContext(lc), "");
		LLVMBuildStore(b, v_off, v_slotoffp);
		v_flags = l_load(b, LLVMInt16TypeInContext(lc), v_flagsp, "tts_flags");
		v_flags = LLVMBuildOr(b, v_flags, l_int16_const(lc, TTS_FLAG_SLOW), "");
		LLVMBuildStore(b, v_flags, v_flagsp);
		LLVMBuildRetVoid(b);
	}

	LLVMDisposeBuilder(b);

	return v_deform_fn;
}
