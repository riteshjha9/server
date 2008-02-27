/******************************************************
Update of a row

(c) 1996 Innobase Oy

Created 12/27/1996 Heikki Tuuri
*******************************************************/

#include "row0upd.h"

#ifdef UNIV_NONINL
#include "row0upd.ic"
#endif

#include "dict0dict.h"
#include "dict0boot.h"
#include "dict0crea.h"
#include "mach0data.h"
#include "trx0undo.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "que0que.h"
#include "row0ext.h"
#include "row0ins.h"
#include "row0sel.h"
#include "row0row.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "log0log.h"
#include "pars0sym.h"
#include "eval0eval.h"
#include "buf0lru.h"


/* What kind of latch and lock can we assume when the control comes to
   -------------------------------------------------------------------
an update node?
--------------
Efficiency of massive updates would require keeping an x-latch on a
clustered index page through many updates, and not setting an explicit
x-lock on clustered index records, as they anyway will get an implicit
x-lock when they are updated. A problem is that the read nodes in the
graph should know that they must keep the latch when passing the control
up to the update node, and not set any record lock on the record which
will be updated. Another problem occurs if the execution is stopped,
as the kernel switches to another query thread, or the transaction must
wait for a lock. Then we should be able to release the latch and, maybe,
acquire an explicit x-lock on the record.
	Because this seems too complicated, we conclude that the less
efficient solution of releasing all the latches when the control is
transferred to another node, and acquiring explicit x-locks, is better. */

/* How is a delete performed? If there is a delete without an
explicit cursor, i.e., a searched delete, there are at least
two different situations:
the implicit select cursor may run on (1) the clustered index or
on (2) a secondary index. The delete is performed by setting
the delete bit in the record and substituting the id of the
deleting transaction for the original trx id, and substituting a
new roll ptr for previous roll ptr. The old trx id and roll ptr
are saved in the undo log record. Thus, no physical changes occur
in the index tree structure at the time of the delete. Only
when the undo log is purged, the index records will be physically
deleted from the index trees.

The query graph executing a searched delete would consist of
a delete node which has as a subtree a select subgraph.
The select subgraph should return a (persistent) cursor
in the clustered index, placed on page which is x-latched.
The delete node should look for all secondary index records for
this clustered index entry and mark them as deleted. When is
the x-latch freed? The most efficient way for performing a
searched delete is obviously to keep the x-latch for several
steps of query graph execution. */

/***************************************************************
Checks if an update vector changes some of the first ordering fields of an
index record. This is only used in foreign key checks and we can assume
that index does not contain column prefixes. */
static
ibool
row_upd_changes_first_fields_binary(
/*================================*/
				/* out: TRUE if changes */
	dtuple_t*	entry,	/* in: old value of index entry */
	dict_index_t*	index,	/* in: index of entry */
	const upd_t*	update,	/* in: update vector for the row */
	ulint		n);	/* in: how many first fields to check */


/*************************************************************************
Checks if index currently is mentioned as a referenced index in a foreign
key constraint. */
static
ibool
row_upd_index_is_referenced(
/*========================*/
				/* out: TRUE if referenced; NOTE that since
				we do not hold dict_operation_lock
				when leaving the function, it may be that
				the referencing table has been dropped when
				we leave this function: this function is only
				for heuristic use! */
	dict_index_t*	index,	/* in: index */
	trx_t*		trx)	/* in: transaction */
{
	dict_table_t*	table		= index->table;
	dict_foreign_t*	foreign;
	ibool		froze_data_dict	= FALSE;
	ibool		is_referenced	= FALSE;

	if (!UT_LIST_GET_FIRST(table->referenced_list)) {

		return(FALSE);
	}

	if (trx->dict_operation_lock_mode == 0) {
		row_mysql_freeze_data_dictionary(trx);
		froze_data_dict = TRUE;
	}

	foreign = UT_LIST_GET_FIRST(table->referenced_list);

	while (foreign) {
		if (foreign->referenced_index == index) {

			is_referenced = TRUE;
			goto func_exit;
		}

		foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
	}

func_exit:
	if (froze_data_dict) {
		row_mysql_unfreeze_data_dictionary(trx);
	}

	return(is_referenced);
}

/*************************************************************************
Checks if possible foreign key constraints hold after a delete of the record
under pcur. NOTE that this function will temporarily commit mtr and lose the
pcur position! */
static
ulint
row_upd_check_references_constraints(
/*=================================*/
				/* out: DB_SUCCESS or an error code */
	upd_node_t*	node,	/* in: row update node */
	btr_pcur_t*	pcur,	/* in: cursor positioned on a record; NOTE: the
				cursor position is lost in this function! */
	dict_table_t*	table,	/* in: table in question */
	dict_index_t*	index,	/* in: index of the cursor */
	ulint*		offsets,/* in/out: rec_get_offsets(pcur.rec, index) */
	que_thr_t*	thr,	/* in: query thread */
	mtr_t*		mtr)	/* in: mtr */
{
	dict_foreign_t*	foreign;
	mem_heap_t*	heap;
	dtuple_t*	entry;
	trx_t*		trx;
	const rec_t*	rec;
	ulint		n_ext;
	ulint		err;
	ibool		got_s_lock	= FALSE;

	if (UT_LIST_GET_FIRST(table->referenced_list) == NULL) {

		return(DB_SUCCESS);
	}

	trx = thr_get_trx(thr);

	rec = btr_pcur_get_rec(pcur);
	ut_ad(rec_offs_validate(rec, index, offsets));

	heap = mem_heap_create(500);

	entry = row_rec_to_index_entry(ROW_COPY_DATA, rec, index, offsets,
				       &n_ext, heap);

	mtr_commit(mtr);

	mtr_start(mtr);

	if (trx->dict_operation_lock_mode == 0) {
		got_s_lock = TRUE;

		row_mysql_freeze_data_dictionary(trx);
	}

	foreign = UT_LIST_GET_FIRST(table->referenced_list);

	while (foreign) {
		/* Note that we may have an update which updates the index
		record, but does NOT update the first fields which are
		referenced in a foreign key constraint. Then the update does
		NOT break the constraint. */

		if (foreign->referenced_index == index
		    && (node->is_delete
			|| row_upd_changes_first_fields_binary(
				entry, index, node->update,
				foreign->n_fields))) {

			if (foreign->foreign_table == NULL) {
				dict_table_get(foreign->foreign_table_name,
					       FALSE);
			}

			if (foreign->foreign_table) {
				mutex_enter(&(dict_sys->mutex));

				(foreign->foreign_table
				 ->n_foreign_key_checks_running)++;

				mutex_exit(&(dict_sys->mutex));
			}

			/* NOTE that if the thread ends up waiting for a lock
			we will release dict_operation_lock temporarily!
			But the counter on the table protects 'foreign' from
			being dropped while the check is running. */

			err = row_ins_check_foreign_constraint(
				FALSE, foreign, table, entry, thr);

			if (foreign->foreign_table) {
				mutex_enter(&(dict_sys->mutex));

				ut_a(foreign->foreign_table
				     ->n_foreign_key_checks_running > 0);

				(foreign->foreign_table
				 ->n_foreign_key_checks_running)--;

				mutex_exit(&(dict_sys->mutex));
			}

			if (err != DB_SUCCESS) {

				goto func_exit;
			}
		}

		foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
	}

	err = DB_SUCCESS;

func_exit:
	if (got_s_lock) {
		row_mysql_unfreeze_data_dictionary(trx);
	}

	mem_heap_free(heap);

	return(err);
}

/*************************************************************************
Creates an update node for a query graph. */
UNIV_INTERN
upd_node_t*
upd_node_create(
/*============*/
				/* out, own: update node */
	mem_heap_t*	heap)	/* in: mem heap where created */
{
	upd_node_t*	node;

	node = mem_heap_alloc(heap, sizeof(upd_node_t));
	node->common.type = QUE_NODE_UPDATE;

	node->state = UPD_NODE_UPDATE_CLUSTERED;
	node->select_will_do_update = FALSE;
	node->in_mysql_interface = FALSE;

	node->row = NULL;
	node->ext = NULL;
	node->upd_row = NULL;
	node->upd_ext = NULL;
	node->index = NULL;
	node->update = NULL;

	node->foreign = NULL;
	node->cascade_heap = NULL;
	node->cascade_node = NULL;

	node->select = NULL;

	node->heap = mem_heap_create(128);
	node->magic_n = UPD_NODE_MAGIC_N;

	node->cmpl_info = 0;

	return(node);
}

/*************************************************************************
Updates the trx id and roll ptr field in a clustered index record in database
recovery. */
UNIV_INTERN
void
row_upd_rec_sys_fields_in_recovery(
/*===============================*/
	rec_t*		rec,	/* in/out: record */
	page_zip_des_t*	page_zip,/* in/out: compressed page, or NULL */
	const ulint*	offsets,/* in: array returned by rec_get_offsets() */
	ulint		pos,	/* in: TRX_ID position in rec */
	dulint		trx_id,	/* in: transaction id */
	dulint		roll_ptr)/* in: roll ptr of the undo log record */
{
	ut_ad(rec_offs_validate(rec, NULL, offsets));

	if (UNIV_LIKELY_NULL(page_zip)) {
		page_zip_write_trx_id_and_roll_ptr(
			page_zip, rec, offsets, pos, trx_id, roll_ptr);
	} else {
		byte*	field;
		ulint	len;

		field = rec_get_nth_field(rec, offsets, pos, &len);
		ut_ad(len == DATA_TRX_ID_LEN);
#if DATA_TRX_ID + 1 != DATA_ROLL_PTR
# error "DATA_TRX_ID + 1 != DATA_ROLL_PTR"
#endif
		trx_write_trx_id(field, trx_id);
		trx_write_roll_ptr(field + DATA_TRX_ID_LEN, roll_ptr);
	}
}

/*************************************************************************
Sets the trx id or roll ptr field of a clustered index entry. */
UNIV_INTERN
void
row_upd_index_entry_sys_field(
/*==========================*/
	const dtuple_t*	entry,	/* in: index entry, where the memory buffers
				for sys fields are already allocated:
				the function just copies the new values to
				them */
	dict_index_t*	index,	/* in: clustered index */
	ulint		type,	/* in: DATA_TRX_ID or DATA_ROLL_PTR */
	dulint		val)	/* in: value to write */
{
	dfield_t*	dfield;
	byte*		field;
	ulint		pos;

	ut_ad(dict_index_is_clust(index));

	pos = dict_index_get_sys_col_pos(index, type);

	dfield = dtuple_get_nth_field(entry, pos);
	field = dfield_get_data(dfield);

	if (type == DATA_TRX_ID) {
		trx_write_trx_id(field, val);
	} else {
		ut_ad(type == DATA_ROLL_PTR);
		trx_write_roll_ptr(field, val);
	}
}

/***************************************************************
Returns TRUE if row update changes size of some field in index or if some
field to be updated is stored externally in rec or update. */
UNIV_INTERN
ibool
row_upd_changes_field_size_or_external(
/*===================================*/
				/* out: TRUE if the update changes the size of
				some field in index or the field is external
				in rec or update */
	dict_index_t*	index,	/* in: index */
	const ulint*	offsets,/* in: rec_get_offsets(rec, index) */
	const upd_t*	update)	/* in: update vector */
{
	const upd_field_t*	upd_field;
	const dfield_t*		new_val;
	ulint			old_len;
	ulint			new_len;
	ulint			n_fields;
	ulint			i;

	ut_ad(rec_offs_validate(NULL, index, offsets));
	n_fields = upd_get_n_fields(update);

	for (i = 0; i < n_fields; i++) {
		upd_field = upd_get_nth_field(update, i);

		new_val = &(upd_field->new_val);
		new_len = dfield_get_len(new_val);

		if (dfield_is_null(new_val) && !rec_offs_comp(offsets)) {
			/* A bug fixed on Dec 31st, 2004: we looked at the
			SQL NULL size from the wrong field! We may backport
			this fix also to 4.0. The merge to 5.0 will be made
			manually immediately after we commit this to 4.1. */

			new_len = dict_col_get_sql_null_size(
				dict_index_get_nth_col(index,
						       upd_field->field_no));
		}

		old_len = rec_offs_nth_size(offsets, upd_field->field_no);

		if (rec_offs_comp(offsets)
		    && rec_offs_nth_sql_null(offsets,
					     upd_field->field_no)) {
			/* Note that in the compact table format, for a
			variable length field, an SQL NULL will use zero
			bytes in the offset array at the start of the physical
			record, but a zero-length value (empty string) will
			use one byte! Thus, we cannot use update-in-place
			if we update an SQL NULL varchar to an empty string! */

			old_len = UNIV_SQL_NULL;
		}

		if (dfield_is_ext(new_val) || old_len != new_len
		    || rec_offs_nth_extern(offsets, upd_field->field_no)) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/***************************************************************
Replaces the new column values stored in the update vector to the record
given. No field size changes are allowed. */
UNIV_INTERN
void
row_upd_rec_in_place(
/*=================*/
	rec_t*		rec,	/* in/out: record where replaced */
	dict_index_t*	index,	/* in: the index the record belongs to */
	const ulint*	offsets,/* in: array returned by rec_get_offsets() */
	const upd_t*	update,	/* in: update vector */
	page_zip_des_t*	page_zip)/* in: compressed page with enough space
				available, or NULL */
{
	const upd_field_t*	upd_field;
	const dfield_t*		new_val;
	ulint			n_fields;
	ulint			i;

	ut_ad(rec_offs_validate(rec, index, offsets));

	if (rec_offs_comp(offsets)) {
		rec_set_info_bits_new(rec, update->info_bits);
	} else {
		rec_set_info_bits_old(rec, update->info_bits);
	}

	n_fields = upd_get_n_fields(update);

	for (i = 0; i < n_fields; i++) {
		upd_field = upd_get_nth_field(update, i);
		new_val = &(upd_field->new_val);
		ut_ad(!dfield_is_ext(new_val) ==
		      !rec_offs_nth_extern(offsets, upd_field->field_no));

		rec_set_nth_field(rec, offsets, upd_field->field_no,
				  dfield_get_data(new_val),
				  dfield_get_len(new_val));
	}

	if (UNIV_LIKELY_NULL(page_zip)) {
		page_zip_write_rec(page_zip, rec, index, offsets, 0);
	}
}

/*************************************************************************
Writes into the redo log the values of trx id and roll ptr and enough info
to determine their positions within a clustered index record. */
UNIV_INTERN
byte*
row_upd_write_sys_vals_to_log(
/*==========================*/
				/* out: new pointer to mlog */
	dict_index_t*	index,	/* in: clustered index */
	trx_t*		trx,	/* in: transaction */
	dulint		roll_ptr,/* in: roll ptr of the undo log record */
	byte*		log_ptr,/* pointer to a buffer of size > 20 opened
				in mlog */
	mtr_t*		mtr __attribute__((unused))) /* in: mtr */
{
	ut_ad(dict_index_is_clust(index));
	ut_ad(mtr);

	log_ptr += mach_write_compressed(log_ptr,
					 dict_index_get_sys_col_pos(
						 index, DATA_TRX_ID));

	trx_write_roll_ptr(log_ptr, roll_ptr);
	log_ptr += DATA_ROLL_PTR_LEN;

	log_ptr += mach_dulint_write_compressed(log_ptr, trx->id);

	return(log_ptr);
}

/*************************************************************************
Parses the log data of system field values. */
UNIV_INTERN
byte*
row_upd_parse_sys_vals(
/*===================*/
			/* out: log data end or NULL */
	byte*	ptr,	/* in: buffer */
	byte*	end_ptr,/* in: buffer end */
	ulint*	pos,	/* out: TRX_ID position in record */
	dulint*	trx_id,	/* out: trx id */
	dulint*	roll_ptr)/* out: roll ptr */
{
	ptr = mach_parse_compressed(ptr, end_ptr, pos);

	if (ptr == NULL) {

		return(NULL);
	}

	if (end_ptr < ptr + DATA_ROLL_PTR_LEN) {

		return(NULL);
	}

	*roll_ptr = trx_read_roll_ptr(ptr);
	ptr += DATA_ROLL_PTR_LEN;

	ptr = mach_dulint_parse_compressed(ptr, end_ptr, trx_id);

	return(ptr);
}

/***************************************************************
Writes to the redo log the new values of the fields occurring in the index. */
UNIV_INTERN
void
row_upd_index_write_log(
/*====================*/
	const upd_t*	update,	/* in: update vector */
	byte*		log_ptr,/* in: pointer to mlog buffer: must
				contain at least MLOG_BUF_MARGIN bytes
				of free space; the buffer is closed
				within this function */
	mtr_t*		mtr)	/* in: mtr into whose log to write */
{
	const upd_field_t*	upd_field;
	const dfield_t*		new_val;
	ulint			len;
	ulint			n_fields;
	byte*			buf_end;
	ulint			i;

	n_fields = upd_get_n_fields(update);

	buf_end = log_ptr + MLOG_BUF_MARGIN;

	mach_write_to_1(log_ptr, update->info_bits);
	log_ptr++;
	log_ptr += mach_write_compressed(log_ptr, n_fields);

	for (i = 0; i < n_fields; i++) {

#if MLOG_BUF_MARGIN <= 30
# error "MLOG_BUF_MARGIN <= 30"
#endif

		if (log_ptr + 30 > buf_end) {
			mlog_close(mtr, log_ptr);

			log_ptr = mlog_open(mtr, MLOG_BUF_MARGIN);
			buf_end = log_ptr + MLOG_BUF_MARGIN;
		}

		upd_field = upd_get_nth_field(update, i);

		new_val = &(upd_field->new_val);

		len = dfield_get_len(new_val);

		log_ptr += mach_write_compressed(log_ptr, upd_field->field_no);
		log_ptr += mach_write_compressed(log_ptr, len);

		if (len != UNIV_SQL_NULL) {
			if (log_ptr + len < buf_end) {
				memcpy(log_ptr, dfield_get_data(new_val), len);

				log_ptr += len;
			} else {
				mlog_close(mtr, log_ptr);

				mlog_catenate_string(mtr,
						     dfield_get_data(new_val),
						     len);

				log_ptr = mlog_open(mtr, MLOG_BUF_MARGIN);
				buf_end = log_ptr + MLOG_BUF_MARGIN;
			}
		}
	}

	mlog_close(mtr, log_ptr);
}

/*************************************************************************
Parses the log data written by row_upd_index_write_log. */
UNIV_INTERN
byte*
row_upd_index_parse(
/*================*/
				/* out: log data end or NULL */
	byte*		ptr,	/* in: buffer */
	byte*		end_ptr,/* in: buffer end */
	mem_heap_t*	heap,	/* in: memory heap where update vector is
				built */
	upd_t**		update_out)/* out: update vector */
{
	upd_t*		update;
	upd_field_t*	upd_field;
	dfield_t*	new_val;
	ulint		len;
	ulint		n_fields;
	ulint		info_bits;
	ulint		i;

	if (end_ptr < ptr + 1) {

		return(NULL);
	}

	info_bits = mach_read_from_1(ptr);
	ptr++;
	ptr = mach_parse_compressed(ptr, end_ptr, &n_fields);

	if (ptr == NULL) {

		return(NULL);
	}

	update = upd_create(n_fields, heap);
	update->info_bits = info_bits;

	for (i = 0; i < n_fields; i++) {
		ulint	field_no;
		upd_field = upd_get_nth_field(update, i);
		new_val = &(upd_field->new_val);

		ptr = mach_parse_compressed(ptr, end_ptr, &field_no);

		if (ptr == NULL) {

			return(NULL);
		}

		upd_field->field_no = field_no;

		ptr = mach_parse_compressed(ptr, end_ptr, &len);

		if (ptr == NULL) {

			return(NULL);
		}

		if (len != UNIV_SQL_NULL) {

			if (end_ptr < ptr + len) {

				return(NULL);
			}

			dfield_set_data(new_val,
					mem_heap_dup(heap, ptr, len), len);
			ptr += len;
		} else {
			dfield_set_null(new_val);
		}
	}

	*update_out = update;

	return(ptr);
}

/*******************************************************************
Builds an update vector from those fields which in a secondary index entry
differ from a record that has the equal ordering fields. NOTE: we compare
the fields as binary strings! */
UNIV_INTERN
upd_t*
row_upd_build_sec_rec_difference_binary(
/*====================================*/
				/* out, own: update vector of differing
				fields */
	dict_index_t*	index,	/* in: index */
	const dtuple_t*	entry,	/* in: entry to insert */
	const rec_t*	rec,	/* in: secondary index record */
	trx_t*		trx,	/* in: transaction */
	mem_heap_t*	heap)	/* in: memory heap from which allocated */
{
	upd_field_t*	upd_field;
	const dfield_t*	dfield;
	const byte*	data;
	ulint		len;
	upd_t*		update;
	ulint		n_diff;
	ulint		i;
	ulint		offsets_[REC_OFFS_SMALL_SIZE];
	const ulint*	offsets;
	rec_offs_init(offsets_);

	/* This function is used only for a secondary index */
	ut_a(!dict_index_is_clust(index));

	update = upd_create(dtuple_get_n_fields(entry), heap);

	n_diff = 0;
	offsets = rec_get_offsets(rec, index, offsets_,
				  ULINT_UNDEFINED, &heap);

	for (i = 0; i < dtuple_get_n_fields(entry); i++) {

		data = rec_get_nth_field(rec, offsets, i, &len);

		dfield = dtuple_get_nth_field(entry, i);

		/* NOTE that it may be that len != dfield_get_len(dfield) if we
		are updating in a character set and collation where strings of
		different length can be equal in an alphabetical comparison,
		and also in the case where we have a column prefix index
		and the last characters in the index field are spaces; the
		latter case probably caused the assertion failures reported at
		row0upd.c line 713 in versions 4.0.14 - 4.0.16. */

		/* NOTE: we compare the fields as binary strings!
		(No collation) */

		if (!dfield_data_is_binary_equal(dfield, len, data)) {

			upd_field = upd_get_nth_field(update, n_diff);

			dfield_copy(&(upd_field->new_val), dfield);

			upd_field_set_field_no(upd_field, i, index, trx);

			n_diff++;
		}
	}

	update->n_fields = n_diff;

	return(update);
}

/*******************************************************************
Builds an update vector from those fields, excluding the roll ptr and
trx id fields, which in an index entry differ from a record that has
the equal ordering fields. NOTE: we compare the fields as binary strings! */
UNIV_INTERN
upd_t*
row_upd_build_difference_binary(
/*============================*/
				/* out, own: update vector of differing
				fields, excluding roll ptr and trx id */
	dict_index_t*	index,	/* in: clustered index */
	const dtuple_t*	entry,	/* in: entry to insert */
	const rec_t*	rec,	/* in: clustered index record */
	trx_t*		trx,	/* in: transaction */
	mem_heap_t*	heap)	/* in: memory heap from which allocated */
{
	upd_field_t*	upd_field;
	const dfield_t*	dfield;
	const byte*	data;
	ulint		len;
	upd_t*		update;
	ulint		n_diff;
	ulint		roll_ptr_pos;
	ulint		trx_id_pos;
	ulint		i;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	const ulint*	offsets;
	rec_offs_init(offsets_);

	/* This function is used only for a clustered index */
	ut_a(dict_index_is_clust(index));

	update = upd_create(dtuple_get_n_fields(entry), heap);

	n_diff = 0;

	roll_ptr_pos = dict_index_get_sys_col_pos(index, DATA_ROLL_PTR);
	trx_id_pos = dict_index_get_sys_col_pos(index, DATA_TRX_ID);

	offsets = rec_get_offsets(rec, index, offsets_,
				  ULINT_UNDEFINED, &heap);

	for (i = 0; i < dtuple_get_n_fields(entry); i++) {

		data = rec_get_nth_field(rec, offsets, i, &len);

		dfield = dtuple_get_nth_field(entry, i);

		/* NOTE: we compare the fields as binary strings!
		(No collation) */

		if (i == trx_id_pos || i == roll_ptr_pos) {

			goto skip_compare;
		}

		if (UNIV_UNLIKELY(!dfield_is_ext(dfield)
				  != !rec_offs_nth_extern(offsets, i))
		    || !dfield_data_is_binary_equal(dfield, len, data)) {

			upd_field = upd_get_nth_field(update, n_diff);

			dfield_copy(&(upd_field->new_val), dfield);

			upd_field_set_field_no(upd_field, i, index, trx);

			n_diff++;
		}
skip_compare:
		;
	}

	update->n_fields = n_diff;

	return(update);
}

/***************************************************************
Fetch a prefix of an externally stored column.  This is similar
to row_ext_lookup(), but the row_ext_t holds the old values
of the column and must not be poisoned with the new values. */
static
byte*
row_upd_ext_fetch(
/*==============*/
					/* out: BLOB prefix */
	const byte*	data,		/* in: 'internally' stored part of the
					field containing also the reference to
					the external part */
	ulint		local_len,	/* in: length of data, in bytes */
	ulint		zip_size,	/* in: nonzero=compressed BLOB
					page size, zero for uncompressed
					BLOBs */
	ulint*		len,		/* in: length of prefix to fetch;
					out: fetched length of the prefix */
	mem_heap_t*	heap)		/* in: heap where to allocate */
{
	byte*	buf = mem_heap_alloc(heap, *len);

	*len = btr_copy_externally_stored_field_prefix(buf, *len,
						       zip_size,
						       data, local_len);

	return(buf);
}

/***************************************************************
Replaces the new column values stored in the update vector to the index entry
given. */
UNIV_INTERN
void
row_upd_index_replace_new_col_vals_index_pos(
/*=========================================*/
	dtuple_t*	entry,	/* in/out: index entry where replaced;
				the clustered index record must be
				covered by a lock or a page latch to
				prevent deletion (rollback or purge) */
	dict_index_t*	index,	/* in: index; NOTE that this may also be a
				non-clustered index */
	const upd_t*	update,	/* in: an update vector built for the index so
				that the field number in an upd_field is the
				index position */
	ibool		order_only,
				/* in: if TRUE, limit the replacement to
				ordering fields of index; note that this
				does not work for non-clustered indexes. */
	mem_heap_t*	heap,	/* in: memory heap to which we allocate and
				copy the new values, set this as NULL if you
				do not want allocation */
	mem_heap_t*	ext_heap)/* in: memory heap where to allocate
				column prefixes of externally stored
				columns, may be NULL if the index
				record does not contain externally
				stored columns or column prefixes */
{
	ulint		j;
	ulint		i;
	ulint		n_fields;

	ut_ad(index);

	dtuple_set_info_bits(entry, update->info_bits);

	if (order_only) {
		n_fields = dict_index_get_n_unique(index);
	} else {
		n_fields = dict_index_get_n_fields(index);
	}

	for (j = 0; j < n_fields; j++) {

		dict_field_t*		field
			= dict_index_get_nth_field(index, j);
		const dict_col_t*	col
			= dict_field_get_col(field);

		for (i = 0; i < upd_get_n_fields(update); i++) {

			upd_field_t*	upd_field;
			dfield_t*	dfield;

			upd_field = upd_get_nth_field(update, i);

			if (upd_field->field_no != j) {
				continue;
			}

			dfield = dtuple_get_nth_field(entry, j);

			dfield_copy_data(dfield, &upd_field->new_val);

			if (dfield_is_null(dfield)) {
				break;
			}

			if (field->prefix_len > 0) {
				ulint		len
					= dfield_get_len(dfield);
				const byte*	data
					= dfield_get_data(dfield);
				ibool		fetch_ext
					= dfield_is_ext(dfield)
					&& len < (ulint) field->prefix_len
					+ BTR_EXTERN_FIELD_REF_SIZE;

				if (fetch_ext) {
					ulint	l
						= len;
					ulint	zip_size
						= dict_table_zip_size(
							index->table);
					ut_a(ext_heap);

					len = field->prefix_len;

					data = row_upd_ext_fetch(data, l,
								 zip_size,
								 &len,
								 ext_heap);
				}

				len = dtype_get_at_most_n_mbchars(
					col->prtype,
					col->mbminlen,
					col->mbmaxlen,
					field->prefix_len,
					len, (const char*) data);

				dfield_set_data(dfield, data, len);

				if (fetch_ext && heap && heap == ext_heap) {
					/* Skip the dfield_dup() below,
					as the column prefix has already
					been allocated from ext_heap. */
					break;
				}
			}

			if (heap) {
				dfield_dup(dfield, heap);
			}

			break;
		}
	}
}

/***************************************************************
Replaces the new column values stored in the update vector to the index entry
given. */
UNIV_INTERN
void
row_upd_index_replace_new_col_vals(
/*===============================*/
	dtuple_t*	entry,	/* in/out: index entry where replaced;
				the clustered index record must be
				covered by a lock or a page latch to
				prevent deletion (rollback or purge) */
	dict_index_t*	index,	/* in: index; NOTE that this may also be a
				non-clustered index */
	const upd_t*	update,	/* in: an update vector built for the
				CLUSTERED index so that the field number in
				an upd_field is the clustered index position */
	mem_heap_t*	heap,	/* in: memory heap to which we allocate and
				copy the new values, set this as NULL if you
				do not want allocation */
	mem_heap_t*	ext_heap)/* in: memory heap where to allocate
				column prefixes of externally stored
				columns, may be NULL if the index
				record does not contain externally
				stored columns or column prefixes */
{
	ulint		j;
	ulint		i;
	dict_index_t*	clust_index;

	ut_ad(index);

	clust_index = dict_table_get_first_index(index->table);

	dtuple_set_info_bits(entry, update->info_bits);

	for (j = 0; j < dict_index_get_n_fields(index); j++) {

		dict_field_t*		field
			= dict_index_get_nth_field(index, j);
		const dict_col_t*	col
			= dict_field_get_col(field);
		const ulint		clust_pos
			= dict_col_get_clust_pos(col, clust_index);

		for (i = 0; i < upd_get_n_fields(update); i++) {

			upd_field_t*	upd_field;
			dfield_t*	dfield;

			upd_field = upd_get_nth_field(update, i);

			if (upd_field->field_no != clust_pos) {
				continue;
			}

			dfield = dtuple_get_nth_field(entry, j);

			dfield_copy_data(dfield, &upd_field->new_val);

			if (dfield_is_null(dfield)) {
				break;
			}

			if (field->prefix_len > 0) {
				ulint		len
					= dfield_get_len(dfield);
				const byte*	data
					= dfield_get_data(dfield);
				ibool		fetch_ext
					= dfield_is_ext(dfield)
					&& len < (ulint) field->prefix_len
					+ BTR_EXTERN_FIELD_REF_SIZE;

				if (fetch_ext) {
					ulint	l
						= len;
					ulint	zip_size
						= dict_table_zip_size(
							index->table);
					ut_a(ext_heap);

					len = field->prefix_len;

					data = row_upd_ext_fetch(data, l,
								 zip_size,
								 &len,
								 ext_heap);
				}

				len = dtype_get_at_most_n_mbchars(
					col->prtype,
					col->mbminlen,
					col->mbmaxlen,
					field->prefix_len,
					len, (const char*) data);

				dfield_set_data(dfield, data, len);

				if (fetch_ext && heap && heap == ext_heap) {
					/* Skip the dfield_dup() below,
					as the column prefix has already
					been allocated from ext_heap. */
					break;
				}
			}

			if (heap) {
				dfield_dup(dfield, heap);
			}

			break;
		}
	}
}

/***************************************************************
Replaces the new column values stored in the update vector. */
UNIV_INTERN
void
row_upd_replace(
/*============*/
	dtuple_t*		row,	/* in/out: row where replaced,
					indexed by col_no;
					the clustered index record must be
					covered by a lock or a page latch to
					prevent deletion (rollback or purge) */
	row_ext_t**		ext,	/* out, own: NULL, or externally
					stored column prefixes */
	const dict_index_t*	index,	/* in: clustered index */
	const upd_t*		update,	/* in: an update vector built for the
					clustered index */
	mem_heap_t*		heap)	/* in: memory heap */
{
	ulint			col_no;
	ulint			i;
	ulint			n_cols;
	ulint			n_ext_cols;
	ulint*			ext_cols;
	const dict_table_t*	table;

	ut_ad(row);
	ut_ad(ext);
	ut_ad(index);
	ut_ad(dict_index_is_clust(index));
	ut_ad(update);
	ut_ad(heap);

	n_cols = dtuple_get_n_fields(row);
	table = index->table;
	ut_ad(n_cols == dict_table_get_n_cols(table));

	ext_cols = mem_heap_alloc(heap, n_cols * sizeof *ext_cols);
	n_ext_cols = 0;

	dtuple_set_info_bits(row, update->info_bits);

	for (col_no = 0; col_no < n_cols; col_no++) {

		const dict_col_t*	col
			= dict_table_get_nth_col(table, col_no);
		const ulint		clust_pos
			= dict_col_get_clust_pos(col, index);
		dfield_t*		dfield;

		if (UNIV_UNLIKELY(clust_pos == ULINT_UNDEFINED)) {

			continue;
		}

		dfield = dtuple_get_nth_field(row, col_no);

		for (i = 0; i < upd_get_n_fields(update); i++) {

			const upd_field_t*	upd_field
				= upd_get_nth_field(update, i);

			if (upd_field->field_no != clust_pos) {

				continue;
			}

			dfield_copy_data(dfield, &upd_field->new_val);
			break;
		}

		if (dfield_is_ext(dfield) && col->ord_part) {
			ext_cols[n_ext_cols++] = col_no;
		}
	}

	if (n_ext_cols) {
		*ext = row_ext_create(n_ext_cols, ext_cols, row,
				      dict_table_zip_size(table), heap);
	} else {
		*ext = NULL;
	}
}

/***************************************************************
Checks if an update vector changes an ordering field of an index record.
This function is fast if the update vector is short or the number of ordering
fields in the index is small. Otherwise, this can be quadratic.
NOTE: we compare the fields as binary strings! */
UNIV_INTERN
ibool
row_upd_changes_ord_field_binary(
/*=============================*/
				/* out: TRUE if update vector changes
				an ordering field in the index record;
				NOTE: the fields are compared as binary
				strings */
	const dtuple_t*	row,	/* in: old value of row, or NULL if the
				row and the data values in update are not
				known when this function is called, e.g., at
				compile time */
	dict_index_t*	index,	/* in: index of the record */
	const upd_t*	update)	/* in: update vector for the row; NOTE: the
				field numbers in this MUST be clustered index
				positions! */
{
	ulint		n_unique;
	ulint		n_upd_fields;
	ulint		i, j;
	dict_index_t*	clust_index;

	ut_ad(update && index);

	n_unique = dict_index_get_n_unique(index);
	n_upd_fields = upd_get_n_fields(update);

	clust_index = dict_table_get_first_index(index->table);

	for (i = 0; i < n_unique; i++) {

		const dict_field_t*	ind_field;
		const dict_col_t*	col;
		ulint			col_pos;
		ulint			col_no;

		ind_field = dict_index_get_nth_field(index, i);
		col = dict_field_get_col(ind_field);
		col_pos = dict_col_get_clust_pos(col, clust_index);
		col_no = dict_col_get_no(col);

		for (j = 0; j < n_upd_fields; j++) {

			const upd_field_t*	upd_field
				= upd_get_nth_field(update, j);

			/* Note that if the index field is a column prefix
			then it may be that row does not contain an externally
			stored part of the column value, and we cannot compare
			the datas */

			if (col_pos == upd_field->field_no
			    && (row == NULL
				|| ind_field->prefix_len > 0
				|| !dfield_datas_are_binary_equal(
					dtuple_get_nth_field(row, col_no),
					&(upd_field->new_val)))) {

				return(TRUE);
			}
		}
	}

	return(FALSE);
}

/***************************************************************
Checks if an update vector changes an ordering field of an index record.
NOTE: we compare the fields as binary strings! */
UNIV_INTERN
ibool
row_upd_changes_some_index_ord_field_binary(
/*========================================*/
					/* out: TRUE if update vector
					may change an ordering field
					in an index record */
	const dict_table_t*	table,	/* in: table */
	const upd_t*		update)	/* in: update vector for the row */
{
	upd_field_t*	upd_field;
	dict_index_t*	index;
	ulint		i;

	index = dict_table_get_first_index(table);

	for (i = 0; i < upd_get_n_fields(update); i++) {

		upd_field = upd_get_nth_field(update, i);

		if (dict_field_get_col(dict_index_get_nth_field(
					       index, upd_field->field_no))
		    ->ord_part) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/***************************************************************
Checks if an update vector changes some of the first ordering fields of an
index record. This is only used in foreign key checks and we can assume
that index does not contain column prefixes. */
static
ibool
row_upd_changes_first_fields_binary(
/*================================*/
				/* out: TRUE if changes */
	dtuple_t*	entry,	/* in: index entry */
	dict_index_t*	index,	/* in: index of entry */
	const upd_t*	update,	/* in: update vector for the row */
	ulint		n)	/* in: how many first fields to check */
{
	ulint		n_upd_fields;
	ulint		i, j;
	dict_index_t*	clust_index;

	ut_ad(update && index);
	ut_ad(n <= dict_index_get_n_fields(index));

	n_upd_fields = upd_get_n_fields(update);
	clust_index = dict_table_get_first_index(index->table);

	for (i = 0; i < n; i++) {

		const dict_field_t*	ind_field;
		const dict_col_t*	col;
		ulint			col_pos;

		ind_field = dict_index_get_nth_field(index, i);
		col = dict_field_get_col(ind_field);
		col_pos = dict_col_get_clust_pos(col, clust_index);

		ut_a(ind_field->prefix_len == 0);

		for (j = 0; j < n_upd_fields; j++) {

			upd_field_t*	upd_field
				= upd_get_nth_field(update, j);

			if (col_pos == upd_field->field_no
			    && !dfield_datas_are_binary_equal(
				    dtuple_get_nth_field(entry, i),
				    &(upd_field->new_val))) {

				return(TRUE);
			}
		}
	}

	return(FALSE);
}

/*************************************************************************
Copies the column values from a record. */
UNIV_INLINE
void
row_upd_copy_columns(
/*=================*/
	rec_t*		rec,	/* in: record in a clustered index */
	const ulint*	offsets,/* in: array returned by rec_get_offsets() */
	sym_node_t*	column)	/* in: first column in a column list, or
				NULL */
{
	byte*	data;
	ulint	len;

	while (column) {
		data = rec_get_nth_field(rec, offsets,
					 column->field_nos[SYM_CLUST_FIELD_NO],
					 &len);
		if (len == UNIV_SQL_NULL) {
			len = UNIV_SQL_NULL;
		}
		eval_node_copy_and_alloc_val(column, data, len);

		column = UT_LIST_GET_NEXT(col_var_list, column);
	}
}

/*************************************************************************
Calculates the new values for fields to update. Note that row_upd_copy_columns
must have been called first. */
UNIV_INLINE
void
row_upd_eval_new_vals(
/*==================*/
	upd_t*	update)	/* in/out: update vector */
{
	que_node_t*	exp;
	upd_field_t*	upd_field;
	ulint		n_fields;
	ulint		i;

	n_fields = upd_get_n_fields(update);

	for (i = 0; i < n_fields; i++) {
		upd_field = upd_get_nth_field(update, i);

		exp = upd_field->exp;

		eval_exp(exp);

		dfield_copy_data(&(upd_field->new_val), que_node_get_val(exp));
	}
}

/***************************************************************
Stores to the heap the row on which the node->pcur is positioned. */
static
void
row_upd_store_row(
/*==============*/
	upd_node_t*	node)	/* in: row update node */
{
	dict_index_t*	clust_index;
	rec_t*		rec;
	mem_heap_t*	heap		= NULL;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	const ulint*	offsets;
	rec_offs_init(offsets_);

	ut_ad(node->pcur->latch_mode != BTR_NO_LATCHES);

	if (node->row != NULL) {
		mem_heap_empty(node->heap);
	}

	clust_index = dict_table_get_first_index(node->table);

	rec = btr_pcur_get_rec(node->pcur);

	offsets = rec_get_offsets(rec, clust_index, offsets_,
				  ULINT_UNDEFINED, &heap);
	node->row = row_build(ROW_COPY_DATA, clust_index, rec, offsets,
			      NULL, &node->ext, node->heap);
	if (node->is_delete) {
		node->upd_row = NULL;
		node->upd_ext = NULL;
	} else {
		node->upd_row = dtuple_copy(node->row, node->heap);
		row_upd_replace(node->upd_row, &node->upd_ext,
				clust_index, node->update, node->heap);
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}

/***************************************************************
Updates a secondary index entry of a row. */
static
ulint
row_upd_sec_index_entry(
/*====================*/
				/* out: DB_SUCCESS if operation successfully
				completed, else error code or DB_LOCK_WAIT */
	upd_node_t*	node,	/* in: row update node */
	que_thr_t*	thr)	/* in: query thread */
{
	mtr_t		mtr;
	rec_t*		rec;
	btr_pcur_t	pcur;
	mem_heap_t*	heap;
	dtuple_t*	entry;
	dict_index_t*	index;
	ibool		found;
	btr_cur_t*	btr_cur;
	ibool		referenced;
	ibool		was_buffered;
	ulint		err = DB_SUCCESS;
	trx_t*		trx = thr_get_trx(thr);
	ulint		mode = BTR_MODIFY_LEAF;

	index = node->index;

	referenced = row_upd_index_is_referenced(index, trx);

	heap = mem_heap_create(1024);

	/* Build old index entry */
	entry = row_build_index_entry(node->row, node->ext, index, heap);
	ut_a(entry);

	log_free_check();
	mtr_start(&mtr);

	btr_pcur_get_btr_cur(&pcur)->thr = thr;

	/* We can only try to use the insert/delete buffer to buffer
	delete-mark operations if the index we're modifying has no foreign
	key constraints referring to it. */
	if (!referenced) {
		mode |= BTR_DELETE_MARK;
	}

	found = row_search_index_entry(
		&was_buffered, index, entry, BTR_MODIFY_LEAF, &pcur, &mtr);

	if (was_buffered) {
		/* Entry was delete marked already. */

		goto close_cur;
	}

	btr_cur = btr_pcur_get_btr_cur(&pcur);

	rec = btr_cur_get_rec(btr_cur);

	if (UNIV_UNLIKELY(!found)) {
		fputs("InnoDB: error in sec index entry update in\n"
		      "InnoDB: ", stderr);
		dict_index_name_print(stderr, trx, index);
		fputs("\n"
		      "InnoDB: tuple ", stderr);
		dtuple_print(stderr, entry);
		fputs("\n"
		      "InnoDB: record ", stderr);
		rec_print(stderr, rec, index);
		putc('\n', stderr);

		trx_print(stderr, trx, 0);

		fputs("\n"
		      "InnoDB: Submit a detailed bug report"
		      " to http://bugs.mysql.com\n", stderr);
	} else {
		/* Delete mark the old index record; it can already be
		delete marked if we return after a lock wait in
		row_ins_index_entry below */

		if (!rec_get_deleted_flag(
			rec, dict_table_is_comp(index->table))) {

			err = btr_cur_del_mark_set_sec_rec(
				0, btr_cur, TRUE, thr, &mtr);

			if (err == DB_SUCCESS && referenced) {

				ulint*	offsets;

				offsets = rec_get_offsets(
					rec, index, NULL, ULINT_UNDEFINED,
					&heap);

				/* NOTE that the following call loses
				the position of pcur ! */
				err = row_upd_check_references_constraints(
					node, &pcur, index->table,
					index, offsets, thr, &mtr);
			}
		}
	}

close_cur:
	btr_pcur_close(&pcur);
	mtr_commit(&mtr);

	if (node->is_delete || err != DB_SUCCESS) {

		goto func_exit;
	}

	/* Build a new index entry */
	entry = row_build_index_entry(node->upd_row, node->upd_ext,
				      index, heap);
	ut_a(entry);

	/* Insert new index entry */
	err = row_ins_index_entry(index, entry, 0, TRUE, thr);

func_exit:
	mem_heap_free(heap);

	return(err);
}

/***************************************************************
Updates the secondary index record if it is changed in the row update or
deletes it if this is a delete. */
UNIV_INLINE
ulint
row_upd_sec_step(
/*=============*/
				/* out: DB_SUCCESS if operation successfully
				completed, else error code or DB_LOCK_WAIT */
	upd_node_t*	node,	/* in: row update node */
	que_thr_t*	thr)	/* in: query thread */
{
	ut_ad((node->state == UPD_NODE_UPDATE_ALL_SEC)
	      || (node->state == UPD_NODE_UPDATE_SOME_SEC));
	ut_ad(!dict_index_is_clust(node->index));

	if (node->state == UPD_NODE_UPDATE_ALL_SEC
	    || row_upd_changes_ord_field_binary(node->row, node->index,
						node->update)) {
		return(row_upd_sec_index_entry(node, thr));
	}

	return(DB_SUCCESS);
}

/***************************************************************
Marks the clustered index record deleted and inserts the updated version
of the record to the index. This function should be used when the ordering
fields of the clustered index record change. This should be quite rare in
database applications. */
static
ulint
row_upd_clust_rec_by_insert(
/*========================*/
				/* out: DB_SUCCESS if operation successfully
				completed, else error code or DB_LOCK_WAIT */
	upd_node_t*	node,	/* in: row update node */
	dict_index_t*	index,	/* in: clustered index of the record */
	que_thr_t*	thr,	/* in: query thread */
	ibool		referenced,/* in: TRUE if index may be referenced in
				a foreign key constraint */
	mtr_t*		mtr)	/* in: mtr; gets committed here */
{
	mem_heap_t*	heap	= NULL;
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	trx_t*		trx;
	dict_table_t*	table;
	dtuple_t*	entry;
	ulint		err;

	ut_ad(node);
	ut_ad(dict_index_is_clust(index));

	trx = thr_get_trx(thr);
	table = node->table;
	pcur = node->pcur;
	btr_cur	= btr_pcur_get_btr_cur(pcur);

	if (node->state != UPD_NODE_INSERT_CLUSTERED) {
		rec_t*		rec;
		dict_index_t*	index;
		ulint		offsets_[REC_OFFS_NORMAL_SIZE];
		ulint*		offsets;
		rec_offs_init(offsets_);

		err = btr_cur_del_mark_set_clust_rec(BTR_NO_LOCKING_FLAG,
						     btr_cur, TRUE, thr, mtr);
		if (err != DB_SUCCESS) {
			mtr_commit(mtr);
			return(err);
		}

		/* Mark as not-owned the externally stored fields which the new
		row inherits from the delete marked record: purge should not
		free those externally stored fields even if the delete marked
		record is removed from the index tree, or updated. */

		rec = btr_cur_get_rec(btr_cur);
		index = dict_table_get_first_index(table);
		offsets = rec_get_offsets(rec, index, offsets_,
					  ULINT_UNDEFINED, &heap);
		btr_cur_mark_extern_inherited_fields(
			btr_cur_get_page_zip(btr_cur),
			rec, index, offsets, node->update, mtr);
		if (referenced) {
			/* NOTE that the following call loses
			the position of pcur ! */

			err = row_upd_check_references_constraints(
				node, pcur, table, index, offsets, thr, mtr);

			if (err != DB_SUCCESS) {

				mtr_commit(mtr);

				if (UNIV_LIKELY_NULL(heap)) {
					mem_heap_free(heap);
				}

				return(err);
			}
		}
	}

	mtr_commit(mtr);

	if (!heap) {
		heap = mem_heap_create(500);
	}
	node->state = UPD_NODE_INSERT_CLUSTERED;

	entry = row_build_index_entry(node->upd_row, node->upd_ext,
				      index, heap);
	ut_a(entry);

	row_upd_index_entry_sys_field(entry, index, DATA_TRX_ID, trx->id);

	if (node->upd_ext) {
		/* If we return from a lock wait, for example, we may have
		extern fields marked as not-owned in entry (marked in the
		if-branch above). We must unmark them. */

		btr_cur_unmark_dtuple_extern_fields(entry);

		/* We must mark non-updated extern fields in entry as
		inherited, so that a possible rollback will not free them. */

		btr_cur_mark_dtuple_inherited_extern(entry, node->update);
	}

	err = row_ins_index_entry(index, entry,
				  node->upd_ext ? node->upd_ext->n_ext : 0,
				  TRUE, thr);
	mem_heap_free(heap);

	return(err);
}

/***************************************************************
Updates a clustered index record of a row when the ordering fields do
not change. */
static
ulint
row_upd_clust_rec(
/*==============*/
				/* out: DB_SUCCESS if operation successfully
				completed, else error code or DB_LOCK_WAIT */
	upd_node_t*	node,	/* in: row update node */
	dict_index_t*	index,	/* in: clustered index */
	que_thr_t*	thr,	/* in: query thread */
	mtr_t*		mtr)	/* in: mtr; gets committed here */
{
	mem_heap_t*	heap	= NULL;
	big_rec_t*	big_rec	= NULL;
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	ulint		err;

	ut_ad(node);
	ut_ad(dict_index_is_clust(index));

	pcur = node->pcur;
	btr_cur = btr_pcur_get_btr_cur(pcur);

	ut_ad(!rec_get_deleted_flag(btr_pcur_get_rec(pcur),
				    dict_table_is_comp(index->table)));

	/* Try optimistic updating of the record, keeping changes within
	the page; we do not check locks because we assume the x-lock on the
	record to update */

	if (node->cmpl_info & UPD_NODE_NO_SIZE_CHANGE) {
		err = btr_cur_update_in_place(BTR_NO_LOCKING_FLAG,
					      btr_cur, node->update,
					      node->cmpl_info, thr, mtr);
	} else {
		err = btr_cur_optimistic_update(BTR_NO_LOCKING_FLAG,
						btr_cur, node->update,
						node->cmpl_info, thr, mtr);
	}

	mtr_commit(mtr);

	if (UNIV_LIKELY(err == DB_SUCCESS)) {

		return(DB_SUCCESS);
	}

	if (buf_LRU_buf_pool_running_out()) {

		return(DB_LOCK_TABLE_FULL);
	}
	/* We may have to modify the tree structure: do a pessimistic descent
	down the index tree */

	mtr_start(mtr);

	/* NOTE: this transaction has an s-lock or x-lock on the record and
	therefore other transactions cannot modify the record when we have no
	latch on the page. In addition, we assume that other query threads of
	the same transaction do not modify the record in the meantime.
	Therefore we can assert that the restoration of the cursor succeeds. */

	ut_a(btr_pcur_restore_position(BTR_MODIFY_TREE, pcur, mtr));

	ut_ad(!rec_get_deleted_flag(btr_pcur_get_rec(pcur),
				    dict_table_is_comp(index->table)));

	err = btr_cur_pessimistic_update(BTR_NO_LOCKING_FLAG, btr_cur,
					 &heap, &big_rec, node->update,
					 node->cmpl_info, thr, mtr);
	mtr_commit(mtr);

	if (err == DB_SUCCESS && big_rec) {
		ulint		offsets_[REC_OFFS_NORMAL_SIZE];
		rec_t*		rec;
		rec_offs_init(offsets_);

		mtr_start(mtr);

		ut_a(btr_pcur_restore_position(BTR_MODIFY_TREE, pcur, mtr));
		rec = btr_cur_get_rec(btr_cur);
		err = btr_store_big_rec_extern_fields(
			index, btr_cur_get_block(btr_cur), rec,
			rec_get_offsets(rec, index, offsets_,
					ULINT_UNDEFINED, &heap),
			big_rec, mtr);
		mtr_commit(mtr);
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	if (big_rec) {
		dtuple_big_rec_free(big_rec);
	}

	return(err);
}

/***************************************************************
Delete marks a clustered index record. */
static
ulint
row_upd_del_mark_clust_rec(
/*=======================*/
				/* out: DB_SUCCESS if operation successfully
				completed, else error code */
	upd_node_t*	node,	/* in: row update node */
	dict_index_t*	index,	/* in: clustered index */
	ulint*		offsets,/* in/out: rec_get_offsets() for the
				record under the cursor */
	que_thr_t*	thr,	/* in: query thread */
	ibool		referenced,
				/* in: TRUE if index may be referenced in
				a foreign key constraint */
	mtr_t*		mtr)	/* in: mtr; gets committed here */
{
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	ulint		err;

	ut_ad(node);
	ut_ad(dict_index_is_clust(index));
	ut_ad(node->is_delete);

	pcur = node->pcur;
	btr_cur = btr_pcur_get_btr_cur(pcur);

	/* Store row because we have to build also the secondary index
	entries */

	row_upd_store_row(node);

	/* Mark the clustered index record deleted; we do not have to check
	locks, because we assume that we have an x-lock on the record */

	err = btr_cur_del_mark_set_clust_rec(BTR_NO_LOCKING_FLAG,
					     btr_cur, TRUE, thr, mtr);
	if (err == DB_SUCCESS && referenced) {
		/* NOTE that the following call loses the position of pcur ! */

		err = row_upd_check_references_constraints(
			node, pcur, index->table, index, offsets, thr, mtr);
	}

	mtr_commit(mtr);

	return(err);
}

/***************************************************************
Updates the clustered index record. */
static
ulint
row_upd_clust_step(
/*===============*/
				/* out: DB_SUCCESS if operation successfully
				completed, DB_LOCK_WAIT in case of a lock wait,
				else error code */
	upd_node_t*	node,	/* in: row update node */
	que_thr_t*	thr)	/* in: query thread */
{
	dict_index_t*	index;
	btr_pcur_t*	pcur;
	ibool		success;
	ulint		err;
	mtr_t*		mtr;
	mtr_t		mtr_buf;
	rec_t*		rec;
	mem_heap_t*	heap		= NULL;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*		offsets;
	ibool		referenced;
	rec_offs_init(offsets_);

	index = dict_table_get_first_index(node->table);

	referenced = row_upd_index_is_referenced(index, thr_get_trx(thr));

	pcur = node->pcur;

	/* We have to restore the cursor to its position */
	mtr = &mtr_buf;

	mtr_start(mtr);

	/* If the restoration does not succeed, then the same
	transaction has deleted the record on which the cursor was,
	and that is an SQL error. If the restoration succeeds, it may
	still be that the same transaction has successively deleted
	and inserted a record with the same ordering fields, but in
	that case we know that the transaction has at least an
	implicit x-lock on the record. */

	ut_a(pcur->rel_pos == BTR_PCUR_ON);

	success = btr_pcur_restore_position(BTR_MODIFY_LEAF, pcur, mtr);

	if (!success) {
		err = DB_RECORD_NOT_FOUND;

		mtr_commit(mtr);

		return(err);
	}

	/* If this is a row in SYS_INDEXES table of the data dictionary,
	then we have to free the file segments of the index tree associated
	with the index */

	if (node->is_delete
	    && ut_dulint_cmp(node->table->id, DICT_INDEXES_ID) == 0) {

		dict_drop_index_tree(btr_pcur_get_rec(pcur), mtr);

		mtr_commit(mtr);

		mtr_start(mtr);

		success = btr_pcur_restore_position(BTR_MODIFY_LEAF, pcur,
						    mtr);
		if (!success) {
			err = DB_ERROR;

			mtr_commit(mtr);

			return(err);
		}
	}

	rec = btr_pcur_get_rec(pcur);
	offsets = rec_get_offsets(rec, index, offsets_,
				  ULINT_UNDEFINED, &heap);

	if (!node->has_clust_rec_x_lock) {
		err = lock_clust_rec_modify_check_and_lock(
			0, btr_pcur_get_block(pcur),
			rec, index, offsets, thr);
		if (err != DB_SUCCESS) {
			mtr_commit(mtr);
			goto exit_func;
		}
	}

	/* NOTE: the following function calls will also commit mtr */

	if (node->is_delete) {
		err = row_upd_del_mark_clust_rec(
			node, index, offsets, thr, referenced, mtr);

		if (err == DB_SUCCESS) {
			node->state = UPD_NODE_UPDATE_ALL_SEC;
			node->index = dict_table_get_next_index(index);
		}
exit_func:
		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}
		return(err);
	}

	/* If the update is made for MySQL, we already have the update vector
	ready, else we have to do some evaluation: */

	if (UNIV_UNLIKELY(!node->in_mysql_interface)) {
		/* Copy the necessary columns from clust_rec and calculate the
		new values to set */
		row_upd_copy_columns(rec, offsets,
				     UT_LIST_GET_FIRST(node->columns));
		row_upd_eval_new_vals(node->update);
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	if (node->cmpl_info & UPD_NODE_NO_ORD_CHANGE) {

		err = row_upd_clust_rec(node, index, thr, mtr);
		return(err);
	}

	row_upd_store_row(node);

	if (row_upd_changes_ord_field_binary(node->row, index, node->update)) {

		/* Update causes an ordering field (ordering fields within
		the B-tree) of the clustered index record to change: perform
		the update by delete marking and inserting.

		TODO! What to do to the 'Halloween problem', where an update
		moves the record forward in index so that it is again
		updated when the cursor arrives there? Solution: the
		read operation must check the undo record undo number when
		choosing records to update. MySQL solves now the problem
		externally! */

		err = row_upd_clust_rec_by_insert(
			node, index, thr, referenced, mtr);

		if (err != DB_SUCCESS) {

			return(err);
		}

		node->state = UPD_NODE_UPDATE_ALL_SEC;
	} else {
		err = row_upd_clust_rec(node, index, thr, mtr);

		if (err != DB_SUCCESS) {

			return(err);
		}

		node->state = UPD_NODE_UPDATE_SOME_SEC;
	}

	node->index = dict_table_get_next_index(index);

	return(err);
}

/***************************************************************
Updates the affected index records of a row. When the control is transferred
to this node, we assume that we have a persistent cursor which was on a
record, and the position of the cursor is stored in the cursor. */
static
ulint
row_upd(
/*====*/
				/* out: DB_SUCCESS if operation successfully
				completed, else error code or DB_LOCK_WAIT */
	upd_node_t*	node,	/* in: row update node */
	que_thr_t*	thr)	/* in: query thread */
{
	ulint	err	= DB_SUCCESS;

	ut_ad(node && thr);

	if (UNIV_LIKELY(node->in_mysql_interface)) {

		/* We do not get the cmpl_info value from the MySQL
		interpreter: we must calculate it on the fly: */

		if (node->is_delete
		    || row_upd_changes_some_index_ord_field_binary(
			    node->table, node->update)) {
			node->cmpl_info = 0;
		} else {
			node->cmpl_info = UPD_NODE_NO_ORD_CHANGE;
		}
	}

	if (node->state == UPD_NODE_UPDATE_CLUSTERED
	    || node->state == UPD_NODE_INSERT_CLUSTERED) {

		err = row_upd_clust_step(node, thr);

		if (err != DB_SUCCESS) {

			goto function_exit;
		}
	}

	if (!node->is_delete && (node->cmpl_info & UPD_NODE_NO_ORD_CHANGE)) {

		goto function_exit;
	}

	while (node->index != NULL) {
		err = row_upd_sec_step(node, thr);

		if (err != DB_SUCCESS) {

			goto function_exit;
		}

		node->index = dict_table_get_next_index(node->index);
	}

function_exit:
	if (err == DB_SUCCESS) {
		/* Do some cleanup */

		if (node->row != NULL) {
			node->row = NULL;
			node->ext = NULL;
			node->upd_row = NULL;
			node->upd_ext = NULL;
			mem_heap_empty(node->heap);
		}

		node->state = UPD_NODE_UPDATE_CLUSTERED;
	}

	return(err);
}

/***************************************************************
Updates a row in a table. This is a high-level function used in SQL execution
graphs. */
UNIV_INTERN
que_thr_t*
row_upd_step(
/*=========*/
				/* out: query thread to run next or NULL */
	que_thr_t*	thr)	/* in: query thread */
{
	upd_node_t*	node;
	sel_node_t*	sel_node;
	que_node_t*	parent;
	ulint		err		= DB_SUCCESS;
	trx_t*		trx;

	ut_ad(thr);

	trx = thr_get_trx(thr);

	trx_start_if_not_started(trx);

	node = thr->run_node;

	sel_node = node->select;

	parent = que_node_get_parent(node);

	ut_ad(que_node_get_type(node) == QUE_NODE_UPDATE);

	if (thr->prev_node == parent) {
		node->state = UPD_NODE_SET_IX_LOCK;
	}

	if (node->state == UPD_NODE_SET_IX_LOCK) {

		if (!node->has_clust_rec_x_lock) {
			/* It may be that the current session has not yet
			started its transaction, or it has been committed: */

			err = lock_table(0, node->table, LOCK_IX, thr);

			if (err != DB_SUCCESS) {

				goto error_handling;
			}
		}

		node->state = UPD_NODE_UPDATE_CLUSTERED;

		if (node->searched_update) {
			/* Reset the cursor */
			sel_node->state = SEL_NODE_OPEN;

			/* Fetch a row to update */

			thr->run_node = sel_node;

			return(thr);
		}
	}

	/* sel_node is NULL if we are in the MySQL interface */

	if (sel_node && (sel_node->state != SEL_NODE_FETCH)) {

		if (!node->searched_update) {
			/* An explicit cursor should be positioned on a row
			to update */

			ut_error;

			err = DB_ERROR;

			goto error_handling;
		}

		ut_ad(sel_node->state == SEL_NODE_NO_MORE_ROWS);

		/* No more rows to update, or the select node performed the
		updates directly in-place */

		thr->run_node = parent;

		return(thr);
	}

	/* DO THE CHECKS OF THE CONSISTENCY CONSTRAINTS HERE */

	err = row_upd(node, thr);

error_handling:
	trx->error_state = err;

	if (err != DB_SUCCESS) {
		return(NULL);
	}

	/* DO THE TRIGGER ACTIONS HERE */

	if (node->searched_update) {
		/* Fetch next row to update */

		thr->run_node = sel_node;
	} else {
		/* It was an explicit cursor update */

		thr->run_node = parent;
	}

	node->state = UPD_NODE_UPDATE_CLUSTERED;

	return(thr);
}

/*************************************************************************
Performs an in-place update for the current clustered index record in
select. */
UNIV_INTERN
void
row_upd_in_place_in_select(
/*=======================*/
	sel_node_t*	sel_node,	/* in: select node */
	que_thr_t*	thr,		/* in: query thread */
	mtr_t*		mtr)		/* in: mtr */
{
	upd_node_t*	node;
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	ulint		err;
	mem_heap_t*	heap		= NULL;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs_init(offsets_);

	ut_ad(sel_node->select_will_do_update);
	ut_ad(sel_node->latch_mode == BTR_MODIFY_LEAF);
	ut_ad(sel_node->asc);

	node = que_node_get_parent(sel_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_UPDATE);

	pcur = node->pcur;
	btr_cur = btr_pcur_get_btr_cur(pcur);

	/* Copy the necessary columns from clust_rec and calculate the new
	values to set */

	row_upd_copy_columns(btr_pcur_get_rec(pcur),
			     rec_get_offsets(btr_pcur_get_rec(pcur),
					     btr_cur->index, offsets_,
					     ULINT_UNDEFINED, &heap),
			     UT_LIST_GET_FIRST(node->columns));
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	row_upd_eval_new_vals(node->update);

	ut_ad(!rec_get_deleted_flag(
		      btr_pcur_get_rec(pcur),
		      dict_table_is_comp(btr_cur->index->table)));

	ut_ad(node->cmpl_info & UPD_NODE_NO_SIZE_CHANGE);
	ut_ad(node->cmpl_info & UPD_NODE_NO_ORD_CHANGE);
	ut_ad(node->select_will_do_update);

	err = btr_cur_update_in_place(BTR_NO_LOCKING_FLAG, btr_cur,
				      node->update, node->cmpl_info,
				      thr, mtr);
	/* TODO: the above can fail with DB_ZIP_OVERFLOW if page_zip != NULL.
	However, this function row_upd_in_place_in_select() is only invoked
	when executing UPDATE statements of the built-in InnoDB SQL parser.
	The built-in SQL is only used for InnoDB system tables, which
	always are in the old, uncompressed format (ROW_FORMAT=REDUNDANT,
	comp == FALSE, page_zip == NULL). */
	ut_ad(err == DB_SUCCESS);
}
