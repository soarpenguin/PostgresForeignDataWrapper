
#include <src/kv_utility.h>
#include "postgres.h"
#include "access/reloptions.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "funcapi.h"
#include "utils/rel.h"
#include "nodes/makefuncs.h"
#include "access/tuptoaster.h"
#include "catalog/pg_operator.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(kv_fdw_handler);
PG_FUNCTION_INFO_V1(kv_fdw_validator);


#define KVKEYJUNK "__key_junk"


/*
 * The plan state is set up in GetForeignRelSize and stashed away in
 * baserel->fdw_private and fetched in GetForeignPaths.
 */
typedef struct {
    void *db;
} TablePlanState;

/*
 * The scan state is for maintaining state for a scan, either for a
 * SELECT or UPDATE or DELETE.
 *
 * It is set up in BeginForeignScan and stashed in node->fdw_state and
 * subsequently used in IterateForeignScan, EndForeignScan and ReScanForeignScan.
 */
typedef struct {
    void *db;
    void *iter;
    bool isKeyBased;
    bool done;
    StringInfo key;
} TableReadState;

/*
 * The modify state is for maintaining state of modify operations.
 *
 * It is set up in BeginForeignModify and stashed in
 * rinfo->ri_FdwState and subsequently used in ExecForeignInsert,
 * ExecForeignUpdate, ExecForeignDelete and EndForeignModify.
 */
typedef struct {
    void *db;
    CmdType operation;
    AttrNumber keyJunkNo;
} TableWriteState;


static void GetForeignRelSize(PlannerInfo *root,
                              RelOptInfo *baserel,
                              Oid foreignTableId) {
    printf("\n-----------------GetForeignRelSize----------------------\n");
    /*
     * Obtain relation size estimates for a foreign table. This is called at
     * the beginning of planning for a query that scans a foreign table. root
     * is the planner's global information about the query; baserel is the
     * planner's information about this table; and foreigntableid is the
     * pg_class OID of the foreign table. (foreigntableid could be obtained
     * from the planner data structures, but it's passed explicitly to save
     * effort.)
     *
     * This function should update baserel->rows to be the expected number of
     * rows returned by the table scan, after accounting for the filtering
     * done by the restriction quals. The initial value of baserel->rows is
     * just a constant default estimate, which should be replaced if at all
     * possible. The function may also choose to update baserel->width if it
     * can compute a better estimate of the average result row width.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TablePlanState *planState = palloc0(sizeof(TablePlanState));

    FdwOptions *fdwOptions = KVGetOptions(foreignTableId);
    planState->db = Open(fdwOptions->filename);

    baserel->fdw_private = (void *) planState;

    /* TODO better estimation */
    baserel->rows = Count(planState->db);
}

static void GetForeignPaths(PlannerInfo *root,
                            RelOptInfo *baserel,
                            Oid foreignTableId) {
    printf("\n-----------------GetForeignPaths----------------------\n");
    /*
     * Create possible access paths for a scan on a foreign table. This is
     * called during query planning. The parameters are the same as for
     * GetForeignRelSize, which has already been called.
     *
     * This function must generate at least one access path (ForeignPath node)
     * for a scan on the foreign table and must call add_path to add each such
     * path to baserel->pathlist. It's recommended to use
     * create_foreignscan_path to build the ForeignPath nodes. The function
     * can generate multiple access paths, e.g., a path which has valid
     * pathkeys to represent a pre-sorted result. Each access path must
     * contain cost estimates, and can contain any FDW-private information
     * that is needed to identify the specific scan method intended.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    Cost startupCost = 0;
    Cost totalCost = startupCost + baserel->rows;

    /* Create a ForeignPath node and add it as only possible path */
    add_path(baserel,
             (Path *) create_foreignscan_path(root,
                                              baserel,
                                              NULL,  /* default pathtarget */
                                              baserel->rows,
                                              startupCost,
                                              totalCost,
                                              NIL,   /* no pathkeys */
                                              NULL,  /* no outer rel either */
                                              NULL,  /* no extra plan */
                                              NIL)); /* no fdw_private data */
}

static ForeignScan *GetForeignPlan(PlannerInfo *root,
                                   RelOptInfo *baserel,
                                   Oid foreignTableId,
                                   ForeignPath *bestPath,
                                   List *targetList,
                                   List *scanClauses,
                                   Plan *outerPlan) {
    printf("\n-----------------GetForeignPlan----------------------\n");
    /*
     * Create a ForeignScan plan node from the selected foreign access path.
     * This is called at the end of query planning. The parameters are as for
     * GetForeignRelSize, plus the selected ForeignPath (previously produced
     * by GetForeignPaths), the target list to be emitted by the plan node,
     * and the restriction clauses to be enforced by the plan node.
     *
     * This function must create and return a ForeignScan plan node; it's
     * recommended to use make_foreignscan to build the ForeignScan node.
     *
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    /*
     * We have no native ability to evaluate restriction clauses, so we just
     * put all the scan_clauses into the plan node's qual list for the
     * executor to check. So all we have to do here is strip RestrictInfo
     * nodes from the clauses and ignore pseudoconstants (which will be
     * handled elsewhere).
     */

    scanClauses = extract_actual_clauses(scanClauses, false);

    /*
     * Build the fdw_private list that will be available to the executor.
     */
    TablePlanState *planState = (TablePlanState *) baserel->fdw_private;
    List *fdwPrivate = list_make1(planState->db);

    /* Create the ForeignScan node */
    return make_foreignscan(targetList,
                            scanClauses,
                            baserel->relid,
                            NIL, /* no expressions to evaluate */
                            fdwPrivate,
                            NIL, /* no custom tlist */
                            NIL, /* no remote quals */
                            NULL);
}

static void SerializeAttribute(TupleDesc tupleDescriptor,
                               Index index,
                               Datum datum,
                               StringInfo buffer) {
    Form_pg_attribute attributeForm = TupleDescAttr(tupleDescriptor, index);
    bool byValue = attributeForm->attbyval;
    int typeLength = attributeForm->attlen;

    uint32 offset = buffer->len;
    uint32 datumLength = att_addlength_datum(offset, typeLength, datum);

    enlargeStringInfo(buffer, datumLength);

    char *current = buffer->data + buffer->len;
    memset(current, 0, datumLength - offset);

    if (typeLength > 0) {
        if (byValue) {
            store_att_byval(current, datum, typeLength);
        } else {
            memcpy(current, DatumGetPointer(datum), typeLength);
        }
    } else {
        memcpy(current, DatumGetPointer(datum), datumLength - offset);
    }

    buffer->len = datumLength;
}

static void GetKeyBasedQual(Node *node,
                            TupleDesc tupleDescriptor,
                            TableReadState *readState) {
    if (!node || !IsA(node, OpExpr)) {
        return;
    }

    OpExpr *op = (OpExpr *) node;
    if (list_length(op->args) != 2) {
        return;
    }

    Node *left = list_nth(op->args, 0);
    if (!IsA(left, Var)) {
        return;
    }

    Node *right = list_nth(op->args, 1);
    if (!IsA(right, Const)) {
        return;
    }

    Index varattno = ((Var *) left)->varattno;
    if (varattno != 1) {
        return;
    }

    /* get the name of the operator according to PG_OPERATOR OID */
    HeapTuple opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(op->opno));
    if (!HeapTupleIsValid(opertup)) {
        ereport(ERROR, (errmsg("cache lookup failed for operator %u", op->opno)));
    }
    Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
    char *oprname = NameStr(operform->oprname);
    if (strncmp(oprname, "=", NAMEDATALEN)) {
        ReleaseSysCache(opertup);
        return;
    }
    ReleaseSysCache(opertup);

    Const *constNode = ((Const *) right);
    Datum datum = constNode->constvalue;

    TypeCacheEntry *typeEntry = lookup_type_cache(constNode->consttype, 0);
    /* Make sure item to be inserted is not toasted */
    if (typeEntry->typlen == -1) {
        datum = PointerGetDatum(PG_DETOAST_DATUM_PACKED(datum));
    }

    if (typeEntry->typlen == -1 && typeEntry->typstorage != 'p' &&
        VARATT_CAN_MAKE_SHORT(datum)) {
        /* convert to short varlena -- no alignment */
        Pointer val = DatumGetPointer(datum);
        uint32 shortSize = VARATT_CONVERTED_SHORT_SIZE(val);
        Pointer temp = palloc0(shortSize);
        SET_VARSIZE_SHORT(temp, shortSize);
        memcpy(temp + 1, VARDATA(val), shortSize - 1);
        datum = PointerGetDatum(temp);
    }

    /*
     * We can push down this qual if:
     * - The operatory is =
     * - The qual is on the key column
     */
    readState->isKeyBased = true;
    readState->key = makeStringInfo();
    SerializeAttribute(tupleDescriptor, varattno-1, datum, readState->key);

    return;
}

static void BeginForeignScan(ForeignScanState *scanState, int executorFlags) {
    printf("\n-----------------BeginForeignScan----------------------\n");
    /*
     * Begin executing a foreign scan. This is called during executor startup.
     * It should perform any initialization needed before the scan can start,
     * but not start executing the actual scan (that should be done upon the
     * first call to IterateForeignScan). The ForeignScanState node has
     * already been created, but its fdw_state field is still NULL.
     * Information about the table to scan is accessible through the
     * ForeignScanState node (in particular, from the underlying ForeignScan
     * plan node, which contains any FDW-private information provided by
     * GetForeignPlan). eflags contains flag bits describing the executor's
     * operating mode for this plan node.
     *
     * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this function
     * should not perform any externally-visible actions; it should only do
     * the minimum required to make the node state valid for
     * ExplainForeignScan and EndForeignScan.
     *
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TableReadState *readState = palloc0(sizeof(TableReadState));

    ForeignScan *foreignScan = (ForeignScan *) scanState->ss.ps.plan;
    readState->db = list_nth((List *) foreignScan->fdw_private, 0);

    readState->iter = NULL;
    readState->isKeyBased = false;
    readState->done = false;
    readState->key = NULL;

    scanState->fdw_state = (void *) readState;

    /* must after readState is recorded, otherwise explain won't close db */
    if (executorFlags & EXEC_FLAG_EXPLAIN_ONLY) {
        return;
    }

    ListCell *lc;
    foreach (lc, scanState->ss.ps.plan->qual) {
        Expr *state = lfirst(lc);
        GetKeyBasedQual((Node *) state,
                        scanState->ss.ss_currentRelation->rd_att,
                        readState);
        if (readState->isKeyBased) {
            printf("\nkey_based_qual\n");
            break;
        }
    }

    if (!readState->isKeyBased) {
        readState->iter = GetIter(readState->db);
    }
}

static void DeserializeTuple(StringInfo key,
                             StringInfo value,
                             TupleTableSlot *tupleSlot) {

    Datum *values = tupleSlot->tts_values;
    bool *nulls = tupleSlot->tts_isnull;

    TupleDesc tupleDescriptor = tupleSlot->tts_tupleDescriptor;
    uint32 count = tupleDescriptor->natts;

    /* initialize all values for this row to null */
    memset(values, 0, count * sizeof(Datum));
    memset(nulls, false, count * sizeof(bool));

    uint32 bufLen = (count - 1 + 7) / 8;

    StringInfo buffer = makeStringInfo();
    enlargeStringInfo(buffer, bufLen);
    buffer->len = bufLen;

    memcpy(buffer->data, value->data, bufLen);

    for (uint32 index = 1; index < count; index++) {

        uint32 byteIndex = (index - 1) / 8;
        uint32 bitIndex = (index - 1) % 8;
        uint8 bitmask = (1 << bitIndex);
        nulls[index] = (buffer->data[byteIndex] & bitmask)? false: true;
    }

    uint32 offset = 0;
    char *current = key->data;
    for (uint32 index = 0; index < count; index++) {

        if (nulls[index]) {
            if (index == 0) {
                ereport(ERROR, (errmsg("first column cannot be null!")));
            }
            continue;
        }

        Form_pg_attribute attributeForm = TupleDescAttr(tupleDescriptor, index);
        bool byValue = attributeForm->attbyval;
        int typeLength = attributeForm->attlen;

        values[index] = fetch_att(current, byValue, typeLength);
        offset = att_addlength_datum(offset, typeLength, current);

        if (index == 0) {
            offset = bufLen;
        }
        current = value->data + offset;
    }
}

static TupleTableSlot *IterateForeignScan(ForeignScanState *scanState) {
    printf("\n-----------------IterateForeignScan----------------------\n");
    /*
     * Fetch one row from the foreign source, returning it in a tuple table
     * slot (the node's ScanTupleSlot should be used for this purpose). Return
     * NULL if no more rows are available. The tuple table slot infrastructure
     * allows either a physical or virtual tuple to be returned; in most cases
     * the latter choice is preferable from a performance standpoint. Note
     * that this is called in a short-lived memory context that will be reset
     * between invocations. Create a memory context in BeginForeignScan if you
     * need longer-lived storage, or use the es_query_cxt of the node's
     * EState.
     *
     * The rows returned must match the column signature of the foreign table
     * being scanned. If you choose to optimize away fetching columns that are
     * not needed, you should insert nulls in those column positions.
     *
     * Note that PostgreSQL's executor doesn't care whether the rows returned
     * violate any NOT NULL constraints that were defined on the foreign table
     * columns — but the planner does care, and may optimize queries
     * incorrectly if NULL values are present in a column declared not to
     * contain them. If a NULL value is encountered when the user has declared
     * that none should be present, it may be appropriate to raise an error
     * (just as you would need to do in the case of a data type mismatch).
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TupleTableSlot *tupleSlot = scanState->ss.ss_ScanTupleSlot;
    ExecClearTuple(tupleSlot);

    TableReadState *readState = (TableReadState *) scanState->fdw_state;
    char *k = NULL, *v = NULL;
    uint32 kLen = 0, vLen = 0;

    bool found = false;
    if (readState->isKeyBased) {
        if (!readState->done) {
            k = readState->key->data;
            kLen = readState->key->len;
            found = Get(readState->db, k, kLen, &v, &vLen);
            readState->done = true;
        }
    } else {
        found = Next(readState->db, readState->iter, &k, &kLen, &v, &vLen);
    }

    if (found) {
        StringInfo key = makeStringInfo();
        appendBinaryStringInfo(key, k, kLen);
        StringInfo value = makeStringInfo();
        appendBinaryStringInfo(value, v, vLen);

        DeserializeTuple(key, value, tupleSlot);

        ExecStoreVirtualTuple(tupleSlot);
    }

    return tupleSlot;
}

static void ReScanForeignScan(ForeignScanState *scanState) {
    printf("\n-----------------ReScanForeignScan----------------------\n");
    /*
     * Restart the scan from the beginning. Note that any parameters the scan
     * depends on may have changed value, so the new scan does not necessarily
     * return exactly the same rows.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));
}

static void EndForeignScan(ForeignScanState *scanState) {
    printf("\n-----------------EndForeignScan----------------------\n");
    /*
     * End the scan and release resources. It is normally not important to
     * release palloc'd memory, but for example open files and connections to
     * remote servers should be cleaned up.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TableReadState *readState = (TableReadState *) scanState->fdw_state;

    if (readState) {
        if (readState->iter) {
            DelIter(readState->iter);
            readState->iter = NULL;
        }

        if (readState->db) {
            Close(readState->db);
            readState->db = NULL;
        }
    }
}

static void AddForeignUpdateTargets(Query *parsetree,
                                    RangeTblEntry *tableEntry,
                                    Relation targetRelation) {
    printf("\n-----------------AddForeignUpdateTargets----------------------\n");
    /*
     * UPDATE and DELETE operations are performed against rows previously
     * fetched by the table-scanning functions. The FDW may need extra
     * information, such as a row ID or the values of primary-key columns, to
     * ensure that it can identify the exact row to update or delete. To
     * support that, this function can add extra hidden, or "junk", target
     * columns to the list of columns that are to be retrieved from the
     * foreign table during an UPDATE or DELETE.
     *
     * To do that, add TargetEntry items to parsetree->targetList, containing
     * expressions for the extra values to be fetched. Each such entry must be
     * marked resjunk = true, and must have a distinct resname that will
     * identify it at execution time. Avoid using names matching ctidN or
     * wholerowN, as the core system can generate junk columns of these names.
     *
     * This function is called in the rewriter, not the planner, so the
     * information available is a bit different from that available to the
     * planning routines. parsetree is the parse tree for the UPDATE or DELETE
     * command, while target_rte and target_relation describe the target
     * foreign table.
     *
     * If the AddForeignUpdateTargets pointer is set to NULL, no extra target
     * expressions are added. (This will make it impossible to implement
     * DELETE operations, though UPDATE may still be feasible if the FDW
     * relies on an unchanging primary key to identify rows.)
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    Form_pg_attribute attr = &RelationGetDescr(targetRelation)->attrs[0];

    /*
     * Code adapted from redis_fdw
     *
     * In KV, we need the key name. It's the first column in the table
     * regardless of the table type. Knowing the key, we can delete it.
     */
    Var *var = makeVar(parsetree->resultRelation,
                       1,
                       attr->atttypid,
                       attr->atttypmod,
                       InvalidOid,
                       0);
    /* Wrap it in a resjunk TLE with the right name ... */
    const char *attrname = KVKEYJUNK;
    AttrNumber resno = list_length(parsetree->targetList) + 1;
    /* is this true? */
    Assert(resno == 1);
    TargetEntry *entry = makeTargetEntry((Expr *) var,
                                         resno,
                                         pstrdup(attrname),
                                         true);
    /* ... and add it to the query's targetlist */
    parsetree->targetList = lappend(parsetree->targetList, entry);
}

static List *PlanForeignModify(PlannerInfo *plannerInfo,
                               ModifyTable *plan,
                               Index resultRelation,
                               int subplanIndex) {
    printf("\n-----------------PlanForeignModify----------------------\n");
    /*
     * Perform any additional planning actions needed for an insert, update,
     * or delete on a foreign table. This function generates the FDW-private
     * information that will be attached to the ModifyTable plan node that
     * performs the update action. This private information must have the form
     * of a List, and will be delivered to BeginForeignModify during the
     * execution stage.
     *
     * root is the planner's global information about the query. plan is the
     * ModifyTable plan node, which is complete except for the fdwPrivLists
     * field. resultRelation identifies the target foreign table by its
     * rangetable index. subplan_index identifies which target of the
     * ModifyTable plan node this is, counting from zero; use this if you want
     * to index into plan->plans or other substructure of the plan node.
     *
     * If the PlanForeignModify pointer is set to NULL, no additional
     * plan-time actions are taken, and the fdw_private list delivered to
     * BeginForeignModify will be NIL.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    CmdType operation = plan->operation;

    if (operation == CMD_UPDATE || operation == CMD_DELETE) {
        RelOptInfo *baserel = plannerInfo->simple_rel_array[resultRelation];
        return list_make1(baserel->fdw_private);
    }

    return NULL;
}

static void BeginForeignModify(ModifyTableState *modifyTableState,
                               ResultRelInfo *relationInfo,
                               List *fdwPrivate,
                               int subplanIndex,
                               int executorFlags) {
    printf("\n-----------------BeginForeignModify----------------------\n");
    /*
     * Begin executing a foreign table modification operation. This routine is
     * called during executor startup. It should perform any initialization
     * needed prior to the actual table modifications. Subsequently,
     * ExecForeignInsert, ExecForeignUpdate or ExecForeignDelete will be
     * called for each tuple to be inserted, updated, or deleted.
     *
     * mtstate is the overall state of the ModifyTable plan node being
     * executed; global data about the plan and execution state is available
     * via this structure. rinfo is the ResultRelInfo struct describing the
     * target foreign table. (The ri_FdwState field of ResultRelInfo is
     * available for the FDW to store any private state it needs for this
     * operation.) fdw_private contains the private data generated by
     * PlanForeignModify, if any. subplan_index identifies which target of the
     * ModifyTable plan node this is. eflags contains flag bits describing the
     * executor's operating mode for this plan node.
     *
     * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this function
     * should not perform any externally-visible actions; it should only do
     * the minimum required to make the node state valid for
     * ExplainForeignModify and EndForeignModify.
     *
     * If the BeginForeignModify pointer is set to NULL, no action is taken
     * during executor startup.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    if (executorFlags & EXEC_FLAG_EXPLAIN_ONLY) {
        return;
    }

    TableWriteState *writeState = palloc0(sizeof(TableWriteState));

    CmdType operation = modifyTableState->operation;
    writeState->operation = operation;

    Relation relation = relationInfo->ri_RelationDesc;

    if (operation == CMD_UPDATE || operation == CMD_DELETE) {
        TablePlanState *planState = (TablePlanState *) list_nth(fdwPrivate, 0);
        writeState->db = planState->db;

    } else if (operation == CMD_INSERT) {
        Oid foreignTableId = RelationGetRelid(relation);
        FdwOptions *fdwOptions = KVGetOptions(foreignTableId);
        writeState->db = Open(fdwOptions->filename);

    } else {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("not insert, update & delete")));
    }

    if (operation == CMD_DELETE) {
        /* Find the ctid resjunk column in the subplan's result */
        Plan *subplan = modifyTableState->mt_plans[subplanIndex]->plan;
        writeState->keyJunkNo =
                ExecFindJunkAttributeInTlist(subplan->targetlist, KVKEYJUNK);
        if (!AttributeNumberIsValid(writeState->keyJunkNo)) {
            ereport(ERROR, (errmsg("could not find key junk column")));
        }
    }

    relationInfo->ri_FdwState = (void *) writeState;
}

static void SerializeTuple(StringInfo key,
                           StringInfo value,
                           TupleTableSlot *tupleSlot) {

    TupleDesc tupleDescriptor = tupleSlot->tts_tupleDescriptor;
    uint32 count = tupleDescriptor->natts;

    uint32 nullsLen = (count - 1 + 7) / 8;

    /*
     * contrary to isnull array, store exists array
     * to accommodate bug from storage engine
     */
    StringInfo nulls = makeStringInfo();
    enlargeStringInfo(nulls, nullsLen);
    nulls->len = nullsLen;
    memset(nulls->data, 0xFF, nullsLen);

    value->len += nullsLen;

    for (uint32 index = 0; index < count; index++) {

        if (tupleSlot->tts_isnull[index]) {
            if (index == 0) {
                ereport(ERROR, (errmsg("first column cannot be null!")));
            }
            uint32 byteIndex = (index - 1) / 8;
            uint32 bitIndex = (index - 1) % 8;
            uint8 bitmask = (1 << bitIndex);
            nulls->data[byteIndex] &= ~bitmask;
            continue;
        }

        Datum datum = tupleSlot->tts_values[index];
        SerializeAttribute(tupleDescriptor, index, datum, index==0? key: value);
    }

    memcpy(value->data, nulls->data, nullsLen);
}

static TupleTableSlot *ExecForeignInsert(EState *executorState,
                                         ResultRelInfo *relationInfo,
                                         TupleTableSlot *tupleSlot,
                                         TupleTableSlot *planSlot) {
    printf("\n-----------------ExecForeignInsert----------------------\n");
    /*
     * Insert one tuple into the foreign table. estate is global execution
     * state for the query. rinfo is the ResultRelInfo struct describing the
     * target foreign table. slot contains the tuple to be inserted; it will
     * match the rowtype definition of the foreign table. planSlot contains
     * the tuple that was generated by the ModifyTable plan node's subplan; it
     * differs from slot in possibly containing additional "junk" columns.
     * (The planSlot is typically of little interest for INSERT cases, but is
     * provided for completeness.)
     *
     * The return value is either a slot containing the data that was actually
     * inserted (this might differ from the data supplied, for example as a
     * result of trigger actions), or NULL if no row was actually inserted
     * (again, typically as a result of triggers). The passed-in slot can be
     * re-used for this purpose.
     *
     * The data in the returned slot is used only if the INSERT query has a
     * RETURNING clause. Hence, the FDW could choose to optimize away
     * returning some or all columns depending on the contents of the
     * RETURNING clause. However, some slot must be returned to indicate
     * success, or the query's reported rowcount will be wrong.
     *
     * If the ExecForeignInsert pointer is set to NULL, attempts to insert
     * into the foreign table will fail with an error message.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TupleDesc tupleDescriptor = tupleSlot->tts_tupleDescriptor;
    if (HeapTupleHasExternal(tupleSlot->tts_tuple)) {
        /* detoast any toasted attributes */
        tupleSlot->tts_tuple = toast_flatten_tuple(tupleSlot->tts_tuple,
                                                   tupleDescriptor);
    }

    slot_getallattrs(tupleSlot);

    StringInfo key = makeStringInfo();
    StringInfo value = makeStringInfo();

    SerializeTuple(key, value, tupleSlot);

    TableWriteState *writeState = (TableWriteState *) relationInfo->ri_FdwState;

    /* copy command may directly call insert without open db */
    bool CMD_COPY = false;
    if (!writeState) {
        CMD_COPY = true;
        writeState = palloc0(sizeof(TableWriteState));

        Oid foreignTableId = RelationGetRelid(relationInfo->ri_RelationDesc);
        FdwOptions *fdwOptions = KVGetOptions(foreignTableId);
        writeState->db = Open(fdwOptions->filename);
    }

    if (!Put(writeState->db, key->data, key->len, value->data, value->len)) {
        ereport(ERROR, (errmsg("error from ExecForeignInsert")));
    }

    /*
     * immediately release resource to prevent conflicts,
     * suffer performance penalty due to close and open.
     */
    if (CMD_COPY) {
        Close(writeState->db);
        pfree(writeState);
    }

    return tupleSlot;
}

static TupleTableSlot *ExecForeignUpdate(EState *executorState,
                                         ResultRelInfo *relationInfo,
                                         TupleTableSlot *tupleSlot,
                                         TupleTableSlot *planSlot) {
    printf("\n-----------------ExecForeignUpdate----------------------\n");
    /*
     * Update one tuple in the foreign table. estate is global execution state
     * for the query. rinfo is the ResultRelInfo struct describing the target
     * foreign table. slot contains the new data for the tuple; it will match
     * the rowtype definition of the foreign table. planSlot contains the
     * tuple that was generated by the ModifyTable plan node's subplan; it
     * differs from slot in possibly containing additional "junk" columns. In
     * particular, any junk columns that were requested by
     * AddForeignUpdateTargets will be available from this slot.
     *
     * The return value is either a slot containing the row as it was actually
     * updated (this might differ from the data supplied, for example as a
     * result of trigger actions), or NULL if no row was actually updated
     * (again, typically as a result of triggers). The passed-in slot can be
     * re-used for this purpose.
     *
     * The data in the returned slot is used only if the UPDATE query has a
     * RETURNING clause. Hence, the FDW could choose to optimize away
     * returning some or all columns depending on the contents of the
     * RETURNING clause. However, some slot must be returned to indicate
     * success, or the query's reported rowcount will be wrong.
     *
     * If the ExecForeignUpdate pointer is set to NULL, attempts to update the
     * foreign table will fail with an error message.
     *
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TupleDesc tupleDescriptor = tupleSlot->tts_tupleDescriptor;
    if (HeapTupleHasExternal(tupleSlot->tts_tuple)) {
        /* detoast any toasted attributes */
        tupleSlot->tts_tuple = toast_flatten_tuple(tupleSlot->tts_tuple,
                                                   tupleDescriptor);
    }
    slot_getallattrs(tupleSlot);

    StringInfo key = makeStringInfo();
    StringInfo value = makeStringInfo();

    SerializeTuple(key, value, tupleSlot);

    TableWriteState *writeState = (TableWriteState *) relationInfo->ri_FdwState;
    if (!Put(writeState->db, key->data, key->len, value->data, value->len)) {
        ereport(ERROR, (errmsg("error from ExecForeignUpdate")));
    }

    return tupleSlot;
}

static TupleTableSlot *ExecForeignDelete(EState *executorState,
                                         ResultRelInfo *relationInfo,
                                         TupleTableSlot *tupleSlot,
                                         TupleTableSlot *planSlot) {
    printf("\n-----------------ExecForeignDelete----------------------\n");
    /*
     * Delete one tuple from the foreign table. estate is global execution
     * state for the query. rinfo is the ResultRelInfo struct describing the
     * target foreign table. slot contains nothing useful upon call, but can
     * be used to hold the returned tuple. planSlot contains the tuple that
     * was generated by the ModifyTable plan node's subplan; in particular, it
     * will carry any junk columns that were requested by
     * AddForeignUpdateTargets. The junk column(s) must be used to identify
     * the tuple to be deleted.
     *
     * The return value is either a slot containing the row that was deleted,
     * or NULL if no row was deleted (typically as a result of triggers). The
     * passed-in slot can be used to hold the tuple to be returned.
     *
     * The data in the returned slot is used only if the DELETE query has a
     * RETURNING clause. Hence, the FDW could choose to optimize away
     * returning some or all columns depending on the contents of the
     * RETURNING clause. However, some slot must be returned to indicate
     * success, or the query's reported rowcount will be wrong.
     *
     * If the ExecForeignDelete pointer is set to NULL, attempts to delete
     * from the foreign table will fail with an error message.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TableWriteState *writeState = (TableWriteState *) relationInfo->ri_FdwState;

    bool isnull = true;
    ExecGetJunkAttribute(planSlot, writeState->keyJunkNo, &isnull);
    if (isnull) {
        ereport(ERROR, (errmsg("can't get junk key value")));
    }

    slot_getallattrs(planSlot);

    StringInfo key = makeStringInfo();
    StringInfo value = makeStringInfo();

    SerializeTuple(key, value, planSlot);

    if (!Delete(writeState->db, key->data, key->len)) {
        ereport(ERROR, (errmsg("error from ExecForeignDelete")));
    }

    return tupleSlot;
}

static void EndForeignModify(EState *executorState, ResultRelInfo *relationInfo) {
    printf("\n-----------------EndForeignModify----------------------\n");
    /*
     * End the table update and release resources. It is normally not
     * important to release palloc'd memory, but for example open files and
     * connections to remote servers should be cleaned up.
     *
     * If the EndForeignModify pointer is set to NULL, no action is taken
     * during executor shutdown.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TableWriteState *writeState = (TableWriteState *) relationInfo->ri_FdwState;

    if (writeState) {
        CmdType operation = writeState->operation;
        if (operation == CMD_INSERT && writeState->db) {
            Close(writeState->db);
        }

        /* CMD_UPDATE and CMD_DELETE close will be taken care of by endScan */
        writeState->db = NULL;
    }
}

static void ExplainForeignScan(ForeignScanState *scanState,
                               struct ExplainState * explainState) {
    printf("\n-----------------ExplainForeignScan----------------------\n");
    /*
     * Print additional EXPLAIN output for a foreign table scan. This function
     * can call ExplainPropertyText and related functions to add fields to the
     * EXPLAIN output. The flag fields in es can be used to determine what to
     * print, and the state of the ForeignScanState node can be inspected to
     * provide run-time statistics in the EXPLAIN ANALYZE case.
     *
     * If the ExplainForeignScan pointer is set to NULL, no additional
     * information is printed during EXPLAIN.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));
}

static void ExplainForeignModify(ModifyTableState *modifyTableState,
                                 ResultRelInfo *relationInfo,
                                 List *fdwPrivate,
                                 int subplanIndex,
                                 struct ExplainState *explainState) {
    printf("\n-----------------ExplainForeignModify----------------------\n");
    /*
     * Print additional EXPLAIN output for a foreign table update. This
     * function can call ExplainPropertyText and related functions to add
     * fields to the EXPLAIN output. The flag fields in es can be used to
     * determine what to print, and the state of the ModifyTableState node can
     * be inspected to provide run-time statistics in the EXPLAIN ANALYZE
     * case. The first four arguments are the same as for BeginForeignModify.
     *
     * If the ExplainForeignModify pointer is set to NULL, no additional
     * information is printed during EXPLAIN.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));
}

static bool AnalyzeForeignTable(Relation relation,
                                AcquireSampleRowsFunc *acquireSampleRowsFunc,
                                BlockNumber *totalPageCount) {
    printf("\n-----------------AnalyzeForeignTable----------------------\n");
    /* ----
     * This function is called when ANALYZE is executed on a foreign table. If
     * the FDW can collect statistics for this foreign table, it should return
     * true, and provide a pointer to a function that will collect sample rows
     * from the table in func, plus the estimated size of the table in pages
     * in totalpages. Otherwise, return false.
     *
     * If the FDW does not support collecting statistics for any tables, the
     * AnalyzeForeignTable pointer can be set to NULL.
     *
     * If provided, the sample collection function must have the signature:
     *
     *	  int
     *	  AcquireSampleRowsFunc (Relation relation, int elevel,
     *							 HeapTuple *rows, int targrows,
     *							 double *totalrows,
     *							 double *totaldeadrows);
     *
     * A random sample of up to targrows rows should be collected from the
     * table and stored into the caller-provided rows array. The actual number
     * of rows collected must be returned. In addition, store estimates of the
     * total numbers of live and dead rows in the table into the output
     * parameters totalrows and totaldeadrows. (Set totaldeadrows to zero if
     * the FDW does not have any concept of dead rows.)
     * ----
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    return false;
}

Datum kv_fdw_handler(PG_FUNCTION_ARGS) {
    printf("\n-----------------fdw_handler----------------------\n");
    FdwRoutine *fdwRoutine = makeNode(FdwRoutine);

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    /*
     * assign the handlers for the FDW
     *
     * This function might be called a number of times. In particular, it is
     * likely to be called for each INSERT statement. For an explanation, see
     * core postgres file src/optimizer/plan/createplan.c where it calls
     * GetFdwRoutineByRelId(().
     */

    /* Required by notations: S=SELECT I=INSERT U=UPDATE D=DELETE */

    /* these are required */
    fdwRoutine->GetForeignRelSize = GetForeignRelSize; /* S U D */
    fdwRoutine->GetForeignPaths = GetForeignPaths; /* S U D */
    fdwRoutine->GetForeignPlan = GetForeignPlan; /* S U D */
    fdwRoutine->BeginForeignScan = BeginForeignScan; /* S U D */
    fdwRoutine->IterateForeignScan = IterateForeignScan; /* S */
    fdwRoutine->ReScanForeignScan = ReScanForeignScan; /* S */
    fdwRoutine->EndForeignScan = EndForeignScan; /* S U D */

    /* remainder are optional - use NULL if not required */
    /* support for insert / update / delete */
    fdwRoutine->AddForeignUpdateTargets = AddForeignUpdateTargets; /* U D */
    fdwRoutine->PlanForeignModify = PlanForeignModify; /* I U D */
    fdwRoutine->BeginForeignModify = BeginForeignModify; /* I U D */
    fdwRoutine->ExecForeignInsert = ExecForeignInsert; /* I */
    fdwRoutine->ExecForeignUpdate = ExecForeignUpdate; /* U */
    fdwRoutine->ExecForeignDelete = ExecForeignDelete; /* D */
    fdwRoutine->EndForeignModify = EndForeignModify; /* I U D */

    /* support for EXPLAIN */
    fdwRoutine->ExplainForeignScan = ExplainForeignScan; /* EXPLAIN S U D */
    fdwRoutine->ExplainForeignModify = ExplainForeignModify; /* EXPLAIN I U D */

    /* support for ANALYSE */
    fdwRoutine->AnalyzeForeignTable = AnalyzeForeignTable; /* ANALYZE only */

    PG_RETURN_POINTER(fdwRoutine);
}

Datum kv_fdw_validator(PG_FUNCTION_ARGS) {
    printf("\n-----------------fdw_validator----------------------\n");
    List *options_list = untransformRelOptions(PG_GETARG_DATUM(0));

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    /* make sure the options are valid */

    /* no options are supported */

    if (list_length(options_list) > 0) {
        ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                        errmsg("invalid options"),
                        errhint("FDW does not support any options")));
    }

    PG_RETURN_VOID();
}
