/*-------------------------------------------------------------------------
 *
 * syscache.c
 *	  系统缓存管理例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/syscache.c
 *
 * NOTES
 *	  这些例程让 parser/planner/executor 能够
 *	  快速查找系统目录的内容。
 *
 *	  缓存 ID 列表见 utils/syscache.h。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_description.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_range.h"
#include "catalog/pg_replication_origin.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_shdescription.h"
#include "catalog/pg_shseclabel.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/catcache.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*---------------------------------------------------------------------------

	添加系统缓存：

	把新缓存加进 include/utils/syscache.h 的列表。
	保持列表按字母顺序排序。

	把条目加入下方 cacheinfo[] 数组。所有缓存列表都
	按字母顺序排列，所以要加在适当位置。请指明关系
	OID、索引 OID、键的数量、键属性号以及初始哈希桶
	数量。

	哈希桶数必须是 2 的幂。合理的做法是设为
	该特定缓存在中等规模数据库中可能具有的
	条目数。

	每个系统缓存底下必须有一个唯一索引（即其键与
	缓存相同的索引）。若还没有这样的索引，
	就用 DECLARE_UNIQUE_INDEX 把其定义加到
	include/catalog/
	pg_*.h。（加索引需要更新 catversion.h，而
	仅仅增删缓存只需重新编译。）

	最后，凡你的关系会被 heap_insert() 或
	heap_update() 调用的地方，应改用能同时更新索引的
	CatalogTupleInsert() 或 CatalogTupleUpdate()；heap_* 则不会。

*---------------------------------------------------------------------------
*/

/*
 *		struct cachedesc：定义单个系统缓存的信息
 */
struct cachedesc
{
	Oid			reloid;			/* 被缓存关系的 OID */
	Oid			indoid;			/* 此缓存索引关系的 OID */
	int			nkeys;			/* 缓存查找所需的键数 */
	int			key[4];			/* 键属性的属性号 */
	int			nbuckets;		/* 此缓存的哈希桶数量 */
};

/* 以便利语法提供 nkeys 与键数组的宏。 */
#define KEY(...) VA_ARGS_NARGS(__VA_ARGS__), { __VA_ARGS__ }

static const struct cachedesc cacheinfo[] = {
	[AGGFNOID] = {
		AggregateRelationId,
		AggregateFnoidIndexId,
		KEY(Anum_pg_aggregate_aggfnoid),
		16
	},
	[AMNAME] = {
		AccessMethodRelationId,
		AmNameIndexId,
		KEY(Anum_pg_am_amname),
		4
	},
	[AMOID] = {
		AccessMethodRelationId,
		AmOidIndexId,
		KEY(Anum_pg_am_oid),
		4
	},
	[AMOPOPID] = {
		AccessMethodOperatorRelationId,
		AccessMethodOperatorIndexId,
		KEY(Anum_pg_amop_amopopr,
			Anum_pg_amop_amoppurpose,
			Anum_pg_amop_amopfamily),
		64
	},
	[AMOPSTRATEGY] = {
		AccessMethodOperatorRelationId,
		AccessMethodStrategyIndexId,
		KEY(Anum_pg_amop_amopfamily,
			Anum_pg_amop_amoplefttype,
			Anum_pg_amop_amoprighttype,
			Anum_pg_amop_amopstrategy),
		64
	},
	[AMPROCNUM] = {
		AccessMethodProcedureRelationId,
		AccessMethodProcedureIndexId,
		KEY(Anum_pg_amproc_amprocfamily,
			Anum_pg_amproc_amproclefttype,
			Anum_pg_amproc_amprocrighttype,
			Anum_pg_amproc_amprocnum),
		16
	},
	[ATTNAME] = {
		AttributeRelationId,
		AttributeRelidNameIndexId,
		KEY(Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attname),
		32
	},
	[ATTNUM] = {
		AttributeRelationId,
		AttributeRelidNumIndexId,
		KEY(Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attnum),
		128
	},
	[AUTHMEMMEMROLE] = {
		AuthMemRelationId,
		AuthMemMemRoleIndexId,
		KEY(Anum_pg_auth_members_member,
			Anum_pg_auth_members_roleid,
			Anum_pg_auth_members_grantor),
		8
	},
	[AUTHMEMROLEMEM] = {
		AuthMemRelationId,
		AuthMemRoleMemIndexId,
		KEY(Anum_pg_auth_members_roleid,
			Anum_pg_auth_members_member,
			Anum_pg_auth_members_grantor),
		8
	},
	[AUTHNAME] = {
		AuthIdRelationId,
		AuthIdRolnameIndexId,
		KEY(Anum_pg_authid_rolname),
		8
	},
	[AUTHOID] = {
		AuthIdRelationId,
		AuthIdOidIndexId,
		KEY(Anum_pg_authid_oid),
		8
	},
	[CASTSOURCETARGET] = {
		CastRelationId,
		CastSourceTargetIndexId,
		KEY(Anum_pg_cast_castsource,
			Anum_pg_cast_casttarget),
		256
	},
	[CLAAMNAMENSP] = {
		OperatorClassRelationId,
		OpclassAmNameNspIndexId,
		KEY(Anum_pg_opclass_opcmethod,
			Anum_pg_opclass_opcname,
			Anum_pg_opclass_opcnamespace),
		8
	},
	[CLAOID] = {
		OperatorClassRelationId,
		OpclassOidIndexId,
		KEY(Anum_pg_opclass_oid),
		8
	},
	[COLLNAMEENCNSP] = {
		CollationRelationId,
		CollationNameEncNspIndexId,
		KEY(Anum_pg_collation_collname,
			Anum_pg_collation_collencoding,
			Anum_pg_collation_collnamespace),
		8
	},
	[COLLOID] = {
		CollationRelationId,
		CollationOidIndexId,
		KEY(Anum_pg_collation_oid),
		8
	},
	[CONDEFAULT] = {
		ConversionRelationId,
		ConversionDefaultIndexId,
		KEY(Anum_pg_conversion_connamespace,
			Anum_pg_conversion_conforencoding,
			Anum_pg_conversion_contoencoding,
			Anum_pg_conversion_oid),
		8
	},
	[CONNAMENSP] = {
		ConversionRelationId,
		ConversionNameNspIndexId,
		KEY(Anum_pg_conversion_conname,
			Anum_pg_conversion_connamespace),
		8
	},
	[CONSTROID] = {
		ConstraintRelationId,
		ConstraintOidIndexId,
		KEY(Anum_pg_constraint_oid),
		16
	},
	[CONVOID] = {
		ConversionRelationId,
		ConversionOidIndexId,
		KEY(Anum_pg_conversion_oid),
		8
	},
	[DATABASEOID] = {
		DatabaseRelationId,
		DatabaseOidIndexId,
		KEY(Anum_pg_database_oid),
		4
	},
	[DEFACLROLENSPOBJ] = {
		DefaultAclRelationId,
		DefaultAclRoleNspObjIndexId,
		KEY(Anum_pg_default_acl_defaclrole,
			Anum_pg_default_acl_defaclnamespace,
			Anum_pg_default_acl_defaclobjtype),
		8
	},
	[ENUMOID] = {
		EnumRelationId,
		EnumOidIndexId,
		KEY(Anum_pg_enum_oid),
		8
	},
	[ENUMTYPOIDNAME] = {
		EnumRelationId,
		EnumTypIdLabelIndexId,
		KEY(Anum_pg_enum_enumtypid,
			Anum_pg_enum_enumlabel),
		8
	},
	[EVENTTRIGGERNAME] = {
		EventTriggerRelationId,
		EventTriggerNameIndexId,
		KEY(Anum_pg_event_trigger_evtname),
		8
	},
	[EVENTTRIGGEROID] = {
		EventTriggerRelationId,
		EventTriggerOidIndexId,
		KEY(Anum_pg_event_trigger_oid),
		8
	},
	[FOREIGNDATAWRAPPERNAME] = {
		ForeignDataWrapperRelationId,
		ForeignDataWrapperNameIndexId,
		KEY(Anum_pg_foreign_data_wrapper_fdwname),
		2
	},
	[FOREIGNDATAWRAPPEROID] = {
		ForeignDataWrapperRelationId,
		ForeignDataWrapperOidIndexId,
		KEY(Anum_pg_foreign_data_wrapper_oid),
		2
	},
	[FOREIGNSERVERNAME] = {
		ForeignServerRelationId,
		ForeignServerNameIndexId,
		KEY(Anum_pg_foreign_server_srvname),
		2
	},
	[FOREIGNSERVEROID] = {
		ForeignServerRelationId,
		ForeignServerOidIndexId,
		KEY(Anum_pg_foreign_server_oid),
		2
	},
	[FOREIGNTABLEREL] = {
		ForeignTableRelationId,
		ForeignTableRelidIndexId,
		KEY(Anum_pg_foreign_table_ftrelid),
		4
	},
	[INDEXRELID] = {
		IndexRelationId,
		IndexRelidIndexId,
		KEY(Anum_pg_index_indexrelid),
		64
	},
	[LANGNAME] = {
		LanguageRelationId,
		LanguageNameIndexId,
		KEY(Anum_pg_language_lanname),
		4
	},
	[LANGOID] = {
		LanguageRelationId,
		LanguageOidIndexId,
		KEY(Anum_pg_language_oid),
		4
	},
	[NAMESPACENAME] = {
		NamespaceRelationId,
		NamespaceNameIndexId,
		KEY(Anum_pg_namespace_nspname),
		4
	},
	[NAMESPACEOID] = {
		NamespaceRelationId,
		NamespaceOidIndexId,
		KEY(Anum_pg_namespace_oid),
		16
	},
	[OPERNAMENSP] = {
		OperatorRelationId,
		OperatorNameNspIndexId,
		KEY(Anum_pg_operator_oprname,
			Anum_pg_operator_oprleft,
			Anum_pg_operator_oprright,
			Anum_pg_operator_oprnamespace),
		256
	},
	[OPEROID] = {
		OperatorRelationId,
		OperatorOidIndexId,
		KEY(Anum_pg_operator_oid),
		32
	},
	[OPFAMILYAMNAMENSP] = {
		OperatorFamilyRelationId,
		OpfamilyAmNameNspIndexId,
		KEY(Anum_pg_opfamily_opfmethod,
			Anum_pg_opfamily_opfname,
			Anum_pg_opfamily_opfnamespace),
		8
	},
	[OPFAMILYOID] = {
		OperatorFamilyRelationId,
		OpfamilyOidIndexId,
		KEY(Anum_pg_opfamily_oid),
		8
	},
	[PARAMETERACLNAME] = {
		ParameterAclRelationId,
		ParameterAclParnameIndexId,
		KEY(Anum_pg_parameter_acl_parname),
		4
	},
	[PARAMETERACLOID] = {
		ParameterAclRelationId,
		ParameterAclOidIndexId,
		KEY(Anum_pg_parameter_acl_oid),
		4
	},
	[PARTRELID] = {
		PartitionedRelationId,
		PartitionedRelidIndexId,
		KEY(Anum_pg_partitioned_table_partrelid),
		32
	},
	[PROCNAMEARGSNSP] = {
		ProcedureRelationId,
		ProcedureNameArgsNspIndexId,
		KEY(Anum_pg_proc_proname,
			Anum_pg_proc_proargtypes,
			Anum_pg_proc_pronamespace),
		128
	},
	[PROCOID] = {
		ProcedureRelationId,
		ProcedureOidIndexId,
		KEY(Anum_pg_proc_oid),
		128
	},
	[PUBLICATIONNAME] = {
		PublicationRelationId,
		PublicationNameIndexId,
		KEY(Anum_pg_publication_pubname),
		8
	},
	[PUBLICATIONNAMESPACE] = {
		PublicationNamespaceRelationId,
		PublicationNamespaceObjectIndexId,
		KEY(Anum_pg_publication_namespace_oid),
		64
	},
	[PUBLICATIONNAMESPACEMAP] = {
		PublicationNamespaceRelationId,
		PublicationNamespacePnnspidPnpubidIndexId,
		KEY(Anum_pg_publication_namespace_pnnspid,
			Anum_pg_publication_namespace_pnpubid),
		64
	},
	[PUBLICATIONOID] = {
		PublicationRelationId,
		PublicationObjectIndexId,
		KEY(Anum_pg_publication_oid),
		8
	},
	[PUBLICATIONREL] = {
		PublicationRelRelationId,
		PublicationRelObjectIndexId,
		KEY(Anum_pg_publication_rel_oid),
		64
	},
	[PUBLICATIONRELMAP] = {
		PublicationRelRelationId,
		PublicationRelPrrelidPrpubidIndexId,
		KEY(Anum_pg_publication_rel_prrelid,
			Anum_pg_publication_rel_prpubid),
		64
	},
	[RANGEMULTIRANGE] = {
		RangeRelationId,
		RangeMultirangeTypidIndexId,
		KEY(Anum_pg_range_rngmultitypid),
		4
	},
	[RANGETYPE] = {
		RangeRelationId,
		RangeTypidIndexId,
		KEY(Anum_pg_range_rngtypid),
		4
	},
	[RELNAMENSP] = {
		RelationRelationId,
		ClassNameNspIndexId,
		KEY(Anum_pg_class_relname,
			Anum_pg_class_relnamespace),
		128
	},
	[RELOID] = {
		RelationRelationId,
		ClassOidIndexId,
		KEY(Anum_pg_class_oid),
		128
	},
	[REPLORIGIDENT] = {
		ReplicationOriginRelationId,
		ReplicationOriginIdentIndex,
		KEY(Anum_pg_replication_origin_roident),
		16
	},
	[REPLORIGNAME] = {
		ReplicationOriginRelationId,
		ReplicationOriginNameIndex,
		KEY(Anum_pg_replication_origin_roname),
		16
	},
	[RULERELNAME] = {
		RewriteRelationId,
		RewriteRelRulenameIndexId,
		KEY(Anum_pg_rewrite_ev_class,
			Anum_pg_rewrite_rulename),
		8
	},
	[SEQRELID] = {
		SequenceRelationId,
		SequenceRelidIndexId,
		KEY(Anum_pg_sequence_seqrelid),
		32
	},
	[STATEXTDATASTXOID] = {
		StatisticExtDataRelationId,
		StatisticExtDataStxoidInhIndexId,
		KEY(Anum_pg_statistic_ext_data_stxoid,
			Anum_pg_statistic_ext_data_stxdinherit),
		4
	},
	[STATEXTNAMENSP] = {
		StatisticExtRelationId,
		StatisticExtNameIndexId,
		KEY(Anum_pg_statistic_ext_stxname,
			Anum_pg_statistic_ext_stxnamespace),
		4
	},
	[STATEXTOID] = {
		StatisticExtRelationId,
		StatisticExtOidIndexId,
		KEY(Anum_pg_statistic_ext_oid),
		4
	},
	[STATRELATTINH] = {
		StatisticRelationId,
		StatisticRelidAttnumInhIndexId,
		KEY(Anum_pg_statistic_starelid,
			Anum_pg_statistic_staattnum,
			Anum_pg_statistic_stainherit),
		128
	},
	[SUBSCRIPTIONNAME] = {
		SubscriptionRelationId,
		SubscriptionNameIndexId,
		KEY(Anum_pg_subscription_subdbid,
			Anum_pg_subscription_subname),
		4
	},
	[SUBSCRIPTIONOID] = {
		SubscriptionRelationId,
		SubscriptionObjectIndexId,
		KEY(Anum_pg_subscription_oid),
		4
	},
	[SUBSCRIPTIONRELMAP] = {
		SubscriptionRelRelationId,
		SubscriptionRelSrrelidSrsubidIndexId,
		KEY(Anum_pg_subscription_rel_srrelid,
			Anum_pg_subscription_rel_srsubid),
		64
	},
	[TABLESPACEOID] = {
		TableSpaceRelationId,
		TablespaceOidIndexId,
		KEY(Anum_pg_tablespace_oid),
		4
	},
	[TRFOID] = {
		TransformRelationId,
		TransformOidIndexId,
		KEY(Anum_pg_transform_oid),
		16
	},
	[TRFTYPELANG] = {
		TransformRelationId,
		TransformTypeLangIndexId,
		KEY(Anum_pg_transform_trftype,
			Anum_pg_transform_trflang),
		16
	},
	[TSCONFIGMAP] = {
		TSConfigMapRelationId,
		TSConfigMapIndexId,
		KEY(Anum_pg_ts_config_map_mapcfg,
			Anum_pg_ts_config_map_maptokentype,
			Anum_pg_ts_config_map_mapseqno),
		2
	},
	[TSCONFIGNAMENSP] = {
		TSConfigRelationId,
		TSConfigNameNspIndexId,
		KEY(Anum_pg_ts_config_cfgname,
			Anum_pg_ts_config_cfgnamespace),
		2
	},
	[TSCONFIGOID] = {
		TSConfigRelationId,
		TSConfigOidIndexId,
		KEY(Anum_pg_ts_config_oid),
		2
	},
	[TSDICTNAMENSP] = {
		TSDictionaryRelationId,
		TSDictionaryNameNspIndexId,
		KEY(Anum_pg_ts_dict_dictname,
			Anum_pg_ts_dict_dictnamespace),
		2
	},
	[TSDICTOID] = {
		TSDictionaryRelationId,
		TSDictionaryOidIndexId,
		KEY(Anum_pg_ts_dict_oid),
		2
	},
	[TSPARSERNAMENSP] = {
		TSParserRelationId,
		TSParserNameNspIndexId,
		KEY(Anum_pg_ts_parser_prsname,
			Anum_pg_ts_parser_prsnamespace),
		2
	},
	[TSPARSEROID] = {
		TSParserRelationId,
		TSParserOidIndexId,
		KEY(Anum_pg_ts_parser_oid),
		2
	},
	[TSTEMPLATENAMENSP] = {
		TSTemplateRelationId,
		TSTemplateNameNspIndexId,
		KEY(Anum_pg_ts_template_tmplname,
			Anum_pg_ts_template_tmplnamespace),
		2
	},
	[TSTEMPLATEOID] = {
		TSTemplateRelationId,
		TSTemplateOidIndexId,
		KEY(Anum_pg_ts_template_oid),
		2
	},
	[TYPENAMENSP] = {
		TypeRelationId,
		TypeNameNspIndexId,
		KEY(Anum_pg_type_typname,
			Anum_pg_type_typnamespace),
		64
	},
	[TYPEOID] = {
		TypeRelationId,
		TypeOidIndexId,
		KEY(Anum_pg_type_oid),
		64
	},
	[USERMAPPINGOID] = {
		UserMappingRelationId,
		UserMappingOidIndexId,
		KEY(Anum_pg_user_mapping_oid),
		2
	},
	[USERMAPPINGUSERSERVER] = {
		UserMappingRelationId,
		UserMappingUserServerIndexId,
		KEY(Anum_pg_user_mapping_umuser,
			Anum_pg_user_mapping_umserver),
		2
	}
};

StaticAssertDecl(lengthof(cacheinfo) == SysCacheSize,
				 "SysCacheSize does not match syscache.c's array");

static CatCache *SysCache[SysCacheSize];

static bool CacheInitialized = false;

/* 拥有缓存的表的 OID 排序数组 */
static Oid	SysCacheRelationOid[SysCacheSize];
static int	SysCacheRelationOidSize;

/* 缓存所用的表与索引的 OID 排序数组 */
static Oid	SysCacheSupportingRelOid[SysCacheSize * 2];
static int	SysCacheSupportingRelOidSize;

static int	oid_compare(const void *a, const void *b);


/*
 * InitCatalogCache - 初始化各缓存
 *
 * 注意这里不做任何数据库访问；只分配内存并
 * 初始化缓存结构。对数据库进行查询以完成某个
 * 缓存的初始化发生在首次使用该
 * 缓存时。
 */
void
InitCatalogCache(void)
{
	int			cacheId;

	Assert(!CacheInitialized);

	SysCacheRelationOidSize = SysCacheSupportingRelOidSize = 0;

	for (cacheId = 0; cacheId < SysCacheSize; cacheId++)
	{
		/*
		 * 断言 syscache.h 中定义的每个枚举值都已在
		 * cacheinfo 数组中被填充。
		 */
		Assert(cacheinfo[cacheId].reloid != 0);

		SysCache[cacheId] = InitCatCache(cacheId,
										 cacheinfo[cacheId].reloid,
										 cacheinfo[cacheId].indoid,
										 cacheinfo[cacheId].nkeys,
										 cacheinfo[cacheId].key,
										 cacheinfo[cacheId].nbuckets);
		if (!PointerIsValid(SysCache[cacheId]))
			elog(ERROR, "could not initialize cache %u (%d)",
				 cacheinfo[cacheId].reloid, cacheId);
		/* 也为 OID 列表积累数据 */
		SysCacheRelationOid[SysCacheRelationOidSize++] =
			cacheinfo[cacheId].reloid;
		SysCacheSupportingRelOid[SysCacheSupportingRelOidSize++] =
			cacheinfo[cacheId].reloid;
		SysCacheSupportingRelOid[SysCacheSupportingRelOidSize++] =
			cacheinfo[cacheId].indoid;
		/* 见 RelationInvalidatesSnapshotsOnly 的注释 */
		Assert(!RelationInvalidatesSnapshotsOnly(cacheinfo[cacheId].reloid));
	}

	Assert(SysCacheRelationOidSize <= lengthof(SysCacheRelationOid));
	Assert(SysCacheSupportingRelOidSize <= lengthof(SysCacheSupportingRelOid));

	/* 排序并去重 OID 数组，以便使用二分查找。 */
	pg_qsort(SysCacheRelationOid, SysCacheRelationOidSize,
			 sizeof(Oid), oid_compare);
	SysCacheRelationOidSize =
		qunique(SysCacheRelationOid, SysCacheRelationOidSize, sizeof(Oid),
				oid_compare);

	pg_qsort(SysCacheSupportingRelOid, SysCacheSupportingRelOidSize,
			 sizeof(Oid), oid_compare);
	SysCacheSupportingRelOidSize =
		qunique(SysCacheSupportingRelOid, SysCacheSupportingRelOidSize,
				sizeof(Oid), oid_compare);

	CacheInitialized = true;
}

/*
 * InitCatalogCachePhase2 - 完成各缓存的初始化
 *
 * 完成所有缓存初始化，包括必要的数据库访问。
 * access.
 *
 * 这并非必需；通常让系统缓存首次使用时初始化。
 * 但作为预载入最常用系统目录条目到 relcache 的
 * 机制，它很有用。因此当需要写出新的 relcache
 * 初始化文件时就会调用本
 * 例程。
 */
void
InitCatalogCachePhase2(void)
{
	int			cacheId;

	Assert(CacheInitialized);

	for (cacheId = 0; cacheId < SysCacheSize; cacheId++)
		InitCatCachePhase2(SysCache[cacheId], true);
}


/*
 * SearchSysCache
 *
 *	在 SearchCatCache 之上的一层，为你完成初始化与
 *	键值设置。
 *
 *	找到元组则返回其缓存副本，否则返回 NULL。
 *	该元组是"缓存"副本，绝不可修改！
 *
 *	调用者用完元组时，应调用 ReleaseSysCache() 释放
 *	SearchSysCache() 抓取的引用计数。如果不这样做，
 *	元组将一直锁定在缓存中直至事务结束，尚可
 *	接受但并不理想。
 *
 *	注意（CAUTION）：返回的元组绝不可由调用者释放！
 */
HeapTuple
SearchSysCache(int cacheId,
			   Datum key1,
			   Datum key2,
			   Datum key3,
			   Datum key4)
{
	Assert(cacheId >= 0 && cacheId < SysCacheSize &&
		   PointerIsValid(SysCache[cacheId]));

	return SearchCatCache(SysCache[cacheId], key1, key2, key3, key4);
}

HeapTuple
SearchSysCache1(int cacheId,
				Datum key1)
{
	Assert(cacheId >= 0 && cacheId < SysCacheSize &&
		   PointerIsValid(SysCache[cacheId]));
	Assert(SysCache[cacheId]->cc_nkeys == 1);

	return SearchCatCache1(SysCache[cacheId], key1);
}

HeapTuple
SearchSysCache2(int cacheId,
				Datum key1, Datum key2)
{
	Assert(cacheId >= 0 && cacheId < SysCacheSize &&
		   PointerIsValid(SysCache[cacheId]));
	Assert(SysCache[cacheId]->cc_nkeys == 2);

	return SearchCatCache2(SysCache[cacheId], key1, key2);
}

HeapTuple
SearchSysCache3(int cacheId,
				Datum key1, Datum key2, Datum key3)
{
	Assert(cacheId >= 0 && cacheId < SysCacheSize &&
		   PointerIsValid(SysCache[cacheId]));
	Assert(SysCache[cacheId]->cc_nkeys == 3);

	return SearchCatCache3(SysCache[cacheId], key1, key2, key3);
}

HeapTuple
SearchSysCache4(int cacheId,
				Datum key1, Datum key2, Datum key3, Datum key4)
{
	Assert(cacheId >= 0 && cacheId < SysCacheSize &&
		   PointerIsValid(SysCache[cacheId]));
	Assert(SysCache[cacheId]->cc_nkeys == 4);

	return SearchCatCache4(SysCache[cacheId], key1, key2, key3, key4);
}

/*
 * ReleaseSysCache
 *		释放先前在元组上抓取的引用计数
 */
void
ReleaseSysCache(HeapTuple tuple)
{
	ReleaseCatCache(tuple);
}

/*
 * SearchSysCacheLocked1
 *
 * 将 SearchSysCache1() 与以 InplaceUpdateTupleLock 模式
 * 获取 LOCKTAG_TUPLE 相结合。这是遵守 README.tuplock
 * 中"锁定以写入原地更新的表"一节的方法。调用者的
 * heap_update() 之后，它应当 UnlockTuple(InplaceUpdateTupleLock)
 * 并 ReleaseSysCache()
 *
 * 返回的元组可能是未提交更新的对象，所以这并不
 * 能防止"元组正被并发更新"错误。
 */
HeapTuple
SearchSysCacheLocked1(int cacheId,
					  Datum key1)
{
	CatCache   *cache = SysCache[cacheId];
	ItemPointerData tid;
	LOCKTAG		tag;

	/*----------
	 * 由于原地更新可能就发生在我们的 LockTuple() 之前，
	 * 我们必须返回在 LockTuple() 之后针对所返回 TID 获取的
	 * 内容。如果只是获取两次而不循环，下面的序列将
	 * 挫败此锁定：
	 *
	 * GRANT:   SearchSysCache1() = TID (1,5)
	 * GRANT:   LockTuple(pg_class, (1,5))
	 * [在我们释放锁之前 (1,5) 不再有原地更新]
	 * CLUSTER: SearchSysCache1() = TID (1,5)
	 * CLUSTER: heap_update() = TID (1,8)
	 * CLUSTER: COMMIT
	 * GRANT:   SearchSysCache1() = TID (1,8)
	 * GRANT:   return (1,8) from SearchSysCacheLocked1()
	 * VACUUM:  SearchSysCache1() = TID (1,8)
	 * VACUUM:  LockTuple(pg_class, (1,8))  # 一个关系锁定两个 TID
	 * VACUUM:  原地更新
	 * GRANT:   heap_update() = (1,9)  # 丢失原地更新
	 *
	 * 在顺利情形下，这需要两次获取：一次确定要锁定的
	 * TID，另一次获取内容并确认 TID 未改变。
	 *
	 * 即使该行被更新为新 TID、旧 TID 变为 LP_UNUSED、然后
	 * 该行又被更新回其旧 TID，这也仍然有效。我们仍会持
	 * 有正确的 LOCKTAG_TUPLE 以及在 LOCKTAG_TUPLE 之后捕获
	 * 的行副本。
	 */
	ItemPointerSetInvalid(&tid);
	for (;;)
	{
		HeapTuple	tuple;
		LOCKMODE	lockmode = InplaceUpdateTupleLock;

		tuple = SearchSysCache1(cacheId, key1);
		if (ItemPointerIsValid(&tid))
		{
			if (!HeapTupleIsValid(tuple))
			{
				LockRelease(&tag, lockmode, false);
				return tuple;
			}
			if (ItemPointerEquals(&tid, &tuple->t_self))
				return tuple;
			LockRelease(&tag, lockmode, false);
		}
		else if (!HeapTupleIsValid(tuple))
			return tuple;

		tid = tuple->t_self;
		ReleaseSysCache(tuple);

		/*
		 * 像 LockTuple(rel,&tid,lockmode) 那样做。cc_relisshared
		 * 在各轮迭代之间不会变，但在我们的第一次
		 * SearchSysCache1() 前是临时"假"。
		 */
		SET_LOCKTAG_TUPLE(tag,
						  cache->cc_relisshared ? InvalidOid : MyDatabaseId,
						  cache->cc_reloid,
						  ItemPointerGetBlockNumber(&tid),
						  ItemPointerGetOffsetNumber(&tid));
		(void) LockAcquire(&tag, lockmode, false, false);

		/*
		 * 如果一个原地更新刚完成，确保我们处理系统缓存
		 * 失效。XXX 这并不足够：原地更新者可能尚未到达
		 * AtEOXact_Inval()。参见 inplace-inval.spec 中的测试。
		 *
		 * 如果某个 heap_update() 刚释放了它的
		 * LOCKTAG_TUPLE，我们很可能会找到旧元组并进入
		 * "元组正被并发更新"。若 heap_update() 中止，
		 * 锁会阻塞原地更新。
		 */
		AcceptInvalidationMessages();
	}
}

/*
 * SearchSysCacheCopy
 *
 * 便利例程：执行 SearchSysCache 并（若成功）返回
 * 该系统缓存条目的可修改副本。原条目于返回
 * 前被释放。调用者用完结果后应对其调用
 * heap_freetuple()。
 */
HeapTuple
SearchSysCacheCopy(int cacheId,
				   Datum key1,
				   Datum key2,
				   Datum key3,
				   Datum key4)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return tuple;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}

/*
 * SearchSysCacheLockedCopy1
 *
 * 把 SearchSysCacheLockedCopy1 与 SearchSysCacheCopy() 融合
 * 起来。在调用者的 heap_update() 之后，应解锁元组并
 * heap_freetuple().
 */
HeapTuple
SearchSysCacheLockedCopy1(int cacheId,
						  Datum key1)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCacheLocked1(cacheId, key1);
	if (!HeapTupleIsValid(tuple))
		return tuple;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}

/*
 * SearchSysCacheExists
 *
 * 一个只探测能否找到元组的便利例程。
 * 系统缓存条目上不保留任何锁。
 */
bool
SearchSysCacheExists(int cacheId,
					 Datum key1,
					 Datum key2,
					 Datum key3,
					 Datum key4)
{
	HeapTuple	tuple;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return false;
	ReleaseSysCache(tuple);
	return true;
}

/*
 * GetSysCacheOid
 *
 * 一个便利例程：执行 SearchSysCache 并返回所找到元组
 * oidcol 列中的 OID，若找不到元组则返回 InvalidOid。
 * 系统缓存条目上不保留任何锁。
 */
Oid
GetSysCacheOid(int cacheId,
			   AttrNumber oidcol,
			   Datum key1,
			   Datum key2,
			   Datum key3,
			   Datum key4)
{
	HeapTuple	tuple;
	bool		isNull;
	Oid			result;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return InvalidOid;
	result = heap_getattr(tuple, oidcol,
						  SysCache[cacheId]->cc_tupdesc,
						  &isNull);
	Assert(!isNull);			/* 用作 oid 的列绝不应为 NULL */
	ReleaseSysCache(tuple);
	return result;
}


/*
 * SearchSysCacheAttName
 *
 * 此例程等价于对 ATTNAME 缓存执行 SearchSysCache，
 * 区别在于若找到的属性被标记为 attisdropped，它会
 * 返回 NULL。对希望表现得好像已删属性不存在
 * 的调用者来说很方便。
 */
HeapTuple
SearchSysCacheAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple;

	tuple = SearchSysCache2(ATTNAME,
							ObjectIdGetDatum(relid),
							CStringGetDatum(attname));
	if (!HeapTupleIsValid(tuple))
		return NULL;
	if (((Form_pg_attribute) GETSTRUCT(tuple))->attisdropped)
	{
		ReleaseSysCache(tuple);
		return NULL;
	}
	return tuple;
}

/*
 * SearchSysCacheCopyAttName
 *
 * 同上，SearchSysCacheCopy 的 attisdropped 感知版本。
 */
HeapTuple
SearchSysCacheCopyAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCacheAttName(relid, attname);
	if (!HeapTupleIsValid(tuple))
		return tuple;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}

/*
 * SearchSysCacheExistsAttName
 *
 * 同上，SearchSysCacheExists 的 attisdropped 感知版本。
 */
bool
SearchSysCacheExistsAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple;

	tuple = SearchSysCacheAttName(relid, attname);
	if (!HeapTupleIsValid(tuple))
		return false;
	ReleaseSysCache(tuple);
	return true;
}


/*
 * SearchSysCacheAttNum
 *
 * 此例程等价于对 ATTNUM 缓存执行 SearchSysCache，
 * 区别在于若找到的属性被标记为 attisdropped，它会
 * 返回 NULL。对希望表现得好像已删属性不存在
 * 的调用者来说很方便。
 */
HeapTuple
SearchSysCacheAttNum(Oid relid, int16 attnum)
{
	HeapTuple	tuple;

	tuple = SearchSysCache2(ATTNUM,
							ObjectIdGetDatum(relid),
							Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tuple))
		return NULL;
	if (((Form_pg_attribute) GETSTRUCT(tuple))->attisdropped)
	{
		ReleaseSysCache(tuple);
		return NULL;
	}
	return tuple;
}

/*
 * SearchSysCacheCopyAttNum
 *
 * 同上，SearchSysCacheCopy 的 attisdropped 感知版本。
 */
HeapTuple
SearchSysCacheCopyAttNum(Oid relid, int16 attnum)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCacheAttNum(relid, attnum);
	if (!HeapTupleIsValid(tuple))
		return NULL;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}


/*
 * SysCacheGetAttr
 *
 *		给定先前由 SearchSysCache() 抓取的元组，
 *		抽取指定的属性。
 *
 * 这等价于对从非缓存关系抓取的元组使用
 * heap_getattr()。通常它只用于可能为 NULL 或变长
 * 的属性；系统表中定长属性只需把元组映射到
 * include/catalog/ 中的 C 结构体声明
 * 即可。
 *
 * 与 heap_getattr() 一样，如果属性属于按引用传递的
 * 类型，则返回指向元组数据区的指针 --- 调用者绝不可
 * 修改或 pfree 该值！
 *
 * 注意：将引用同一目录另一个缓存的 cacheId 用于
 * SysCacheGetAttr() 也合法（元组抓取自该目录）。
 */
Datum
SysCacheGetAttr(int cacheId, HeapTuple tup,
				AttrNumber attributeNumber,
				bool *isNull)
{
	/*
	 * 我们只需从缓存条目中取出 TupleDesc，然后就能
	 * 应用 heap_getattr()。通常缓存控制数据已经有效（因为
	 * 调用者刚由同一缓存抓取了该元组），但有些情形
	 * 下我们必须在这里初始化缓存。
	 */
	if (cacheId < 0 || cacheId >= SysCacheSize ||
		!PointerIsValid(SysCache[cacheId]))
		elog(ERROR, "invalid cache ID: %d", cacheId);
	if (!PointerIsValid(SysCache[cacheId]->cc_tupdesc))
	{
		InitCatCachePhase2(SysCache[cacheId], false);
		Assert(PointerIsValid(SysCache[cacheId]->cc_tupdesc));
	}

	return heap_getattr(tup, attributeNumber,
						SysCache[cacheId]->cc_tupdesc,
						isNull);
}

/*
 * SysCacheGetAttrNotNull
 *
 * 同上，SysCacheGetAttr 的一个版本，它知道该属性不可为
 * NULL。
 */
Datum
SysCacheGetAttrNotNull(int cacheId, HeapTuple tup,
					   AttrNumber attributeNumber)
{
	bool		isnull;
	Datum		attr;

	attr = SysCacheGetAttr(cacheId, tup, attributeNumber, &isnull);

	if (isnull)
	{
		elog(ERROR,
			 "unexpected null value in cached tuple for catalog %s column %s",
			 get_rel_name(cacheinfo[cacheId].reloid),
			 NameStr(TupleDescAttr(SysCache[cacheId]->cc_tupdesc, attributeNumber - 1)->attname));
	}

	return attr;
}

/*
 * GetSysCacheHashValue
 *
 * 获取在指定缓存中具有给定搜索键的元组将会使用的
 * 哈希值。
 *
 * 之所以把它作为 API 的一部分公开，是因为哈希值会
 * 在缓存失效操作中被公开，所以 catcache 代码之外也有
 * 地方需要能够计算哈希值。
 */
uint32
GetSysCacheHashValue(int cacheId,
					 Datum key1,
					 Datum key2,
					 Datum key3,
					 Datum key4)
{
	if (cacheId < 0 || cacheId >= SysCacheSize ||
		!PointerIsValid(SysCache[cacheId]))
		elog(ERROR, "invalid cache ID: %d", cacheId);

	return GetCatCacheHashValue(SysCache[cacheId], key1, key2, key3, key4);
}

/*
 * 列表搜索接口
 */
struct catclist *
SearchSysCacheList(int cacheId, int nkeys,
				   Datum key1, Datum key2, Datum key3)
{
	if (cacheId < 0 || cacheId >= SysCacheSize ||
		!PointerIsValid(SysCache[cacheId]))
		elog(ERROR, "invalid cache ID: %d", cacheId);

	return SearchCatCacheList(SysCache[cacheId], nkeys,
							  key1, key2, key3);
}

/*
 * SysCacheInvalidate
 *
 *	根据给定的哈希值使指定缓存中的条目失效。
 *	详情见 CatCacheInvalidate()。
 *
 *	此例程只是准公开的：只应由 inval.c 使用。
 */
void
SysCacheInvalidate(int cacheId, uint32 hashValue)
{
	if (cacheId < 0 || cacheId >= SysCacheSize)
		elog(ERROR, "invalid cache ID: %d", cacheId);

	/* 若此缓存尚未初始化，则无需做任何事 */
	if (!PointerIsValid(SysCache[cacheId]))
		return;

	CatCacheInvalidate(SysCache[cacheId], hashValue);
}

/*
 * 某些没有系统缓存的关系会发送快照失效消息来代替
 * catcache 消息。这是为了 GetCatalogSnapshot() 的利益，
 * 这样它就可以复用现有的 MVCC 快照来扫描这些目录
 * 之一，而不是取新快照——只要尚未收到失效
 * 消息的话。
 *
 * 拥有系统缓存的关系不必（也不得）列在这里。catcache
 * 失效消息也会刷新快照。如果你为这些关系之一添加了
 * 系统缓存，就把它从该列表中移除。
 */
bool
RelationInvalidatesSnapshotsOnly(Oid relid)
{
	switch (relid)
	{
		case DbRoleSettingRelationId:
		case DependRelationId:
		case SharedDependRelationId:
		case DescriptionRelationId:
		case SharedDescriptionRelationId:
		case SecLabelRelationId:
		case SharedSecLabelRelationId:
			return true;
		default:
			break;
	}

	return false;
}

/*
 * 测试某关系是否拥有系统缓存。
 */
bool
RelationHasSysCache(Oid relid)
{
	int			low = 0,
				high = SysCacheRelationOidSize - 1;

	while (low <= high)
	{
		int			middle = low + (high - low) / 2;

		if (SysCacheRelationOid[middle] == relid)
			return true;
		if (SysCacheRelationOid[middle] < relid)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return false;
}

/*
 * 测试某关系是否支持系统缓存，即它要么是被缓存
 * 表，要么是缓存所用的索引。
 */
bool
RelationSupportsSysCache(Oid relid)
{
	int			low = 0,
				high = SysCacheSupportingRelOidSize - 1;

	while (low <= high)
	{
		int			middle = low + (high - low) / 2;

		if (SysCacheSupportingRelOid[middle] == relid)
			return true;
		if (SysCacheSupportingRelOid[middle] < relid)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return false;
}


/*
 * pg_qsort 的 OID 比较器
 */
static int
oid_compare(const void *a, const void *b)
{
	Oid			oa = *((const Oid *) a);
	Oid			ob = *((const Oid *) b);

	if (oa == ob)
		return 0;
	return (oa > ob) ? 1 : -1;
}
