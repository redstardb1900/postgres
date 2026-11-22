/*-------------------------------------------------------------------------
 *
 * acl.h
 *	  访问控制列表数据结构的定义（及支持）。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/acl.h
 *
 * 注
 *	  ACL 数组即 AclItem 数组，表示各条目所代表
 *	  特权的并集。零长度数组表示"无特权"。
 *	  见上。
 *
 *	  数组中条目次序很重要，因客户端工具（尤其
 *	  pg_dump，也可能有其他客户端）期望能按数组
 *	  条目次序发出 GRANT。原因是带 GRANT OPTION 的
 *	  GRANT 必须排在依赖它的 GRANT 之前。后端更新
 *	  ACL 时就地追加新项，且仅在无依赖时才删除
 *	  既有条目（其上不能再有 GRANT；若有则那些
 *	  GRANT 一并删除）。
 *	  见上。
 *	  见上。
 *
 *	  为向后兼容，系统目录须允许空 ACL 条目。空
 *	  ACL 视为"默认保护"（即 acldefault() 的返回
 *	  值）。
 *-------------------------------------------------------------------------
 */
#ifndef ACL_H
#define ACL_H

#include "access/htup.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "utils/snapshot.h"


/*
 * typedef AclMode 在 parsenodes.h 声明，各特权位
 * 含义也在那里定义
 */

#define ACL_ID_PUBLIC	0		/* PUBLIC acl 条目中 id 的占位 */

/*
 * AclItem
 *
 * 注意：各平台须同大小，因 pg_type.h 中 aclitem
 * 条目硬编码了该大小。
 */
typedef struct AclItem
{
	Oid			ai_grantee;		/* 本条目授予特权的目标 ID */
	Oid			ai_grantor;		/* 特权授予者 */
	AclMode		ai_privs;		/* 特权位 */
} AclItem;

/*
 * AclItem 的 ai_privs 字段高 32 位是授予选项位，
 * 低 32 位是实际特权。我们用"rights"表示授予选项
 * 与特权位字段的组合。
 */
#define ACLITEM_GET_PRIVS(item)    ((item).ai_privs & 0xFFFFFFFF)
#define ACLITEM_GET_GOPTIONS(item) (((item).ai_privs >> 32) & 0xFFFFFFFF)
#define ACLITEM_GET_RIGHTS(item)   ((item).ai_privs)

#define ACL_GRANT_OPTION_FOR(privs) (((AclMode) (privs) & 0xFFFFFFFF) << 32)
#define ACL_OPTION_TO_PRIVS(privs)	(((AclMode) (privs) >> 32) & 0xFFFFFFFF)

#define ACLITEM_SET_PRIVS(item,privs) \
  ((item).ai_privs = ((item).ai_privs & ~((AclMode) 0xFFFFFFFF)) | \
					 ((AclMode) (privs) & 0xFFFFFFFF))
#define ACLITEM_SET_GOPTIONS(item,goptions) \
  ((item).ai_privs = ((item).ai_privs & ~(((AclMode) 0xFFFFFFFF) << 32)) | \
					 (((AclMode) (goptions) & 0xFFFFFFFF) << 32))
#define ACLITEM_SET_RIGHTS(item,rights) \
  ((item).ai_privs = (AclMode) (rights))

#define ACLITEM_SET_PRIVS_GOPTIONS(item,privs,goptions) \
  ((item).ai_privs = ((AclMode) (privs) & 0xFFFFFFFF) | \
					 (((AclMode) (goptions) & 0xFFFFFFFF) << 32))


#define ACLITEM_ALL_PRIV_BITS		((AclMode) 0xFFFFFFFF)
#define ACLITEM_ALL_GOPTION_BITS	((AclMode) 0xFFFFFFFF << 32)

/*
 * 便于访问 Acl（AclItem 数组）的定义。
 * 这些是标准 PostgreSQL 数组，但限制为一维且无
 * 空值。读时忽略下界，写时设为 1。
 * 见上。
 *
 * 注意：自 PostgreSQL 7.1 起这些数组可 toast（与
 * 其他数组类型一样）。故请用所提供宏 detoast，
 * 除非确定某数组不可能已被 toast。
 * 见上。
 */


/*
 * Acl		一维 AclItem 数组
 */
typedef struct ArrayType Acl;

#define ACL_NUM(ACL)			(ARR_DIMS(ACL)[0])
#define ACL_DAT(ACL)			((AclItem *) ARR_DATA_PTR(ACL))
#define ACL_N_SIZE(N)			(ARR_OVERHEAD_NONULLS(1) + ((N) * sizeof(AclItem)))
#define ACL_SIZE(ACL)			ARR_SIZE(ACL)

/*
 * 这些类型的 fmgr 宏
 */
#define DatumGetAclItemP(X)		   ((AclItem *) DatumGetPointer(X))
#define PG_GETARG_ACLITEM_P(n)	   DatumGetAclItemP(PG_GETARG_DATUM(n))
#define PG_RETURN_ACLITEM_P(x)	   PG_RETURN_POINTER(x)

#define DatumGetAclP(X)			   ((Acl *) PG_DETOAST_DATUM(X))
#define DatumGetAclPCopy(X)		   ((Acl *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_ACL_P(n)		   DatumGetAclP(PG_GETARG_DATUM(n))
#define PG_GETARG_ACL_P_COPY(n)    DatumGetAclPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_ACL_P(x)		   PG_RETURN_POINTER(x)

/*
 * aclupdate 的 ACL 修改操作码
 */
#define ACL_MODECHG_ADD			1
#define ACL_MODECHG_DEL			2
#define ACL_MODECHG_EQL			3

/*
 * 特权位的外部表示 --- aclitemin/aclitemout
 * 用互不相同的单字符代码表示每个可能的特权位
 */
#define ACL_INSERT_CHR			'a' /* 旧称 "append" */
#define ACL_SELECT_CHR			'r' /* 旧称 "read" */
#define ACL_UPDATE_CHR			'w' /* 旧称 "write" */
#define ACL_DELETE_CHR			'd'
#define ACL_TRUNCATE_CHR		'D' /* 可谓超级删除 */
#define ACL_REFERENCES_CHR		'x'
#define ACL_TRIGGER_CHR			't'
#define ACL_EXECUTE_CHR			'X'
#define ACL_USAGE_CHR			'U'
#define ACL_CREATE_CHR			'C'
#define ACL_CREATE_TEMP_CHR		'T'
#define ACL_CONNECT_CHR			'c'
#define ACL_SET_CHR				's'
#define ACL_ALTER_SYSTEM_CHR	'A'

/* 按位掩码次序容纳全部特权代码字符的字符串 */
#define ACL_ALL_RIGHTS_STR	"arwdDxtXUCTcsA"

/*
 * 定义各支持对象类型的"全部权利"位掩码
 */
#define ACL_ALL_RIGHTS_COLUMN		(ACL_INSERT|ACL_SELECT|ACL_UPDATE|ACL_REFERENCES)
#define ACL_ALL_RIGHTS_RELATION		(ACL_INSERT|ACL_SELECT|ACL_UPDATE|ACL_DELETE|ACL_TRUNCATE|ACL_REFERENCES|ACL_TRIGGER)
#define ACL_ALL_RIGHTS_SEQUENCE		(ACL_USAGE|ACL_SELECT|ACL_UPDATE)
#define ACL_ALL_RIGHTS_DATABASE		(ACL_CREATE|ACL_CREATE_TEMP|ACL_CONNECT)
#define ACL_ALL_RIGHTS_FDW			(ACL_USAGE)
#define ACL_ALL_RIGHTS_FOREIGN_SERVER (ACL_USAGE)
#define ACL_ALL_RIGHTS_FUNCTION		(ACL_EXECUTE)
#define ACL_ALL_RIGHTS_LANGUAGE		(ACL_USAGE)
#define ACL_ALL_RIGHTS_LARGEOBJECT	(ACL_SELECT|ACL_UPDATE)
#define ACL_ALL_RIGHTS_PARAMETER_ACL (ACL_SET|ACL_ALTER_SYSTEM)
#define ACL_ALL_RIGHTS_SCHEMA		(ACL_USAGE|ACL_CREATE)
#define ACL_ALL_RIGHTS_TABLESPACE	(ACL_CREATE)
#define ACL_ALL_RIGHTS_TYPE			(ACL_USAGE)

/* pg_*_aclmask 的操作码 */
typedef enum
{
	ACLMASK_ALL,				/* 普通：计算全部位 */
	ACLMASK_ANY					/* 结果已知非零时返回 */
} AclMaskHow;

/* pg_*_aclcheck 的结果码 */
typedef enum
{
	ACLCHECK_OK = 0,
	ACLCHECK_NO_PRIV,
	ACLCHECK_NOT_OWNER
} AclResult;


/*
 * 内部使用的例程
 */
extern Acl *acldefault(ObjectType objtype, Oid ownerId);
extern Acl *get_user_default_acl(ObjectType objtype, Oid ownerId,
								 Oid nsp_oid);
extern void recordDependencyOnNewAcl(Oid classId, Oid objectId, int32 objsubId,
									 Oid ownerId, Acl *acl);

extern Acl *aclupdate(const Acl *old_acl, const AclItem *mod_aip,
					  int modechg, Oid ownerId, DropBehavior behavior);
extern Acl *aclnewowner(const Acl *old_acl, Oid oldOwnerId, Oid newOwnerId);
extern Acl *make_empty_acl(void);
extern Acl *aclcopy(const Acl *orig_acl);
extern Acl *aclconcat(const Acl *left_acl, const Acl *right_acl);
extern Acl *aclmerge(const Acl *left_acl, const Acl *right_acl, Oid ownerId);
extern void aclitemsort(Acl *acl);
extern bool aclequal(const Acl *left_acl, const Acl *right_acl);

extern AclMode aclmask(const Acl *acl, Oid roleid, Oid ownerId,
					   AclMode mask, AclMaskHow how);
extern int	aclmembers(const Acl *acl, Oid **roleids);

extern bool has_privs_of_role(Oid member, Oid role);
extern bool member_can_set_role(Oid member, Oid role);
extern void check_can_set_role(Oid member, Oid role);
extern bool is_member_of_role(Oid member, Oid role);
extern bool is_member_of_role_nosuper(Oid member, Oid role);
extern bool is_admin_of_role(Oid member, Oid role);
extern Oid	select_best_admin(Oid member, Oid role);
extern Oid	get_role_oid(const char *rolname, bool missing_ok);
extern Oid	get_role_oid_or_public(const char *rolname);
extern Oid	get_rolespec_oid(const RoleSpec *role, bool missing_ok);
extern void check_rolespec_name(const RoleSpec *role, const char *detail_msg);
extern HeapTuple get_rolespec_tuple(const RoleSpec *role);
extern char *get_rolespec_name(const RoleSpec *role);

extern void select_best_grantor(Oid roleId, AclMode privileges,
								const Acl *acl, Oid ownerId,
								Oid *grantorId, AclMode *grantOptions);

extern void initialize_acl(void);

/*
 * aclchk.c 中函数的原型
 */
extern void ExecuteGrantStmt(GrantStmt *stmt);
extern void ExecAlterDefaultPrivilegesStmt(ParseState *pstate, AlterDefaultPrivilegesStmt *stmt);

extern void RemoveRoleFromObjectACL(Oid roleid, Oid classid, Oid objid);

extern AclMode pg_class_aclmask(Oid table_oid, Oid roleid,
								AclMode mask, AclMaskHow how);

/* 通用函数 */
extern AclResult object_aclcheck(Oid classid, Oid objectid, Oid roleid, AclMode mode);

/* 特殊情形 */
extern AclResult pg_attribute_aclcheck(Oid table_oid, AttrNumber attnum,
									   Oid roleid, AclMode mode);
extern AclResult pg_attribute_aclcheck_ext(Oid table_oid, AttrNumber attnum,
										   Oid roleid, AclMode mode,
										   bool *is_missing);
extern AclResult pg_attribute_aclcheck_all(Oid table_oid, Oid roleid,
										   AclMode mode, AclMaskHow how);
extern AclResult pg_class_aclcheck(Oid table_oid, Oid roleid, AclMode mode);
extern AclResult pg_class_aclcheck_ext(Oid table_oid, Oid roleid,
									   AclMode mode, bool *is_missing);
extern AclResult pg_parameter_aclcheck(const char *name, Oid roleid,
									   AclMode mode);
extern AclResult pg_largeobject_aclcheck_snapshot(Oid lobj_oid, Oid roleid,
												  AclMode mode, Snapshot snapshot);

extern void aclcheck_error(AclResult aclerr, ObjectType objtype,
						   const char *objectname);

extern void aclcheck_error_col(AclResult aclerr, ObjectType objtype,
							   const char *objectname, const char *colname);

extern void aclcheck_error_type(AclResult aclerr, Oid typeOid);

extern void recordExtObjInitPriv(Oid objoid, Oid classoid);
extern void removeExtObjInitPriv(Oid objoid, Oid classoid);


/* ownercheck 仅返 true(所有者)/false */
extern bool object_ownercheck(Oid classid, Oid objectid, Oid roleid);
extern bool has_createrole_privilege(Oid roleid);
extern bool has_bypassrls_privilege(Oid roleid);

#endif							/* ACL_H */
