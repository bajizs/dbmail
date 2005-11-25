/*
  Copyright (C) 2004-2005 NFG Net Facilities Group BV, info@nfg.nl

  This program is free software; you can redistribute it and/or 
  modify it under the terms of the GNU General Public License 
  as published by the Free Software Foundation; either 
  version 2 of the License, or (at your option) any later 
  version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/**
 * \file dbmail-mailbox.c
 *
 * implements DbmailMailbox object
 */

#include "dbmail.h"

extern db_param_t _db_params;
#define DBPFX _db_params.pfx

/* internal utilities */


/* class methods */

struct DbmailMailbox * dbmail_mailbox_new(u64_t id)
{
	struct DbmailMailbox *self = g_new0(struct DbmailMailbox, 1);
	assert(self);
	dbmail_mailbox_set_id(self,id);
	dbmail_mailbox_open(self);
	self->search = NULL;
	return self;
}


void dbmail_mailbox_free(struct DbmailMailbox *self)
{
	if (self->ids)
		g_tree_destroy(self->ids);		
	if (self->msn)
		g_tree_destroy(self->msn);
	if (self->search)
		g_node_destroy(self->search);
	if (self->sorted) {
		g_list_destroy(self->sorted);
		self->sorted = NULL;
	}
	
	g_free(self);
}

void dbmail_mailbox_set_id(struct DbmailMailbox *self, u64_t id)
{
	assert(id > 0);
	self->id = id;
}

u64_t dbmail_mailbox_get_id(struct DbmailMailbox *self)
{
	assert(self->id > 0);
	return self->id;
}

struct DbmailMailbox * dbmail_mailbox_open(struct DbmailMailbox *self)
{
	u64_t row,rows;
	GString *q = g_string_new("");
	u64_t *uid, *msn;

	g_string_printf(q, "SELECT message_idnr FROM %smessages "
		 "WHERE mailbox_idnr = '%llu' "
		 "AND status IN ('%d','%d') "
		 "ORDER BY message_idnr", DBPFX, 
		 dbmail_mailbox_get_id(self), 
		 MESSAGE_STATUS_NEW, MESSAGE_STATUS_SEEN);
	
	if (db_query(q->str) == DM_EQUERY) {
		g_string_free(q,TRUE);
		return self;
	}
		
	if ((rows  = db_num_rows()) < 1) {
		trace(TRACE_INFO, "%s,%s: no messages in mailbox",
				__FILE__, __func__);
		db_free_result();
		g_string_free(q,TRUE);
		return self;
	}
	
	if (self->ids) {
		g_tree_destroy(self->ids);
		self->ids = NULL;
	}
	
	self->ids = g_tree_new_full((GCompareDataFunc)ucmp,NULL,(GDestroyNotify)g_free,(GDestroyNotify)g_free);
	self->msn = g_tree_new_full((GCompareDataFunc)ucmp,NULL,NULL,NULL);
	
	for (row=0; row < rows; row++) {
		uid = g_new0(u64_t,1);
		msn = g_new0(u64_t,1);
		*uid= db_get_result_u64(row,0);
		*msn = row+1;
		g_tree_insert(self->ids,uid,msn);
		g_tree_insert(self->msn,msn,uid);
	}
	
	db_free_result();
	return self;
}

#define FROM_STANDARD_DATE "Tue Oct 11 13:06:24 2005"

static size_t dump_message_to_stream(struct DbmailMessage *message, GMimeStream *ostream)
{
	size_t r = 0;
	gchar *s;
	GString *sender;
	GString *date;
	InternetAddressList *ialist;
	InternetAddress *ia;
	
	GString *t;
	
	g_return_val_if_fail(GMIME_IS_MESSAGE(message->content),0);

	s = dbmail_message_to_string(message);

	if (! strncmp(s,"From ",5)==0) {
		ialist = internet_address_parse_string(g_mime_message_get_sender(GMIME_MESSAGE(message->content)));
		sender = g_string_new("nobody@foo");
		if (ialist) {
			ia = ialist->address;
			if (ia) 
				g_string_printf(sender,"%s", ia->value.addr);
		}
		internet_address_list_destroy(ialist);
		
		date = g_string_new(dbmail_message_get_internal_date(message));
		if (date->len < 1)
			date = g_string_new(FROM_STANDARD_DATE);
		
		t = g_string_new("From ");
		g_string_append_printf(t,"%s %s\n", sender->str, date->str);

		r = g_mime_stream_write_string(ostream,t->str);

		g_string_free(t,TRUE);
		g_string_free(sender,TRUE);
		g_string_free(date,TRUE);
		
	}
	
	r += g_mime_stream_write_string(ostream,s);
	r += g_mime_stream_write_string(ostream,"\n");
	
	g_free(s);
	return r;
}

int dbmail_mailbox_dump(struct DbmailMailbox *self, FILE *file)
{
	unsigned i,j;
	int count=0;
	gboolean h;
	GMimeStream *ostream;
	GList *ids, *slice;
	struct DbmailMessage *message = NULL;
	GString *q, *t;

	if (g_tree_nnodes(self->ids) == 0) {
		trace(TRACE_DEBUG,"%s,%s: cannot dump empty mailbox",__FILE__, __func__);
		return 0;
	}

	q = g_string_new("");
	t = g_string_new("");
	ostream = g_mime_stream_file_new(file);
	
	ids = g_tree_keys(self->ids);
	slice = g_list_slices(ids,100);
	slice = g_list_first(slice);
	g_list_free(ids);

	while (slice) {
		g_string_printf(q,"SELECT is_header,messageblk FROM %smessageblks b "
				"JOIN %sphysmessage p ON b.physmessage_id=p.id "
				"JOIN %smessages m ON m.physmessage_id=p.id "
				"WHERE m.message_idnr IN (%s)", DBPFX, DBPFX, DBPFX,
				(char *)slice->data);
		
		if (db_query(q->str) == -1)
			return -1;

		if ((j = db_num_rows()) < 1)
			break;
		
		for (i=0; i<j; i++) {
			h = db_get_result_int(i,0);
			if (h) {
				if (t->len > 0) {
					message = dbmail_message_new();
					message = dbmail_message_init_with_string(message,t);
					if(dump_message_to_stream(message,ostream) > 0)
						count++;
					dbmail_message_free(message);
				}
				g_string_printf(t,"%s", db_get_result(i,1));
			} else {
				g_string_append_printf(t,"%s",db_get_result(i,1));
			}
		}
		db_free_result();

		if (! g_list_next(slice))
			break;
		
		slice = g_list_next(slice);
	}
	
	if (t->len) {
		message = dbmail_message_new();
		message = dbmail_message_init_with_string(message,t);
		if (dump_message_to_stream(message,ostream) > 0)
			count++;
		dbmail_message_free(message);
	}
	 
	g_list_foreach(slice,(GFunc)g_free,NULL);
	g_list_free(slice);
	
	g_string_free(t,TRUE);
	g_string_free(q,TRUE);
	g_object_unref(ostream);
	
	return count;
}

static gboolean _tree_foreach(gpointer key UNUSED, gpointer value, GString * data)
{
	gboolean res = FALSE;
	u64_t *id;
	GList *sublist = (GList *)value;
	GString *t = g_string_new("");
	int m = g_list_length(sublist);
	
	sublist = g_list_first(sublist);
	while(sublist) {
		id = sublist->data;
		g_string_append_printf(t, "(%llu)", *id);
		
		if (! g_list_next(sublist))
			break;
		sublist = g_list_next(sublist);
	}
	if (m > 1)
		g_string_append_printf(data, "(%s)", t->str);
	else
		g_string_append_printf(data, "%s", t->str);

	g_string_free(t,TRUE);

	return res;
}
char * dbmail_mailbox_orderedsubject(struct DbmailMailbox *self, gboolean uid)
{
	GList *sublist = NULL;
	GString *q = g_string_new("");
	u64_t i = 0, r = 0, idnr = 0;
	char *subj;
	char *res = NULL;
	u64_t *id, *msn;
	GTree *tree;
	GString *threads;
	
	/* thread-roots (ordered) */
	g_string_printf(q, "SELECT min(message_idnr),subjectfield "
			"FROM %smessages "
			"JOIN %ssubjectfield USING (physmessage_id) "
			"JOIN %sdatefield USING (physmessage_id) "
			"WHERE mailbox_idnr=%llu "
			"AND status IN (%d, %d) "
			"GROUP BY subjectfield",
			DBPFX, DBPFX, DBPFX,
			dbmail_mailbox_get_id(self),
			MESSAGE_STATUS_NEW, MESSAGE_STATUS_SEEN);

	if (db_query(q->str) == DM_EQUERY) {
		g_string_free(q,TRUE);
		return res;
	}
	if ((r = db_num_rows())==0) {
		g_string_free(q,TRUE);
		return res;
	}
	
	tree = g_tree_new_full((GCompareDataFunc)strcmp,NULL,(GDestroyNotify)g_free, NULL);
	
	i=0;
	while (i < r) {
		idnr = db_get_result_u64(i,0);
		if (! g_tree_lookup(self->ids,&idnr)) {
			i++;
			continue;
		}
		subj = (char *)db_get_result(i,1);
		g_tree_insert(tree,g_strdup(subj), NULL);
		i++;
	}
	db_free_result();
		
	/* full threads (unordered) */
	g_string_printf(q, "SELECT message_idnr,subjectfield "
			"FROM %smessages "
			"JOIN %ssubjectfield using (physmessage_id) "
			"JOIN %sdatefield using (physmessage_id) "
			"WHERE mailbox_idnr=%llu "
			"AND status IN (%d,%d) "
			"ORDER BY subjectfield,datefield", 
			DBPFX, DBPFX, DBPFX,
			dbmail_mailbox_get_id(self),
			MESSAGE_STATUS_NEW,MESSAGE_STATUS_SEEN);
		
	if (db_query(q->str) == DM_EQUERY) {
		g_string_free(q,TRUE);
		return res;
	}
	if ((r = db_num_rows())==0) {
		g_string_free(q,TRUE);
		return res;
	}
	
	i=0;
	while (i < r) {
		idnr = db_get_result_u64(i,0);
		if (! (msn = g_tree_lookup(self->ids, &idnr))) {
			i++;
			continue;
		}
		subj = (char *)db_get_result(i,1);
		
		id = g_new0(u64_t,1);
		if (uid==TRUE)
			*id = idnr;
		else
			*id = *msn;
		
		sublist = g_tree_lookup(tree,(gconstpointer)subj);
		sublist = g_list_append(sublist,id);
		g_tree_insert(tree,g_strdup(subj),sublist);
		i++;
	}
	db_free_result();

	threads = g_string_new("");
	g_tree_foreach(tree,(GTraverseFunc)_tree_foreach,threads);
	res = threads->str;

	g_string_free(threads,FALSE);
	g_string_free(q,TRUE);

	return res;
}

/* imap sorted search */
static search_key_t * append_search(struct DbmailMailbox *self, search_key_t *value, gboolean descend)
{
	GNode *n;
	
	if (self->search)
		n = g_node_append_data(self->search, value);
	else {
		descend = TRUE;
		n = g_node_new(value);
	}
	
	if (descend)
		self->search = n;
	
	trace(TRACE_DEBUG, "%s,%s: [%d] [%d] type [%d] field [%s] search [%s] at depth [%u]\n", 
			__FILE__, __func__, (int)value, descend, 
			value->type, value->hdrfld, value->search, 
			g_node_depth(self->search));
	return g_new0(search_key_t,1);
}

static void _append_join(char *join, char *table)
{
	char *tmp;
	tmp = g_strdup_printf("LEFT JOIN %s%s ON p.id=%s%s.physmessage_id ", DBPFX, table, DBPFX, table);
	g_strlcat(join, tmp, MAX_SEARCH_LEN);
	g_free(tmp);
}

static void _append_sort(char *order, char *field, gboolean reverse)
{
	char *tmp;
	tmp = g_strdup_printf("%s%s,", field, reverse ? " DESC" : "");
	g_strlcat(order, tmp, MAX_SEARCH_LEN);
	g_free(tmp);
}

static int _handle_sort_args(struct DbmailMailbox *self, char **search_keys, search_key_t *value, u64_t *idx)
{
	value->type = IST_SORT;
			
	gboolean reverse = FALSE;

	g_return_val_if_fail(search_keys && search_keys[*idx], -1);

	char *key = search_keys[*idx];
	
	if ( MATCH(key, "reverse") ) {
		reverse = TRUE;
		(*idx)++;
		key = search_keys[*idx];
	} 
	
	if ( MATCH(key, "arrival") ) {
		_append_sort(value->order, "internal_date", reverse);
		(*idx)++;
	} 
	
	else if ( MATCH(key, "size") ) {
		_append_sort(value->order, "messagesize", reverse);
		(*idx)++;
	} 
	
	else if ( MATCH(key, "from") ) {
		_append_join(value->table, "fromfield");
		_append_sort(value->order, "fromaddr", reverse);	
		(*idx)++;
	} 
	
	else if ( MATCH(key, "subject") ) {
		_append_join(value->table, "subjectfield");
		_append_sort(value->order, "subjectfield", reverse);
		(*idx)++;
	} 
	
	else if ( MATCH(key, "cc") ) {
		_append_join(value->table, "ccfield");
		_append_sort(value->order, "ccaddr", reverse);
		(*idx)++;
	} 
	
	else if ( MATCH(key, "to") ) {
		_append_join(value->table, "tofield");
		_append_sort(value->order, "toaddr", reverse);
		(*idx)++;
	} 
	
	else if ( MATCH(key, "date") ) {
		_append_join(value->table, "datefield");
		_append_sort(value->order, "datefield", reverse);
		(*idx)++;
	}	

	else if ( MATCH(key, "(") )
		(*idx)++;

	else if ( MATCH(key, ")") ) 
		(*idx)++;
	
	else if ( MATCH(key, "utf-8") ) 
		(*idx)++;
	
	else if ( MATCH(key, "us-ascii") ) 
		(*idx)++;
	
	else if ( MATCH(key, "iso-8859-1") ) 
		(*idx)++;

	else {
		if (value->type)
			value = append_search(self, value, 0);
		return 1; /* done */
	}

	return 0;
}

static void pop_search(struct DbmailMailbox *self)
{
	// switch back to parent 
	if (self->search && self->search->parent) 
		self->search = self->search->parent;
}

static int _handle_search_args(struct DbmailMailbox *self, char **search_keys, u64_t *idx)
{
	int result = 0;

	g_return_val_if_fail(search_keys && search_keys[*idx], -1);

	char *key = search_keys[*idx];

	search_key_t *value = g_new0(search_key_t,1);
	
	/* SEARCH */

	if ( MATCH(key, "all") ) {
		value->type = IST_SET;
		strcpy(value->search, "1:*");
		(*idx)++;
		
	} 
	
	else if ( MATCH(key, "uid") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		g_return_val_if_fail(check_msg_set(search_keys[*idx + 1]),-1);
		value->type = IST_SET_UID;
		(*idx)++;
		strncpy(value->search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	}

	/*
	 * FLAG search keys
	 */

	else if ( MATCH(key, "answered") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "answered_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
		
	} else if ( MATCH(key, "deleted") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "deleted_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
		
	} else if ( MATCH(key, "flagged") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "flagged_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
		
	} else if ( MATCH(key, "recent") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "recent_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
		
	} else if ( MATCH(key, "seen") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "seen_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
		
	} else if ( MATCH(key, "keyword") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_SET;
		(*idx)++;
		strcpy(value->search, "0");
		(*idx)++;
		
	} else if ( MATCH(key, "draft") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "draft_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
		
	} else if ( MATCH(key, "new") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "(seen_flag=0 AND recent_flag=1)", MAX_SEARCH_LEN);
		(*idx)++;
		
	} else if ( MATCH(key, "old") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "recent_flag=0", MAX_SEARCH_LEN);
		(*idx)++;
		
	} else if ( MATCH(key, "unanswered") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "answered_flag=0", MAX_SEARCH_LEN);
		(*idx)++;

	} else if ( MATCH(key, "undeleted") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "deleted_flag=0", MAX_SEARCH_LEN);
		(*idx)++;
	
	} else if ( MATCH(key, "unflagged") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "flagged_flag=0", MAX_SEARCH_LEN);
		(*idx)++;
	
	} else if ( MATCH(key, "unseen") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "seen_flag=0", MAX_SEARCH_LEN);
		(*idx)++;
	
	} else if ( MATCH(key, "unkeyword") ) {
		g_return_val_if_fail(search_keys[(*idx) + 1],-1);
		value->type = IST_SET;
		(*idx)++;
		strcpy(value->search, "1:*");
		(*idx)++;
	
	} else if ( MATCH(key, "undraft") ) {
		value->type = IST_FLAG;
		strncpy(value->search, "draft_flag=0", MAX_SEARCH_LEN);
		(*idx)++;
	
	}

	/*
	 * HEADER search keys
	 */

	else if ( MATCH(key, "bcc") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_HDR;
		strncpy(value->hdrfld, "bcc", MIME_FIELD_MAX);
		(*idx)++;
		strncpy(value->search, search_keys[*idx], MAX_SEARCH_LEN);
		(*idx)++;
		
	} else if ( MATCH(key, "cc") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_HDR;
		strncpy(value->hdrfld, "cc", MIME_FIELD_MAX);
		(*idx)++;
		strncpy(value->search, search_keys[*idx], MAX_SEARCH_LEN);
		(*idx)++;
	
	} else if ( MATCH(key, "from") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_HDR;
		strncpy(value->hdrfld, "from", MIME_FIELD_MAX);
		(*idx)++;
		strncpy(value->search, search_keys[*idx], MAX_SEARCH_LEN);
		(*idx)++;
	
	} else if ( MATCH(key, "to") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_HDR;
		strncpy(value->hdrfld, "to", MIME_FIELD_MAX);
		(*idx)++;
		strncpy(value->search, search_keys[*idx], MAX_SEARCH_LEN);
		(*idx)++;
	
	} else if ( MATCH(key, "subject") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_HDR;
		strncpy(value->hdrfld, "subject", MIME_FIELD_MAX);
		(*idx)++;
		strncpy(value->search, search_keys[*idx], MAX_SEARCH_LEN);
		(*idx)++;
	
	} else if ( MATCH(key, "header") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		g_return_val_if_fail(search_keys[*idx + 2], -1);
		value->type = IST_HDR;
		strncpy(value->hdrfld, search_keys[*idx + 1], MIME_FIELD_MAX);
		strncpy(value->search, search_keys[*idx + 2], MAX_SEARCH_LEN);
		(*idx) += 3;

	} else if ( MATCH(key, "sentbefore") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_HDRDATE_BEFORE;
		strncpy(value->hdrfld, "datefield", MIME_FIELD_MAX);
		(*idx)++;
		strncpy(value->search, search_keys[*idx], MAX_SEARCH_LEN);
		(*idx)++;

	} else if ( MATCH(key, "senton") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_HDRDATE_ON;
		strncpy(value->hdrfld, "datefield", MIME_FIELD_MAX);
		(*idx)++;
		strncpy(value->search, search_keys[*idx], MAX_SEARCH_LEN);
		(*idx)++;
	
	} else if ( MATCH(key, "sentsince") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_HDRDATE_SINCE;
		strncpy(value->hdrfld, "datefield", MIME_FIELD_MAX);
		(*idx)++;
		strncpy(value->search, search_keys[*idx], MAX_SEARCH_LEN);
		(*idx)++;
	
	}

	/*
	 * INTERNALDATE keys
	 */

	else if ( MATCH(key, "before") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		g_return_val_if_fail(check_date(search_keys[*idx + 1]),-1);
		value->type = IST_IDATE;
		(*idx)++;
		g_snprintf(value->search, MAX_SEARCH_LEN, "internal_date < '%s'", date_imap2sql(search_keys[*idx]));
		(*idx)++;
		
	} else if ( MATCH(key, "on") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		g_return_val_if_fail(check_date(search_keys[*idx + 1]),-1);
		value->type = IST_IDATE;
		(*idx)++;
		g_snprintf(value->search, MAX_SEARCH_LEN, "internal_date LIKE '%s%%'", date_imap2sql(search_keys[*idx]));
		(*idx)++;
		
	} else if ( MATCH(key, "since") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		g_return_val_if_fail(check_date(search_keys[*idx + 1]),-1);
		value->type = IST_IDATE;
		(*idx)++;
		g_snprintf(value->search, MAX_SEARCH_LEN, "internal_date > '%s'", date_imap2sql(search_keys[*idx]));
		(*idx)++;
	}

	/*
	 * DATA-keys
	 */

	else if ( MATCH(key, "body") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_DATA_BODY;
		(*idx)++;
		strncpy(value->search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	
	} else if ( MATCH(key, "text") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_DATA_TEXT;
		(*idx)++;
		strncpy(value->search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	}

	/*
	 * SIZE keys
	 */

	else if ( MATCH(key, "larger") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_SIZE_LARGER;
		(*idx)++;
		value->size = strtoull(search_keys[(*idx)], NULL, 10);
		(*idx)++;
	
	} else if ( MATCH(key, "smaller") ) {
		g_return_val_if_fail(search_keys[*idx + 1], -1);
		value->type = IST_SIZE_SMALLER;
		(*idx)++;
		value->size = strtoull(search_keys[(*idx)], NULL, 10);
		(*idx)++;
	
	}

	/*
	 * NOT, OR, ()
	 */
	
	else if ( MATCH(key, "not") ) {
		value->type = IST_SUBSEARCH_NOT;
		(*idx)++;
	
		if ((result = dbmail_mailbox_build_imap_search(self, search_keys, idx, 0)) < 0) 
			return result;
		
	} else if ( MATCH(key, "or") ) {
		value->type = IST_SUBSEARCH_OR;
		(*idx)++;
		
		value = append_search(self, value, 1);
		if ((result = _handle_search_args(self, search_keys, idx)) < 0)
			return result;
		if ((result =  _handle_search_args(self, search_keys, idx)) < 0)
			return result;
		pop_search(self);
		
		return result;

	} else if ( MATCH(key, "(") ) {
		value->type = IST_SUBSEARCH_AND;
		(*idx)++;
		
		value = append_search(self,value,1);
		while ((result = dbmail_mailbox_build_imap_search(self, search_keys, idx, 0)) == 0);
		pop_search(self);
		
		return result;

	} else if ( MATCH(key, ")") ) {
		(*idx)++;
		
		return 1;
	
	} else if (check_msg_set(key)) {
		value->type = IST_SET;
		strncpy(value->search, search_keys[*idx], MAX_SEARCH_LEN);
		(*idx)++;
	
	} else {
		/* unknown search key */
		return -1;
	}
	
	append_search(self, value, 0);

	return 0;
}

/*
 * build_imap_search()
 *
 * builds a linked list of search items from a set of IMAP search keys
 * sl should be initialized; new search items are simply added to the list
 *
 * returns -1 on syntax error, -2 on memory error; 0 on success, 1 if ')' has been encountered
 */
int dbmail_mailbox_build_imap_search(struct DbmailMailbox *self, char **search_keys, u64_t *idx, int sorted)
{
	int result = 0;
	search_key_t * value;
	
	if (! (search_keys && search_keys[*idx]))
		return 1;

	/* SORT */
	if (sorted) {
		value = g_new0(search_key_t,1);
		while(((result = _handle_sort_args(self, search_keys, value, idx)) == 0) && search_keys[*idx]);
	}

	/* SEARCH */
	while(((result = _handle_search_args(self, search_keys, idx)) == 0) && search_keys[*idx]);
	
	return result;
}


static gboolean _do_sort(GNode *node, struct DbmailMailbox *self)
{
	GString *q;
	u64_t *id;
	unsigned i, rows;
	search_key_t *s = (search_key_t *)node->data;
	
	if (s->type != IST_SORT)
		return TRUE;
	
	
	q = g_string_new("");
	g_string_printf(q, "SELECT message_idnr FROM %smessages m "
			 "LEFT JOIN %sphysmessage p ON m.physmessage_id=p.id "
			 "%s"
			 "WHERE m.mailbox_idnr = '%llu' AND m.status IN ('%d','%d') " 
			 "ORDER BY %smessage_idnr", DBPFX, DBPFX, s->table,
			 dbmail_mailbox_get_id(self), MESSAGE_STATUS_NEW, MESSAGE_STATUS_SEEN, s->order);

	if (db_query(q->str) == -1)
		return TRUE;

	if (self->sorted) {
		g_list_destroy(self->sorted);
		self->sorted = NULL;
	}
	
	rows = db_num_rows();
	for (i=0; i< rows; i++) {
		id = g_new0(u64_t,1);
		*id = db_get_result_u64(i,0);
		self->sorted = g_list_append(self->sorted,id);
	}
	g_string_free(q,TRUE);
	db_free_result();
	
	return FALSE;
}
static GTree * mailbox_search(struct DbmailMailbox *self, search_key_t *s)
{
	unsigned i, rows, date;
	u64_t *k, *v, *w;
	u64_t id;
	
	GString *t = g_string_new("");
	GString *q = g_string_new("");

	if (!s->search)
		return NULL;

	switch (s->type) {
		case IST_HDRDATE_ON:
		case IST_HDRDATE_SINCE:
		case IST_HDRDATE_BEFORE:

		date = num_from_imapdate(s->search);
		if (s->type == IST_HDRDATE_SINCE)
			g_string_printf(t,"%s >= %d", s->hdrfld, date);
		else if (s->type == IST_HDRDATE_BEFORE)
			g_string_printf(t,"%s < %d", s->hdrfld, date);
		else
			g_string_printf(t,"%s >= %d AND %s < %d", s->hdrfld, date, s->hdrfld, date+1);
		
		g_string_printf(q,"SELECT message_idnr FROM %smessages m "
			"JOIN %sphysmessage p ON m.physmessage_id=p.id "
			"JOIN %sdatefield d ON d.physmessage_id=p.id "
			"WHERE mailbox_idnr= %llu AND status IN ('%d','%d') "
			"AND %s "
			"ORDER BY message_idnr", DBPFX, DBPFX, DBPFX,
			dbmail_mailbox_get_id(self), 
			MESSAGE_STATUS_NEW, MESSAGE_STATUS_SEEN, 
			t->str);
			break;
			
		case IST_HDR:
		
		g_string_printf(q, "SELECT message_idnr FROM %smessages m "
			 "JOIN %sphysmessage p ON m.physmessage_id=p.id "
			 "JOIN %sheadervalue v ON v.physmessage_id=p.id "
			 "JOIN %sheadername n ON v.headername_id=n.id "
			 "WHERE mailbox_idnr = %llu "
			 "AND status IN ('%d','%d') "
			 "AND headername = '%s' AND headervalue LIKE '%%%s%%' "
			 "ORDER BY message_idnr", 
			 DBPFX, DBPFX, DBPFX, DBPFX,
			 dbmail_mailbox_get_id(self), 
			 MESSAGE_STATUS_NEW, MESSAGE_STATUS_SEEN, 
			 s->hdrfld, s->search);
			break;

		case IST_IDATE:
			
		g_string_printf(q, "SELECT message_idnr FROM %smessages m "
			 "JOIN %sphysmessage p ON m.physmessage_id=p.id "
			 "WHERE mailbox_idnr = '%llu' "
			 "AND status IN ('%d','%d') AND p.%s "
			 "ORDER BY message_idnr", 
			 DBPFX, DBPFX, 
			 dbmail_mailbox_get_id(self),
			 MESSAGE_STATUS_NEW, MESSAGE_STATUS_SEEN, 
			 s->search);
		break;
		
		default:
		g_string_printf(q, "SELECT message_idnr FROM %smessages "
			 "WHERE mailbox_idnr = '%llu' "
			 "AND status IN ('%d','%d') AND %s "
			 "ORDER BY message_idnr", DBPFX, 
			 dbmail_mailbox_get_id(self), 
			 MESSAGE_STATUS_NEW, MESSAGE_STATUS_SEEN, 
			 s->search);
		break;
		
	}
	
	g_string_free(t,TRUE);
	if (db_query(q->str) == -1) {
		trace(TRACE_ERROR, "%s,%s: could not execute query",
		      __FILE__, __func__);
		g_string_free(q,TRUE);
		return NULL;
	}
	g_string_free(q,TRUE);

	rows = db_num_rows();
	s->found = g_tree_new_full((GCompareDataFunc)ucmp,NULL,(GDestroyNotify)g_free, (GDestroyNotify)g_free);
	
	for (i=0; i < rows; i++) {
		k = g_new0(u64_t,1);
		v = g_new0(u64_t,1);
		id = db_get_result_u64(i,0);
		if (! (w = g_tree_lookup(self->ids, &id))) {
			trace(TRACE_ERROR, "%s,%s: key missing in self->ids: [%llu]\n", 
					__FILE__, __func__, id);
			continue;
		}
		assert(w);
		
		*k = id;
		*v = *w;

		g_tree_insert(s->found, k, v);
	}

	db_free_result();
	return s->found;
}

GTree * dbmail_mailbox_get_set(struct DbmailMailbox *self, search_key_t *sk)
{
	GList *ids = NULL, *sets = NULL;
	GString *t;
	char *rest;
	u64_t i, l, r, lo = 0, hi = 0;
	u64_t *k, *v, *w = NULL;
	GTree *a, *b;
	
	b = g_tree_new_full((GCompareDataFunc)ucmp,NULL, (GDestroyNotify)g_free, (GDestroyNotify)g_free);
	
	if (! (self->ids && sk->search))
		return b;

	g_return_val_if_fail(g_tree_nnodes(self->ids)>0,b);

	switch(sk->type) {
		case IST_SET_UID:
			ids = g_tree_keys(self->ids);
			ids = g_list_last(ids);
			hi = *((u64_t *)ids->data);
			ids = g_list_first(ids);
			lo = *((u64_t *)ids->data);
			g_list_free(ids);
			break;
		case IST_SET:
			lo = 1;
			hi = g_tree_nnodes(self->ids);
			break;
	}
	
	a = g_tree_new_full((GCompareDataFunc)ucmp,NULL, (GDestroyNotify)g_free, (GDestroyNotify)g_free);

	t = g_string_new(sk->search);
	
	sets = g_string_split(t,",");
	sets = g_list_first(sets);
	
	while(sets) {
		
		l = 0;
		r = 0;
		
		if (strlen((char *)sets->data) < 1)
			break;
		
		rest = sets->data;
		
		if (rest[0] == '*') {
			l = hi;
			r = l;

			rest++;
			if (rest[0]==':') {
				rest++;
				
				if (rest[0] == '*') {
					r = hi;
				} else {
					r = strtoull(rest,NULL,10);
					if (!r || r > hi)
						break;
					
					if (r < lo)
						r = lo;
				}
			}

		} else {
			l = strtoull(sets->data,&rest,10);
			if (! l)
				break;
			
			l = max(l,lo);
			r = l;
			
			if (rest[0]==':') {
				rest++;
			
				if (rest[0] == '*')
					r = hi;
				else {
					r = strtoull(rest,NULL,10);
					if (! r || r > hi)
						break;
					if (r < lo)
						r = lo;
				}
			}
		}
		
		if (! (l && r))
			break;
		
		for (i = min(l,r); i <= max(l,r); i++) {

			switch (sk->type) {
				case IST_SET_UID:
					if (! (w = g_tree_lookup(self->ids,&i))) 
						continue;
					break;
				case IST_SET:
					if (! (w = g_tree_lookup(self->msn,&i)))
						continue;
					break;
			}

			k = g_new0(u64_t,1);
			v = g_new0(u64_t,1);
			
			*k = i;
			*v = *w;
			
			g_tree_insert(a,k,v);

		}
		
		g_tree_merge(b,a,IST_SUBSEARCH_OR);
		
		if (! g_list_next(sets))
			break;

		sets = g_list_next(sets);
	}

	if (a)
		g_tree_destroy(a);
	
	return b;
}

static gboolean _do_search(GNode *node, struct DbmailMailbox *self)
{
	search_key_t *s = (search_key_t *)node->data;
	GTree *set = NULL;
	

	switch (s->type) {
		case IST_SET:
		case IST_SET_UID:
			if (! (set = dbmail_mailbox_get_set(self, s)))
				return TRUE;
			break;

		case IST_SIZE_LARGER:
		case IST_SIZE_SMALLER:
		case IST_HDRDATE_BEFORE:
		case IST_HDRDATE_SINCE:
		case IST_HDRDATE_ON:
		case IST_IDATE:
		case IST_FLAG:
		case IST_HDR:
			if (! (set = mailbox_search(self, s)))
				return TRUE;
			break;
		/* 
		 * these all have in common that all messages need to be parsed 
		 */
		case IST_DATA_BODY:
		case IST_DATA_TEXT:
			//result = db_search_parsed(rset, setlen, sk, mb, condition);
			break;

		case IST_SUBSEARCH_NOT:
		case IST_SUBSEARCH_AND:
		case IST_SUBSEARCH_OR:
			g_node_children_foreach(node, G_TRAVERSE_ALL, (GNodeForeachFunc)_do_search, (gpointer)self);
			break;

		case IST_SORT:
			break;

		default:
			return TRUE;
	}

	s->found = set;

	trace(TRACE_DEBUG,"%s,%s: type [%d] rows [%d]\n", __FILE__,  __func__, 
			s->type, set ? g_tree_nnodes(set): 0);

	return FALSE;
}	
static gboolean _merge_search(GNode *node, GTree *found)
{
	search_key_t *s = (search_key_t *)node->data;
	search_key_t *a, *b;
	GNode *x, *y;

	switch(s->type) {
		case IST_SUBSEARCH_AND:
			g_node_children_foreach(node, G_TRAVERSE_ALL, (GNodeForeachFunc)_merge_search, (gpointer)found);
			break;
			
		case IST_SUBSEARCH_NOT:
			g_tree_merge(found, s->found, IST_SUBSEARCH_NOT);
			break;
			
		case IST_SUBSEARCH_OR:
			x = g_node_nth_child(node,0);
			y = g_node_nth_child(node,1);
			a = (search_key_t *)x->data;
			b = (search_key_t *)y->data;
			g_tree_merge(a->found,b->found,IST_SUBSEARCH_OR);
			g_tree_merge(found,a->found,IST_SUBSEARCH_AND);
			break;
			
		default:
			g_tree_merge(found, s->found, IST_SUBSEARCH_AND);
			break;
	}

	return FALSE;
}
	
int dbmail_mailbox_sort(struct DbmailMailbox *self) 
{
	if (! self->search)
		return 0;
	
	g_node_traverse(g_node_get_root(self->search), G_IN_ORDER, G_TRAVERSE_LEAVES, -1, (GNodeTraverseFunc)_do_sort, (gpointer)self);
	
	return 0;
}


int dbmail_mailbox_search(struct DbmailMailbox *self) 
{
	if (! self->search)
		return 0;
	
	g_node_children_foreach(g_node_get_root(self->search), G_TRAVERSE_ALL, (GNodeForeachFunc)_do_search, (gpointer)self);
	g_node_children_foreach(g_node_get_root(self->search), G_TRAVERSE_ALL, (GNodeForeachFunc)_merge_search, (gpointer)self->ids);
	
	trace(TRACE_DEBUG,"%s,%s: found [%d] ids\n", 
			__FILE__, __func__, 
			g_tree_nnodes(self->ids));
	
	return 0;
}


#ifdef OLD

/* 
 * build_set()
 *
 * builds a msn-set from a IMAP message set spec. the IMAP set is supposed to be correct,
 * no checks are performed.
 */
static void build_set(unsigned int *set, unsigned int setlen, char *cset)
{
	unsigned int i;
	u64_t num, num2;
	char *sep = NULL;

	if ((! set) || (! cset))
		return;

	memset(set, 0, setlen * sizeof(int));

	do {
		num = strtoull(cset, &sep, 10);
		if (num <= setlen && num > 0) {
			if (!*sep)
				set[num - 1] = 1;
			else if (*sep == ',') {
				set[num - 1] = 1;
				cset = sep + 1;
			} else {
				/* sep == ':' here */
				sep++;
				if (*sep == '*') {
					for (i = num - 1; i < setlen; i++)
						set[i] = 1;
					cset = sep + 1;
				} else {
					cset = sep;
					num2 = strtoull(cset, &sep, 10);

					if (num2 > setlen)
						num2 = setlen;
					if (num2 > 0) {
						/* NOTE: here: num2 > 0, num > 0 */
						if (num2 < num) {
							/* swap! */
							i = num;
							num = num2;
							num2 = i;
						}

						for (i = num - 1; i < num2; i++)
							set[i] = 1;
					}
					if (*sep)
						cset = sep + 1;
				}
			}
		} else if (*sep) {
			/* invalid char, skip it */
			cset = sep + 1;
			sep++;
		}
	} while (sep && *sep && cset && *cset);
}


/* 
 * build_uid_set()
 *
 * as build_set() but takes uid's instead of MSN's
 */
static void build_uid_set(unsigned int *set, unsigned int setlen, char *cset,
		   mailbox_t * mb)
{
	unsigned int i, msn, msn2;
	int result;
	int num2found = 0;
	u64_t num, num2;
	char *sep = NULL;

	if (!set)
		return;

	memset(set, 0, setlen * sizeof(int));

	if (!cset || setlen == 0)
		return;

	do {
		num = strtoull(cset, &sep, 10);
		result =
		    binary_search(mb->seq_list, mb->exists, num, &msn);

		if (result < 0 && num < mb->seq_list[mb->exists - 1]) {
			/* ok this num is not a UID, but if a range is specified (i.e. 1:*) 
			 * it is valid -> check *sep
			 */
			if (*sep == ':') {
				result = 1;
				for (msn = 0; mb->seq_list[msn] < num;
				     msn++);
				if (msn >= mb->exists)
					msn = mb->exists - 1;
			}
		}

		if (result >= 0) {
			if (!*sep)
				set[msn] = 1;
			else if (*sep == ',') {
				set[msn] = 1;
				cset = sep + 1;
			} else {
				/* sep == ':' here */
				sep++;
				if (*sep == '*') {
					for (i = msn; i < setlen; i++)
						set[i] = 1;

					cset = sep + 1;
				} else {
					/* fetch second number */
					cset = sep;
					num2 = strtoull(cset, &sep, 10);
					result =
					    binary_search(mb->seq_list,
							  mb->exists, num2,
							  &msn2);

					if (result < 0) {
						/* in a range: (like 1:1000) so this number doesnt need to exist;
						 * find the closest match below this UID value
						 */
						if (mb->exists == 0)
							num2found = 0;
						else {
							for (msn2 =
							     mb->exists -
							     1;; msn2--) {
								if (msn2 ==
								    0
								    && mb->
								    seq_list
								    [msn2]
								    > num2) {
									num2found
									    =
									    0;
									break;
								} else
								    if
								    (mb->
								     seq_list
								     [msn2]
								     <=
								     num2)
								{
									/* found! */
									num2found
									    =
									    1;
									break;
								}
							}
						}

					} else
						num2found = 1;

					if (num2found == 1) {
						if (msn2 < msn) {
							/* swap! */
							i = msn;
							msn = msn2;
							msn2 = i;
						}

						for (i = msn; i <= msn2;
						     i++)
							set[i] = 1;
					}

					if (*sep)
						cset = sep + 1;
				}
			}
		} else {
			/* invalid num, skip it */
			if (*sep) {
				cset = sep + 1;
				sep++;
			}
		}
	} while (sep && *sep && cset && *cset);
}

#endif
