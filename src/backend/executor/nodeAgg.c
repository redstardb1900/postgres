/*-------------------------------------------------------------------------
 *
 * nodeAgg.c
 *	  处理聚合节点的例程。     
 *
 *	  ExecAgg 通常按以下步骤计算每一个聚合：            
 *
 *		 transvalue = initcond
 *		 对每个输入元组:
 *			transvalue = transfunc(transvalue, input_value(s))
 *		 result = finalfunc(transvalue, direct_argument(s))
 *
 *	  若未提供 finalfunc，则结果就是 transvalue 的          
 *	  最终值。        
 *
 *	  其他行为可通过 "aggsplit" 模式选择，该模式用于     
 *	  支持部分聚合。可行的选择包括：      
 *	  * 跳过 finalfunc 的执行，使输出始终是最终的    
 *	  transvalue 状态。   
 *	  * 用 combinefunc 代替 transfunc，使 transvalue               
 *	  状态（由子部分聚合步骤传递上来）被合并而非处理  
 *	  原始输入行。（下文关于 transfunc 的说明同样        
 *	  适用于 combinefunc，当选中它时。）                       
 *	  * 对输出值应用 serializefunc（这只在跳过 finalfunc       
 *	  时才有意义，因为 serializefunc 只作用于              
 *	  transvalue类型。）
 *	  * 对输入值应用 deserializefunc（这只在使用                
 *	  combinefunc 时才有意义，理由类似）。 
 *	  规划器负责把这些采用替代行为的 Agg 节点以合理的  
 *	  方式连接起来，让部分聚合结果被馈送给需要它们的  
 *	  节点。                                   
 *
 *	  若普通的聚合调用指定了 DISTINCT 或 ORDER BY，我们      
 *	  先对输入元组排序并（在需要时）去除重复，之后再
 *	  执行上述过程。（但有序集聚合不做此处理；它们的  
 *	  "ORDER BY" 输入就本模块而言只是普通聚合参数，      
 *	  故本模块不作特殊处理。）注意：这些情形不支持
 *	  部分聚合，因为无法保证输入全局有序或          
 *	  无重复性。                        
 *
 *	  若 transfunc 在 pg_proc 中被标记为 "strict" 且 initcond 
 *	  为 NULL，则首个非 NULL 的 input_value 直接成为              
 *	  transvalue，直到第二个非 NULL 的 input_value 才会调用 
 *	  transfunc！此时聚合首个输入类型必须与 transtype 相同！
 *
 *	  若 transfunc 被标记为 "strict"，则 NULL 的 input_value     
 *	  会被跳过，并保留之前的 transvalue。若 transfunc 非    
 *	  strict，则每个输入元组都会调用它，须自行处理 
 *	  NULL initcond 或 input_value。
 *
 *	  若 finalfunc 被标记为 "strict"，则当最终的         
 *	  transvalue 为 NULL 时不调用它，而是自动产生   
 *	  一个 NULL 结果（这当然只是严格函数的常规          
 *	  处理方式）。非 strict 的 finalfunc 可自行决定    
 *	  对 NULL 最终 transvalue 返回什么。  
 *
 *	  有序集聚合在另一方面被特殊对待：我们会        
 *	  计算所有 "direct" 参数，并把它们连同过渡值一起    
 *	  传给 finalfunc。       
 *
 *	  finalfunc 除 transvalue 和任何 "direct" 参数之外，        
 *	  还可以有其他参数，它们对应于聚合的输入参数。 
 *	  这些参数总是以 NULL 传入。此类参数可能是解析         
 *	  多态聚合结果类型时所必需的。                          
 *
 *	  我们在一个临时 econtext（aggstate->tmpcontext）中            
 *	  计算聚合输入表达式并运行转移函数。该上下文至少  
 *	  每输入元组重置一次，因此当 transvalue 的  
 *	  数据类型为引用传递时，必须小心将其复制到更长寿  
 *	  的内存上下文中，并释放旧值以避免内存泄漏。我们
 *	  把 transvalue 存放在另一组 econtext，即 aggstate           
 *	  ->aggcontexts（每个分组集一个，见下文）中，这些        
 *	  econtext 在 AGG_HASHED 模式下也用于存放哈希表              
 *	  表结构。分组边界处它们被重新扫描而非仅重置，   
 *	  使转移函数经AggRegisterCallback注册回调。
 *
 *	  节点的常规 econtext（aggstate->ss.ps.ps_ExprContext）           
 *	  用于运行最终函数并计算输出元组；该上下文可         
 *	  每个输出元组重置。 
 *
 *	  执行器的 AggState 节点会作为 fmgr 的 "context" 值         
 *	  传入所有 transfunc 和 finalfunc 调用。不建议           
 *	  转移函数直接查看 AggState 节点，但它们可以           
 *	  用 AggCheckCallContext() 来验证自己是被                  
 *	  nodeAgg.c 调用（而非作为普通 SQL 函数）。转移     
 *	  函数想知道这一点，主要是为了能避免每次调用    
 *	  都 palloc 一个定长的引用传递过渡值：                 
 *	  它可以直接涂写并返回其左输入。通常情况下，       
 *	  函数完全禁止修改引用传递的输入参数，但在聚合    
 *	  场景中我们知道左输入要么是初始过渡值，要么是    
 *	  先前的函数结果，且无论哪种情况，它的值都无需    
 *	  被保留。示例参见 int8inc()。请注意，                       
 *	  EEOP_AGG_PLAIN_TRANS 步骤在编码时避免在返回               
 *	  前一个过渡值指针时进行数据复制。还可以在过渡     
 *	  值为展开对象时避免反复的数据复制：要做到这        
 *	  一点，转移函数必须注意返回的展开对象要位于     
 *	  AggCheckCallContext() 所返回内存上下文的子上下文      
 *	  中。此外，有些转移函数还希望在名义过渡值之外      
 *	  存放额外的工作状态；它们可以利用                        
 *	  AggCheckCallContext() 返回的内存上下文来做到。              
 *
 *	  注意：AggCheckCallContext() 自 PostgreSQL 9.0 起              
 *	  可用。更早的版本（追溯到 8.1）中 AggState 会作为  
 *	  context 传入，但 9.0 前需直接检查节点才能使用。  
 *
 *	  从 9.4 起，聚合转移函数也可以使用 AggGetAggref()       
 *	  来获取其聚合调用的 Aggref 表达式节点。              
 *	  这主要面向有序集聚合，其不支持作为窗口函数  
 *	  使用。（普通聚合函数要用它需要一些后备逻辑， 
 *	  因为窗口函数没有对应的 Aggref                          
 *	  节点。）           
 *
 *	  分组集：  
 *
 *	  在结构上等价于 ROLLUP 子句的分组集列表（例如      
 *	  (a,b,c)、(a,b)、(a)）就能单趟处理完有序数据。            
 *	  我们的做法是：为每个并发处理的分组集保留一组      
 *	  独立的过渡值；对每个输入元组，我们都会更新它们   
 *	  全部；分组边界处我们会重置那些（从列表        
 *	  前端开始），这些状态的分组值已经发生了变化       
 *	  （分组集列表按从最具体到最不具体的顺序排列  
 *	  ）。    
 *
 *	  在使用更复杂的分组集时，我们会把它们分解为   
 *	  若干个"阶段"，每个阶段有不同的排序顺序（除了    
 *	  专用于哈希的阶段 0）。在每个非最后的阶段中，   
 *	  输入元组还会额外存入一个 tuplesort，它以下一阶段      
 *	  的排序顺序为键；在每个非最初阶段，输入元组    
 *	  则取自先前已排序好的数据。（第一阶段的排序       
 *	  由规划器负责完成，因为它可能由底层的节点来满
 *	  足。）                      
 *
 *	  哈希可以与排序分组混合使用。为此，我们提供   
 *	  AGG_MIXED 策略：在第一个排序阶段填充哈希表，           
 *	  并在完成全部排序阶段后切换为从中读出。                
 *	  我们也可以在支持多个哈希表的同时，完全不做        
 *	  排序 
 *
 *	  从聚合转移函数与最终函数的角度来看，关于分      
 *	  组集只有一点问题：聚合函数的单一调用点（flinfo）  
 *	  可能被用来依次更新多个不同的过渡值。因此函    
 *	  数不得在 flinfo 中缓存任何逻辑上属于过渡值组成      
 *	  部分的东西（最重要的是过渡值所在的内存上下       
 *	  文）。支持 API 函数（AggCheckCallContext、                   
 *	  AggRegisterCallback）对于聚合函数当前被调用于              
 *	  哪个分组集是                                               
 *	  敏感的。           
 *
 *	  计划结构：
 *
 *	  我们从规划器得到的其实是一个"真实"的 Agg 节点，  
 *	  它本身就是计划树的组成部分，但可选地通过"chain"      
 *	  字段在侧面挂上额外的 Agg 节点列表。这是因为           
 *	  Agg 节点恰好是分组集所需全部数据的一种便利        
 *	  表示。              
 *
 *	  在许多场合，我们把"真实"节点当作就是链上的          
 *	  第一个节点。链的顺序必须保证哈希项排在排序/      
 *	  普通项之前；若两类节点都存在，真实节点就标记为 
 *	  AGG_MIXED（此时真实节点描述的是其中一个哈希           
 *	  分组，链上其后可以跟其他 AGG_HASHED 节点，再往         
 *	  后依次是 AGG_SORTED 或（单个）AGG_PLAIN 节点）。若       
 *	  真实节点标记为 AGG_HASHED 或 AGG_SORTED，则链上           
 *	  所有链接节点必须同类型；若为 AGG_PLAIN 则不        
 *	  能有。     
 *
 *	  我们把所有哈希节点收进一个编号为 0 的"阶段"，        
 *	  并为每个 AGG_SORTED 或 AGG_PLAIN 节点创建一个排          
 *	  序阶段（编号 1..n）。即使没有哈希也会分配阶段 0， 
 *	  闲置。 
 *
 *	  AGG_HASHED 节点实际上各自只针对单个分组集，        
 *	  因为每个哈希分组都需要单独的 grpColIdx 和         
 *	  numGroups 估计值。AGG_SORTED 节点代表一个"rollup"，       
 *	  即共享一个排序顺序的分组集列表。除第一个之外的  
 *	  每个 AGG_SORTED 节点都关联一个 Sort 节点，用于描述      
 *	  要采用的排序顺序；第一个排序节点的输入取自外层    
 *	  子树，规划器已安排好让它提供有序数据。      
 *
 *	  内存和 ExprContext：     
 *
 *	  因为我们要跨输入行累积聚合值，所以需要用到          
 *	  比单纯输入/输出元组上下文更多的内存上下文。      
 *	  事实上，对一个 rollup 而言，每个分组集都需要单独   
 *	  上下文，以便在分组边界处重置内层（更细粒度）的   
 *	  聚合，同时继续为外层                            
 *	  （更粗粒度）分组累积值。除此之外，我们还可能      
 *	  同时填充哈希表；不过所有哈希表只需一个上下文  
 *	  即可。  
 *
 *	  于是我们创建一个 aggcontexts 数组，为将要处理的         
 *	  最大 rollup 的每个分组集各建一个 ExprContext，并用 
 *	  这些 ExprContext 的每元组内存上下文来存放聚合的     
 *	  过渡值。hashcontext 是单独创建、用来支撑所有            
 *	  哈希表的。 
 *
 *	  溢出到磁盘 
 *
 *	  执行哈希聚合时，如果哈希表内存超出限制（参见    
 *	  hash_agg_check_limits()），我们就会进入"溢出模式"。    
 *	  在溢出模式下，我们只为哈希表中已有的分组推进   
 *	  过渡状态。需新建哈希表项（并初始化新过渡状  
 *	  态）的元组，我们会改为把它们溢出到磁盘，留待      
 *	  以后再处理。元组会以分区化的方式溢出，这样后 
 *	  续批次更小、更不容易超出 hash_mem（如果某一批         
 *	  次确实超出 hash_mem，就必须递归溢出             
 *	  ）。       
 *
 *	  溢出的数据会写入逻辑磁带。与每次溢出都用一个    
 *	  BufFile 相比，这样能更好地控制内存用量、磁盘        
 *	  空间和文件数量。我们并不知道算法开始时需要多        
 *	  少条磁带（因为算法可能递归），所以一开始就分配   
 *	  一个磁带集，之后按需创建各条磁带。                    
 *	  读取某条磁带时，logtape.c 会回收其磁盘空间。当       
 *	  一条磁带读完后，它会被完全销毁。     
 *
 *	  当许多磁带同时打开时，磁带缓冲区可能占用大量       
 *	  内存。读模式下我们一次只需打开一条磁带（使用     
 *	  BLCKSZ 倍数的缓冲区）；但写模式下每个分区都需要        
 *	  打开一条磁带（缓冲区大小为 BLCKSZ）。  
 *
 *	  请注意，过渡状态可能开始时很小，之后却变得非   
 *	  常巨大；例如 ARRAY_AGG 的情况。在这类情况下，        
 *	  仍然可能大幅超出 hash_mem。我们会尽量估算可用      
 *	  内存的容纳量，并对分组数量施加独立于已耗内存     
 *	  量的限制，来尽量避免这种                                  
 *	  情况。       
 *
 *    转移/组合函数的调用：          
 *
 *    出于性能考虑，转移函数（包括组合函数）并不
 *    会在 nodeAgg.c 中先用表达式求值引擎算出参数再     
 *    逐个调用。取而代之的是，                     
 *    ExecBuildAggTrans() 会构建一个大表达式，同时完成         
 *    参数求值和转移函数调用两件事。这避免了因反复    
 *    使用表达式求值导致的性能问题，避免了过滤表达     
 *    式必须提前求值的复杂性问题，还允许把整个表达   
 *    式 JIT 成一个本机函数。               
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeAgg.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "executor/execExpr.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "lib/hyperloglog.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/dynahash.h"
#include "utils/expandeddatum.h"
#include "utils/logtape.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"

/*
 * 控制在把 HashAgg 溢出到磁盘时创建的分区数量    
 *      
 *
 * HASHAGG_PARTITION_FACTOR 会乘以估计所需的分区数       
 * 量，使每个分区都能装进内存。该系数设得比 1 大，    
 * 这是因为分区稍微偏多并不算大的代价，而且这样        
 * 分区需要被递归溢出的可能性更小。拥有更多、更        
 * 小的分区还有一个好处：由于内存缓存效应，小           
 * 哈希表的性能可能比大哈希表                                   
 * 更好。       
 *
 * 我们还为每次溢出指定了分区数量的下限和上限。         
 * 太少可能意味着同一批元组被反复溢出，浪费大量     
 * I/O。太多则会让大量内存浪费在缓冲溢出文件上           
 * （本可花在更大的哈希表上）。     
 */
#define HASHAGG_PARTITION_FACTOR 1.50
#define HASHAGG_MIN_PARTITIONS 4
#define HASHAGG_MAX_PARTITIONS 1024

/*
 * 读取磁带时，缓冲区大小必须是 BLCKSZ 的        
 * 倍数。并发读取多条磁带时，更大的值会有帮助，       
 * 但 HashAgg 中不会这样，所以读取时我们直接用             
 * BLCKSZ；写磁带缓冲总是 BLCKSZ。 
 */
#define HASHAGG_READ_BUFFER_SIZE BLCKSZ
#define HASHAGG_WRITE_BUFFER_SIZE BLCKSZ

/*
 * HyperLogLog 用于估计某个分区中溢出元组的基数。            
 * 5 位对应约 32 字节的大小和约 18% 的最坏情况误差，   
 * 这对递归时选择合理的分区数量来说已经             
 * 足够有效。                                
 */
#define HASHAGG_HLL_BIT_WIDTH 5

/*
 * 把块开销估计为常量 16 字节。XXX：要不要            
 * 改进？
 */
#define CHUNKHDRSZ 16

/*
 * 表示单个哈希表的分区化溢出数据。包含把元组路    
 * 由到正确分区所需的信息，以及把溢出数据转变成    
 * 新批次所需的信息。                 
 *
 * 高位用于分区选择（递归时，我们会忽略在更早          
 * 层级已被用于分区选择的那些位                                
 * ）。 
 */
typedef struct HashAggSpill
{
	int			npartitions;	/* 分区数量         */
	LogicalTape **partitions;	/* 溢出分区磁带    */
	int64	   *ntuples;		/* 每个分区中的元组数量     */
	uint32		mask;			/* 从哈希值找出分区的掩码      */
	int			shift;			/* 掩码之后再按此量移位      */
	hyperLogLogState *hll_card; /* 对内容的基数估计          */
} HashAggSpill;

/*
 * 表示哈希聚合单趟处理（仅含一个分组集）所需的        
 * 工作。     
 *
 * 同时还跟踪之前各轮迭代已用于分区选择的哈希         
 * 位，从而本批次能使用新的位。如果所有位都已被       
 * 使用过，就不会再做分区（所有溢出数据都会进入       
 * 单个输出磁带）。 
 */
typedef struct HashAggBatch
{
	int			setno;			/* 分组集    */
	int			used_bits;		/* 哈希位中已被使用的位数   */
	LogicalTape *input_tape;	/* 输入分区磁带   */
	int64		input_tuples;	/* 本批次中的元组数量    */
	double		input_card;		/* 分组基数的估计值    */
} HashAggBatch;

/* 用于找出被引用的列号 */
typedef struct FindColsContext
{
	bool		is_aggref;		/* 是否在aggref下 */
	Bitmapset  *aggregated;		/* aggref 之下的列引用         */
	Bitmapset  *unaggregated;	/* 其他列引用         */
} FindColsContext;

static void select_current_set(AggState *aggstate, int setno, bool is_hash);
static void initialize_phase(AggState *aggstate, int newphase);
static TupleTableSlot *fetch_input_tuple(AggState *aggstate);
static void initialize_aggregates(AggState *aggstate,
								  AggStatePerGroup *pergroups,
								  int numReset);
static void advance_transition_function(AggState *aggstate,
										AggStatePerTrans pertrans,
										AggStatePerGroup pergroupstate);
static void advance_aggregates(AggState *aggstate);
static void process_ordered_aggregate_single(AggState *aggstate,
											 AggStatePerTrans pertrans,
											 AggStatePerGroup pergroupstate);
static void process_ordered_aggregate_multi(AggState *aggstate,
											AggStatePerTrans pertrans,
											AggStatePerGroup pergroupstate);
static void finalize_aggregate(AggState *aggstate,
							   AggStatePerAgg peragg,
							   AggStatePerGroup pergroupstate,
							   Datum *resultVal, bool *resultIsNull);
static void finalize_partialaggregate(AggState *aggstate,
									  AggStatePerAgg peragg,
									  AggStatePerGroup pergroupstate,
									  Datum *resultVal, bool *resultIsNull);
static inline void prepare_hash_slot(AggStatePerHash perhash,
									 TupleTableSlot *inputslot,
									 TupleTableSlot *hashslot);
static void prepare_projection_slot(AggState *aggstate,
									TupleTableSlot *slot,
									int currentSet);
static void finalize_aggregates(AggState *aggstate,
								AggStatePerAgg peraggs,
								AggStatePerGroup pergroup);
static TupleTableSlot *project_aggregates(AggState *aggstate);
static void find_cols(AggState *aggstate, Bitmapset **aggregated,
					  Bitmapset **unaggregated);
static bool find_cols_walker(Node *node, FindColsContext *context);
static void build_hash_tables(AggState *aggstate);
static void build_hash_table(AggState *aggstate, int setno, long nbuckets);
static void hashagg_recompile_expressions(AggState *aggstate, bool minslot,
										  bool nullcheck);
static long hash_choose_num_buckets(double hashentrysize,
									long ngroups, Size memory);
static int	hash_choose_num_partitions(double input_groups,
									   double hashentrysize,
									   int used_bits,
									   int *log2_npartitions);
static void initialize_hash_entry(AggState *aggstate,
								  TupleHashTable hashtable,
								  TupleHashEntry entry);
static void lookup_hash_entries(AggState *aggstate);
static TupleTableSlot *agg_retrieve_direct(AggState *aggstate);
static void agg_fill_hash_table(AggState *aggstate);
static bool agg_refill_hash_table(AggState *aggstate);
static TupleTableSlot *agg_retrieve_hash_table(AggState *aggstate);
static TupleTableSlot *agg_retrieve_hash_table_in_memory(AggState *aggstate);
static void hash_agg_check_limits(AggState *aggstate);
static void hash_agg_enter_spill_mode(AggState *aggstate);
static void hash_agg_update_metrics(AggState *aggstate, bool from_tape,
									int npartitions);
static void hashagg_finish_initial_spills(AggState *aggstate);
static void hashagg_reset_spill_state(AggState *aggstate);
static HashAggBatch *hashagg_batch_new(LogicalTape *input_tape, int setno,
									   int64 input_tuples, double input_card,
									   int used_bits);
static MinimalTuple hashagg_batch_read(HashAggBatch *batch, uint32 *hashp);
static void hashagg_spill_init(HashAggSpill *spill, LogicalTapeSet *tapeset,
							   int used_bits, double input_groups,
							   double hashentrysize);
static Size hashagg_spill_tuple(AggState *aggstate, HashAggSpill *spill,
								TupleTableSlot *inputslot, uint32 hash);
static void hashagg_spill_finish(AggState *aggstate, HashAggSpill *spill,
								 int setno);
static Datum GetAggInitVal(Datum textInitVal, Oid transtype);
static void build_pertrans_for_aggref(AggStatePerTrans pertrans,
									  AggState *aggstate, EState *estate,
									  Aggref *aggref, Oid transfn_oid,
									  Oid aggtranstype, Oid aggserialfn,
									  Oid aggdeserialfn, Datum initValue,
									  bool initValueIsNull, Oid *inputTypes,
									  int numArguments);


/*
 * 选择当前分组集；会影响 current_set 与       
 * curaggcontext.
 */
static void
select_current_set(AggState *aggstate, int setno, bool is_hash)
{
	/*
	 * 改动此处时，也要适配 ExecAggPlainTransByVal() 和
	 * ExecAggPlainTransByRef().
	 */
	if (is_hash)
		aggstate->curaggcontext = aggstate->hashcontext;
	else
		aggstate->curaggcontext = aggstate->aggcontexts[setno];

	aggstate->current_set = setno;
}

/*
 * 切换到阶段 "newphase"，它必须是 0、1（用于重置）    
 * 或 current_phase+1。相应调度各 tuplesort。   
 *
 * 阶段 0 用于哈希；在 AGG_MIXED 情形中它被排在            
 * 最后，所以进入阶段 0 只需丢弃未完的排序。         
 */
static void
initialize_phase(AggState *aggstate, int newphase)
{
	Assert(newphase <= 1 || newphase == aggstate->current_phase + 1);

	/*
	 * 无论之前状态如何，我们已经用完了正在         
	 * 用的 tuplesort。  
	 */
	if (aggstate->sort_in)
	{
		tuplesort_end(aggstate->sort_in);
		aggstate->sort_in = NULL;
	}

	if (newphase <= 1)
	{
		/*
		 * 丢弃已有的输出 tuplesort。    
		 */
		if (aggstate->sort_out)
		{
			tuplesort_end(aggstate->sort_out);
			aggstate->sort_out = NULL;
		}
	}
	else
	{
		/*
		 * 旧的输出 tuplesort 会变成新的输入 tuplesort，          
		 * 现在就实际对它排序。 
		 */
		aggstate->sort_in = aggstate->sort_out;
		aggstate->sort_out = NULL;
		Assert(aggstate->sort_in);
		tuplesort_performsort(aggstate->sort_in);
	}

	/*
	 * 若这不是最后一个阶段，我们需要按序列中的       
	 * 阶段相应排序。  
	 */
	if (newphase > 0 && newphase < aggstate->numphases - 1)
	{
		Sort	   *sortnode = aggstate->phases[newphase + 1].sortnode;
		PlanState  *outerNode = outerPlanState(aggstate);
		TupleDesc	tupDesc = ExecGetResultType(outerNode);

		aggstate->sort_out = tuplesort_begin_heap(tupDesc,
												  sortnode->numCols,
												  sortnode->sortColIdx,
												  sortnode->sortOperators,
												  sortnode->collations,
												  sortnode->nullsFirst,
												  work_mem,
												  NULL, TUPLESORT_NONE);
	}

	aggstate->current_phase = newphase;
	aggstate->phase = &aggstate->phases[newphase];
}

/*
 * 从外层计划（用于阶段 1）或前一阶段填充好的           
 * 排序器中取出一条元组。若还有下一阶段，则复制        
 * 过去 
 *
 * 调用方不能指望返回槽位中元组的内存在随后取出      
 * 任何元组之后仍然有效。   
 */
static TupleTableSlot *
fetch_input_tuple(AggState *aggstate)
{
	TupleTableSlot *slot;

	if (aggstate->sort_in)
	{
		/* 确保在这条路径的两个分支里都检查中断        */
		CHECK_FOR_INTERRUPTS();
		if (!tuplesort_gettupleslot(aggstate->sort_in, true, false,
									aggstate->sort_slot, NULL))
			return NULL;
		slot = aggstate->sort_slot;
	}
	else
		slot = ExecProcNode(outerPlanState(aggstate));

	if (!TupIsNull(slot) && aggstate->sort_out)
		tuplesort_puttupleslot(aggstate->sort_out, slot);

	return slot;
}

/*
 * （重新）初始化单个聚合。   
 *
 * 本函数只处理一个分组集，它已在 aggstate->   
 * current_set 中。    
 *
 * 调用时，CurrentMemoryContext 应为每查询上下文。      
 */
static void
initialize_aggregate(AggState *aggstate, AggStatePerTrans pertrans,
					 AggStatePerGroup pergroupstate)
{
	/*
	 * 为每个 DISTINCT/ORDER BY 聚合开一次全新排序操作。  
	 */
	if (pertrans->aggsortrequired)
	{
		/*
		 * 若为重新扫描：是否可能有未完成的排序？  
		 * 若有可能就清理掉。   
		 */
		if (pertrans->sortstates[aggstate->current_set])
			tuplesort_end(pertrans->sortstates[aggstate->current_set]);


		/*
		 * 只有一个输入列时，用普通 Datum 排序器；        
		 * 否则对完整元组排序。（参见          
		 * process_ordered_aggregate_single.)
		 */
		if (pertrans->numInputs == 1)
		{
			Form_pg_attribute attr = TupleDescAttr(pertrans->sortdesc, 0);

			pertrans->sortstates[aggstate->current_set] =
				tuplesort_begin_datum(attr->atttypid,
									  pertrans->sortOperators[0],
									  pertrans->sortCollations[0],
									  pertrans->sortNullsFirst[0],
									  work_mem, NULL, TUPLESORT_NONE);
		}
		else
			pertrans->sortstates[aggstate->current_set] =
				tuplesort_begin_heap(pertrans->sortdesc,
									 pertrans->numSortCols,
									 pertrans->sortColIdx,
									 pertrans->sortOperators,
									 pertrans->sortCollations,
									 pertrans->sortNullsFirst,
									 work_mem, NULL, TUPLESORT_NONE);
	}

	/*
	 * 将 transValue 重置为初始值。    
	 *
	 * 注意当初始值为引用传递时，我们必须把它复制       
	 * （到 aggcontext），因为会 pfree transValue。     
	 */
	if (pertrans->initValueIsNull)
		pergroupstate->transValue = pertrans->initValue;
	else
	{
		MemoryContext oldContext;

		oldContext = MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);
		pergroupstate->transValue = datumCopy(pertrans->initValue,
											  pertrans->transtypeByVal,
											  pertrans->transtypeLen);
		MemoryContextSwitchTo(oldContext);
	}
	pergroupstate->transValueIsNull = pertrans->initValueIsNull;

	/*
	 * 如果过渡状态的初始值在 pg_aggregate 表中不存在， 
	 * 那么我们就让外层 procNode 返回的第一个非 NULL         
	 * 值成为初始值。（这对 max() 和 min() 这类聚合很       
	 * 有用。）noTransValue 标志表示我们仍需要                   
	 * 继续这样做。    
	 */
	pergroupstate->noTransValue = pertrans->initValueIsNull;
}

/*
 * 为一组新的输入值初始化所有聚合过渡状态。               
 *
 * 若有多个分组集，我们只初始化其中前 numReset 个          
 * （分组集经过排序，最具体的排在最前，它重置得         
 * 也最频繁）。方便起见，若 numReset 为 0，则会把       
 * 它们全部重置。 
 *
 * 注意：这不能用于哈希聚合，因为那些场合的分组        
 * 集编号必须从更上层指定。       
 *
 * 调用时，CurrentMemoryContext 应为每查询上下文。      
 */
static void
initialize_aggregates(AggState *aggstate,
					  AggStatePerGroup *pergroups,
					  int numReset)
{
	int			transno;
	int			numGroupingSets = Max(aggstate->phase->numsets, 1);
	int			setno = 0;
	int			numTrans = aggstate->numtrans;
	AggStatePerTrans transstates = aggstate->pertrans;

	if (numReset == 0)
		numReset = numGroupingSets;

	for (setno = 0; setno < numReset; setno++)
	{
		AggStatePerGroup pergroup = pergroups[setno];

		select_current_set(aggstate, setno, false);

		for (transno = 0; transno < numTrans; transno++)
		{
			AggStatePerTrans pertrans = &transstates[transno];
			AggStatePerGroup pergroupstate = &pergroup[transno];

			initialize_aggregate(aggstate, pertrans, pergroupstate);
		}
	}
}

/*
 * 给定新的输入值，只推进单个分组集（已在                 
 * aggstate->current_set 中设置）内的一个聚合状态               
 *
 * 新值（以及空标志）已预先装入 pertrans->transfn_fcinfo        
 * 的参数位置 1 及更高位，因此无需再复制一遍来传给      
 * 转移函数。我们还预期 fcinfo 的静态字段已经初始         
 * 化；这是由 ExecInitAgg() 完成的。                         
 *
 * 在哪个内存上下文中调用它都没有关系。   
 */
static void
advance_transition_function(AggState *aggstate,
							AggStatePerTrans pertrans,
							AggStatePerGroup pergroupstate)
{
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	MemoryContext oldContext;
	Datum		newVal;

	if (pertrans->transfn.fn_strict)
	{
		/*
		 * 对于 strict 的 transfn，NULL 输入时不会有任何          
		 * 动静；保留旧transValue。
		 */
		int			numTransInputs = pertrans->numTransInputs;
		int			i;

		for (i = 1; i <= numTransInputs; i++)
		{
			if (fcinfo->args[i].isnull)
				return;
		}
		if (pergroupstate->noTransValue)
		{
			/*
			 * transValue 尚未初始化。这是第一个非 NULL 的输入 
			 * 值。我们用它作为 transValue 的初值。（已检查过
			 * 聚合的输入类型与其 transtype 二进制兼容，      
			 * 所以这里直接复制即可。）             
			 *
			 * 若 datum 是引用传递的，必须把它复制进           
			 * aggcontext；transValue 为 NULL 故无需 pfree。      
			 */
			oldContext = MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);
			pergroupstate->transValue = datumCopy(fcinfo->args[1].value,
												  pertrans->transtypeByVal,
												  pertrans->transtypeLen);
			pergroupstate->transValueIsNull = false;
			pergroupstate->noTransValue = false;
			MemoryContextSwitchTo(oldContext);
			return;
		}
		if (pergroupstate->transValueIsNull)
		{
			/*
			 * 不得用NULL输入调用 strict 函数。注意，即便 
			 * 通过了上述测试，当 transfn 为 strict *且*上一轮  
			 * 返回了 NULL，仍可能到达此处。若发生这种      
			 * 情况，NULL 会被一路传递到最后。      
			 */
			return;
		}
	}

	/* 我们在每输入元组的内存上下文中运行转移函数   */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	/* 为 AggGetAggref() 设置 aggstate->curpertrans */
	aggstate->curpertrans = pertrans;

	/*
	 * 可以调用转移函数了       
	 */
	fcinfo->args[0].value = pergroupstate->transValue;
	fcinfo->args[0].isnull = pergroupstate->transValueIsNull;
	fcinfo->isnull = false;		/* 以防 transfn 未设置它         */

	newVal = FunctionCallInvoke(fcinfo);

	aggstate->curpertrans = NULL;

	/*
	 * 如果是引用传递类型，必须把新值复制进 aggcontext   
	 * 并释放先前的 transValue。但若 transfn 返回的指针      
	 * 指向首个输入，则无需操作。   
	 *
	 * 即使两者中任一为 NULL，直接比较 newVal 与             
	 * pergroup->transValue 也是安全的，因为                         
	 * ExecAggCopyTransValue 会确保在 NULL 时把                       
	 * transValue 置为 0。否则，当 transValue 与 newValue      
	 * 数值相同而实际为 NULL 时，我们可能会碰巧不做      
	 * 重新挂接。此路径相对热门，故不宜另设分支解决  
	 * 转移函数返回（修改过的）输入参数的常见     
	 * 情形。
	 */
	if (!pertrans->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(pergroupstate->transValue))
		newVal = ExecAggCopyTransValue(aggstate, pertrans,
									   newVal, fcinfo->isnull,
									   pergroupstate->transValue,
									   pergroupstate->transValueIsNull);

	pergroupstate->transValue = newVal;
	pergroupstate->transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}

/*
 * 为一条输入元组推进每个聚合的过渡状态。输入        
 * 元组已存入 tmpcontext->ecxt_outertuple，以便               
 * ExecEvalExpr 访问。     
 *
 * 我们有两套要处理的过渡状态：一套用于排序聚合，      
 * 一套用于哈希聚合；我们同处处理两者，避免再次       
 * 求值。  
 *
 * 调用时，CurrentMemoryContext 应为每查询上下文。      
 */
static void
advance_aggregates(AggState *aggstate)
{
	bool		dummynull;

	ExecEvalExprSwitchContext(aggstate->phase->evaltrans,
							  aggstate->tmpcontext,
							  &dummynull);
}

/*
 * 为只有一个输入的 DISTINCT 或 ORDER BY 聚合           
 * 运行转移函数。它在所有输入值录入排序器后
 * 调用。我们完成排序，按序读出各值，并对           
 * 每个值运行转移函数（适用时施加                    
 * DISTINCT）。                                            
 *
 * 注意：往排序中录入值时已经检查过转移函数的     
 * 严格性，因此这里不再检查；我们只应用标准的    
 * SQL DISTINCT 逻辑。                    
 *
 * 单输入与多输入情形分开处理，是出于性能考虑：
 * 对于单列按值传递输入，例如常见的                
 * count(distinct id)，tuplesort_getdatum 代码路径               
 * 要快 300% 左右。（按引用类型的提速幅度较小，    
 * 但仍很可观。） 
 *
 * 本函数只处理一个分组集（该集已在 aggstate->
 * current_set 里）。  
 *
 * 调用时，CurrentMemoryContext 应为每查询上下文。      
 */
static void
process_ordered_aggregate_single(AggState *aggstate,
								 AggStatePerTrans pertrans,
								 AggStatePerGroup pergroupstate)
{
	Datum		oldVal = (Datum) 0;
	bool		oldIsNull = true;
	bool		haveOldVal = false;
	MemoryContext workcontext = aggstate->tmpcontext->ecxt_per_tuple_memory;
	MemoryContext oldContext;
	bool		isDistinct = (pertrans->numDistinctCols > 0);
	Datum		newAbbrevVal = (Datum) 0;
	Datum		oldAbbrevVal = (Datum) 0;
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	Datum	   *newVal;
	bool	   *isNull;

	Assert(pertrans->numDistinctCols < 2);

	tuplesort_performsort(pertrans->sortstates[aggstate->current_set]);

	/* 把该列装入参数 1（参数 0 将是过渡值）            */
	newVal = &fcinfo->args[1].value;
	isNull = &fcinfo->args[1].isnull;

	/*
	 * 注意：若输入类型是引用传递，排序返回的 datum        
	 * 都是在每查询上下文中新 palloc 出来的，所以必须  
	 * 小心在不再需要时将其 pfree。   
	 */

	while (tuplesort_getdatum(pertrans->sortstates[aggstate->current_set],
							  true, false, newVal, isNull, &newAbbrevVal))
	{
		/*
		 * 清空并为相等函数与转移函数的求值选择工作       
		 * 上下文。                     
		 */
		MemoryContextReset(workcontext);
		oldContext = MemoryContextSwitchTo(workcontext);

		/*
		 * 若 DISTINCT 模式且与先前无差异，跳过。   
		 */
		if (isDistinct &&
			haveOldVal &&
			((oldIsNull && *isNull) ||
			 (!oldIsNull && !*isNull &&
			  oldAbbrevVal == newAbbrevVal &&
			  DatumGetBool(FunctionCall2Coll(&pertrans->equalfnOne,
											 pertrans->aggCollation,
											 oldVal, *newVal)))))
		{
			MemoryContextSwitchTo(oldContext);
			continue;
		}
		else
		{
			advance_transition_function(aggstate, pertrans, pergroupstate);

			MemoryContextSwitchTo(oldContext);

			/*
			 * 忘掉旧值（若有），并记住新值供后续       
			 * 相等性检查之用。   
			 */
			if (!pertrans->inputtypeByVal)
			{
				if (!oldIsNull)
					pfree(DatumGetPointer(oldVal));
				if (!*isNull)
					oldVal = datumCopy(*newVal, pertrans->inputtypeByVal,
									   pertrans->inputtypeLen);
			}
			else
				oldVal = *newVal;
			oldAbbrevVal = newAbbrevVal;
			oldIsNull = *isNull;
			haveOldVal = true;
		}
	}

	if (!oldIsNull && !pertrans->inputtypeByVal)
		pfree(DatumGetPointer(oldVal));

	tuplesort_end(pertrans->sortstates[aggstate->current_set]);
	pertrans->sortstates[aggstate->current_set] = NULL;
}

/*
 * 为有多个输入的 DISTINCT 或 ORDER BY 聚合运行        
 * 转移函数。这是在把所有输入值都录入排序对象  
 * 之后调用的。我们完成排序，按排序顺序读出        
 * 各值，并对每个值运行转移函数（适用时施加     
 * DISTINCT）。                                            
 *
 * 本函数只处理一个分组集（该集已在 aggstate->
 * current_set 里）。  
 *
 * 调用时，CurrentMemoryContext 应为每查询上下文。      
 */
static void
process_ordered_aggregate_multi(AggState *aggstate,
								AggStatePerTrans pertrans,
								AggStatePerGroup pergroupstate)
{
	ExprContext *tmpcontext = aggstate->tmpcontext;
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	TupleTableSlot *slot1 = pertrans->sortslot;
	TupleTableSlot *slot2 = pertrans->uniqslot;
	int			numTransInputs = pertrans->numTransInputs;
	int			numDistinctCols = pertrans->numDistinctCols;
	Datum		newAbbrevVal = (Datum) 0;
	Datum		oldAbbrevVal = (Datum) 0;
	bool		haveOldValue = false;
	TupleTableSlot *save = aggstate->tmpcontext->ecxt_outertuple;
	int			i;

	tuplesort_performsort(pertrans->sortstates[aggstate->current_set]);

	ExecClearTuple(slot1);
	if (slot2)
		ExecClearTuple(slot2);

	while (tuplesort_gettupleslot(pertrans->sortstates[aggstate->current_set],
								  true, true, slot1, &newAbbrevVal))
	{
		CHECK_FOR_INTERRUPTS();

		tmpcontext->ecxt_outertuple = slot1;
		tmpcontext->ecxt_innertuple = slot2;

		if (numDistinctCols == 0 ||
			!haveOldValue ||
			newAbbrevVal != oldAbbrevVal ||
			!ExecQual(pertrans->equalfnMulti, tmpcontext))
		{
			/*
			 * 把前 numTransInputs 列提取为要传给 transfn 的      
			 * datum。    
			 */
			slot_getsomeattrs(slot1, numTransInputs);

			/* 把值装入 fcinfo     */
			/* 从 1 开始，因为第 0 个参数将是过渡值          */
			for (i = 0; i < numTransInputs; i++)
			{
				fcinfo->args[i + 1].value = slot1->tts_values[i];
				fcinfo->args[i + 1].isnull = slot1->tts_isnull[i];
			}

			advance_transition_function(aggstate, pertrans, pergroupstate);

			if (numDistinctCols > 0)
			{
				/* 交换槽位指针以保住当前元组            */
				TupleTableSlot *tmpslot = slot2;

				slot2 = slot1;
				slot1 = tmpslot;
				/* 复用缩写键以避免 ExecQual() 调用         */
				oldAbbrevVal = newAbbrevVal;
				haveOldValue = true;
			}
		}

		/* 每次都重置上下文*/
		ResetExprContext(tmpcontext);

		ExecClearTuple(slot1);
	}

	if (slot2)
		ExecClearTuple(slot2);

	tuplesort_end(pertrans->sortstates[aggstate->current_set]);
	pertrans->sortstates[aggstate->current_set] = NULL;

	/* 恢复先前的槽位（可能供分组集使用）         */
	tmpcontext->ecxt_outertuple = save;
}

/*
 * 计算单个聚合的最终值。        
 *
 * 本函数只处理一个分组集（该集已在 aggstate->
 * current_set 里）。  
 *
 * 最终函数会在输出元组上下文中运行并交付
 * 结果；调用方的 CurrentMemoryContext 无关紧要。          
 * （但注意，在某些情况下，比如没有最终函数时， 
 * 结果可能是指向聚合过渡值或其内部的指针。）  
 *
 * 最终函数使用 transno 中设定的状态。其他聚合         
 * 函数可能也正在使用该状态，因此重要的一点是       
 * 我们在这里不做任何破坏性操作。此外，聚合的       
 * 最终值可能被多处使用，故不能返回 R/W 展开 datum。   
 */
static void
finalize_aggregate(AggState *aggstate,
				   AggStatePerAgg peragg,
				   AggStatePerGroup pergroupstate,
				   Datum *resultVal, bool *resultIsNull)
{
	LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
	bool		anynull = false;
	MemoryContext oldContext;
	int			i;
	ListCell   *lc;
	AggStatePerTrans pertrans = &aggstate->pertrans[peragg->transno];

	oldContext = MemoryContextSwitchTo(aggstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * 计算所有直接参数。即使没有最终函数（这本来就   
	 * 不太可能），我们也照样计算，以便副作用按预期  
	 * 发生。直接参数会放入参数位置 1 及更高位，位置 0   
	 * 则留给过渡状态值。    
	 */
	i = 1;
	foreach(lc, peragg->aggdirectargs)
	{
		ExprState  *expr = (ExprState *) lfirst(lc);

		fcinfo->args[i].value = ExecEvalExpr(expr,
											 aggstate->ss.ps.ps_ExprContext,
											 &fcinfo->args[i].isnull);
		anynull |= fcinfo->args[i].isnull;
		i++;
	}

	/*
	 * 若聚合有最终函数则应用，否则返回 transValue。     
	 */
	if (OidIsValid(peragg->finalfn_oid))
	{
		int			numFinalArgs = peragg->numFinalArgs;

		/* 为 AggGetAggref() 设置 aggstate->curperagg */
		aggstate->curperagg = peragg;

		InitFunctionCallInfoData(*fcinfo, &peragg->finalfn,
								 numFinalArgs,
								 pertrans->aggCollation,
								 (void *) aggstate, NULL);

		/* 填入过渡状态值              */
		fcinfo->args[0].value =
			MakeExpandedObjectReadOnly(pergroupstate->transValue,
									   pergroupstate->transValueIsNull,
									   pertrans->transtypeLen);
		fcinfo->args[0].isnull = pergroupstate->transValueIsNull;
		anynull |= pergroupstate->transValueIsNull;

		/* 把其余参数位置填入空值                */
		for (; i < numFinalArgs; i++)
		{
			fcinfo->args[i].value = (Datum) 0;
			fcinfo->args[i].isnull = true;
			anynull = true;
		}

		if (fcinfo->flinfo->fn_strict && anynull)
		{
			/* 不要以 NULL 输入调用 strict 函数     */
			*resultVal = (Datum) 0;
			*resultIsNull = true;
		}
		else
		{
			Datum		result;

			result = FunctionCallInvoke(fcinfo);
			*resultIsNull = fcinfo->isnull;
			*resultVal = MakeExpandedObjectReadOnly(result,
													fcinfo->isnull,
													peragg->resulttypeLen);
		}
		aggstate->curperagg = NULL;
	}
	else
	{
		*resultVal =
			MakeExpandedObjectReadOnly(pergroupstate->transValue,
									   pergroupstate->transValueIsNull,
									   pertrans->transtypeLen);
		*resultIsNull = pergroupstate->transValueIsNull;
	}

	MemoryContextSwitchTo(oldContext);
}

/*
 * 计算单个部分聚合的输出值。           
 *
 * 序列化函数会运行，其结果也会在输出元组上下文      
 * 中交付；调用方的 CurrentMemoryContext 无关紧要。       
 */
static void
finalize_partialaggregate(AggState *aggstate,
						  AggStatePerAgg peragg,
						  AggStatePerGroup pergroupstate,
						  Datum *resultVal, bool *resultIsNull)
{
	AggStatePerTrans pertrans = &aggstate->pertrans[peragg->transno];
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(aggstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * 如果必须在返回之前序列化过渡值，则会设置       
	 * serialfn_oid
	 */
	if (OidIsValid(pertrans->serialfn_oid))
	{
		/* 不要以 NULL 输入调用 strict 序列化函数。       */
		if (pertrans->serialfn.fn_strict && pergroupstate->transValueIsNull)
		{
			*resultVal = (Datum) 0;
			*resultIsNull = true;
		}
		else
		{
			FunctionCallInfo fcinfo = pertrans->serialfn_fcinfo;
			Datum		result;

			fcinfo->args[0].value =
				MakeExpandedObjectReadOnly(pergroupstate->transValue,
										   pergroupstate->transValueIsNull,
										   pertrans->transtypeLen);
			fcinfo->args[0].isnull = pergroupstate->transValueIsNull;
			fcinfo->isnull = false;

			result = FunctionCallInvoke(fcinfo);
			*resultIsNull = fcinfo->isnull;
			*resultVal = MakeExpandedObjectReadOnly(result,
													fcinfo->isnull,
													peragg->resulttypeLen);
		}
	}
	else
	{
		*resultVal =
			MakeExpandedObjectReadOnly(pergroupstate->transValue,
									   pergroupstate->transValueIsNull,
									   pertrans->transtypeLen);
		*resultIsNull = pergroupstate->transValueIsNull;
	}

	MemoryContextSwitchTo(oldContext);
}

/*
 * 把构成分组键的属性提取到 hashslot 中。这是   
 * 计算哈希或执行查找所必需的。                          
 */
static inline void
prepare_hash_slot(AggStatePerHash perhash,
				  TupleTableSlot *inputslot,
				  TupleTableSlot *hashslot)
{
	int			i;

	/* 只把所需的列传入 hashslot              */
	slot_getsomeattrs(inputslot, perhash->largestGrpColIdx);
	ExecClearTuple(hashslot);

	for (i = 0; i < perhash->numhashGrpCols; i++)
	{
		int			varNumber = perhash->hashGrpColIdxInput[i] - 1;

		hashslot->tts_values[i] = inputslot->tts_values[varNumber];
		hashslot->tts_isnull[i] = inputslot->tts_isnull[varNumber];
	}
	ExecStoreVirtualTuple(hashslot);
}

/*
 * 准备好基于给定的代表元组槽位和分组集进行终结         
 * 与投影。          
 *
 * 在给定的元组槽位中，把在当前分组集语境下应当      
 * 读为 null 的属性强制置为 null。同时把当前分组          
 * 位图存放到 GroupingExpr 能取到的地方。    
 *
 * 这依赖于三个条件：     
 *
 * 1) 永远不会有人试图从这个槽位提取完整元组，            
 * 只会在求值中引用它，而求值只会访问单个          
 * 属性。  
 *
 * 2) 不需要有系统列被置为 null。（如果 group 子句            
 * 中引用了某个系统列，它实际上会在外层计划的         
 * tlist.)
 *
 * 3) 在给定阶段内，某个属性一旦被置为 null，我们          
 * 就无需恢复其值。     
 *
 * 这样去戳槽位的内部有点丑，但大家一致认为              
 * 替代方案更糟。 
 */
static void
prepare_projection_slot(AggState *aggstate, TupleTableSlot *slot, int currentSet)
{
	if (aggstate->phase->grouped_cols)
	{
		Bitmapset  *grouped_cols = aggstate->phase->grouped_cols[currentSet];

		aggstate->grouped_cols = grouped_cols;

		if (TTS_EMPTY(slot))
		{
			/*
			 * 若处理的是空输入元组，则把所有值置为 NULL   
			 * （也就是未提供输入行的空分组集           
			 * ）。    
			 */
			ExecStoreAllNullTuple(slot);
		}
		else if (aggstate->all_grouped_cols)
		{
			ListCell   *lc;

			/* all_grouped_cols 按降序排列           */
			slot_getsomeattrs(slot, linitial_int(aggstate->all_grouped_cols));

			foreach(lc, aggstate->all_grouped_cols)
			{
				int			attnum = lfirst_int(lc);

				if (!bms_is_member(attnum, grouped_cols))
					slot->tts_isnull[attnum - 1] = true;
			}
		}
	}
}

/*
 * 为一个分组的所有聚合计算最终值。        
 *
 * 本函数一次只处理一个分组集，调用方必须已经             
 * 把它选择好。调整所给的 pergroup 参数，使其指向            
 * 当前分组集的过渡值，这也是调用方的责任。 
 *
 * 结果存放在输出 econtext 的 aggvalues/aggnulls 中。 
 */
static void
finalize_aggregates(AggState *aggstate,
					AggStatePerAgg peraggs,
					AggStatePerGroup pergroup)
{
	ExprContext *econtext = aggstate->ss.ps.ps_ExprContext;
	Datum	   *aggvalues = econtext->ecxt_aggvalues;
	bool	   *aggnulls = econtext->ecxt_aggnulls;
	int			aggno;

	/*
	 * 如果有任何 DISTINCT 和/或 ORDER BY 聚合，就对其     
	 * 输入排序并运行转移函数。    
	 */
	for (int transno = 0; transno < aggstate->numtrans; transno++)
	{
		AggStatePerTrans pertrans = &aggstate->pertrans[transno];
		AggStatePerGroup pergroupstate;

		pergroupstate = &pergroup[transno];

		if (pertrans->aggsortrequired)
		{
			Assert(aggstate->aggstrategy != AGG_HASHED &&
				   aggstate->aggstrategy != AGG_MIXED);

			if (pertrans->numInputs == 1)
				process_ordered_aggregate_single(aggstate,
												 pertrans,
												 pergroupstate);
			else
				process_ordered_aggregate_multi(aggstate,
												pertrans,
												pergroupstate);
		}
		else if (pertrans->numDistinctCols > 0 && pertrans->haslast)
		{
			pertrans->haslast = false;

			if (pertrans->numDistinctCols == 1)
			{
				if (!pertrans->inputtypeByVal && !pertrans->lastisnull)
					pfree(DatumGetPointer(pertrans->lastdatum));

				pertrans->lastisnull = false;
				pertrans->lastdatum = (Datum) 0;
			}
			else
				ExecClearTuple(pertrans->uniqslot);
		}
	}

	/*
	 * 运行最终函数。   
	 */
	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peragg = &peraggs[aggno];
		int			transno = peragg->transno;
		AggStatePerGroup pergroupstate;

		pergroupstate = &pergroup[transno];

		if (DO_AGGSPLIT_SKIPFINAL(aggstate->aggsplit))
			finalize_partialaggregate(aggstate, peragg, pergroupstate,
									  &aggvalues[aggno], &aggnulls[aggno]);
		else
			finalize_aggregate(aggstate, peragg, pergroupstate,
							   &aggvalues[aggno], &aggnulls[aggno]);
	}
}

/*
 * 投影出一个分组的结果（其聚合已由                         
 * finalize_aggregates 算好）。返回结果槽位，若无行     
 * 被投影，则返回 NULL。  
 */
static TupleTableSlot *
project_aggregates(AggState *aggstate)
{
	ExprContext *econtext = aggstate->ss.ps.ps_ExprContext;

	/*
	 * 检查 qual（HAVING 子句）；分组不匹配就忽略它。        
	 */
	if (ExecQual(aggstate->ss.ps.qual, econtext))
	{
		/*
		 * 用聚合结果和代表输入元组构成并返回投影       
		 * 元组。                      
		 */
		return ExecProject(aggstate->ss.ps.ps_ProjInfo);
	}
	else
		InstrCountFiltered1(aggstate, 1);

	return NULL;
}

/*
 * 找出所需输入元组列，把它们分成已聚合与   
 * 未聚合两组。               
 */
static void
find_cols(AggState *aggstate, Bitmapset **aggregated, Bitmapset **unaggregated)
{
	Agg		   *agg = (Agg *) aggstate->ss.ps.plan;
	FindColsContext context;

	context.is_aggref = false;
	context.aggregated = NULL;
	context.unaggregated = NULL;

	/* 检查 tlist 与 quals  */
	(void) find_cols_walker((Node *) agg->plan.targetlist, &context);
	(void) find_cols_walker((Node *) agg->plan.qual, &context);

	/* 某些情况下，分组列不会出现在 tlist 中         */
	for (int i = 0; i < agg->numCols; i++)
		context.unaggregated = bms_add_member(context.unaggregated,
											  agg->grpColIdx[i]);

	*aggregated = context.aggregated;
	*unaggregated = context.unaggregated;
}

static bool
find_cols_walker(Node *node, FindColsContext *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* setrefs.c 应已把 varno 设置为 OUTER_VAR    */
		Assert(var->varno == OUTER_VAR);
		Assert(var->varlevelsup == 0);
		if (context->is_aggref)
			context->aggregated = bms_add_member(context->aggregated,
												 var->varattno);
		else
			context->unaggregated = bms_add_member(context->unaggregated,
												   var->varattno);
		return false;
	}
	if (IsA(node, Aggref))
	{
		Assert(!context->is_aggref);
		context->is_aggref = true;
		expression_tree_walker(node, find_cols_walker, (void *) context);
		context->is_aggref = false;
		return false;
	}
	return expression_tree_walker(node, find_cols_walker,
								  (void *) context);
}

/*
 * （重新）将哈希表初始化为空。 
 *
 * 要实现哈希聚合，我们需要一个哈希表，为每组   
 * 不同的 GROUP BY 列值组合存储一个代表元组，以及一个
 * AggStatePerGroup 结构数组。哈希键由 GROUP BY 列算出。       
 * 每组数据在 lookup_hash_entry() 中为每个条目分别              
 * 进行分配。
 *
 * 我们对每个做哈希的分组集，都有一个单独的哈希表及   
 * 与之关联的 perhash 数据结构。    
 *
 * 哈希表的内容始终存放在 hashcontext 的每元组内存          
 * 上下文中（所有表共用一个这样的上下文，因为它们    
 * 都会在同一时间重置）。    
 */
static void
build_hash_tables(AggState *aggstate)
{
	int			setno;

	for (setno = 0; setno < aggstate->num_hashes; ++setno)
	{
		AggStatePerHash perhash = &aggstate->perhash[setno];
		long		nbuckets;
		Size		memory;

		if (perhash->hashtable != NULL)
		{
			ResetTupleHashTable(perhash->hashtable);
			continue;
		}

		Assert(perhash->aggnode->numGroups > 0);

		memory = aggstate->hash_mem_limit / aggstate->num_hashes;

		/* 为每个哈希表选择合理的桶数           */
		nbuckets = hash_choose_num_buckets(aggstate->hashentrysize,
										   perhash->aggnode->numGroups,
										   memory);

		build_hash_table(aggstate, setno, nbuckets);
	}

	aggstate->hash_ngroups_current = 0;
}

/*
 * 为该分组集构建单个哈希表。        
 */
static void
build_hash_table(AggState *aggstate, int setno, long nbuckets)
{
	AggStatePerHash perhash = &aggstate->perhash[setno];
	MemoryContext metacxt = aggstate->hash_metacxt;
	MemoryContext hashcxt = aggstate->hashcontext->ecxt_per_tuple_memory;
	MemoryContext tmpcxt = aggstate->tmpcontext->ecxt_per_tuple_memory;
	Size		additionalsize;

	Assert(aggstate->aggstrategy == AGG_HASHED ||
		   aggstate->aggstrategy == AGG_MIXED);

	/*
	 * 用于确保哈希表的初始分配不超过 hash_mem。注意
	 * 估算不含按引用传递的过渡数据值的空间，  
	 * 也不包含每组代表元组                                      
	 * 的空间。        
	 */
	additionalsize = aggstate->numtrans * sizeof(AggStatePerGroupData);

	perhash->hashtable = BuildTupleHashTableExt(&aggstate->ss.ps,
												perhash->hashslot->tts_tupleDescriptor,
												perhash->numCols,
												perhash->hashGrpColIdxHash,
												perhash->eqfuncoids,
												perhash->hashfunctions,
												perhash->aggnode->grpCollations,
												nbuckets,
												additionalsize,
												metacxt,
												hashcxt,
												tmpcxt,
												DO_AGGSPLIT_SKIPFINAL(aggstate->aggsplit));
}

/*
 * 计算真正需要存入哈希表条目中的那些列。子计划节点  
 * 传来的输入元组将包含分组列、在我们的目标列表和  
 * qual 中引用的其他列、用于计算聚合函数的列，或许
 * 还有一些我们根本就用不到的 junk 列。只有前两类列才   
 * 需要存入哈希表，而去掉其余的列就能显著缩小表   
 * 条目的大小。哈希表只包含相关的列，并且会在    
 * lookup_hash_entry() / agg_retrieve_hash_table() 中，按普通输入    
 * 描述符的格式进行打包与                                        
 * 解包。                                      
 *
 * 除分组所依据的列之外，额外的列来自两个来源：其一
 * 是函数依赖列（它们自身无需作为分组依据），其二是   
 * 行标记所用的 ctid。                      
 *
 * 为消除重复，我们为所需的列构建一个 bitmapset，然后  
 * 再为哈希表中包含的列构建一个数组。若传入的       
 * grpColIdx 含有重复项，则仍可能出现重复，这种情况        
 * 可见于半连接/去重等边界情形；这些重复不一定     
 * 总能去除，因为无法确定重复列是否使用相同哈希     
 * 函数。     
 *
 * 注意该数组会在 ExecReScanAgg 之后仍保留，故将其          
 * 分配于查询期上下文（哈希表除外）。  
 */
static void
find_hash_columns(AggState *aggstate)
{
	Bitmapset  *base_colnos;
	Bitmapset  *aggregated_colnos;
	TupleDesc	scanDesc = aggstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	List	   *outerTlist = outerPlanState(aggstate)->plan->targetlist;
	int			numHashes = aggstate->num_hashes;
	EState	   *estate = aggstate->ss.ps.state;
	int			j;

	/* 找出 tlist 和 quals 中将需要的 Var      */
	find_cols(aggstate, &aggregated_colnos, &base_colnos);
	aggstate->colnos_needed = bms_union(base_colnos, aggregated_colnos);
	aggstate->max_colno_needed = 0;
	aggstate->all_cols_needed = true;

	for (int i = 0; i < scanDesc->natts; i++)
	{
		int			colno = i + 1;

		if (bms_is_member(colno, aggstate->colnos_needed))
			aggstate->max_colno_needed = colno;
		else
			aggstate->all_cols_needed = false;
	}

	for (j = 0; j < numHashes; ++j)
	{
		AggStatePerHash perhash = &aggstate->perhash[j];
		Bitmapset  *colnos = bms_copy(base_colnos);
		AttrNumber *grpColIdx = perhash->aggnode->grpColIdx;
		List	   *hashTlist = NIL;
		TupleDesc	hashDesc;
		int			maxCols;
		int			i;

		perhash->largestGrpColIdx = 0;

		/*
		 * 如果我们在做分组集，有些 Var 可能是为其他分组  
		 * 集而在 tlist/qual 中被引用，但哈希时不需要；即  
		 * 是说 prepare_projection_slot 会把它们置空，所以存储 
		 * 它们毫无意义。用 prepare_projection_slot 的逻辑判定
		 * Var 属于此类。      
		 */
		if (aggstate->phases[0].grouped_cols)
		{
			Bitmapset  *grouped_cols = aggstate->phases[0].grouped_cols[j];
			ListCell   *lc;

			foreach(lc, aggstate->all_grouped_cols)
			{
				int			attnum = lfirst_int(lc);

				if (!bms_is_member(attnum, grouped_cols))
					colnos = bms_del_member(colnos, attnum);
			}
		}

		/*
		 * 计算输入列的最大数目，把 grpColIdx 数组中可能 
		 * 出现的重复计算在内。这种重复在某些边界情形中
		 * 可能出现，此时 HashAggregate 作为半连接或 DISTINCT   
		 * 生成。
		 */
		maxCols = bms_num_members(colnos) + perhash->numCols;

		perhash->hashGrpColIdxInput =
			palloc(maxCols * sizeof(AttrNumber));
		perhash->hashGrpColIdxHash =
			palloc(perhash->numCols * sizeof(AttrNumber));

		/* 把所有分组列都加入 colnos     */
		for (i = 0; i < perhash->numCols; i++)
			colnos = bms_add_member(colnos, grpColIdx[i]);

		/*
		 * 先为直接哈希的列建立映射。它们排在最前，  
		 * 是因为计算哈希值和比较元组以确认精确匹配时  
		 * 都要访问它们。我们还为 execGrouping 建立一个     
		 * 简单映射，让它知道在输入中何处查找待哈希/ 
		 * 比较的列。               
		 */
		for (i = 0; i < perhash->numCols; i++)
		{
			perhash->hashGrpColIdxInput[i] = grpColIdx[i];
			perhash->hashGrpColIdxHash[i] = i + 1;
			perhash->numhashGrpCols++;
			/* 删除已映射过的列      */
			colnos = bms_del_member(colnos, grpColIdx[i]);
		}

		/* 然后加入其余的列      */
		i = -1;
		while ((i = bms_next_member(colnos, i)) >= 0)
		{
			perhash->hashGrpColIdxInput[perhash->numhashGrpCols] = i;
			perhash->numhashGrpCols++;
		}

		/* 然后为哈希表构建元组描述符        */
		for (i = 0; i < perhash->numhashGrpCols; i++)
		{
			int			varNumber = perhash->hashGrpColIdxInput[i] - 1;

			hashTlist = lappend(hashTlist, list_nth(outerTlist, varNumber));
			perhash->largestGrpColIdx =
				Max(varNumber + 1, perhash->largestGrpColIdx);
		}

		hashDesc = ExecTypeFromTL(hashTlist);

		execTuplesHashPrepare(perhash->numCols,
							  perhash->aggnode->grpOperators,
							  &perhash->eqfuncoids,
							  &perhash->hashfunctions);
		perhash->hashslot =
			ExecAllocTableSlot(&estate->es_tupleTable, hashDesc,
							   &TTSOpsMinimalTuple);

		list_free(hashTlist);
		bms_free(colnos);
	}

	bms_free(base_colnos);
}

/*
 * 估算每个哈希表条目的开销。
 */
Size
hash_agg_entry_size(int numTrans, Size tupleWidth, Size transitionSpace)
{
	Size		tupleChunkSize;
	Size		pergroupChunkSize;
	Size		transitionChunkSize;
	Size		tupleSize = (MAXALIGN(SizeofMinimalTupleHeader) +
							 tupleWidth);
	Size		pergroupSize = numTrans * sizeof(AggStatePerGroupData);

	tupleChunkSize = CHUNKHDRSZ + tupleSize;

	if (pergroupSize > 0)
		pergroupChunkSize = CHUNKHDRSZ + pergroupSize;
	else
		pergroupChunkSize = 0;

	if (transitionSpace > 0)
		transitionChunkSize = CHUNKHDRSZ + transitionSpace;
	else
		transitionChunkSize = 0;

	return
		sizeof(TupleHashEntryData) +
		tupleChunkSize +
		pergroupChunkSize +
		transitionChunkSize;
}

/*
 * hashagg_recompile_expressions()
 *
 * 确定正确的阶段，按参数编译相应表达式，然后把 
 * phase->evalfunc 设置为该表达式。                    
 *
 * 根据哈希聚合是否已溢出、以及是从外层计划还是从
 * 磁带读取，需要用到编译表达式的不同版本。在溢出     
 * 到磁盘之前，表达式从外层计划读取，且无需执行     
 * NULL 检查。在 HashAgg 开始溢出之后，哈希表中不会   
 * 创建新组，并且 AggStatePerGroup 数组可能为 NULL；因此      
 * 我们需要在表达式中加入空指针检查。然后，当从磁带   
 * 读取溢出数据时，我们把外层槽的类型改为固定的     
 * 最小元组槽。                                                   
 *
 * 每次重新编译太浪费，因此把编译好的表达式缓存 
 * 到 AggStatePerPhase 中，并在适当时加以重用。       
 */
static void
hashagg_recompile_expressions(AggState *aggstate, bool minslot, bool nullcheck)
{
	AggStatePerPhase phase;
	int			i = minslot ? 1 : 0;
	int			j = nullcheck ? 1 : 0;

	Assert(aggstate->aggstrategy == AGG_HASHED ||
		   aggstate->aggstrategy == AGG_MIXED);

	if (aggstate->aggstrategy == AGG_HASHED)
		phase = &aggstate->phases[0];
	else						/* AGG_MIXED */
		phase = &aggstate->phases[1];

	if (phase->evaltrans_cache[i][j] == NULL)
	{
		const TupleTableSlotOps *outerops = aggstate->ss.ps.outerops;
		bool		outerfixed = aggstate->ss.ps.outeropsfixed;
		bool		dohash = true;
		bool		dosort = false;

		/*
		 * 如果 minslot 为真，说明我们正在处理溢出的批次 
		 * （在 agg_refill_hash_table() 中），此时绝不能推进
		 * 已排序分组集。
		 */
		if (aggstate->aggstrategy == AGG_MIXED && !minslot)
			dosort = true;

		/* 在编译表达式期间临时更改 outerops                  */
		if (minslot)
		{
			aggstate->ss.ps.outerops = &TTSOpsMinimalTuple;
			aggstate->ss.ps.outeropsfixed = true;
		}

		phase->evaltrans_cache[i][j] = ExecBuildAggTrans(aggstate, phase,
														 dosort, dohash,
														 nullcheck);

		/* 改回      */
		aggstate->ss.ps.outerops = outerops;
		aggstate->ss.ps.outeropsfixed = outerfixed;
	}

	phase->evaltrans = phase->evaltrans_cache[i][j];
}

/*
 * 设置触发溢出的限制，以避免超出 hash_mem。需要考虑     
 * 预计创建的分区数（若确实发生溢出）。    
 *
 * 有两种限制：内存限制，及组数限制。当我们预计  
 * 过渡值会显著增长到远大于初始值时，ngroups 限制就    
 * 会变得重要。                          
 */
void
hash_agg_set_limits(double hashentrysize, double input_groups, int used_bits,
					Size *mem_limit, uint64 *ngroups_limit,
					int *num_partitions)
{
	int			npartitions;
	Size		partition_mem;
	Size		hash_mem_limit = get_hash_memory_limit();

	/* 若预计不会溢出，则用满 hash_mem    */
	if (input_groups * hashentrysize <= hash_mem_limit)
	{
		if (num_partitions != NULL)
			*num_partitions = 0;
		*mem_limit = hash_mem_limit;
		*ngroups_limit = hash_mem_limit / hashentrysize;
		return;
	}

	/*
	 * 计算溢出所需的内存开销，即所有需要同时保持打开 
	 * 状态的磁带所需的缓冲区大小。然后，把它从可用于
	 * 容纳哈希表的内存中扣除。                                  
	 */
	npartitions = hash_choose_num_partitions(input_groups,
											 hashentrysize,
											 used_bits,
											 NULL);
	if (num_partitions != NULL)
		*num_partitions = npartitions;

	partition_mem =
		HASHAGG_READ_BUFFER_SIZE +
		HASHAGG_WRITE_BUFFER_SIZE * npartitions;

	/*
	 * 不要把限制设得低于 hash_mem 的 3/4。在那种情形下，  
	 * 我们已处于最小分区数，因此并不会大幅超出           
	 * work_mem。     
	 */
	if (hash_mem_limit > 4 * partition_mem)
		*mem_limit = hash_mem_limit - partition_mem;
	else
		*mem_limit = hash_mem_limit * 0.75;

	if (*mem_limit > hashentrysize)
		*ngroups_limit = *mem_limit / hashentrysize;
	else
		*ngroups_limit = 1;
}

/*
 * hash_agg_check_limits
 *
 * 向哈希表加入新组之后，检查是否需要进入溢出模式。  
 * 不加入新组也可能发生内存分配（例如过渡状态的大      
 * 小增长时），因此这种检查并不完美。             
 */
static void
hash_agg_check_limits(AggState *aggstate)
{
	uint64		ngroups = aggstate->hash_ngroups_current;
	Size		meta_mem = MemoryContextMemAllocated(aggstate->hash_metacxt,
													 true);
	Size		hashkey_mem = MemoryContextMemAllocated(aggstate->hashcontext->ecxt_per_tuple_memory,
														true);

	/*
	 * 除非哈希表中已有至少一个组，否则不要溢出，这样
	 * 才能确保在边界情形下也能前进。   
	 */
	if (aggstate->hash_ngroups_current > 0 &&
		(meta_mem + hashkey_mem > aggstate->hash_mem_limit ||
		 ngroups > aggstate->hash_ngroups_limit))
	{
		hash_agg_enter_spill_mode(aggstate);
	}
}

/*
 * 进入"溢出模式"，意思是不会再向任何哈希表加入          
 * 新组。本会创建新组的元组改为被溢出，稍后再加以
 * 处理。       
 */
static void
hash_agg_enter_spill_mode(AggState *aggstate)
{
	aggstate->hash_spill_mode = true;
	hashagg_recompile_expressions(aggstate, aggstate->table_filled, true);

	if (!aggstate->hash_ever_spilled)
	{
		Assert(aggstate->hash_tapeset == NULL);
		Assert(aggstate->hash_spills == NULL);

		aggstate->hash_ever_spilled = true;

		aggstate->hash_tapeset = LogicalTapeSetCreate(true, NULL, -1);

		aggstate->hash_spills = palloc(sizeof(HashAggSpill) * aggstate->num_hashes);

		for (int setno = 0; setno < aggstate->num_hashes; setno++)
		{
			AggStatePerHash perhash = &aggstate->perhash[setno];
			HashAggSpill *spill = &aggstate->hash_spills[setno];

			hashagg_spill_init(spill, aggstate->hash_tapeset, 0,
							   perhash->aggnode->numGroups,
							   aggstate->hashentrysize);
		}
	}
}

/*
 * 在填满哈希表之后更新度量值。  
 *
 * 如果从外层计划读取，from_tape 应为 false；如果从另一    
 * 条磁带读取，则应为 true。    
 */
static void
hash_agg_update_metrics(AggState *aggstate, bool from_tape, int npartitions)
{
	Size		meta_mem;
	Size		hashkey_mem;
	Size		buffer_mem;
	Size		total_mem;

	if (aggstate->aggstrategy != AGG_MIXED &&
		aggstate->aggstrategy != AGG_HASHED)
		return;

	/* 哈希表本身所需的内存   */
	meta_mem = MemoryContextMemAllocated(aggstate->hash_metacxt, true);

	/* 组键与过渡状态所需的内存            */
	hashkey_mem = MemoryContextMemAllocated(aggstate->hashcontext->ecxt_per_tuple_memory, true);

	/* 若溢出，读写磁带缓冲所需的内存  */
	buffer_mem = npartitions * HASHAGG_WRITE_BUFFER_SIZE;
	if (from_tape)
		buffer_mem += HASHAGG_READ_BUFFER_SIZE;

	/* 更新 peak mem */
	total_mem = meta_mem + hashkey_mem + buffer_mem;
	if (total_mem > aggstate->hash_mem_peak)
		aggstate->hash_mem_peak = total_mem;

	/* 更新磁盘用量*/
	if (aggstate->hash_tapeset != NULL)
	{
		uint64		disk_used = LogicalTapeSetBlocks(aggstate->hash_tapeset) * (BLCKSZ / 1024);

		if (aggstate->hash_disk_used < disk_used)
			aggstate->hash_disk_used = disk_used;
	}

	/* 根据内容更新 hashentrysize 估算值      */
	if (aggstate->hash_ngroups_current > 0)
	{
		aggstate->hashentrysize =
			sizeof(TupleHashEntryData) +
			(hashkey_mem / (double) aggstate->hash_ngroups_current);
	}
}

/*
 * 为哈希表的初始大小选择合理的桶数。                   
 */
static long
hash_choose_num_buckets(double hashentrysize, long ngroups, Size memory)
{
	long		max_nbuckets;
	long		nbuckets = ngroups;

	max_nbuckets = memory / hashentrysize;

	/*
	 * 估算偏低比估算偏高要好。桶太多会挤占组键与      
	 * 过渡状态值的空间。                          
	 */
	max_nbuckets >>= 1;

	if (nbuckets > max_nbuckets)
		nbuckets = max_nbuckets;

	return Max(nbuckets, 1);
}

/*
 * 确定溢出时要创建的分区数，它将总是 2 的幂。如果 
 * log2_npartitions 非 NULL，则把分区数的 log2() 存入  
 * *其中。                                                  
 */
static int
hash_choose_num_partitions(double input_groups, double hashentrysize,
						   int used_bits, int *log2_npartitions)
{
	Size		hash_mem_limit = get_hash_memory_limit();
	double		partition_limit;
	double		mem_wanted;
	double		dpartitions;
	int			npartitions;
	int			partition_bits;

	/*
	 * 避免创建过多分区，以致打开的分区文件所需的内存
	 * 超过 hash_mem 的 1/4。                            
	 */
	partition_limit =
		(hash_mem_limit * 0.25 - HASHAGG_READ_BUFFER_SIZE) /
		HASHAGG_WRITE_BUFFER_SIZE;

	mem_wanted = HASHAGG_PARTITION_FACTOR * input_groups * hashentrysize;

	/* 创建足够多的分区，使每个分区都可能装进内存    */
	dpartitions = 1 + (mem_wanted / hash_mem_limit);

	if (dpartitions > partition_limit)
		dpartitions = partition_limit;

	if (dpartitions < HASHAGG_MIN_PARTITIONS)
		dpartitions = HASHAGG_MIN_PARTITIONS;
	if (dpartitions > HASHAGG_MAX_PARTITIONS)
		dpartitions = HASHAGG_MAX_PARTITIONS;

	/* HASHAGG_MAX_PARTITIONS 上限保证安全    */
	npartitions = (int) dpartitions;

	/* ceil(log2(npartitions)) */
	partition_bits = my_log2(npartitions);

	/* 确保不会耗尽哈希位                   */
	if (partition_bits + used_bits >= 32)
		partition_bits = 32 - used_bits;

	if (log2_npartitions != NULL)
		*log2_npartitions = partition_bits;

	/* 分区数将是 2 的幂                    */
	npartitions = 1 << partition_bits;

	return npartitions;
}

/*
 * 初始化一个新建的 TupleHashEntry。  
 */
static void
initialize_hash_entry(AggState *aggstate, TupleHashTable hashtable,
					  TupleHashEntry entry)
{
	AggStatePerGroup pergroup;
	int			transno;

	aggstate->hash_ngroups_current++;
	hash_agg_check_limits(aggstate);

	/* 无需分配或初始化每组状态              */
	if (aggstate->numtrans == 0)
		return;

	pergroup = (AggStatePerGroup)
		MemoryContextAlloc(hashtable->tablecxt,
						   sizeof(AggStatePerGroupData) * aggstate->numtrans);

	entry->additional = pergroup;

	/*
	 * 为新元组组初始化聚合，lookup_hash_entries() 已经   
	 * 选择好了相关的分组集。              
	 */
	for (transno = 0; transno < aggstate->numtrans; transno++)
	{
		AggStatePerTrans pertrans = &aggstate->pertrans[transno];
		AggStatePerGroup pergroupstate = &pergroup[transno];

		initialize_aggregate(aggstate, pertrans, pergroupstate);
	}
}

/*
 * 在所有做哈希的分组集中查找当前元组的哈希条目。  
 *
 * 注意 lookup_hash_entry 可能重置 tmpcontext。      
 *
 * 如果我们处于"溢出模式"，有些条目可能仍为 NULL。同 
 * 一个元组对每个分组集都属于不同的组，所以可能对某   
 * 个分组集匹配到内存中已有的组，而对另一分组集则匹 
 * 配到内存中没有的组。处于"溢出模式"时，元组会在未   
 * 匹配内存组的分组集上溢出。  
 *
 * 注意：同一个元组可能为多个不同分组集被溢出。这   
 * 看似浪费，但实际上是一种权衡：如果我们为多个分组
 * 集多次溢出同一个元组，即可对每个分组集分别分区，  
 * 从而使哈希表的重新填充变得非常                     
 * 高效。 
 */
static void
lookup_hash_entries(AggState *aggstate)
{
	AggStatePerGroup *pergroup = aggstate->hash_pergroup;
	TupleTableSlot *outerslot = aggstate->tmpcontext->ecxt_outertuple;
	int			setno;

	for (setno = 0; setno < aggstate->num_hashes; setno++)
	{
		AggStatePerHash perhash = &aggstate->perhash[setno];
		TupleHashTable hashtable = perhash->hashtable;
		TupleTableSlot *hashslot = perhash->hashslot;
		TupleHashEntry entry;
		uint32		hash;
		bool		isnew = false;
		bool	   *p_isnew;

		/* 若哈希表已溢出，则不再创建新条目        */
		p_isnew = aggstate->hash_spill_mode ? NULL : &isnew;

		select_current_set(aggstate, setno, true);
		prepare_hash_slot(perhash,
						  outerslot,
						  hashslot);

		entry = LookupTupleHashEntry(hashtable, hashslot,
									 p_isnew, &hash);

		if (entry != NULL)
		{
			if (isnew)
				initialize_hash_entry(aggstate, hashtable, entry);
			pergroup[setno] = entry->additional;
		}
		else
		{
			HashAggSpill *spill = &aggstate->hash_spills[setno];
			TupleTableSlot *slot = aggstate->tmpcontext->ecxt_outertuple;

			if (spill->partitions == NULL)
				hashagg_spill_init(spill, aggstate->hash_tapeset, 0,
								   perhash->aggnode->numGroups,
								   aggstate->hashentrysize);

			hashagg_spill_tuple(aggstate, spill, slot, hash);
			pergroup[setno] = NULL;
		}
	}
}

/*
 * ExecAgg -
 *
 *	  ExecAgg 从外层子计划接收元组，并对节点的目标    
 *	  列表或 qual 中出现的每个聚合函数使用点（Aggref  
 *	  节点），对合适属性进行聚合。要聚合的元组数   
 *	  量取决于分组聚合还是普通聚合。在分组聚合   
 *	  中，我们为各组产出一个结果行；在普通聚合中，整
 *	  个查询只有一个结果行。任何情况下，各聚合的值  
 *	  都存储在表达式上下文中，供 ExecProject 对             
 *	  结果元组进行求值时加以                                     
 *	  使用。        
 */
static TupleTableSlot *
ExecAgg(PlanState *pstate)
{
	AggState   *node = castNode(AggState, pstate);
	TupleTableSlot *result = NULL;

	CHECK_FOR_INTERRUPTS();

	if (!node->agg_done)
	{
		/* 根据策略进行分发   */
		switch (node->phase->aggstrategy)
		{
			case AGG_HASHED:
				if (!node->table_filled)
					agg_fill_hash_table(node);
				/* FALLTHROUGH */
			case AGG_MIXED:
				result = agg_retrieve_hash_table(node);
				break;
			case AGG_PLAIN:
			case AGG_SORTED:
				result = agg_retrieve_direct(node);
				break;
		}

		if (!TupIsNull(result))
			return result;
	}

	return NULL;
}

/*
 * 非哈希情形的 ExecAgg 
 */
static TupleTableSlot *
agg_retrieve_direct(AggState *aggstate)
{
	Agg		   *node = aggstate->phase->aggnode;
	ExprContext *econtext;
	ExprContext *tmpcontext;
	AggStatePerAgg peragg;
	AggStatePerGroup *pergroups;
	TupleTableSlot *outerslot;
	TupleTableSlot *firstSlot;
	TupleTableSlot *result;
	bool		hasGroupingSets = aggstate->phase->numsets > 0;
	int			numGroupingSets = Max(aggstate->phase->numsets, 1);
	int			currentSet;
	int			nextSetSize;
	int			numReset;
	int			i;

	/*
	 * 取得节点状态信息
	 *
	 * econtext 是每个输出元组的表达式上下文
	 *
	 * tmpcontext 为每个输入元组表达式上下文  
	 */
	econtext = aggstate->ss.ps.ps_ExprContext;
	tmpcontext = aggstate->tmpcontext;

	peragg = aggstate->peragg;
	pergroups = aggstate->pergroups;
	firstSlot = aggstate->ss.ss_ScanTupleSlot;

	/*
	 * 我们循环检索各组，直到找到匹配       
	 * aggstate->ss.ps.qual
	 *
	 * 对分组集，我们有这样的不变式：aggstate->projected_set 
	 * 要么为 -1（初次调用），要么为刚完成的组在 
	 * gset_lengths 中的下标（从 0 开始），无论是投影出了 
	 * 一行还是因 qual 被丢弃）。 
	 */
	while (!aggstate->agg_done)
	{
		/*
		 * 为每组清空每输出元组上下文与 aggcontext（     
		 * 含旧组的任何按引用传递过渡值）。一些聚合     
		 * 函数把工作状态存于子上下文，这些上下文则  
		 * 会自动重置，无需我们做任何                          
		 * 特殊处理。     
		 *
		 * 用 ReScanExprContext 而不是 ResetExprContext，因为         
		 * 希望已注册的关闭回调被调用。这样聚合函数
		 * 能确保自己清理掉所有非内存                        
		 * 资源。 
		 */
		ReScanExprContext(econtext);

		/*
		 * 确定在此边界处有多少个分组集需要重置。          
		 */
		if (aggstate->projected_set >= 0 &&
			aggstate->projected_set < numGroupingSets)
			numReset = aggstate->projected_set + 1;
		else
			numReset = numGroupingSets;

		/*
		 * numReset 在阶段边界处会改变，但没关系；我们      
		 * 要重置的是 _this_ 阶段所用的上下文，稍后在可能 
		 * 改换阶段后，为 _new_ 阶段初始化正确数量的       
		 * 聚合。   
		 */

		for (i = 0; i < numReset; i++)
		{
			ReScanExprContext(aggstate->aggcontexts[i]);
		}

		/*
		 * 检查输入是否已完备，以及本阶段是否没有可投影
		 * 的组；转入下一阶段或标记为完成。  
		 */
		if (aggstate->input_done == true &&
			aggstate->projected_set >= (numGroupingSets - 1))
		{
			if (aggstate->current_phase < aggstate->numphases - 1)
			{
				initialize_phase(aggstate, aggstate->current_phase + 1);
				aggstate->input_done = false;
				aggstate->projected_set = -1;
				numGroupingSets = Max(aggstate->phase->numsets, 1);
				node = aggstate->phase->aggnode;
				numReset = numGroupingSets;
			}
			else if (aggstate->aggstrategy == AGG_MIXED)
			{
				/*
				 * 混合模式；已输出全部分组内容，而且    
				 * 哈希表已满，因此改为输出它们。  
				 */
				initialize_phase(aggstate, 0);
				aggstate->table_filled = true;
				ResetTupleHashIterator(aggstate->perhash[0].hashtable,
									   &aggstate->perhash[0].hashiter);
				select_current_set(aggstate, 0, true);
				return agg_retrieve_hash_table(aggstate);
			}
			else
			{
				aggstate->agg_done = true;
				break;
			}
		}

		/*
		 * 取最后已投影分组集之后，下一个分组集的列数  
		 * （若有）。这就是用来判断是否也到达该组边界的 
		 * 所要比较的列数。                       
		 */
		if (aggstate->projected_set >= 0 &&
			aggstate->projected_set < (numGroupingSets - 1))
			nextSetSize = aggstate->phase->gset_lengths[aggstate->projected_set + 1];
		else
			nextSetSize = 0;

		/*----------
		 * 如果当前分组集的子组存在，就投影它。            
		 *
		 * 何时出现新组：  
		 *	- 输入已耗尽但尚未投影所有分组集             
		 *	  （见上文）
		 * OR
		 *	  - 已投影的行并非来自最后一个分组                
		 *		集
		 *	  AND
		 *	  - 下个分组集至少有一个分组列（因空分组集    
		 *		只有在输入耗尽后才投影一次）               
		 *	  AND
		 *	  - 前一行与待定行于下一个分组集分组列上存在
		 *		差异。               
		 *----------
		 */
		tmpcontext->ecxt_innertuple = econtext->ecxt_outertuple;
		if (aggstate->input_done ||
			(node->aggstrategy != AGG_PLAIN &&
			 aggstate->projected_set != -1 &&
			 aggstate->projected_set < (numGroupingSets - 1) &&
			 nextSetSize > 0 &&
			 !ExecQualAndReset(aggstate->phase->eqfunctions[nextSetSize - 1],
							   tmpcontext)))
		{
			aggstate->projected_set += 1;

			Assert(aggstate->projected_set < numGroupingSets);
			Assert(nextSetSize > 0 || aggstate->input_done);
		}
		else
		{
			/*
			 * 不再关心刚投影的组，下次投影总是        
			 * 第一个（或唯一）分组集，除非                
			 * 输入最终为空。                 
			 */
			aggstate->projected_set = 0;

			/*
			 * 如果还没有新组的首个元组，就从外层计划 
			 * 取出它。                 
			 */
			if (aggstate->grp_firstTuple == NULL)
			{
				outerslot = fetch_input_tuple(aggstate);
				if (!TupIsNull(outerslot))
				{
					/*
					 * 复制首个输入元组；我们将用它进行      
					 * 比较（分组模式下）和投影。            
					 */
					aggstate->grp_firstTuple = ExecCopySlotHeapTuple(outerslot);
				}
				else
				{
					/* 外层计划完全没有产生元组 */
					if (hasGroupingSets)
					{
						/*
						 * 如果完全没有输入，我们需要投影行
						 * 的前提是存在大小为 0 的分组集。  
						 * 注意这意味着不能有任何对未分组 
						 * Var 的引用，否则空输出槽会出           
						 * 问题。                               
						 *
						 * XXX：这已不再成立，此事目前由         
						 * finalize_aggregates()处理。
						 */
						aggstate->input_done = true;

						while (aggstate->phase->gset_lengths[aggstate->projected_set] > 0)
						{
							aggstate->projected_set += 1;
							if (aggstate->projected_set >= numGroupingSets)
							{
								/*
								 * 这里不能设 agg_done，即使输入    
								 * 已空，仍可能有更多阶段。故
								 * 我们需要重新启动整个           
								 * 外层循环。  
								 */
								break;
							}
						}

						if (aggstate->projected_set >= numGroupingSets)
							continue;
					}
					else
					{
						aggstate->agg_done = true;
						/* 如果我们在分组，也应同样不产出元组 */
						if (node->aggstrategy != AGG_PLAIN)
							return NULL;
					}
				}
			}

			/*
			 * 为新输入元组组初始化工作状态。        
			 */
			initialize_aggregates(aggstate, pergroups, numReset);

			if (aggstate->grp_firstTuple != NULL)
			{
				/*
				 * 把复制出的首个输入元组存入元组表中       
				 * 为它保留的槽位。当元组从槽中清除时，
				 * 它就会被删除。 
				 */
				ExecForceStoreHeapTuple(aggstate->grp_firstTuple,
										firstSlot, true);
				aggstate->grp_firstTuple = NULL;	/* 不要保留两个指针*/

				/* 为首次 advance_aggregates 调用准备*/
				tmpcontext->ecxt_outertuple = firstSlot;

				/*
				 * 处理每个外层计划元组，再取下一个，直到  
				 * 外层计划耗尽或越过组边界为止。             
				 */
				for (;;)
				{
					/*
					 * 只有在混合聚合的阶段 1 中，才要由     
					 * advance_aggregates 更新哈希表。    
					 */
					if (aggstate->aggstrategy == AGG_MIXED &&
						aggstate->current_phase == 1)
					{
						lookup_hash_entries(aggstate);
					}

					/* 推进聚合（或合并函数）             */
					advance_aggregates(aggstate);

					/* 每元组之后重置每输入元组上下文  */
					ResetExprContext(tmpcontext);

					outerslot = fetch_input_tuple(aggstate);
					if (TupIsNull(outerslot))
					{
						/* 外层计划没有更多元组可用*/

						/* 若构建了哈希表，则收尾所有溢出*/
						if (aggstate->aggstrategy == AGG_MIXED &&
							aggstate->current_phase == 1)
							hashagg_finish_initial_spills(aggstate);

						if (hasGroupingSets)
						{
							aggstate->input_done = true;
							break;
						}
						else
						{
							aggstate->agg_done = true;
							break;
						}
					}
					/* 下次 advance_aggregates 调用准备  */
					tmpcontext->ecxt_outertuple = outerslot;

					/*
					 * 若正在分组，则检查是否已跨过组的       
					 * 边界。
					 */
					if (node->aggstrategy != AGG_PLAIN && node->numCols > 0)
					{
						tmpcontext->ecxt_innertuple = firstSlot;
						if (!ExecQual(aggstate->phase->eqfunctions[node->numCols - 1],
									  tmpcontext))
						{
							aggstate->grp_firstTuple = ExecCopySlotHeapTuple(outerslot);
							break;
						}
					}
				}
			}

			/*
			 * 在聚合直接参数、节点 qual 和 tlist 中引用 
			 * 非聚合输入列之处，都用代表输入元组。（如果
			 * 我们未在分组，并且没有任何输入行，那么就   
			 * 会带着空的 firstSlot 到此……但若不分组，    
			 * 则不可能引用非聚合输入列，                  
			 * 所以没有问题。）                     
			 */
			econtext->ecxt_outertuple = firstSlot;
		}

		Assert(aggstate->projected_set >= 0);

		currentSet = aggstate->projected_set;

		prepare_projection_slot(aggstate, econtext->ecxt_outertuple, currentSet);

		select_current_set(aggstate, currentSet, false);

		finalize_aggregates(aggstate,
							peragg,
							pergroups[currentSet]);

		/*
		 * 如果现在没有可投影的行，就必须继续执行而非
		 * 返回空值，因为可能还有更多的组。       
		 */
		result = project_aggregates(aggstate);
		if (result)
			return result;
	}

	/* 无更多分组*/
	return NULL;
}

/*
 * ExecAgg 哈希情形：读取输入并构建哈希表   
 */
static void
agg_fill_hash_table(AggState *aggstate)
{
	TupleTableSlot *outerslot;
	ExprContext *tmpcontext = aggstate->tmpcontext;

	/*
	 * 处理每个外层计划元组，然后取下一个，直到我们  
	 * 耗尽外层计划。  
	 */
	for (;;)
	{
		outerslot = fetch_input_tuple(aggstate);
		if (TupIsNull(outerslot))
			break;

		/* 为 lookup_hash_entries/advance_aggregates 做准备  */
		tmpcontext->ecxt_outertuple = outerslot;

		/* 查找或构建哈希表项     */
		lookup_hash_entries(aggstate);

		/* 推进各聚合（或组合函数）          */
		advance_aggregates(aggstate);

		/*
		 * 每个元组之后重置每输入元组上下文，但要注意  
		 * 哈希查找亦如此。
		 */
		ResetExprContext(aggstate->tmpcontext);
	}

	/* 若有溢出则收尾   */
	hashagg_finish_initial_spills(aggstate);

	aggstate->table_filled = true;
	/* 初始化以遍历第一个哈希表    */
	select_current_set(aggstate, 0, true);
	ResetTupleHashIterator(aggstate->perhash[0].hashtable,
						   &aggstate->perhash[0].hashiter);
}

/*
 * 若哈希聚合期间溢出了数据，则重置哈希表，并重       
 * 处理一批溢出的数据。重处理完一批后，哈希表将       
 * 再度含有数据，可供消费                     
 * agg_retrieve_hash_table_in_memory() 
 *
 * 只应在内存中所有哈希表项都已终结并输出             
 * 之后才可调用。 
 *
 * 当输入已耗尽且没有更多工作要做时返回 false；          
 * 否则返回 true。  
 */
static bool
agg_refill_hash_table(AggState *aggstate)
{
	HashAggBatch *batch;
	AggStatePerHash perhash;
	HashAggSpill spill;
	LogicalTapeSet *tapeset = aggstate->hash_tapeset;
	bool		spill_initialized = false;

	if (aggstate->hash_batches == NIL)
		return false;

	/* hash_batches 是一个栈，栈顶项位于列表末尾           */
	batch = llast(aggstate->hash_batches);
	aggstate->hash_batches = list_delete_last(aggstate->hash_batches);

	hash_agg_set_limits(aggstate->hashentrysize, batch->input_card,
						batch->used_bits, &aggstate->hash_mem_limit,
						&aggstate->hash_ngroups_limit, NULL);

	/*
	 * 每个批次只处理一个分组集；把其余的设为 NULL，  
	 * 让 advance_aggregates() 知道忽略它们。这里          
	 * 不去动排序分组集所用的 pergroups，因为若之后          
	 * 重新扫描时还需要用到它们。而重编译之后，         
	 * 反正不会求值这些表达式。
	 */
	MemSet(aggstate->hash_pergroup, 0,
		   sizeof(AggStatePerGroup) * aggstate->num_hashes);

	/* 释放内存并重置哈希表    */
	ReScanExprContext(aggstate->hashcontext);
	for (int setno = 0; setno < aggstate->num_hashes; setno++)
		ResetTupleHashTable(aggstate->perhash[setno].hashtable);

	aggstate->hash_ngroups_current = 0;

	/*
	 * 在 AGG_MIXED 模式下，哈希聚合发生在阶段 1，而输出  
	 * 发生在阶段 0。因此处理批次时切换到阶段 1，        
	 * 批次完成后切回阶段 0。            
	 */
	Assert(aggstate->current_phase == 0);
	if (aggstate->phase->aggstrategy == AGG_MIXED)
	{
		aggstate->current_phase = 1;
		aggstate->phase = &aggstate->phases[aggstate->current_phase];
	}

	select_current_set(aggstate, batch->setno, true);

	perhash = &aggstate->perhash[aggstate->current_set];

	/*
	 * 溢出的元组总是以 MinimalTuple 形式读回，这可能    
	 * 与外层计划不同，所以要重新编译聚合表达式。       
	 *
	 * 我们仍需要 NULL 检查，因为我们每次只处理一个 
	 * 分组集，其余的将为 NULL。              
	 */
	hashagg_recompile_expressions(aggstate, true, true);

	for (;;)
	{
		TupleTableSlot *spillslot = aggstate->hash_spill_rslot;
		TupleTableSlot *hashslot = perhash->hashslot;
		TupleHashEntry entry;
		MinimalTuple tuple;
		uint32		hash;
		bool		isnew = false;
		bool	   *p_isnew = aggstate->hash_spill_mode ? NULL : &isnew;

		CHECK_FOR_INTERRUPTS();

		tuple = hashagg_batch_read(batch, &hash);
		if (tuple == NULL)
			break;

		ExecStoreMinimalTuple(tuple, spillslot, true);
		aggstate->tmpcontext->ecxt_outertuple = spillslot;

		prepare_hash_slot(perhash,
						  aggstate->tmpcontext->ecxt_outertuple,
						  hashslot);
		entry = LookupTupleHashEntryHash(perhash->hashtable, hashslot,
										 p_isnew, hash);

		if (entry != NULL)
		{
			if (isnew)
				initialize_hash_entry(aggstate, perhash->hashtable, entry);
			aggstate->hash_pergroup[batch->setno] = entry->additional;
			advance_aggregates(aggstate);
		}
		else
		{
			if (!spill_initialized)
			{
				/*
				 * 在实际需要之前不要初始化溢出结构，以免
				 * 分配永远不会被用到的磁带。            
				 */
				spill_initialized = true;
				hashagg_spill_init(&spill, tapeset, batch->used_bits,
								   batch->input_card, aggstate->hashentrysize);
			}
			/* 没有内存容纳新组，溢出*/
			hashagg_spill_tuple(aggstate, &spill, spillslot, hash);

			aggstate->hash_pergroup[batch->setno] = NULL;
		}

		/*
		 * 每个元组之后重置每输入元组上下文，但要注意  
		 * 哈希查找亦如此。
		 */
		ResetExprContext(aggstate->tmpcontext);
	}

	LogicalTapeClose(batch->input_tape);

	/* 切回阶段 0         */
	aggstate->current_phase = 0;
	aggstate->phase = &aggstate->phases[aggstate->current_phase];

	if (spill_initialized)
	{
		hashagg_spill_finish(aggstate, &spill, batch->setno);
		hash_agg_update_metrics(aggstate, true, spill.npartitions);
	}
	else
		hash_agg_update_metrics(aggstate, true, 0);

	aggstate->hash_spill_mode = false;

	/* 准备开始遍历第一个哈希表 */
	select_current_set(aggstate, batch->setno, true);
	ResetTupleHashIterator(aggstate->perhash[batch->setno].hashtable,
						   &aggstate->perhash[batch->setno].hashiter);

	pfree(batch);

	return true;
}

/*
 * ExecAgg 哈希情形：从哈希表中取出分组        
 *
 * 在内存元组耗尽后，还会尝试用之前溢出的元组           
 * 重新填充哈希表。只有当所有内存与溢出元组都     
 * 耗尽才返回 NULL。      
 */
static TupleTableSlot *
agg_retrieve_hash_table(AggState *aggstate)
{
	TupleTableSlot *result = NULL;

	while (result == NULL)
	{
		result = agg_retrieve_hash_table_in_memory(aggstate);
		if (result == NULL)
		{
			if (!agg_refill_hash_table(aggstate))
			{
				aggstate->agg_done = true;
				break;
			}
		}
	}

	return result;
}

/*
 * 不考虑任何溢出元组，从内存中的哈希表取出              
 * 各分组。   
 */
static TupleTableSlot *
agg_retrieve_hash_table_in_memory(AggState *aggstate)
{
	ExprContext *econtext;
	AggStatePerAgg peragg;
	AggStatePerGroup pergroup;
	TupleHashEntryData *entry;
	TupleTableSlot *firstSlot;
	TupleTableSlot *result;
	AggStatePerHash perhash;

	/*
	 * 取得节点状态信息 
	 *
	 * econtext 是每输出元组的表达式上下文。 
	 */
	econtext = aggstate->ss.ps.ps_ExprContext;
	peragg = aggstate->peragg;
	firstSlot = aggstate->ss.ss_ScanTupleSlot;

	/*
	 * 注意 perhash（以及通过它访问的东西）可能          
	 * 在循环中改变，因为我们会切换分组集。     
	 */
	perhash = &aggstate->perhash[aggstate->current_set];

	/*
	 * 我们循环取出分组，直到找到一个满足   
	 * aggstate->ss.ps.qual
	 */
	for (;;)
	{
		TupleTableSlot *hashslot = perhash->hashslot;
		int			i;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 查找哈希表中的下一个表项 
		 */
		entry = ScanTupleHashTable(perhash->hashtable, &perhash->hashiter);
		if (entry == NULL)
		{
			int			nextset = aggstate->current_set + 1;

			if (nextset < aggstate->num_hashes)
			{
				/*
				 * 切到下一个分组集，重新初始化，重启循    
				 * 环  
				 */
				select_current_set(aggstate, nextset, true);

				perhash = &aggstate->perhash[aggstate->current_set];

				ResetTupleHashIterator(perhash->hashtable, &perhash->hashiter);

				continue;
			}
			else
			{
				return NULL;
			}
		}

		/*
		 * 为每个分组清空每输出元组上下文    
		 *
		 * 我们有意不在这里使用 ReScanExprContext；若任何       
		 * 聚合注册了关闭回调，此时还不能调用它们，因为 
		 * 该聚合可能尚未完成。  
		 */
		ResetExprContext(econtext);

		/*
		 * 把代表元组转换回具有正确列的元组形        
		 * 式。  
		 */
		ExecStoreMinimalTuple(entry->firstTuple, hashslot, false);
		slot_getallattrs(hashslot);

		ExecClearTuple(firstSlot);
		memset(firstSlot->tts_isnull, true,
			   firstSlot->tts_tupleDescriptor->natts * sizeof(bool));

		for (i = 0; i < perhash->numhashGrpCols; i++)
		{
			int			varNumber = perhash->hashGrpColIdxInput[i] - 1;

			firstSlot->tts_values[varNumber] = hashslot->tts_values[i];
			firstSlot->tts_isnull[varNumber] = hashslot->tts_isnull[i];
		}
		ExecStoreVirtualTuple(firstSlot);

		pergroup = (AggStatePerGroup) entry->additional;

		/*
		 * 对 qual 和 tlist 中非聚合输入列的引用，均 
		 * 使用代表输入元组。                        
		 */
		econtext->ecxt_outertuple = firstSlot;

		prepare_projection_slot(aggstate,
								econtext->ecxt_outertuple,
								aggstate->current_set);

		finalize_aggregates(aggstate, peragg, pergroup);

		result = project_aggregates(aggstate);
		if (result)
			return result;
	}

	/* 无更多分组*/
	return NULL;
}

/*
 * hashagg_spill_init
 *
 * 在我们确定有必要溢出之后调用。选择要创建的分区数 
 * 并初始化它们。                         
 */
static void
hashagg_spill_init(HashAggSpill *spill, LogicalTapeSet *tapeset, int used_bits,
				   double input_groups, double hashentrysize)
{
	int			npartitions;
	int			partition_bits;

	npartitions = hash_choose_num_partitions(input_groups, hashentrysize,
											 used_bits, &partition_bits);

	spill->partitions = palloc0(sizeof(LogicalTape *) * npartitions);
	spill->ntuples = palloc0(sizeof(int64) * npartitions);
	spill->hll_card = palloc0(sizeof(hyperLogLogState) * npartitions);

	for (int i = 0; i < npartitions; i++)
		spill->partitions[i] = LogicalTapeCreate(tapeset);

	spill->shift = 32 - used_bits - partition_bits;
	spill->mask = (npartitions - 1) << spill->shift;
	spill->npartitions = npartitions;

	for (int i = 0; i < npartitions; i++)
		initHyperLogLog(&spill->hll_card[i], HASHAGG_HLL_BIT_WIDTH);
}

/*
 * hashagg_spill_tuple
 *
 * 哈希表中没有空间容纳新组。先保存到相应分区，         
 * 备用。 
 */
static Size
hashagg_spill_tuple(AggState *aggstate, HashAggSpill *spill,
					TupleTableSlot *inputslot, uint32 hash)
{
	TupleTableSlot *spillslot;
	int			partition;
	MinimalTuple tuple;
	LogicalTape *tape;
	int			total_written = 0;
	bool		shouldFree;

	Assert(spill->partitions != NULL);

	/* 只溢出我们真正需要的属性        */
	if (!aggstate->all_cols_needed)
	{
		spillslot = aggstate->hash_spill_wslot;
		slot_getsomeattrs(inputslot, aggstate->max_colno_needed);
		ExecClearTuple(spillslot);
		for (int i = 0; i < spillslot->tts_tupleDescriptor->natts; i++)
		{
			if (bms_is_member(i + 1, aggstate->colnos_needed))
			{
				spillslot->tts_values[i] = inputslot->tts_values[i];
				spillslot->tts_isnull[i] = inputslot->tts_isnull[i];
			}
			else
				spillslot->tts_isnull[i] = true;
		}
		ExecStoreVirtualTuple(spillslot);
	}
	else
		spillslot = inputslot;

	tuple = ExecFetchSlotMinimalTuple(spillslot, &shouldFree);

	partition = (hash & spill->mask) >> spill->shift;
	spill->ntuples[partition]++;

	/*
	 * 所有去往给定分区的哈希值都有一些共同的位， 
	 * 这会导致 HLL 基数估计不准。对哈希值再做哈希，   
	 * 以获得更均匀的分布。  
	 */
	addHyperLogLog(&spill->hll_card[partition], hash_bytes_uint32(hash));

	tape = spill->partitions[partition];

	LogicalTapeWrite(tape, &hash, sizeof(uint32));
	total_written += sizeof(uint32);

	LogicalTapeWrite(tape, tuple, tuple->t_len);
	total_written += tuple->t_len;

	if (shouldFree)
		pfree(tuple);

	return total_written;
}

/*
 * hashagg_batch_new
 *
 * 构造一个 HashAggBatch 条目，它代表待执行的一轮             
 * HashAgg 
 */
static HashAggBatch *
hashagg_batch_new(LogicalTape *input_tape, int setno,
				  int64 input_tuples, double input_card, int used_bits)
{
	HashAggBatch *batch = palloc0(sizeof(HashAggBatch));

	batch->setno = setno;
	batch->used_bits = used_bits;
	batch->input_tape = input_tape;
	batch->input_tuples = input_tuples;
	batch->input_card = input_card;

	return batch;
}

/*
 * hashagg_batch_read
 * 		从批次的磁带读取下一元组；没有则返回 NULL。   
 */
static MinimalTuple
hashagg_batch_read(HashAggBatch *batch, uint32 *hashp)
{
	LogicalTape *tape = batch->input_tape;
	MinimalTuple tuple;
	uint32		t_len;
	size_t		nread;
	uint32		hash;

	nread = LogicalTapeRead(tape, &hash, sizeof(uint32));
	if (nread == 0)
		return NULL;
	if (nread != sizeof(uint32))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg_internal("unexpected EOF for tape %p: requested %zu bytes, read %zu bytes",
								 tape, sizeof(uint32), nread)));
	if (hashp != NULL)
		*hashp = hash;

	nread = LogicalTapeRead(tape, &t_len, sizeof(t_len));
	if (nread != sizeof(uint32))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg_internal("unexpected EOF for tape %p: requested %zu bytes, read %zu bytes",
								 tape, sizeof(uint32), nread)));

	tuple = (MinimalTuple) palloc(t_len);
	tuple->t_len = t_len;

	nread = LogicalTapeRead(tape,
							(char *) tuple + sizeof(uint32),
							t_len - sizeof(uint32));
	if (nread != t_len - sizeof(uint32))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg_internal("unexpected EOF for tape %p: requested %zu bytes, read %zu bytes",
								 tape, t_len - sizeof(uint32), nread)));

	return tuple;
}

/*
 * hashagg_finish_initial_spills
 *
 * 在一个 HashAggBatch 处理完之后，它可能已把元组溢     
 * 写到磁盘上。若是这样，就把溢出的分区变成必须稍    
 * 后执行。
 */
static void
hashagg_finish_initial_spills(AggState *aggstate)
{
	int			setno;
	int			total_npartitions = 0;

	if (aggstate->hash_spills != NULL)
	{
		for (setno = 0; setno < aggstate->num_hashes; setno++)
		{
			HashAggSpill *spill = &aggstate->hash_spills[setno];

			total_npartitions += spill->npartitions;
			hashagg_spill_finish(aggstate, spill, setno);
		}

		/*
		 * 我们不再处理来自外层计划的元组；只处理 
		 * 溢出元组的批次。最初的溢出结构                     
		 * 已不再需要。   
		 */
		pfree(aggstate->hash_spills);
		aggstate->hash_spills = NULL;
	}

	hash_agg_update_metrics(aggstate, false, total_npartitions);
	aggstate->hash_spill_mode = false;
}

/*
 * hashagg_spill_finish
 *
 * 把溢出分区转换成新批次。        
 */
static void
hashagg_spill_finish(AggState *aggstate, HashAggSpill *spill, int setno)
{
	int			i;
	int			used_bits = 32 - spill->shift;

	if (spill->npartitions == 0)
		return;					/* 未溢出    */

	for (i = 0; i < spill->npartitions; i++)
	{
		LogicalTape *tape = spill->partitions[i];
		HashAggBatch *new_batch;
		double		cardinality;

		/* 若分区为空，则不创建新的工作批次            */
		if (spill->ntuples[i] == 0)
			continue;

		cardinality = estimateHyperLogLog(&spill->hll_card[i]);
		freeHyperLogLog(&spill->hll_card[i]);

		/* 不使用时进行回卷会释放缓冲区  */
		LogicalTapeRewindForRead(tape, HASHAGG_READ_BUFFER_SIZE);

		new_batch = hashagg_batch_new(tape, setno,
									  spill->ntuples[i], cardinality,
									  used_bits);
		aggstate->hash_batches = lappend(aggstate->hash_batches, new_batch);
		aggstate->hash_batches_used++;
	}

	pfree(spill->ntuples);
	pfree(spill->hll_card);
	pfree(spill->partitions);
}

/*
 * 释放与溢出 HashAgg 相关的资源。  
 */
static void
hashagg_reset_spill_state(AggState *aggstate)
{
	/* 释放初始阶段的溢出   */
	if (aggstate->hash_spills != NULL)
	{
		int			setno;

		for (setno = 0; setno < aggstate->num_hashes; setno++)
		{
			HashAggSpill *spill = &aggstate->hash_spills[setno];

			pfree(spill->ntuples);
			pfree(spill->partitions);
		}
		pfree(aggstate->hash_spills);
		aggstate->hash_spills = NULL;
	}

	/* 释放批次 */
	list_free_deep(aggstate->hash_batches);
	aggstate->hash_batches = NIL;

	/* 关闭磁带集*/
	if (aggstate->hash_tapeset != NULL)
	{
		LogicalTapeSetClose(aggstate->hash_tapeset);
		aggstate->hash_tapeset = NULL;
	}
}


/* -----------------
 * ExecInitAgg
 *
 *	为规划器生成的 agg 节点创建运行时信息，         
 *	并初始化其外层子树。            
 *
 * -----------------
 */
AggState *
ExecInitAgg(Agg *node, EState *estate, int eflags)
{
	AggState   *aggstate;
	AggStatePerAgg peraggs;
	AggStatePerTrans pertransstates;
	AggStatePerGroup *pergroups;
	Plan	   *outerPlan;
	ExprContext *econtext;
	TupleDesc	scanDesc;
	int			max_aggno;
	int			max_transno;
	int			numaggrefs;
	int			numaggs;
	int			numtrans;
	int			phase;
	int			phaseidx;
	ListCell   *l;
	Bitmapset  *all_grouped_cols = NULL;
	int			numGroupingSets = 1;
	int			numPhases;
	int			numHashes;
	int			i = 0;
	int			j = 0;
	bool		use_hashing = (node->aggstrategy == AGG_HASHED ||
							   node->aggstrategy == AGG_MIXED);

	/* 检查不受支持的标志 */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 创建状态结构    
	 */
	aggstate = makeNode(AggState);
	aggstate->ss.ps.plan = (Plan *) node;
	aggstate->ss.ps.state = estate;
	aggstate->ss.ps.ExecProcNode = ExecAgg;

	aggstate->aggs = NIL;
	aggstate->numaggs = 0;
	aggstate->numtrans = 0;
	aggstate->aggstrategy = node->aggstrategy;
	aggstate->aggsplit = node->aggsplit;
	aggstate->maxsets = 0;
	aggstate->projected_set = -1;
	aggstate->current_set = 0;
	aggstate->peragg = NULL;
	aggstate->pertrans = NULL;
	aggstate->curperagg = NULL;
	aggstate->curpertrans = NULL;
	aggstate->input_done = false;
	aggstate->agg_done = false;
	aggstate->pergroups = NULL;
	aggstate->grp_firstTuple = NULL;
	aggstate->sort_in = NULL;
	aggstate->sort_out = NULL;

	/*
	 * phases[0] 总是存在，排序/普通模式下是占位  
	 */
	numPhases = (use_hashing ? 1 : 2);
	numHashes = (use_hashing ? 1 : 0);

	/*
	 * 计算任何阶段中分组集数量的最大值；这决定了 
	 * 某些内存分配的大小。同时还要计算阶段的数量，    
	 * 因为所有 hashed/mixed 节点都只贡献单个阶段。            
	 */
	if (node->groupingSets)
	{
		numGroupingSets = list_length(node->groupingSets);

		foreach(l, node->chain)
		{
			Agg		   *agg = lfirst(l);

			numGroupingSets = Max(numGroupingSets,
								  list_length(agg->groupingSets));

			/*
			 * 额外的 AGG_HASHED 聚合变成阶段 0 的一部分， 
			 * 其余另加一阶段。  
			 */
			if (agg->aggstrategy != AGG_HASHED)
				++numPhases;
			else
				++numHashes;
		}
	}

	aggstate->maxsets = numGroupingSets;
	aggstate->numphases = numPhases;

	aggstate->aggcontexts = (ExprContext **)
		palloc0(sizeof(ExprContext *) * numGroupingSets);

	/*
	 * 创建表达式上下文。需要三个或更多：一个  
	 * 用于每输入元组处理，一个用于每输出元组处理，  
	 * 一个用于所有哈希表，一个用于每个分组集。每个   
	 * 分组集 ExprContext（aggcontext）的每元组内存          
	 * 上下文取代了以前用于存放过渡值的独立内存上下     
	 * 文。用 ExecAssignExprContext() 构建它们算是                
	 * 小作弊。
	 *
	 * 注意：哪些内容存于 aggcontext、哪些存于常规的       
	 * 每查询内存上下文，由简单决定驱动：我们要  
	 * 在分组边界处（非哈希时）重置 aggcontext，并          
	 * 亦在 ExecReScanAgg 中重置以收回不再需要的空间。 
	 */
	ExecAssignExprContext(estate, &aggstate->ss.ps);
	aggstate->tmpcontext = aggstate->ss.ps.ps_ExprContext;

	for (i = 0; i < numGroupingSets; ++i)
	{
		ExecAssignExprContext(estate, &aggstate->ss.ps);
		aggstate->aggcontexts[i] = aggstate->ss.ps.ps_ExprContext;
	}

	if (use_hashing)
		aggstate->hashcontext = CreateWorkExprContext(estate);

	ExecAssignExprContext(estate, &aggstate->ss.ps);

	/*
	 * 初始化子节点。  
	 *
	 * 若我们在做哈希聚合，那么子计划不需要高效地       
	 * 处理 REWIND；参见 ExecReScanAgg。         
	 */
	if (node->aggstrategy == AGG_HASHED)
		eflags &= ~EXEC_FLAG_REWIND;
	outerPlan = outerPlan(node);
	outerPlanState(aggstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * 初始化源元组类型。  
	 */
	aggstate->ss.ps.outerops =
		ExecGetResultSlotOps(outerPlanState(&aggstate->ss),
							 &aggstate->ss.ps.outeropsfixed);
	aggstate->ss.ps.outeropsset = true;

	ExecCreateScanSlotFromOuterPlan(estate, &aggstate->ss,
									aggstate->ss.ps.outerops);
	scanDesc = aggstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

	/*
	 * 如果阶段数超过两个（包括潜在的占位阶段           
	 * 0），输入会用 tuplesort 重排。需要一个槽。        
	 */
	if (numPhases > 2)
	{
		aggstate->sort_slot = ExecInitExtraTupleSlot(estate, scanDesc,
													 &TTSOpsMinimalTuple);

		/*
		 * tuplesort 的输出与外层子节点的输出可能不使用   
		 * 同一种类型的槽。多数情况下子节点会是 Sort，   
		 * 因而返回 TTSOpsMinimalTuple 类型的槽——但若           
		 * 输入因索引而预排序，那它就可能是另一种类       
		 * 型的槽。             
		 *
		 * XXX: 为了效率，最好另外/同时为各阶段生成   
		 * 带有相应 outerops* 设置的表达式——对于每组      
		 * 行数多的聚合，deform 往往是瓶颈。如果       
		 * 有多个排序，我们知道除第一个外都用          
		 * TTSOpsMinimalTuple（经由 nodeAgg.c 内部的                  
		 * tuplesort）。                   
		 */
		if (aggstate->ss.ps.outeropsfixed &&
			aggstate->ss.ps.outerops != &TTSOpsMinimalTuple)
			aggstate->ss.ps.outeropsfixed = false;
	}

	/*
	 * 初始化结果类型、槽和投影。     
	 */
	ExecInitResultTupleSlotTL(&aggstate->ss.ps, &TTSOpsVirtual);
	ExecAssignProjectionInfo(&aggstate->ss.ps, NULL);

	/*
	 * 初始化子表达式       
	 *
	 * 我们预期解析器已检查过：任何聚合的参数中都不 
	 * 会包含其他聚合调用（为保险起见，我们在初始化     
	 * 计划节点时会再验证一次）。这在 SQL 语义下     
	 * 毫无意义，而且规范也禁止它。既然如此，我们   
	 * 不必操心以任何特定顺序求聚合的值。                   
	 *
	 * 注意：execExpr.c 会找出 Aggrefs 并加入 aggstate->aggs。      
	 * qual 中的 Aggrefs 在这里找到；targetlist 中的则在上面    
	 * ExecAssignProjectionInfo 期间找到  
	 */
	aggstate->ss.ps.qual =
		ExecInitQual(node->plan.qual, (PlanState *) aggstate);

	/*
	 * 现在应已找出 targetlist 和 qual 中的所有 Aggrefs。   
	 */
	numaggrefs = list_length(aggstate->aggs);
	max_aggno = -1;
	max_transno = -1;
	foreach(l, aggstate->aggs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(l);

		max_aggno = Max(max_aggno, aggref->aggno);
		max_transno = Max(max_transno, aggref->aggtransno);
	}
	numaggs = max_aggno + 1;
	numtrans = max_transno + 1;

	/*
	 * 为每个阶段准备分组集数据和比较函数的 fmgr       
	 * 查找数据。顺带累加 all_grouped_cols。            
	 */
	aggstate->phases = palloc0(numPhases * sizeof(AggStatePerPhaseData));

	aggstate->num_hashes = numHashes;
	if (numHashes)
	{
		aggstate->perhash = palloc0(sizeof(AggStatePerHashData) * numHashes);
		aggstate->phases[0].numsets = 0;
		aggstate->phases[0].gset_lengths = palloc(numHashes * sizeof(int));
		aggstate->phases[0].grouped_cols = palloc(numHashes * sizeof(Bitmapset *));
	}

	phase = 0;
	for (phaseidx = 0; phaseidx <= list_length(node->chain); ++phaseidx)
	{
		Agg		   *aggnode;
		Sort	   *sortnode;

		if (phaseidx > 0)
		{
			aggnode = list_nth_node(Agg, node->chain, phaseidx - 1);
			sortnode = castNode(Sort, outerPlan(aggnode));
		}
		else
		{
			aggnode = node;
			sortnode = NULL;
		}

		Assert(phase <= 1 || sortnode);

		if (aggnode->aggstrategy == AGG_HASHED
			|| aggnode->aggstrategy == AGG_MIXED)
		{
			AggStatePerPhase phasedata = &aggstate->phases[0];
			AggStatePerHash perhash;
			Bitmapset  *cols = NULL;

			Assert(phase == 0);
			i = phasedata->numsets++;
			perhash = &aggstate->perhash[i];

			/* 哈希情形下阶段 0 总是指向"真正的" Agg      */
			phasedata->aggnode = node;
			phasedata->aggstrategy = node->aggstrategy;

			/* 但代表这个哈希的实际 Agg 节点保存在这里     */
			perhash->aggnode = aggnode;

			phasedata->gset_lengths[i] = perhash->numCols = aggnode->numCols;

			for (j = 0; j < aggnode->numCols; ++j)
				cols = bms_add_member(cols, aggnode->grpColIdx[j]);

			phasedata->grouped_cols[i] = cols;

			all_grouped_cols = bms_add_members(all_grouped_cols, cols);
			continue;
		}
		else
		{
			AggStatePerPhase phasedata = &aggstate->phases[++phase];
			int			num_sets;

			phasedata->numsets = num_sets = list_length(aggnode->groupingSets);

			if (num_sets)
			{
				phasedata->gset_lengths = palloc(num_sets * sizeof(int));
				phasedata->grouped_cols = palloc(num_sets * sizeof(Bitmapset *));

				i = 0;
				foreach(l, aggnode->groupingSets)
				{
					int			current_length = list_length(lfirst(l));
					Bitmapset  *cols = NULL;

					/* 规划器强制这是正确的    */
					for (j = 0; j < current_length; ++j)
						cols = bms_add_member(cols, aggnode->grpColIdx[j]);

					phasedata->grouped_cols[i] = cols;
					phasedata->gset_lengths[i] = current_length;

					++i;
				}

				all_grouped_cols = bms_add_members(all_grouped_cols,
												   phasedata->grouped_cols[0]);
			}
			else
			{
				Assert(phaseidx == 0);

				phasedata->gset_lengths = NULL;
				phasedata->grouped_cols = NULL;
			}

			/*
			 * 若在分组，则为内层循环预计算 fmgr 查找数据。
			 */
			if (aggnode->aggstrategy == AGG_SORTED)
			{
				/*
				 * 为每个需要比较的列子集构建一个单独的   
				 * 函数。           
				 */
				phasedata->eqfunctions =
					(ExprState **) palloc0(aggnode->numCols * sizeof(ExprState *));

				/* 对每个分组集    */
				for (int k = 0; k < phasedata->numsets; k++)
				{
					int			length = phasedata->gset_lengths[k];

					/* 对空分组集无需做任何事    */
					if (length == 0)
						continue;

					/* 若已有同长度的，直接用它即可     */
					if (phasedata->eqfunctions[length - 1] != NULL)
						continue;

					phasedata->eqfunctions[length - 1] =
						execTuplesMatchPrepare(scanDesc,
											   length,
											   aggnode->grpColIdx,
											   aggnode->grpOperators,
											   aggnode->grpCollations,
											   (PlanState *) aggstate);
				}

				/* 以及所有分组列，除非已计算过           */
				if (aggnode->numCols > 0 &&
					phasedata->eqfunctions[aggnode->numCols - 1] == NULL)
				{
					phasedata->eqfunctions[aggnode->numCols - 1] =
						execTuplesMatchPrepare(scanDesc,
											   aggnode->numCols,
											   aggnode->grpColIdx,
											   aggnode->grpOperators,
											   aggnode->grpCollations,
											   (PlanState *) aggstate);
				}
			}

			phasedata->aggnode = aggnode;
			phasedata->aggstrategy = aggnode->aggstrategy;
			phasedata->sortnode = sortnode;
		}
	}

	/*
	 * 把 all_grouped_cols 转为降序列表。          
	 */
	i = -1;
	while ((i = bms_next_member(all_grouped_cols, i)) >= 0)
		aggstate->all_grouped_cols = lcons_int(i, aggstate->all_grouped_cols);

	/*
	 * 在输出表达式上下文中建立聚合结果存储，同时     
	 * 分配我私有的每聚合工作存储    
	 */
	econtext = aggstate->ss.ps.ps_ExprContext;
	econtext->ecxt_aggvalues = (Datum *) palloc0(sizeof(Datum) * numaggs);
	econtext->ecxt_aggnulls = (bool *) palloc0(sizeof(bool) * numaggs);

	peraggs = (AggStatePerAgg) palloc0(sizeof(AggStatePerAggData) * numaggs);
	pertransstates = (AggStatePerTrans) palloc0(sizeof(AggStatePerTransData) * numtrans);

	aggstate->peragg = peraggs;
	aggstate->pertrans = pertransstates;


	aggstate->all_pergroups =
		(AggStatePerGroup *) palloc0(sizeof(AggStatePerGroup)
									 * (numGroupingSets + numHashes));
	pergroups = aggstate->all_pergroups;

	if (node->aggstrategy != AGG_HASHED)
	{
		for (i = 0; i < numGroupingSets; i++)
		{
			pergroups[i] = (AggStatePerGroup) palloc0(sizeof(AggStatePerGroupData)
													  * numaggs);
		}

		aggstate->pergroups = pergroups;
		pergroups += numGroupingSets;
	}

	/*
	 * 哈希只能出现在初始阶段。         
	 */
	if (use_hashing)
	{
		Plan	   *outerplan = outerPlan(node);
		uint64		totalGroups = 0;

		aggstate->hash_metacxt = AllocSetContextCreate(aggstate->ss.ps.state->es_query_cxt,
													   "HashAgg meta context",
													   ALLOCSET_DEFAULT_SIZES);
		aggstate->hash_spill_rslot = ExecInitExtraTupleSlot(estate, scanDesc,
															&TTSOpsMinimalTuple);
		aggstate->hash_spill_wslot = ExecInitExtraTupleSlot(estate, scanDesc,
															&TTSOpsVirtual);

		/* 这是一个指针数组，不是结构数组*/
		aggstate->hash_pergroup = pergroups;

		aggstate->hashentrysize = hash_agg_entry_size(aggstate->numtrans,
													  outerplan->plan_width,
													  node->transitionSpace);

		/*
		 * 设置限制和估计分区数量时，把所有分组集一起   
		 * 考虑。当有多个分组集时，这可能会不准确，   
		 * 但估计仍然应该                                        
		 * 合理。  
		 */
		for (int k = 0; k < aggstate->num_hashes; k++)
			totalGroups += aggstate->perhash[k].aggnode->numGroups;

		hash_agg_set_limits(aggstate->hashentrysize, totalGroups, 0,
							&aggstate->hash_mem_limit,
							&aggstate->hash_ngroups_limit,
							&aggstate->hash_planned_partitions);
		find_hash_columns(aggstate);

		/* 若只做 EXPLAIN，则跳过庞大的内存分配          */
		if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
			build_hash_tables(aggstate);

		aggstate->table_filled = false;

		/* 初始化为 1，表示尚无溢出                */
		aggstate->hash_batches_used = 1;
	}

	/*
	 * 把当前与阶段相关的值初始化为初始阶段。对于        
	 * 用排序的策略，初始阶段都是 1（第一趟排序）      
	 * （若同时也做哈希，则阶段 0 最后处理）；但若只     
	 * 做哈希，那么就只有阶段 0。              
	 */
	if (node->aggstrategy == AGG_HASHED)
	{
		aggstate->current_phase = 0;
		initialize_phase(aggstate, 0);
		select_current_set(aggstate, 0, true);
	}
	else
	{
		aggstate->current_phase = 1;
		initialize_phase(aggstate, 1);
		select_current_set(aggstate, 0, false);
	}

	/*
	 * 查找聚合函数信息，并初始化每聚合和每过渡  
	 * 数据中不变的字段。                         
	 */
	foreach(l, aggstate->aggs)
	{
		Aggref	   *aggref = lfirst(l);
		AggStatePerAgg peragg;
		AggStatePerTrans pertrans;
		Oid			aggTransFnInputTypes[FUNC_MAX_ARGS];
		int			numAggTransFnArgs;
		int			numDirectArgs;
		HeapTuple	aggTuple;
		Form_pg_aggregate aggform;
		AclResult	aclresult;
		Oid			finalfn_oid;
		Oid			serialfn_oid,
					deserialfn_oid;
		Oid			aggOwner;
		Expr	   *finalfnexpr;
		Oid			aggtranstype;

		/* 规划器应该已把聚合分配到正确的层级     */
		Assert(aggref->agglevelsup == 0);
		/* ……拆分模式也应匹配      */
		Assert(aggref->aggsplit == aggstate->aggsplit);

		peragg = &peraggs[aggref->aggno];

		/* 检查是否已经为该聚合初始化过状态。           */
		if (peragg->aggref != NULL)
			continue;

		peragg->aggref = aggref;
		peragg->transno = aggref->aggtransno;

		/* 获取 pg_aggregate 行    */
		aggTuple = SearchSysCache1(AGGFNOID,
								   ObjectIdGetDatum(aggref->aggfnoid));
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "cache lookup failed for aggregate %u",
				 aggref->aggfnoid);
		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		/* 检查调用聚合函数的权限           */
		aclresult = object_aclcheck(ProcedureRelationId, aggref->aggfnoid, GetUserId(),
									ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_AGGREGATE,
						   get_func_name(aggref->aggfnoid));
		InvokeFunctionExecuteHook(aggref->aggfnoid);

		/* 规划器已在 Aggref 本身中记录了过渡状态类型 */
		aggtranstype = aggref->aggtranstype;
		Assert(OidIsValid(aggtranstype));

		/* 只有在对聚合做最终处理时才需要最终函数       */
		if (DO_AGGSPLIT_SKIPFINAL(aggstate->aggsplit))
			peragg->finalfn_oid = finalfn_oid = InvalidOid;
		else
			peragg->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

		serialfn_oid = InvalidOid;
		deserialfn_oid = InvalidOid;

		/*
		 * 检查是否需要序列化/反序列化。我们只对具有     
		 * INTERNAL transtype 的聚合才这样做。 
		 */
		if (aggtranstype == INTERNALOID)
		{
			/*
			 * 只有每个含 INTERNAL 状态的聚合都有序列化函数 
			 * 时，规划器才应生成序列化聚合节点。       
			 * 请验证这一点。  
			 */
			if (DO_AGGSPLIT_SERIALIZE(aggstate->aggsplit))
			{
				/* 序列化仅在未运行 finalfn 时有效        */
				Assert(DO_AGGSPLIT_SKIPFINAL(aggstate->aggsplit));

				if (!OidIsValid(aggform->aggserialfn))
					elog(ERROR, "serialfunc not provided for serialization aggregation");
				serialfn_oid = aggform->aggserialfn;
			}

			/* 反序列化函数亦然               */
			if (DO_AGGSPLIT_DESERIALIZE(aggstate->aggsplit))
			{
				/* 反序列化仅在合并状态时有效          */
				Assert(DO_AGGSPLIT_COMBINE(aggstate->aggsplit));

				if (!OidIsValid(aggform->aggdeserialfn))
					elog(ERROR, "deserialfunc not provided for deserialization aggregation");
				deserialfn_oid = aggform->aggdeserialfn;
			}
		}

		/* 检查聚合的所有者是否有权调用其组件函数       */
		{
			HeapTuple	procTuple;

			procTuple = SearchSysCache1(PROCOID,
										ObjectIdGetDatum(aggref->aggfnoid));
			if (!HeapTupleIsValid(procTuple))
				elog(ERROR, "cache lookup failed for function %u",
					 aggref->aggfnoid);
			aggOwner = ((Form_pg_proc) GETSTRUCT(procTuple))->proowner;
			ReleaseSysCache(procTuple);

			if (OidIsValid(finalfn_oid))
			{
				aclresult = object_aclcheck(ProcedureRelationId, finalfn_oid, aggOwner,
											ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_FUNCTION,
								   get_func_name(finalfn_oid));
				InvokeFunctionExecuteHook(finalfn_oid);
			}
			if (OidIsValid(serialfn_oid))
			{
				aclresult = object_aclcheck(ProcedureRelationId, serialfn_oid, aggOwner,
											ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_FUNCTION,
								   get_func_name(serialfn_oid));
				InvokeFunctionExecuteHook(serialfn_oid);
			}
			if (OidIsValid(deserialfn_oid))
			{
				aclresult = object_aclcheck(ProcedureRelationId, deserialfn_oid, aggOwner,
											ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_FUNCTION,
								   get_func_name(deserialfn_oid));
				InvokeFunctionExecuteHook(deserialfn_oid);
			}
		}

		/*
		 * 获取（名义上的）聚合输入的实际数据类型。  
		 * 当聚合接受 ANY 或多态类型时，它们可能与聚合  
		 * 声明的输入类型不同。        
		 */
		numAggTransFnArgs = get_aggregate_argtypes(aggref,
												   aggTransFnInputTypes);

		/* 统计"直接"参数（如果有）  */
		numDirectArgs = list_length(aggref->aggdirectargs);

		/* 确定要传递给 finalfn 的参数个数       */
		if (aggform->aggfinalextra)
			peragg->numFinalArgs = numAggTransFnArgs + 1;
		else
			peragg->numFinalArgs = numDirectArgs + 1;

		/* 初始化所有直接参数表达式       */
		peragg->aggdirectargs = ExecInitExprList(aggref->aggdirectargs,
												 (PlanState *) aggstate);

		/*
		 * 如果存在且需要 finalfn，就用实际参数与结果类型  
		 * 为其构建表达式树。           
		 */
		if (OidIsValid(finalfn_oid))
		{
			build_aggregate_finalfn_expr(aggTransFnInputTypes,
										 peragg->numFinalArgs,
										 aggtranstype,
										 aggref->aggtype,
										 aggref->inputcollid,
										 finalfn_oid,
										 &finalfnexpr);
			fmgr_info(finalfn_oid, &peragg->finalfn);
			fmgr_info_set_expr((Node *) finalfnexpr, &peragg->finalfn);
		}

		/* 获取输出值数据类型的相关信息 */
		get_typlenbyval(aggref->aggtype,
						&peragg->resulttypeLen,
						&peragg->resulttypeByVal);

		/*
		 * 为调用过渡函数构建相应的工作状态，如果我们
		 * 尚未构建的话。   
		 */
		pertrans = &pertransstates[aggref->aggtransno];
		if (pertrans->aggref == NULL)
		{
			Datum		textInitVal;
			Datum		initValue;
			bool		initValueIsNull;
			Oid			transfn_oid;

			/*
			 * 若聚合执行状态合并，则不用过渡函数，而     
			 * 是改用合并                                        
			 * 函数。
			 */
			if (DO_AGGSPLIT_COMBINE(aggstate->aggsplit))
			{
				transfn_oid = aggform->aggcombinefn;

				/* 若未设置，则规划器出了错  */
				if (!OidIsValid(transfn_oid))
					elog(ERROR, "combinefn not set for aggregate function");
			}
			else
				transfn_oid = aggform->aggtransfn;

			aclresult = object_aclcheck(ProcedureRelationId, transfn_oid, aggOwner, ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_FUNCTION,
							   get_func_name(transfn_oid));
			InvokeFunctionExecuteHook(transfn_oid);

			/*
			 * initval 可能为 NULL，故不要当作结构体字段    
			 * 访问它；必须借助 SysCacheGetAttr 用笨办法。   
			 */
			textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple,
										  Anum_pg_aggregate_agginitval,
										  &initValueIsNull);
			if (initValueIsNull)
				initValue = (Datum) 0;
			else
				initValue = GetAggInitVal(textInitVal, aggtranstype);

			if (DO_AGGSPLIT_COMBINE(aggstate->aggsplit))
			{
				Oid			combineFnInputTypes[] = {aggtranstype,
				aggtranstype};

				/*
				 * 合并时仅一个输入：待合并的过渡值。此处
				 * 不把该过渡值计算在                           
				 * 内  
				 */
				pertrans->numTransInputs = 1;

				/* aggcombinefn 的两个参数总是 aggtranstype 类型*/
				build_pertrans_for_aggref(pertrans, aggstate, estate,
										  aggref, transfn_oid, aggtranstype,
										  serialfn_oid, deserialfn_oid,
										  initValue, initValueIsNull,
										  combineFnInputTypes, 2);

				/*
				 * 确保合并 INTERNAL 状态的合并函数不是严格  
				 * 函数。这本应在 CREATE AGGREGATE 时检查过，    
				 * 但此后严格属性可能已被                         
				 * 更改。  
				 */
				if (pertrans->transfn.fn_strict && aggtranstype == INTERNALOID)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("combine function with transition type %s must not be declared STRICT",
									format_type_be(aggtranstype))));
			}
			else
			{
				/* 确定要传递给 transfn 的参数个数       */
				if (AGGKIND_IS_ORDERED_SET(aggref->aggkind))
					pertrans->numTransInputs = list_length(aggref->args);
				else
					pertrans->numTransInputs = numAggTransFnArgs;

				build_pertrans_for_aggref(pertrans, aggstate, estate,
										  aggref, transfn_oid, aggtranstype,
										  serialfn_oid, deserialfn_oid,
										  initValue, initValueIsNull,
										  aggTransFnInputTypes,
										  numAggTransFnArgs);

				/*
				 * 若 transfn 严格且 initval 为 NULL，则须确保输入
				 * 类型与过渡类型相同（或至少           
				 * 二进制兼容），这样才能将首个          
				 * 被聚合的输入值用作初始过渡值。          
				 * 这一点本应在定义聚合时检查，但鉴于    
				 * transfn 的严格性可能已被更改，必须           
				 * 再次检查。  
				 */
				if (pertrans->transfn.fn_strict && pertrans->initValueIsNull)
				{
					if (numAggTransFnArgs <= numDirectArgs ||
						!IsBinaryCoercible(aggTransFnInputTypes[numDirectArgs],
										   aggtranstype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
								 errmsg("aggregate %u needs to have compatible input type and transition type",
										aggref->aggfnoid)));
				}
			}
		}
		else
			pertrans->aggshared = true;
		ReleaseSysCache(aggTuple);
	}

	/*
	 * 把 aggstate->numaggs 更新为所找到的不重复聚合个数。  
	 * 并将 numstates 设为所找到的不重复过渡状态个数。  
	 */
	aggstate->numaggs = numaggs;
	aggstate->numtrans = numtrans;

	/*
	 * 最后，检查在处理聚合参数表达式时节点上是否又   
	 * 添加了更多聚合（不仅包括上文刚处理过的常规参数  
	 * 和 FILTER 表达式，还包括我们可能更早处理过的直    
	 * 接参数）。如果是这样，就说明存在嵌套聚合函数，
	 * 这在语义上毫无意义，所以要报错。（这本应由       
	 * 解析器捕获，故我们不必费力编写有用的错误消息； 
	 * 但保险起见，无论如何都应在此对它加以防范，       
	 * 以防万一。）            
	 */
	if (numaggrefs != list_length(aggstate->aggs))
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("aggregate function calls cannot be nested")));

	/*
	 * 构建能一次完成所有过渡工作的表达式。我们会为 
	 * 每个阶段构建不同的表达式，因为过渡函数的调用
	 * 次数可能因阶段而异。注意无论过渡还是合并函 
	 * 数都适用（尽管后者情形下只会有                      
	 * 一个阶段）。        
	 */
	for (phaseidx = 0; phaseidx < aggstate->numphases; phaseidx++)
	{
		AggStatePerPhase phase = &aggstate->phases[phaseidx];
		bool		dohash = false;
		bool		dosort = false;

		/* 阶段 0 不一定存在          */
		if (!phase->aggnode)
			continue;

		if (aggstate->aggstrategy == AGG_MIXED && phaseidx == 1)
		{
			/*
			 * 混合聚合中只有阶段一会同时进行排序和     
			 * 聚合两项工作。   
			 */
			dohash = true;
			dosort = true;
		}
		else if (aggstate->aggstrategy == AGG_MIXED && phaseidx == 0)
		{
			/*
			 * AGG_MIXED 阶段 0 无需计算过渡函数——哈希表中  
			 * 的内容将在阶段 1 期                               
			 * 间被算好。
			 */
			continue;
		}
		else if (phase->aggstrategy == AGG_PLAIN ||
				 phase->aggstrategy == AGG_SORTED)
		{
			dohash = false;
			dosort = true;
		}
		else if (phase->aggstrategy == AGG_HASHED)
		{
			dohash = true;
			dosort = false;
		}
		else
			Assert(false);

		phase->evaltrans = ExecBuildAggTrans(aggstate, phase, dosort, dohash,
											 false);

		/* 缓存为 outer slot 编译的表达式，不做 NULL 检查*/
		phase->evaltrans_cache[0][0] = phase->evaltrans;
	}

	return aggstate;
}

/*
 * 为聚合构建计算状态值所需的状态。                   
 *
 * 这会初始化 'pertrans' 中的所有字段。'aggref' 是要为其   
 * 初始化状态的聚合。'transfn_oid'、'aggtranstype' 及其余     
 * 参数本可从 'aggref' 算出，但调用者已经算过，不妨   
 * 直接传过来。                                  
 *
 * aggcombinefn 的 Oid。                                                   
 */
static void
build_pertrans_for_aggref(AggStatePerTrans pertrans,
						  AggState *aggstate, EState *estate,
						  Aggref *aggref,
						  Oid transfn_oid, Oid aggtranstype,
						  Oid aggserialfn, Oid aggdeserialfn,
						  Datum initValue, bool initValueIsNull,
						  Oid *inputTypes, int numArguments)
{
	int			numGroupingSets = Max(aggstate->maxsets, 1);
	Expr	   *transfnexpr;
	int			numTransArgs;
	Expr	   *serialfnexpr = NULL;
	Expr	   *deserialfnexpr = NULL;
	ListCell   *lc;
	int			numInputs;
	int			numDirectArgs;
	List	   *sortlist;
	int			numSortCols;
	int			numDistinctCols;
	int			i;

	/* 开始填充 pertrans 数据       */
	pertrans->aggref = aggref;
	pertrans->aggshared = false;
	pertrans->aggCollation = aggref->inputcollid;
	pertrans->transfn_oid = transfn_oid;
	pertrans->serialfn_oid = aggserialfn;
	pertrans->deserialfn_oid = aggdeserialfn;
	pertrans->initValue = initValue;
	pertrans->initValueIsNull = initValueIsNull;

	/* 统计"直接"参数（如果有）  */
	numDirectArgs = list_length(aggref->aggdirectargs);

	/* 统计聚合输入列的个数               */
	pertrans->numInputs = numInputs = list_length(aggref->args);

	pertrans->aggtranstype = aggtranstype;

	/* 把当前过渡状态计入              */
	numTransArgs = pertrans->numTransInputs + 1;

	/*
	 * 搭建调用 transfn 所需的基础设施；注意此处不需要     
	 * invtransfn。   
	 */
	build_aggregate_transfn_expr(inputTypes,
								 numArguments,
								 numDirectArgs,
								 aggref->aggvariadic,
								 aggtranstype,
								 aggref->inputcollid,
								 transfn_oid,
								 InvalidOid,
								 &transfnexpr,
								 NULL);

	fmgr_info(transfn_oid, &pertrans->transfn);
	fmgr_info_set_expr((Node *) transfnexpr, &pertrans->transfn);

	pertrans->transfn_fcinfo =
		(FunctionCallInfo) palloc(SizeForFunctionCallInfo(numTransArgs));
	InitFunctionCallInfoData(*pertrans->transfn_fcinfo,
							 &pertrans->transfn,
							 numTransArgs,
							 pertrans->aggCollation,
							 (void *) aggstate, NULL);

	/* 获取状态值数据类型的相关信息*/
	get_typlenbyval(aggtranstype,
					&pertrans->transtypeLen,
					&pertrans->transtypeByVal);

	if (OidIsValid(aggserialfn))
	{
		build_aggregate_serialfn_expr(aggserialfn,
									  &serialfnexpr);
		fmgr_info(aggserialfn, &pertrans->serialfn);
		fmgr_info_set_expr((Node *) serialfnexpr, &pertrans->serialfn);

		pertrans->serialfn_fcinfo =
			(FunctionCallInfo) palloc(SizeForFunctionCallInfo(1));
		InitFunctionCallInfoData(*pertrans->serialfn_fcinfo,
								 &pertrans->serialfn,
								 1,
								 InvalidOid,
								 (void *) aggstate, NULL);
	}

	if (OidIsValid(aggdeserialfn))
	{
		build_aggregate_deserialfn_expr(aggdeserialfn,
										&deserialfnexpr);
		fmgr_info(aggdeserialfn, &pertrans->deserialfn);
		fmgr_info_set_expr((Node *) deserialfnexpr, &pertrans->deserialfn);

		pertrans->deserialfn_fcinfo =
			(FunctionCallInfo) palloc(SizeForFunctionCallInfo(2));
		InitFunctionCallInfoData(*pertrans->deserialfn_fcinfo,
								 &pertrans->deserialfn,
								 2,
								 InvalidOid,
								 (void *) aggstate, NULL);
	}

	/*
	 * 如果普通聚合要做 DISTINCT 或 ORDER BY，就有一个      
	 * SortGroupClause 节点列表；把其中数据取出存入数组。
	 * 但有序集聚合的 ORDER BY 会被忽略；那由聚合自身的
	 * transfn 和 finalfn 负责。                                   
	 *
	 * 若已设置 aggpresorted 标志，聚合输入已排好序，  
	 * 对 ORDER BY 聚合，我们可以直接把它们当作普通聚合   
	 * 对待。预排序的 DISTINCT 聚合必须增加一个         
	 * 额外步骤来去除连续出现的重复输                         
	 * 入。 
	 *
	 * 注意，按构造方式，如果存在 DISTINCT 子句，那么 ORDER 
	 *  BY 子句是其前缀（见 transformDistinctClause）。
	 */
	if (AGGKIND_IS_ORDERED_SET(aggref->aggkind))
	{
		sortlist = NIL;
		numSortCols = numDistinctCols = 0;
		pertrans->aggsortrequired = false;
	}
	else if (aggref->aggpresorted && aggref->aggdistinct == NIL)
	{
		sortlist = NIL;
		numSortCols = numDistinctCols = 0;
		pertrans->aggsortrequired = false;
	}
	else if (aggref->aggdistinct)
	{
		sortlist = aggref->aggdistinct;
		numSortCols = numDistinctCols = list_length(sortlist);
		Assert(numSortCols >= list_length(aggref->aggorder));
		pertrans->aggsortrequired = !aggref->aggpresorted;
	}
	else
	{
		sortlist = aggref->aggorder;
		numSortCols = list_length(sortlist);
		numDistinctCols = 0;
		pertrans->aggsortrequired = (numSortCols > 0);
	}

	pertrans->numSortCols = numSortCols;
	pertrans->numDistinctCols = numDistinctCols;

	/*
	 * 如果聚合要排序或过滤，就为该聚合的聚合输入创  
	 * 建对应的 tupledesc 与槽（含排序表达            
	 * 式）。               
	 */
	if (numSortCols > 0 || aggref->aggfilter)
	{
		pertrans->sortdesc = ExecTypeFromTL(aggref->args);
		pertrans->sortslot =
			ExecInitExtraTupleSlot(estate, pertrans->sortdesc,
								   &TTSOpsMinimalTuple);
	}

	if (numSortCols > 0)
	{
		/*
		 * 在 HASHED 模式下，暂不实现 DISTINCT/ORDER BY 聚合   
		 * (暂)
		 */
		Assert(aggstate->aggstrategy != AGG_HASHED && aggstate->aggstrategy != AGG_MIXED);

		/* 在部分聚合的情形下不支持 ORDER BY 聚合           */
		Assert(!DO_AGGSPLIT_COMBINE(aggstate->aggsplit));

		/* 若仅一个输入，则需要其 len/byval 信息。  */
		if (numInputs == 1)
		{
			get_typlenbyval(inputTypes[numDirectArgs],
							&pertrans->inputtypeLen,
							&pertrans->inputtypeByVal);
		}
		else if (numDistinctCols > 0)
		{
			/* 需要一个额外槽来存储先前的值       */
			pertrans->uniqslot =
				ExecInitExtraTupleSlot(estate, pertrans->sortdesc,
									   &TTSOpsMinimalTuple);
		}

		/* 提取排序信息供以后使用          */
		pertrans->sortColIdx =
			(AttrNumber *) palloc(numSortCols * sizeof(AttrNumber));
		pertrans->sortOperators =
			(Oid *) palloc(numSortCols * sizeof(Oid));
		pertrans->sortCollations =
			(Oid *) palloc(numSortCols * sizeof(Oid));
		pertrans->sortNullsFirst =
			(bool *) palloc(numSortCols * sizeof(bool));

		i = 0;
		foreach(lc, sortlist)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(lc);
			TargetEntry *tle = get_sortgroupclause_tle(sortcl, aggref->args);

			/* 解析器应已确保这一点           */
			Assert(OidIsValid(sortcl->sortop));

			pertrans->sortColIdx[i] = tle->resno;
			pertrans->sortOperators[i] = sortcl->sortop;
			pertrans->sortCollations[i] = exprCollation((Node *) tle->expr);
			pertrans->sortNullsFirst[i] = sortcl->nulls_first;
			i++;
		}
		Assert(i == numSortCols);
	}

	if (aggref->aggdistinct)
	{
		Oid		   *ops;

		Assert(numArguments > 0);
		Assert(list_length(aggref->aggdistinct) == numDistinctCols);

		ops = palloc(numDistinctCols * sizeof(Oid));

		i = 0;
		foreach(lc, aggref->aggdistinct)
			ops[i++] = ((SortGroupClause *) lfirst(lc))->eqop;

		/* 查找/构建所需的比较器          */
		if (numDistinctCols == 1)
			fmgr_info(get_opcode(ops[0]), &pertrans->equalfnOne);
		else
			pertrans->equalfnMulti =
				execTuplesMatchPrepare(pertrans->sortdesc,
									   numDistinctCols,
									   pertrans->sortColIdx,
									   ops,
									   pertrans->sortCollations,
									   &aggstate->ss.ps);
		pfree(ops);
	}

	pertrans->sortstates = (Tuplesortstate **)
		palloc0(sizeof(Tuplesortstate *) * numGroupingSets);
}


static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = TextDatumGetCString(textInitVal);
	initVal = OidInputFunctionCall(typinput, strInitVal,
								   typioparam, -1);
	pfree(strInitVal);
	return initVal;
}

void
ExecEndAgg(AggState *node)
{
	PlanState  *outerPlan;
	int			transno;
	int			numGroupingSets = Max(node->maxsets, 1);
	int			setno;

	/*
	 * 并行 worker 结束时，把它收集的统计信息拷回共享 
	 * 享内存，以便主进程可以取出这些信息，用于在       
	 * EXPLAIN ANALYZE 中报告。         
	 */
	if (node->shared_info && IsParallelWorker())
	{
		AggregateInstrumentation *si;

		Assert(ParallelWorkerNumber <= node->shared_info->num_workers);
		si = &node->shared_info->sinstrument[ParallelWorkerNumber];
		si->hash_batches_used = node->hash_batches_used;
		si->hash_disk_used = node->hash_disk_used;
		si->hash_mem_peak = node->hash_mem_peak;
	}

	/* 确保已关闭所有打开的 tuplesort     */

	if (node->sort_in)
		tuplesort_end(node->sort_in);
	if (node->sort_out)
		tuplesort_end(node->sort_out);

	hashagg_reset_spill_state(node);

	if (node->hash_metacxt != NULL)
	{
		MemoryContextDelete(node->hash_metacxt);
		node->hash_metacxt = NULL;
	}

	for (transno = 0; transno < node->numtrans; transno++)
	{
		AggStatePerTrans pertrans = &node->pertrans[transno];

		for (setno = 0; setno < numGroupingSets; setno++)
		{
			if (pertrans->sortstates[setno])
				tuplesort_end(pertrans->sortstates[setno]);
		}
	}

	/* 并确保所有聚合关闭回调都已被调用       */
	for (setno = 0; setno < numGroupingSets; setno++)
		ReScanExprContext(node->aggcontexts[setno]);
	if (node->hashcontext)
		ReScanExprContext(node->hashcontext);

	/*
	 * 我们并不在此真正释放任何 ExprContext（参见   
	 * ExecFreeExprContext 注释），把输出者从计划节点解开     
	 * 即可。
	 */
	ExecFreeExprContext(&node->ss.ps);

	/* 清理元组表      */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}

void
ExecReScanAgg(AggState *node)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	PlanState  *outerPlan = outerPlanState(node);
	Agg		   *aggnode = (Agg *) node->ss.ps.plan;
	int			transno;
	int			numGroupingSets = Max(node->maxsets, 1);
	int			setno;

	node->agg_done = false;

	if (node->aggstrategy == AGG_HASHED)
	{
		/*
		 * 哈希情形下若尚未建哈希表，可直接返回；什么都
		 * 还没做，所以无可撤销。如果 subnode 的 chgParam 不为
		 * NULL，则 ExecProcNode 会重扫；否则                      
		 * 根本不必重扫。               
		 */
		if (!node->table_filled)
			return;

		/*
		 * 若确有哈希表，它从未溢出，子计划无参数变化， 
		 * 而且我们的参数变化不影响聚合输入表达式，那么
		 * 就可以直接重新扫描现有的哈希表，无需再         
		 * 重新构建它一遍                                          
		 * 即可
		 */
		if (outerPlan->chgParam == NULL && !node->hash_ever_spilled &&
			!bms_overlap(node->ss.ps.chgParam, aggnode->aggParams))
		{
			ResetTupleHashIterator(node->perhash[0].hashtable,
								   &node->perhash[0].hashiter);
			select_current_set(node, 0, true);
			return;
		}
	}

	/* 确保已关闭所有打开的 tuplesort     */
	for (transno = 0; transno < node->numtrans; transno++)
	{
		for (setno = 0; setno < numGroupingSets; setno++)
		{
			AggStatePerTrans pertrans = &node->pertrans[transno];

			if (pertrans->sortstates[setno])
			{
				tuplesort_end(pertrans->sortstates[setno]);
				pertrans->sortstates[setno] = NULL;
			}
		}
	}

	/*
	 * 这里无需对输出元组上下文调用 ReScanExprContext；  
	 * ExecReScan 已做过此事。但我们确实需要重置各分组集   
	 * 用的上下文，其中可能存有过渡值。（之所以用    
	 *  rescan 而非仅 reset，因过渡函数可能注册了现在需    
	 * 要运行的回调。）AGG_HASHED 情形见下文。        
	 */

	for (setno = 0; setno < numGroupingSets; setno++)
	{
		ReScanExprContext(node->aggcontexts[setno]);
	}

	/* 若已制作副本，就释放该组第一个元组  */
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/* 忘掉当前聚合值     */
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * node->numaggs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * node->numaggs);

	/*
	 * AGG_HASHED/MIXED 情形下，哈希表分配在 hashcontext 的一个 
	 * 子上下文中。这曾是问题，但现在重置上下文将自动  
	 * 删除其子上下文。               
	 */
	if (node->aggstrategy == AGG_HASHED || node->aggstrategy == AGG_MIXED)
	{
		hashagg_reset_spill_state(node);

		node->hash_ever_spilled = false;
		node->hash_spill_mode = false;
		node->hash_ngroups_current = 0;

		ReScanExprContext(node->hashcontext);
		/* 重建一个空哈希表    */
		build_hash_tables(node);
		node->table_filled = false;
		/* 填充该表时，迭代器会被重置         */

		hashagg_recompile_expressions(node, false, false);
	}

	if (node->aggstrategy != AGG_HASHED)
	{
		/*
		 * 重置每组的状态（尤其是把过渡值标记为空）。 
		 */
		for (setno = 0; setno < numGroupingSets; setno++)
		{
			MemSet(node->pergroups[setno], 0,
				   sizeof(AggStatePerGroupData) * node->numaggs);
		}

		/* 回到阶段 1   */
		initialize_phase(node, 1);

		node->input_done = false;
		node->projected_set = -1;
	}

	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}


/***********************************************************************
 * 向聚合函数公开的 API      
 ***********************************************************************/


/*
 * AggCheckCallContext - 测试某个 SQL 函数是否正作为聚合被调用 
 *
 * 聚合的过渡函数/最终函数可能想验证自己是否作为聚  
 * 合被调用而非作为普通 SQL 函数被调用。它们应使 
 * 用本函数来验证。若作为聚合被调用则返回值非零，
 * 否则返回零。（具体非零值是 AGG_CONTEXT_AGGREGATE 或     
 * AGG_CONTEXT_WINDOW，不过将来也可能出现别的                  
 * 取值。）                               
 *
 * 如果 aggcontext 非 NULL，该函数还会把聚合过渡值所存 
 * 储在的内存上下文的身份存入 *aggcontext。注意，同一    
 * 聚合调用点（flinfo）可能在不同上下文中交替地用于    
 * 不同的过渡值，因此把 aggcontext 缓存在 fn_extra 中是      
 * 不合规矩的。不过，把它缓存在过渡值自身内部（对于 
 * internal 类型的过渡值）则是合规矩的。               
 */
int
AggCheckCallContext(FunctionCallInfo fcinfo, MemoryContext *aggcontext)
{
	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		if (aggcontext)
		{
			AggState   *aggstate = ((AggState *) fcinfo->context);
			ExprContext *cxt = aggstate->curaggcontext;

			*aggcontext = cxt->ecxt_per_tuple_memory;
		}
		return AGG_CONTEXT_AGGREGATE;
	}
	if (fcinfo->context && IsA(fcinfo->context, WindowAggState))
	{
		if (aggcontext)
			*aggcontext = ((WindowAggState *) fcinfo->context)->curaggcontext;
		return AGG_CONTEXT_WINDOW;
	}

	/* 这一赋值只是为了避免"未初始化变量"警告  */
	if (aggcontext)
		*aggcontext = NULL;
	return 0;
}

/*
 * AggGetAggref - 允许聚合支持函数获取其 Aggref 节点      
 *
 * 若函数作为聚合支持函数被调用，返回聚合调用  
 * 的 Aggref 节点；否则返回 NULL。                               
 *
 * 输入与过渡函数相同的聚合，可合并成单次过渡计 
 * 算。若过渡函数调用 AggGetAggref，它将得到其正在        
 * 执行的某个 Aggref。因此它绝不能关注与最终函数相关 
 * 的那些 Aggref 字段，因为其值是不确定的。但如果是     
 * 最终函数调用 AggGetAggref，则将得到一个精确的            
 * 结果。                                                 
 *
 * 注意，若聚合正被用作窗口函数，这里会返回 NULL。   
 * 在这种情形下，我们本可以提供一个类似函数来返回相
 * 关的 WindowFunc 节点，但目前尚不需要。    
 */
Aggref *
AggGetAggref(FunctionCallInfo fcinfo)
{
	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		AggState   *aggstate = (AggState *) fcinfo->context;
		AggStatePerAgg curperagg;
		AggStatePerTrans curpertrans;

		/* 检查 curperagg（在最终函数中时有效）*/
		curperagg = aggstate->curperagg;

		if (curperagg)
			return curperagg->aggref;

		/* 检查 curpertrans（在过渡函数中时有效）     */
		curpertrans = aggstate->curpertrans;

		if (curpertrans)
			return curpertrans->aggref;
	}
	return NULL;
}

/*
 * AggGetTempMemoryContext - 获取聚合的短期内存上下文          
 *
 * 这对聚合的最终函数有用；返回的上下文是最终函数  
 * 可按需安全重置的上下文。它对过渡函数没什么用， 
 * 因返回的上下文可能（我们不承诺）正是调用它们的  
 * 的那个上下文相同。                    
 *
 * 如上所述，它目前对作为窗口函数调用的聚合没有用处。
 */
MemoryContext
AggGetTempMemoryContext(FunctionCallInfo fcinfo)
{
	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		AggState   *aggstate = (AggState *) fcinfo->context;

		return aggstate->tmpcontext->ecxt_per_tuple_memory;
	}
	return NULL;
}

/*
 * AggStateIsShared - 查明过渡状态是否为共享          
 *
 * 若该函数作为聚合支持函数被调用，聚合的过     
 * 渡状态被多个聚合共享，则返回 true；否则返回  
 *  false。                               
 *
 * 若非作为聚合支持函数被调用，则返回 true。 
 * 这意在给出保守答案："不，你最好别乱改你的     
 * 输入"。                                                      
 * 特别是，聚合被用作窗口函数时会返回 true，那种
 * 场景下修改过渡状态并非好主意，将来或许会改
 * 进窗口情形下的行为。                            
 */
bool
AggStateIsShared(FunctionCallInfo fcinfo)
{
	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		AggState   *aggstate = (AggState *) fcinfo->context;
		AggStatePerAgg curperagg;
		AggStatePerTrans curpertrans;

		/* 检查 curperagg（在最终函数中时有效）*/
		curperagg = aggstate->curperagg;

		if (curperagg)
			return aggstate->pertrans[curperagg->transno].aggshared;

		/* 检查 curpertrans（在过渡函数中时有效）     */
		curpertrans = aggstate->curpertrans;

		if (curpertrans)
			return curpertrans->aggshared;
	}
	return true;
}

/*
 * AggRegisterCallback - 为聚合注册清理回调                 
 *
 * 这对聚合很有用，可用来注册关闭回调，确保非内存资 
 * 源得到释放。回调将在关联的 aggcontext（即                 
 *  AggCheckCallContext 返回的那个）被重置之前发生，无论   
 * 是在组间还是因重新扫描查询而发生。错误路径上不会   
 * 调用该回调。典型用例是释放在 aggcontext 中维护的        
 * tuplestore 或 tuplesort，或者聚合函数所创建槽持有的       
 * pin。（在 finalfn 的结果不再需要之后回调才会被调用，   
 * 因此 finalfn 返回将来会被回调释放的                           
 * 数据是安全的。）                           
 *
 * 如上所述，它目前对作为窗口函数调用的聚合没有用处。
 */
void
AggRegisterCallback(FunctionCallInfo fcinfo,
					ExprContextCallbackFunction func,
					Datum arg)
{
	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		AggState   *aggstate = (AggState *) fcinfo->context;
		ExprContext *cxt = aggstate->curaggcontext;

		RegisterExprContextCallback(cxt, func, arg);

		return;
	}
	elog(ERROR, "aggregate function cannot register a callback in this context");
}


/* ----------------------------------------------------------------
 *						并行查询支持    
 * ----------------------------------------------------------------
 */

 /* ----------------------------------------------------------------
  *		ExecAggEstimate
  *
  *		估算传播聚合统计信息所需的空间。          
  * ----------------------------------------------------------------
  */
void
ExecAggEstimate(AggState *node, ParallelContext *pcxt)
{
	Size		size;

	/* 若未插桩或没有 worker，则不需要它     */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = mul_size(pcxt->nworkers, sizeof(AggregateInstrumentation));
	size = add_size(size, offsetof(SharedAggInfo, sinstrument));
	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecAggInitializeDSM
 *
 *		为聚合统计信息初始化 DSM 空间。  
 * ----------------------------------------------------------------
 */
void
ExecAggInitializeDSM(AggState *node, ParallelContext *pcxt)
{
	Size		size;

	/* 若未插桩或没有 worker，则不需要它     */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = offsetof(SharedAggInfo, sinstrument)
		+ pcxt->nworkers * sizeof(AggregateInstrumentation);
	node->shared_info = shm_toc_allocate(pcxt->toc, size);
	/* 确保任何未填充的槽都将包含零    */
	memset(node->shared_info, 0, size);
	node->shared_info->num_workers = pcxt->nworkers;
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id,
				   node->shared_info);
}

/* ----------------------------------------------------------------
 *		ExecAggInitializeWorker
 *
 *		把 worker 附加到聚合统计的 DSM 空间。   
 * ----------------------------------------------------------------
 */
void
ExecAggInitializeWorker(AggState *node, ParallelWorkerContext *pwcxt)
{
	node->shared_info =
		shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, true);
}

/* ----------------------------------------------------------------
 *		ExecAggRetrieveInstrumentation
 *
 *		把聚合统计信息从 DSM 传送到私有内存。    
 * ----------------------------------------------------------------
 */
void
ExecAggRetrieveInstrumentation(AggState *node)
{
	Size		size;
	SharedAggInfo *si;

	if (node->shared_info == NULL)
		return;

	size = offsetof(SharedAggInfo, sinstrument)
		+ node->shared_info->num_workers * sizeof(AggregateInstrumentation);
	si = palloc(size);
	memcpy(si, node->shared_info, size);
	node->shared_info = si;
}
