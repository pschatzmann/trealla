#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <float.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#define USE_MMAP 0
#else
#ifndef USE_MMAP
#define USE_MMAP 1
#endif
#if USE_MMAP
#include <sys/mman.h>
#endif
#endif

#include "trealla.h"
#include "internal.h"
#include "network.h"
#include "base64.h"
#include "library.h"
#include "parser.h"
#include "module.h"
#include "prolog.h"
#include "query.h"
#include "heap.h"
#include "utf8.h"
#include "history.h"

static int get_named_stream(prolog *pl, const char *name, size_t len)
{
	for (int i = 0; i < MAX_STREAMS; i++) {
		stream *str = &pl->streams[i];

		if (!str->fp)
			continue;

		if (str->name && (strlen(str->name) == len)
			&& !strncmp(str->name, name, len))
			return i;

		if (str->filename && (strlen(str->filename) == len)
			&& !strncmp(str->filename, name, len))
			return i;
	}

	return -1;
}

static bool is_closed_stream(prolog *pl, cell *p1)
{
	if (!(p1->flags&FLAG_INT_STREAM))
		return false;

	if (pl->streams[get_smallint(p1)].fp)
		return false;

	return true;
}

static void add_stream_properties(query *q, int n)
{
	stream *str = &q->pl->streams[n];
	char tmpbuf[1024*8];
	char *dst = tmpbuf;
	*dst = '\0';
	off_t pos = ftello(str->fp);
	bool at_end_of_file = false;

	if (!str->at_end_of_file && (n > 2)) {
		if (str->p) {
			if (str->p->srcptr && *str->p->srcptr) {
				int ch = get_char_utf8((const char**)&str->p->srcptr);
				str->ungetch = ch;
			}
		}

		int ch = str->ungetch ? str->ungetch : net_getc(str);

		if (str->ungetch)
			;
		else if (feof(str->fp) || ferror(str->fp)) {
			clearerr(str->fp);

			if (str->eof_action != eof_action_reset)
				at_end_of_file = true;
		} else
			str->ungetch = ch;
	}

	char tmpbuf2[1024];
	formatted(tmpbuf2, sizeof(tmpbuf2), str->name, strlen(str->name), false);
	dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, alias('%s')).\n", n, tmpbuf2);
	formatted(tmpbuf2, sizeof(tmpbuf2), str->filename, strlen(str->filename), false);
	dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, file_name('%s')).\n", n, tmpbuf2);
	dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, mode(%s)).\n", n, str->mode);
	dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, type(%s)).\n", n, str->binary ? "binary" : "text");
	dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, line_count(%d)).\n", n, str->p ? str->p->line_nbr : 1);
	dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, position(%llu)).\n", n, (unsigned long long)(pos != -1 ? pos : 0));
	dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, reposition(%s)).\n", n, (n < 3) || str->socket ? "false" : "true");
	dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, end_of_stream(%s)).\n", n, str->at_end_of_file ? "past" : at_end_of_file ? "at" : "not");
	dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, eof_action(%s)).\n", n, str->eof_action == eof_action_eof_code ? "eof_code" : str->eof_action == eof_action_error ? "error" : str->eof_action == eof_action_reset ? "reset" : "none");

	if (!str->binary) {
		dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, bom(%s)).\n", n, str->bom ? "true" : "false");
		dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, encoding('%s')).\n", n, "UTF-8");
	}

	if (!strcmp(str->mode, "read"))
		dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, input).\n", n);
	else
		dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, output).\n", n);

	dst += snprintf(dst, sizeof(tmpbuf)-strlen(tmpbuf), "'$stream_property'(%d, newline(%s)).\n", n, NEWLINE_MODE);

	parser *p = create_parser(q->st.m);
	p->srcptr = tmpbuf;
	p->consulting = true;
	tokenize(p, false, false);
	destroy_parser(p);
}

#ifndef SANDBOX
static void del_stream_properties(query *q, int n)
{
	cell *tmp = alloc_on_heap(q, 3);
	make_literal(tmp+0, g_sys_stream_property_s);
	make_int(tmp+1, n);
	make_variable(tmp+2, g_anon_s, create_vars(q, 1));
	tmp->nbr_cells = 3;
	tmp->arity = 2;
	q->retry = QUERY_OK;

#if 0
	predicate *pr = find_predicate(q->st.m, tmp);

	if (!pr) {
		DISCARD_RESULT throw_error(q, tmp, "existence_error", "procedure");
		return;
	}
#endif

	while (do_retract(q, tmp, q->st.curr_frame, DO_STREAM_RETRACT)) {
		if (q->did_throw)
			return;

		q->retry = QUERY_RETRY;
		retry_choice(q);
	}

	q->retry = QUERY_OK;
}
#endif

static pl_status do_stream_property(query *q)
{
	GET_FIRST_ARG(pstr,any);
	GET_NEXT_ARG(p1,any);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	cell *c = p1 + 1;
	c = deref(q, c, p1_ctx);
	pl_idx_t c_ctx = q->latest_ctx;

	if (!CMP_SLICE2(q, p1, "file_name")) {
		cell tmp;
		may_error(make_cstring(&tmp, str->filename));
		pl_status ok = unify(q, c, c_ctx, &tmp, q->st.curr_frame);
		unshare_cell(&tmp);
		return ok;
	}

	if (!CMP_SLICE2(q, p1, "alias")) {
		cell tmp;
		may_error(make_cstring(&tmp, str->name));
		pl_status ok = unify(q, c, c_ctx, &tmp, q->st.curr_frame);
		unshare_cell(&tmp);
		return ok;
	}

	if (!CMP_SLICE2(q, p1, "mode")) {
		cell tmp;
		may_error(make_cstring(&tmp, str->mode));
		pl_status ok = unify(q, c, c_ctx, &tmp, q->st.curr_frame);
		unshare_cell(&tmp);
		return ok;
	}

	if (!CMP_SLICE2(q, p1, "bom") && !str->binary) {
		cell tmp;
		may_error(make_cstring(&tmp, str->bom?"true":"false"));
		pl_status ok = unify(q, c, c_ctx, &tmp, q->st.curr_frame);
		unshare_cell(&tmp);
		return ok;
	}

	if (!CMP_SLICE2(q, p1, "type")) {
		cell tmp;
		may_error(make_cstring(&tmp, str->binary ? "binary" : "text"));
		pl_status ok = unify(q, c, c_ctx, &tmp, q->st.curr_frame);
		unshare_cell(&tmp);
		return ok;
	}

	if (!CMP_SLICE2(q, p1, "reposition")) {
		cell tmp;
		may_error(make_cstring(&tmp, str->socket || (n <= 2) ? "false" : "true"));
		pl_status ok = unify(q, c, c_ctx, &tmp, q->st.curr_frame);
		unshare_cell(&tmp);
		return ok;
	}

	if (!CMP_SLICE2(q, p1, "encoding") && !str->binary) {
		cell tmp;
		may_error(make_cstring(&tmp, "UTF-8"));
		pl_status ok = unify(q, c, c_ctx, &tmp, q->st.curr_frame);
		unshare_cell(&tmp);
		return ok;
	}

	if (!CMP_SLICE2(q, p1, "newline")) {
		cell tmp;
		may_error(make_cstring(&tmp, NEWLINE_MODE));
		pl_status ok = unify(q, c, c_ctx, &tmp, q->st.curr_frame);
		unshare_cell(&tmp);
		return ok;
	}

	if (!CMP_SLICE2(q, p1, "input"))
		return !strcmp(str->mode, "read");

	if (!CMP_SLICE2(q, p1, "output"))
		return strcmp(str->mode, "read");

	if (!CMP_SLICE2(q, p1, "eof_action") && is_stream(pstr)) {
		cell tmp;

		if (str->eof_action == eof_action_eof_code)
			make_literal(&tmp, index_from_pool(q->pl, "eof_code"));
		else if (str->eof_action == eof_action_error)
			make_literal(&tmp, index_from_pool(q->pl, "error"));
		else if (str->eof_action == eof_action_reset)
			make_literal(&tmp, index_from_pool(q->pl, "reset"));
		else
			make_literal(&tmp, index_from_pool(q->pl, "none"));

		return unify(q, c, c_ctx, &tmp, q->st.curr_frame);
	}

	if (!CMP_SLICE2(q, p1, "end_of_stream") && is_stream(pstr)) {
		bool at_end_of_file = false;

		if (!str->at_end_of_file && (n > 2)) {
			if (str->p) {
				if (str->p->srcptr && *str->p->srcptr) {
					int ch = get_char_utf8((const char**)&str->p->srcptr);
					str->ungetch = ch;
				}
			}

			int ch = str->ungetch ? str->ungetch : net_getc(str);

			if (str->ungetch)
				;
			else if (feof(str->fp) || ferror(str->fp)) {
				clearerr(str->fp);

				if (str->eof_action != eof_action_reset)
					at_end_of_file = true;
			} else
				str->ungetch = ch;
		}

		cell tmp;

		if (str->at_end_of_file)
			make_literal(&tmp, index_from_pool(q->pl, "past"));
		else if (at_end_of_file)
			make_literal(&tmp, index_from_pool(q->pl, "at"));
		else
			make_literal(&tmp, index_from_pool(q->pl, "not"));

		return unify(q, c, c_ctx, &tmp, q->st.curr_frame);
	}

	if (!CMP_SLICE2(q, p1, "position") && !is_variable(pstr)) {
		cell tmp;
		make_int(&tmp, ftello(str->fp));
		return unify(q, c, c_ctx, &tmp, q->st.curr_frame);
	}

	if (!CMP_SLICE2(q, p1, "line_count") && !is_variable(pstr)) {
		cell tmp;
		make_int(&tmp, str->p->line_nbr);
		return unify(q, c, c_ctx, &tmp, q->st.curr_frame);
	}

	return pl_failure;
}

static void clear_streams_properties(query *q)
{
	cell tmp;
	make_literal(&tmp, g_sys_stream_property_s);
	tmp.nbr_cells = 1;
	tmp.arity = 2;

	predicate *pr = find_predicate(q->st.m, &tmp);

	if (pr) {
		for (db_entry *dbe = pr->head; dbe;) {
			db_entry *save = dbe;
			dbe = dbe->next;
			add_to_dirty_list(q->st.m, save);
		}

		pr->head = pr->tail = NULL;
		pr->cnt = 0;
	}
}

static const char *s_properties =
	"alias,file_name,mode,encoding,type,line_count,"			\
	"position,reposition,end_of_stream,eof_action,"				\
	"input,output,newline";

static USE_RESULT pl_status fn_iso_stream_property_2(query *q)
{
	GET_FIRST_ARG(pstr,any);
	GET_NEXT_ARG(p1,any);

	if (!is_stream_or_var(pstr)) {
		if (is_closed_stream(q->pl, pstr))
			return throw_error(q, pstr, q->st.curr_frame, "existence_error", "stream");
		else
			return throw_error(q, pstr, q->st.curr_frame, "domain_error", "stream");
	}

	if (p1->arity > 1)
		return throw_error(q, p1, p1_ctx, "domain_error", "stream_property");

	if (!is_variable(p1) && !is_callable(p1))
		return throw_error(q, p1, p1_ctx, "domain_error", "stream_property");

	if (!is_variable(pstr) && !is_variable(p1))
		return do_stream_property(q);

	if (!q->retry) {
		clear_streams_properties(q);

		for (int i = 0; i < MAX_STREAMS; i++) {
			if (!q->pl->streams[i].fp)
				continue;

			stream *str = &q->pl->streams[i];

			if (!str->socket)
				add_stream_properties(q, i);
		}
	}

	cell *tmp = deep_copy_to_tmp(q, q->st.curr_cell, q->st.curr_frame, false, false);
	unify(q, tmp, q->st.curr_frame, q->st.curr_cell, q->st.curr_frame);
	tmp->val_off = g_sys_stream_property_s;

	if (match_clause(q, tmp, q->st.curr_frame, DO_CLAUSE) != pl_success) {
		clear_streams_properties(q);

		if (is_callable(p1) && !strstr(s_properties, GET_STR(q, p1)))
			return throw_error(q, p1, p1_ctx, "domain_error", "stream_property");

		return pl_failure;
	}

	clause *r = &q->st.curr_clause2->cl;
	GET_FIRST_ARG(pstrx,smallint);
	pstrx->flags |= FLAG_INT_STREAM | FLAG_INT_HEX;
	stash_me(q, r, false);
	return pl_success;
}

#ifndef SANDBOX
#ifndef _WIN32
static USE_RESULT pl_status fn_popen_4(query *q)
{
	GET_FIRST_ARG(p1,atom);
	GET_NEXT_ARG(p2,atom);
	GET_NEXT_ARG(p3,variable);
	GET_NEXT_ARG(p4,list_or_nil);
	int n = new_stream(q->pl);
	char *src = NULL;

	if (n < 0)
		return throw_error(q, p1, p1_ctx, "resource_error", "too_many_streams");

	const char *filename;

	if (is_atom(p1))
		filename = src = DUP_SLICE(q, p1);
	else
		return throw_error(q, p1, p1_ctx, "domain_error", "source_sink");

	if (is_iso_list(p1)) {
		size_t len = scan_is_chars_list(q, p1, p1_ctx, true);

		if (!len)
			return throw_error(q, p1, p1_ctx, "type_error", "atom");

		src = chars_list_to_string(q, p1, p1_ctx, len);
		filename = src;
	}

	stream *str = &q->pl->streams[n];
	str->domain = true;
	may_ptr_error(str->filename = strdup(filename));
	may_ptr_error(str->name = strdup(filename));
	may_ptr_error(str->mode = DUP_SLICE(q, p2));
	str->eof_action = eof_action_eof_code;
	bool binary = false;
	LIST_HANDLER(p4);

	while (is_list(p4)) {
		cell *h = LIST_HEAD(p4);
		cell *c = deref(q, h, p4_ctx);

		if (is_variable(c))
			return throw_error(q, c, q->latest_ctx, "instantiation_error", "args_not_sufficiently_instantiated");

		if (is_structure(c) && (c->arity == 1)) {
			cell *name = c + 1;
			name = deref(q, name, q->latest_ctx);


			if (get_named_stream(q->pl, GET_STR(q, name), LEN_STR(q, name)) >= 0)
				return throw_error(q, c, q->latest_ctx, "permission_error", "open,source_sink");

			if (!CMP_SLICE2(q, c, "alias")) {
				free(str->name);
				str->name = DUP_SLICE(q, name);
			} else if (!CMP_SLICE2(q, c, "type")) {
				if (is_atom(name) && !CMP_SLICE2(q, name, "binary")) {
					str->binary = true;
					binary = true;
				} else if (is_atom(name) && !CMP_SLICE2(q, name, "text"))
					binary = false;
			} else if (!CMP_SLICE2(q, c, "eof_action")) {
				if (is_atom(name) && !CMP_SLICE2(q, name, "error")) {
					str->eof_action = eof_action_error;
				} else if (is_atom(name) && !CMP_SLICE2(q, name, "eof_code")) {
					str->eof_action = eof_action_eof_code;
				} else if (is_atom(name) && !CMP_SLICE2(q, name, "reset")) {
					str->eof_action = eof_action_reset;
				}
			}
		} else
			return throw_error(q, c, q->latest_ctx, "domain_error", "stream_option");

		p4 = LIST_TAIL(p4);
		p4 = deref(q, p4, p4_ctx);
		p4_ctx = q->latest_ctx;

		if (is_variable(p4))
			return throw_error(q, p4, p4_ctx, "instantiation_error", "args_not_sufficiently_instantiated");
	}

	if (!strcmp(str->mode, "read"))
		str->fp = popen(filename, binary?"rb":"r");
	else if (!strcmp(str->mode, "write"))
		str->fp = popen(filename, binary?"wb":"w");
	else
		return throw_error(q, p2, p2_ctx, "domain_error", "io_mode");

	free(src);

	if (!str->fp) {
		if ((errno == EACCES) || (strcmp(str->mode, "read") && (errno == EROFS)))
			return throw_error(q, p1, p1_ctx, "permission_error", "open,source_sink");
		else
			return throw_error(q, p1, p1_ctx, "existence_error", "source_sink");
	}

	cell tmp;
	make_int(&tmp, n);
	tmp.flags |= FLAG_INT_STREAM | FLAG_INT_HEX;
	set_var(q, p3, p3_ctx, &tmp, q->st.curr_frame);
	return pl_success;
}
#endif
#endif

#ifndef SANDBOX
static USE_RESULT pl_status fn_iso_open_4(query *q)
{
	GET_FIRST_ARG(p1,atom_or_structure);
	GET_NEXT_ARG(p2,atom);
	GET_NEXT_ARG(p3,variable);
	GET_NEXT_ARG(p4,list_or_nil);
	int n = new_stream(q->pl);
	char *src = NULL;

	if (n < 0)
		return throw_error(q, p1, p1_ctx, "resource_error", "too_many_streams");

	const char *filename;
	stream *oldstr = NULL;

	if (is_structure(p1) && (p1->arity == 1) && !CMP_SLICE2(q, p1, "stream")) {
		int oldn = get_stream(q, p1+1);

		if (oldn < 0)
			return throw_error(q, p1, p1_ctx, "type_error", "not_a_stream");

		stream *oldstr = &q->pl->streams[oldn];
		filename = oldstr->filename;
	} else if (is_atom(p1))
		filename = src = DUP_SLICE(q, p1);
	else
		return throw_error(q, p1, p1_ctx, "domain_error", "source_sink");

	if (is_iso_list(p1)) {
		size_t len = scan_is_chars_list(q, p1, p1_ctx, true);

		if (!len)
			return throw_error(q, p1, p1_ctx, "type_error", "atom");

		filename = src = chars_list_to_string(q, p1, p1_ctx, len);
	}

	stream *str = &q->pl->streams[n];
	may_ptr_error(str->filename = strdup(filename));
	may_ptr_error(str->name = strdup(filename));
	may_ptr_error(str->mode = DUP_SLICE(q, p2));
	str->eof_action = eof_action_eof_code;
	free(src);

#if USE_MMAP
	cell *mmap_var = NULL;
	pl_idx_t mmap_ctx = 0;
#endif

	bool bom_specified = false, use_bom = false;
	LIST_HANDLER(p4);

	while (is_list(p4)) {
		cell *h = LIST_HEAD(p4);
		cell *c = deref(q, h, p4_ctx);
		pl_idx_t c_ctx = q->latest_ctx;

		if (is_variable(c))
			return throw_error(q, c, q->latest_ctx, "instantiation_error", "args_not_sufficiently_instantiated");

		cell *name = c + 1;
		name = deref(q, name, c_ctx);

		if (!CMP_SLICE2(q, c, "mmap")) {
#if USE_MMAP
			mmap_var = name;
			mmap_var = deref(q, mmap_var, q->latest_ctx);
			mmap_ctx = q->latest_ctx;
#endif
		} else if (!CMP_SLICE2(q, c, "encoding")) {
			if (is_variable(name))
				return throw_error(q, name, q->latest_ctx, "instantiation_error", "stream_option");

			if (!is_atom(name))
				return throw_error(q, c, c_ctx, "domain_error", "stream_option");
		} else if (!CMP_SLICE2(q, c, "alias")) {
			if (is_variable(name))
				return throw_error(q, name, q->latest_ctx, "instantiation_error", "stream_option");

			if (!is_atom(name))
				return throw_error(q, c, c_ctx, "domain_error", "stream_option");

			if (get_named_stream(q->pl, GET_STR(q, name), LEN_STR(q, name)) >= 0)
				return throw_error(q, c, c_ctx, "permission_error", "open,source_sink");

			free(str->name);
			str->name = DUP_SLICE(q, name);
		} else if (!CMP_SLICE2(q, c, "type")) {
			if (is_variable(name))
				return throw_error(q, name, q->latest_ctx, "instantiation_error", "stream_option");

			if (!is_atom(name))
				return throw_error(q, c, c_ctx, "domain_error", "stream_option");

			if (is_atom(name) && !CMP_SLICE2(q, name, "binary")) {
				str->binary = true;
			} else if (is_atom(name) && !CMP_SLICE2(q, name, "text"))
				str->binary = false;
			else
				return throw_error(q, c, c_ctx, "domain_error", "stream_option");
		} else if (!CMP_SLICE2(q, c, "bom")) {
			if (is_variable(name))
				return throw_error(q, name, q->latest_ctx, "instantiation_error", "stream_option");

			if (!is_atom(name))
				return throw_error(q, c, c_ctx, "domain_error", "stream_option");

			bom_specified = true;

			if (is_atom(name) && !CMP_SLICE2(q, name, "true"))
				use_bom = true;
			else if (is_atom(name) && !CMP_SLICE2(q, name, "false"))
				use_bom = false;
		} else if (!CMP_SLICE2(q, c, "reposition")) {
			if (is_variable(name))
				return throw_error(q, name, q->latest_ctx, "instantiation_error", "stream_option");

			if (!is_atom(name))
				return throw_error(q, c, c_ctx, "domain_error", "stream_option");

			if (is_atom(name) && !CMP_SLICE2(q, name, "true"))
				str->repo = true;
			else if (is_atom(name) && !CMP_SLICE2(q, name, "false"))
				str->repo = false;
		} else if (!CMP_SLICE2(q, c, "eof_action")) {
			if (is_variable(name))
				return throw_error(q, name, q->latest_ctx, "instantiation_error", "stream_option");

			if (!is_atom(name))
				return throw_error(q, c, c_ctx, "domain_error", "stream_option");

			if (is_atom(name) && !CMP_SLICE2(q, name, "error")) {
				str->eof_action = eof_action_error;
			} else if (is_atom(name) && !CMP_SLICE2(q, name, "eof_code")) {
				str->eof_action = eof_action_eof_code;
			} else if (is_atom(name) && !CMP_SLICE2(q, name, "reset")) {
				str->eof_action = eof_action_reset;
			}
		} else {
			return throw_error(q, c, c_ctx, "domain_error", "stream_option");
		}

		p4 = LIST_TAIL(p4);
		p4 = deref(q, p4, p4_ctx);
		p4_ctx = q->latest_ctx;

		if (is_variable(p4))
			return throw_error(q, p4, p4_ctx, "instantiation_error", "args_not_sufficiently_instantiated");
	}

	if (oldstr) {
		int fd = fileno(oldstr->fp);

		if (!strcmp(str->mode, "read"))
			str->fp = fdopen(fd, str->binary?"rb":"r");
		else if (!strcmp(str->mode, "write"))
			str->fp = fdopen(fd, str->binary?"wb":"w");
		else if (!strcmp(str->mode, "append"))
			str->fp = fdopen(fd, str->binary?"ab":"a");
		else if (!strcmp(str->mode, "update"))
			str->fp = fdopen(fd, str->binary?"rb+":"r+");
		else
			return throw_error(q, p2, p2_ctx, "domain_error", "io_mode");
	} else {
		if (!strcmp(str->mode, "read"))
			str->fp = fopen(str->filename, str->binary?"rb":"r");
		else if (!strcmp(str->mode, "write"))
			str->fp = fopen(str->filename, str->binary?"wb":"w");
		else if (!strcmp(str->mode, "append"))
			str->fp = fopen(str->filename, str->binary?"ab":"a");
		else if (!strcmp(str->mode, "update"))
			str->fp = fopen(str->filename, str->binary?"rb+":"r+");
		else
			return throw_error(q, p2, p2_ctx, "domain_error", "io_mode");
	}

	if (!str->fp) {
		if ((errno == EACCES) || (strcmp(str->mode, "read") && (errno == EROFS)))
			return throw_error(q, p1, p1_ctx, "permission_error", "open,source_sink");
		//else if ((strcmp(str->mode, "read") && (errno == EISDIR)))
		//	return throw_error(q, p1, p1_ctx, "permission_error", "open,isadir");
		else
			return throw_error(q, p1, p1_ctx, "existence_error", "source_sink");
	}

	size_t offset = 0;

	if (!strcmp(str->mode, "read") && !str->binary && (!bom_specified || use_bom)) {
		int ch = xgetc_utf8(net_getc, str);

		if (feof(str->fp))
			clearerr(str->fp);

		if ((unsigned)ch == 0xFEFF) {
			str->bom = true;
			offset = 3;
		} else
			fseek(str->fp, 0, SEEK_SET);
	} else if (!strcmp(str->mode, "write") && !str->binary && use_bom) {
		int ch = 0xFEFF;
		char tmpbuf[10];
		put_char_utf8(tmpbuf, ch);
		net_write(tmpbuf, strlen(tmpbuf), str);
		str->bom = true;
	}

#if USE_MMAP
	int prot = 0;

	if (!strcmp(str->mode, "read"))
		prot = PROT_READ;
	else
		prot = PROT_WRITE;

	if (mmap_var && is_variable(mmap_var)) {
		int fd = fileno(str->fp);
		struct stat st = {0};
		fstat(fd, &st);
		size_t len = st.st_size;
		void *addr = mmap(0, len, prot, MAP_PRIVATE, fd, offset);
		cell tmp = {0};
		tmp.tag = TAG_CSTR;
		tmp.flags = FLAG_CSTR_BLOB | FLAG_CSTR_STRING | FLAG_STATIC;
		tmp.nbr_cells = 1;
		tmp.arity = 2;
		tmp.val_str = addr;
		tmp.str_len = len;
		set_var(q, mmap_var, mmap_ctx, &tmp, q->st.curr_frame);
	}
#endif

	cell tmp ;
	make_int(&tmp, n);
	tmp.flags |= FLAG_INT_STREAM | FLAG_INT_HEX;
	set_var(q, p3, p3_ctx, &tmp, q->st.curr_frame);
	return pl_success;
}
#endif

#ifndef SANDBOX
static USE_RESULT pl_status fn_iso_close_1(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];

	if ((str->fp == stdin)
		|| (str->fp == stdout)
		|| (str->fp == stderr))
		return pl_success;

	if (q->pl->current_input == n)
		q->pl->current_input = 0;

	if (q->pl->current_output == n)
		q->pl->current_output = 1;

	if (q->pl->current_error == n)
		q->pl->current_error = 2;

	if (str->p)
		destroy_parser(str->p);

	if (!str->socket)
		del_stream_properties(q, n);

	net_close(str);
	free(str->mode);
	free(str->filename);
	free(str->name);
	free(str->data);
	return pl_success;
}
#endif

#ifndef SANDBOX
static USE_RESULT pl_status fn_iso_close_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	GET_NEXT_ARG(p1,list_or_nil);
	LIST_HANDLER(p1);

	while (is_list(p1)) {
		cell *h = LIST_HEAD(p1);
		h = deref(q, h, p1_ctx);

		if (!is_structure(h)
			|| CMP_SLICE2(q, h, "force")
			|| CMP_SLICE2(q, h+1, "true"))
			return throw_error(q, h, q->latest_ctx, "domain_error", "close_option");

		p1 = LIST_TAIL(p1);
		p1 = deref(q, p1, p1_ctx);
		p1_ctx = q->latest_ctx;
	}

	if (is_variable(p1))
		return throw_error(q, p1, p1_ctx, "instantiation_error", "close_option");

	if (!is_nil(p1))
		return throw_error(q, p1, p1_ctx, "type_error", "list");

	return fn_iso_close_1(q);
}
#endif

static USE_RESULT pl_status fn_iso_at_end_of_stream_0(query *q)
{
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = get_char_utf8((const char**)&str->p->srcptr);
			str->ungetch = ch;
		}
	}

	if (!str->socket) {
		int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);
		str->ungetch = ch;
	}

	if (!feof(str->fp) && !ferror(str->fp))
		return pl_failure;

	if (str->eof_action == eof_action_reset)
		clearerr(str->fp);

	return pl_success;
}

static USE_RESULT pl_status fn_iso_at_end_of_stream_1(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];

	if (strcmp(str->mode, "read") && strcmp(str->mode, "update"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "input,stream");

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = get_char_utf8((const char**)&str->p->srcptr);
			str->ungetch = ch;
		}
	}

	if (!str->socket) {
		int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);
		str->ungetch = ch;
	}

	if (!feof(str->fp) && !ferror(str->fp))
		return pl_failure;

	if (str->eof_action == eof_action_reset)
		clearerr(str->fp);

	return pl_success;
}

static USE_RESULT pl_status fn_iso_flush_output_0(query *q)
{
	int n = q->pl->current_output;
	stream *str = &q->pl->streams[n];
	fflush(str->fp);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_flush_output_1(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];

	if (!strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "output,stream");

	fflush(str->fp);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_nl_0(query *q)
{
	int n = q->pl->current_output;
	stream *str = &q->pl->streams[n];
	fputc('\n', str->fp);
	//fflush(str->fp);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_nl_1(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];

	if (!strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "output,stream");

	fputc('\n', str->fp);
	//fflush(str->fp);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_read_1(query *q)
{
	GET_FIRST_ARG(p1,any);
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	cell tmp;
	make_literal(&tmp, g_nil_s);
	return do_read_term(q, str, p1, p1_ctx, &tmp, q->st.curr_frame, NULL);
}

static USE_RESULT pl_status fn_iso_read_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,any);

	if (strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "input,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	cell tmp;
	make_literal(&tmp, g_nil_s);
	return do_read_term(q, str, p1, p1_ctx, &tmp, q->st.curr_frame, NULL);
}

static USE_RESULT pl_status fn_iso_read_term_2(query *q)
{
	GET_FIRST_ARG(p1,any);
	GET_NEXT_ARG(p2,list_or_nil);
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	return do_read_term(q, str, p1, p1_ctx, p2, p2_ctx, NULL);
}

static USE_RESULT pl_status fn_iso_read_term_3(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,any);
	GET_NEXT_ARG(p2,list_or_nil);

	if (strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "input,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	return do_read_term(q, str, p1, p1_ctx, p2, p2_ctx, NULL);
}

static USE_RESULT pl_status fn_iso_write_1(query *q)
{
	GET_FIRST_ARG(p1,any);
	int n = q->pl->current_output;
	stream *str = &q->pl->streams[n];

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	print_term_to_stream(q, str, p1, p1_ctx, 1);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_write_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,any);

	if (!strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "output,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	print_term_to_stream(q, str, p1, p1_ctx, 1);
	q->numbervars = false;
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_writeq_1(query *q)
{
	GET_FIRST_ARG(p1,any);
	int n = q->pl->current_output;
	stream *str = &q->pl->streams[n];

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	q->quoted = 1;
	q->numbervars = true;
	print_term_to_stream(q, str, p1, p1_ctx, 1);
	q->numbervars = false;
	q->quoted = 0;
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_writeq_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,any);

	if (!strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "output,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	q->quoted = 1;
	q->numbervars = true;
	print_term_to_stream(q, str, p1, p1_ctx, 1);
	q->numbervars = false;
	q->quoted = 0;
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_write_canonical_1(query *q)
{
	GET_FIRST_ARG(p1,any);
	int n = q->pl->current_output;
	stream *str = &q->pl->streams[n];

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	print_canonical(q, str->fp, p1, p1_ctx, 1);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_write_canonical_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,any);

	if (!strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "output,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	print_canonical(q, str->fp, p1, p1_ctx, 1);
	return !ferror(str->fp);
}

bool parse_write_params(query *q, cell *c, pl_idx_t c_ctx, cell **vnames, pl_idx_t *vnames_ctx)
{
	if (is_variable(c)) {
		DISCARD_RESULT throw_error(q, c, c_ctx, "instantiation_error", "write_option");
		return false;
	}

	if (!is_literal(c) || !is_structure(c)) {
		DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
		return false;
	}

	cell *c1 = deref(q, c+1, c_ctx);
	pl_idx_t c1_ctx = q->latest_ctx;

	if (!CMP_SLICE2(q, c, "max_depth")) {
		if (is_variable(c1)) {
			DISCARD_RESULT throw_error(q, c1, c_ctx, "instantiation_error", "write_option");
			return false;
		}

		if (is_integer(c1) && (get_int(&c[1]) >= 1))
			q->max_depth = get_int(&c[1]);
	} else if (!CMP_SLICE2(q, c, "fullstop")) {
		if (is_variable(c1)) {
			DISCARD_RESULT throw_error(q, c1, c_ctx, "instantiation_error", "write_option");
			return false;
		}

		if (!is_literal(c1) || (CMP_SLICE2(q, c1, "true") && CMP_SLICE2(q, c1, "false"))) {
			DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
			return false;
		}

		q->fullstop = !CMP_SLICE2(q, c1, "true");
	} else if (!CMP_SLICE2(q, c, "nl")) {
		if (is_variable(c1)) {
			DISCARD_RESULT throw_error(q, c1, c_ctx, "instantiation_error", "write_option");
			return false;
		}

		if (!is_literal(c1) || (CMP_SLICE2(q, c1, "true") && CMP_SLICE2(q, c1, "false"))) {
			DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
			return false;
		}

		q->nl = !CMP_SLICE2(q, c1, "true");
	} else if (!CMP_SLICE2(q, c, "quoted")) {
		if (is_variable(c1)) {
			DISCARD_RESULT throw_error(q, c1, c_ctx, "instantiation_error", "write_option");
			return false;
		}

		if (!is_literal(c1) || (CMP_SLICE2(q, c1, "true") && CMP_SLICE2(q, c1, "false"))) {
			DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
			return false;
		}

		q->quoted = !CMP_SLICE2(q, c1, "true");
	} else if (!CMP_SLICE2(q, c, "varnames")) {
		if (is_variable(c1)) {
			DISCARD_RESULT throw_error(q, c1, c_ctx, "instantiation_error", "write_option");
			return false;
		}

		if (!is_literal(c1) || (CMP_SLICE2(q, c1, "true") && CMP_SLICE2(q, c1, "false"))) {
			DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
			return false;
		}

		q->varnames = !CMP_SLICE2(q, c1, "true");
	} else if (!CMP_SLICE2(q, c, "ignore_ops")) {
		if (is_variable(c1)) {
			DISCARD_RESULT throw_error(q, c1, c_ctx, "instantiation_error", "write_option");
			return false;
		}

		if (!is_literal(c1) || (CMP_SLICE2(q, c1, "true") && CMP_SLICE2(q, c1, "false"))) {
			DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
			return false;
		}

		q->ignore_ops = !CMP_SLICE2(q, c1, "true");
	} else if (!CMP_SLICE2(q, c, "numbervars")) {
		if (is_variable(c1)) {
			DISCARD_RESULT throw_error(q, c1, c_ctx, "instantiation_error", "write_option");
			return false;
		}

		if (!is_literal(c1) || (CMP_SLICE2(q, c1, "true") && CMP_SLICE2(q, c1, "false"))) {
			DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
			return false;
		}

		q->numbervars = !CMP_SLICE2(q, c1, "true");
	} else if (!CMP_SLICE2(q, c, "variable_names")) {
		if (is_variable(c1)) {
			DISCARD_RESULT throw_error(q, c1, c_ctx, "instantiation_error", "write_option");
			return false;
		}

		if (!is_list_or_nil(c1)) {
			DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
			return false;
		}

		cell *c1_orig = c1;
		pl_idx_t c1_orig_ctx = c1_ctx;
		LIST_HANDLER(c1);

		while (is_list(c1)) {
			cell *h = LIST_HEAD(c1);
			h = deref(q, h, c1_ctx);
			pl_idx_t h_ctx = q->latest_ctx;

			if (is_variable(h)) {
				DISCARD_RESULT throw_error(q, h, h_ctx, "instantiation_error", "write_option");
				return false;
			}

			if (!is_structure(h)) {
				DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
				return false;
			}

			if (CMP_SLICE2(q, h, "=")) {
				DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
				return false;
			}

			if (is_literal(h)) {
				cell *h1 = deref(q, h+1, h_ctx);

				if (is_variable(h1)) {
					DISCARD_RESULT throw_error(q, c, c_ctx, "instantiation_error", "write_option");
					return false;
				} else if (!is_atom(h1)) {
					DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
					return false;
				}

#if 0
				cell *h2 = deref(q, h+2, h_ctx);

				if (!is_variable(h2)) {
					DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
					return false;
				}
#endif
			}

			c1 = LIST_TAIL(c1);
			c1 = deref(q, c1, c1_ctx);
			c1_ctx = q->latest_ctx;
		}

		if (is_variable(c1)) {
			DISCARD_RESULT throw_error(q, c1_orig, c_ctx, "instantiation_error", "write_option");
			return false;
		}

		if (!is_nil(c1)) {
			DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
			return false;
		}

		if (vnames) *vnames = c1_orig;
		if (vnames_ctx) *vnames_ctx = c1_orig_ctx;
	} else {
		DISCARD_RESULT throw_error(q, c, c_ctx, "domain_error", "write_option");
		return false;
	}

	return true;
}

static USE_RESULT pl_status fn_iso_write_term_2(query *q)
{
	GET_FIRST_ARG(p1,any);
	GET_NEXT_ARG(p2,list_or_nil);
	int n = q->pl->current_output;
	stream *str = &q->pl->streams[n];

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	q->flag = q->st.m->flag;
	cell *p2_orig = p2, *vnames = NULL;
	pl_idx_t p2_orig_ctx = p2_ctx, vnames_ctx = 0;
	LIST_HANDLER(p2);

	while (is_list(p2)) {
		cell *h = LIST_HEAD(p2);
		h = deref(q, h, p2_ctx);
		pl_idx_t h_ctx = q->latest_ctx;

		if (!parse_write_params(q, h, h_ctx, &vnames, &vnames_ctx)) {
			clear_write_options(q);
			return pl_success;
		}

		p2 = LIST_TAIL(p2);
		p2 = deref(q, p2, p2_ctx);
		p2_ctx = q->latest_ctx;
	}

	if (is_variable(p2)) {
		clear_write_options(q);
		return throw_error(q, p2_orig, p2_orig_ctx, "instantiation_error", "write_option");
	}

	if (!is_nil(p2)) {
		clear_write_options(q);
		return throw_error(q, p2_orig, p2_orig_ctx, "type_error", "list");
	}

	q->variable_names = vnames;
	q->variable_names_ctx = vnames_ctx;

	if (q->ignore_ops)
		print_canonical_to_stream(q, str, p1, p1_ctx, 1);
	else
		print_term_to_stream(q, str, p1, p1_ctx, 1);

	if (q->fullstop)
		net_write(".", 1, str);

	if (q->nl) {
		net_write("\n", 1, str);
		//fflush(str->fp);
	}

	clear_write_options(q);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_write_term_3(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,any);
	GET_NEXT_ARG(p2,list_or_nil);

	if (!strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "output,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	q->flag = q->st.m->flag;
	cell *p2_orig = p2, *vnames = NULL;
	pl_idx_t p2_orig_ctx = p2_ctx, vnames_ctx;
	LIST_HANDLER(p2);

	while (is_list(p2)) {
		cell *h = LIST_HEAD(p2);
		h = deref(q, h, p2_ctx);
		pl_idx_t h_ctx = q->latest_ctx;

		if (!parse_write_params(q, h, h_ctx, &vnames, &vnames_ctx)) {
			clear_write_options(q);
			return pl_success;
		}

		p2 = LIST_TAIL(p2);
		p2 = deref(q, p2, p2_ctx);
		p2_ctx = q->latest_ctx;
	}

	if (is_variable(p2)) {
		clear_write_options(q);
		return throw_error(q, p2_orig, p2_orig_ctx, "instantiation_error", "write_option");
	}

	if (!is_nil(p2)) {
		clear_write_options(q);
		return throw_error(q, p2_orig, p2_orig_ctx, "type_error", "list");
	}

	q->latest_ctx = p1_ctx;
	q->variable_names = vnames;
	q->variable_names_ctx = vnames_ctx;

	if (q->ignore_ops)
		print_canonical_to_stream(q, str, p1, p1_ctx, 1);
	else
		print_term_to_stream(q, str, p1, p1_ctx, 1);

	if (q->fullstop)
		net_write(".", 1, str);

	if (q->nl) {
		net_write("\n", 1, str);
		//fflush(str->fp);
	}

	clear_write_options(q);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_put_char_1(query *q)
{
	GET_FIRST_ARG(p1,character);
	int n = q->pl->current_output;
	stream *str = &q->pl->streams[n];
	size_t len = len_char_utf8(GET_STR(q, p1));

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	if (len != LEN_STR(q, p1))
		return throw_error(q, p1, p1_ctx, "type_error", "character");

	const char *src = GET_STR(q, p1);
	int ch = get_char_utf8(&src);
	char tmpbuf[80];
	put_char_utf8(tmpbuf, ch);
	net_write(tmpbuf, strlen(tmpbuf), str);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_put_char_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,character);
	size_t len = len_char_utf8(GET_STR(q, p1));

	if (!strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "output,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	if (len != LEN_STR(q, p1))
		return throw_error(q, p1, p1_ctx, "type_error", "character");

	const char *src = GET_STR(q, p1);
	int ch = get_char_utf8(&src);
	char tmpbuf[80];
	put_char_utf8(tmpbuf, ch);
	net_write(tmpbuf, strlen(tmpbuf), str);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_put_code_1(query *q)
{
	GET_FIRST_ARG(p1,integer);
	int n = q->pl->current_output;
	stream *str = &q->pl->streams[n];

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	if (!is_integer(p1))
		return throw_error(q, p1, p1_ctx, "type_error", "integer");

	if (is_integer(p1) && is_le(p1,-1))
		return throw_error(q, p1, p1_ctx, "representation_error", "character_code");

	int ch = (int)get_int(p1);
	char tmpbuf[80];
	put_char_utf8(tmpbuf, ch);
	net_write(tmpbuf, strlen(tmpbuf), str);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_put_code_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,integer);

	if (!strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "output,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,binary_stream");
	}

	if (!is_integer(p1))
		return throw_error(q, p1, p1_ctx, "type_error", "integer");

	if (is_integer(p1) && is_le(p1,-1))
		return throw_error(q, p1, p1_ctx, "representation_error", "character_code");

	int ch = (int)get_int(p1);
	char tmpbuf[80];
	put_char_utf8(tmpbuf, ch);
	net_write(tmpbuf, strlen(tmpbuf), str);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_put_byte_1(query *q)
{
	GET_FIRST_ARG(p1,byte);
	int n = q->pl->current_output;
	stream *str = &q->pl->streams[n];

	if (!str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "output,text_stream");
	}

	if (!is_integer(p1))
		return throw_error(q, p1, p1_ctx, "type_error", "integer");

	if (is_integer(p1) && is_le(p1,-1))
		return throw_error(q, p1, p1_ctx, "representation_error", "character_code");

	int ch = (int)get_int(p1);
	char tmpbuf[80];
	snprintf(tmpbuf, sizeof(tmpbuf), "%c", ch);
	net_write(tmpbuf, 1, str);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_put_byte_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,byte);

	if (!strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "output,stream");

	if (!str->binary)
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "output,text_stream");

	if (!is_integer(p1))
		return throw_error(q, p1, p1_ctx, "type_error", "integer");

	if (is_integer(p1) && is_le(p1,-1))
		return throw_error(q, p1, p1_ctx, "representation_error", "character_code");

	int ch = (int)get_int(p1);
	char tmpbuf[80];
	snprintf(tmpbuf, sizeof(tmpbuf), "%c", ch);
	net_write(tmpbuf, 1, str);
	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_iso_get_char_1(query *q)
{
	GET_FIRST_ARG(p1,in_character_or_var);
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = get_char_utf8((const char**)&str->p->srcptr);
			str->ungetch = ch;
		}
	}

	if (isatty(fileno(str->fp)) && !str->did_getc && !str->ungetch) {
		fprintf(str->fp, "%s", PROMPT);
		fflush(str->fp);
	}

	int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}

	str->did_getc = true;

	if (FEOF(str)) {
		str->did_getc = false;
		str->at_end_of_file = str->eof_action != eof_action_reset;

		if (str->eof_action == eof_action_reset)
			clearerr(str->fp);

		cell tmp;
		make_literal(&tmp, g_eof_s);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = 0;

	if (ch == '\n') {
		str->did_getc = false;

		if (str->p)
			str->p->line_nbr++;
	}

	char tmpbuf[80];
	n = put_char_utf8(tmpbuf, ch);
	cell tmp;
	make_smalln(&tmp, tmpbuf, n);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

static USE_RESULT pl_status fn_iso_get_char_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,in_character_or_var);

	if (strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "input,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = get_char_utf8((const char**)&str->p->srcptr);
			str->ungetch = ch;
		}
	}

	if (isatty(fileno(str->fp)) && !str->did_getc && !str->ungetch) {
		fprintf(str->fp, "%s", PROMPT);
		fflush(str->fp);
	}

	int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}

	str->did_getc = true;

	if (FEOF(str)) {
		str->did_getc = false;
		str->at_end_of_file = str->eof_action != eof_action_reset;

		if (str->eof_action == eof_action_reset)
			clearerr(str->fp);

		cell tmp;
		make_literal(&tmp, g_eof_s);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = 0;

	if (ch == '\n') {
		str->did_getc = false;

		if (str->p)
			str->p->line_nbr++;
	}

	char tmpbuf[80];
	n = put_char_utf8(tmpbuf, ch);
	cell tmp;
	make_smalln(&tmp, tmpbuf, n);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

static USE_RESULT pl_status fn_iso_get_code_1(query *q)
{
	GET_FIRST_ARG(p1,integer_or_var);
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	if (is_integer(p1) && (get_int(p1) < -1))
		return throw_error(q, p1, p1_ctx, "representation_error", "in_character_code");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = get_char_utf8((const char**)&str->p->srcptr);
			str->ungetch = ch;
		}
	}

	if (isatty(fileno(str->fp)) && !str->did_getc && !str->ungetch) {
		fprintf(str->fp, "%s", PROMPT);
		fflush(str->fp);
	}

	int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}

	str->did_getc = true;

	if (FEOF(str)) {
		str->did_getc = false;
		str->at_end_of_file = str->eof_action != eof_action_reset;

		if (str->eof_action == eof_action_reset)
			clearerr(str->fp);

		cell tmp;
		make_int(&tmp, -1);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = 0;

	if (ch == '\n') {
		str->did_getc = false;

		if (str->p)
			str->p->line_nbr++;
	} else if (ch == EOF)
		str->did_getc = false;

	cell tmp;
	make_int(&tmp, ch);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

static USE_RESULT pl_status fn_iso_get_code_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,integer_or_var);

	if (is_integer(p1) && (get_int(p1) < -1))
		return throw_error(q, p1, p1_ctx, "representation_error", "in_character_code");

	if (strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "input,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = get_char_utf8((const char**)&str->p->srcptr);
			str->ungetch = ch;
		}
	}

	if (isatty(fileno(str->fp)) && !str->did_getc && !str->ungetch) {
		fprintf(str->fp, "%s", PROMPT);
		fflush(str->fp);
	}

	int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}

	str->did_getc = true;

	if (FEOF(str)) {
		str->did_getc = false;
		str->at_end_of_file = str->eof_action != eof_action_reset;

		if (str->eof_action == eof_action_reset)
			clearerr(str->fp);

		cell tmp;
		make_int(&tmp, -1);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = 0;

	if (ch == '\n') {
		str->did_getc = false;

		if (str->p)
			str->p->line_nbr++;
	}

	cell tmp;
	make_int(&tmp, ch);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

static USE_RESULT pl_status fn_iso_get_byte_1(query *q)
{
	GET_FIRST_ARG(p1,in_byte_or_var);
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	if (!str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,text_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = *str->p->srcptr++;
			str->ungetch = ch;
		}
	}

	if (isatty(fileno(str->fp)) && !str->did_getc && !str->ungetch) {
		fprintf(str->fp, "%s", PROMPT);
		fflush(str->fp);
	}

	int ch = str->ungetch ? str->ungetch : net_getc(str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}

	str->did_getc = true;

	if (FEOF(str)) {
		str->did_getc = false;
		str->at_end_of_file = str->eof_action != eof_action_reset;

		if (str->eof_action == eof_action_reset)
			clearerr(str->fp);

		cell tmp;
		make_int(&tmp, -1);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = 0;

	cell tmp;
	make_int(&tmp, ch);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

static USE_RESULT pl_status fn_iso_get_byte_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,in_byte_or_var);

	if (strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "input,stream");

	if (!str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,text_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = *str->p->srcptr;
			str->ungetch = ch;
		}
	}

	if (isatty(fileno(str->fp)) && !str->did_getc && !str->ungetch) {
		fprintf(str->fp, "%s", PROMPT);
		fflush(str->fp);
	}

	int ch = str->ungetch ? str->ungetch : net_getc(str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}

	str->did_getc = true;

	if (FEOF(str)) {
		str->did_getc = false;
		str->at_end_of_file = str->eof_action != eof_action_reset;

		if (str->eof_action == eof_action_reset)
			clearerr(str->fp);

		cell tmp;
		make_int(&tmp, -1);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = 0;
	cell tmp;
	make_int(&tmp, ch);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

static USE_RESULT pl_status fn_iso_peek_char_1(query *q)
{
	GET_FIRST_ARG(p1,in_character_or_var);
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = peek_char_utf8((const char*)str->p->srcptr);
			str->ungetch = ch;
		}
	}

	int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}


	if (FEOF(str)) {
		str->did_getc = false;
		clearerr(str->fp);
		cell tmp;
		make_literal(&tmp, g_eof_s);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = ch;
	char tmpbuf[80];
	n = put_char_utf8(tmpbuf, ch);
	cell tmp;
	make_smalln(&tmp, tmpbuf, n);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

static USE_RESULT pl_status fn_iso_peek_char_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,in_character_or_var);

	if (strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "input,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = peek_char_utf8((const char*)str->p->srcptr);
			str->ungetch = ch;
		}
	}

	int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}

	if (FEOF(str)) {
		str->did_getc = false;
		clearerr(str->fp);
		cell tmp;
		make_literal(&tmp, g_eof_s);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = ch;
	char tmpbuf[80];
	n = put_char_utf8(tmpbuf, ch);
	cell tmp;
	make_smalln(&tmp, tmpbuf, n);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

static USE_RESULT pl_status fn_iso_peek_code_1(query *q)
{
	GET_FIRST_ARG(p1,integer_or_var);
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	if (is_integer(p1) && (get_int(p1) < -1))
		return throw_error(q, p1, p1_ctx, "representation_error", "in_character_code");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = peek_char_utf8((const char*)str->p->srcptr);
			str->ungetch = ch;
		}
	}

	int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}

	if (FEOF(str)) {
		str->did_getc = false;
		clearerr(str->fp);
		cell tmp;
		make_int(&tmp, -1);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = ch;
	cell tmp;
	make_int(&tmp, ch);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

static USE_RESULT pl_status fn_iso_peek_code_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,integer_or_var);

	if (is_integer(p1) && (get_int(p1) < -1))
		return throw_error(q, p1, p1_ctx, "representation_error", "in_character_code");

	if (strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "input,stream");

	if (str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,binary_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = peek_char_utf8((const char*)str->p->srcptr);
			str->ungetch = ch;
		}
	}

	int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}

	if (FEOF(str)) {
		str->did_getc = false;
		clearerr(str->fp);
		cell tmp;
		make_int(&tmp, -1);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = ch;
	cell tmp;
	make_int(&tmp, ch);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

static USE_RESULT pl_status fn_iso_peek_byte_1(query *q)
{
	GET_FIRST_ARG(p1,in_byte_or_var);
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	if (!str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,text_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = peek_char_utf8((const char*)str->p->srcptr);
			str->ungetch = ch;
		}
	}

	int ch = str->ungetch ? str->ungetch : net_getc(str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}

	if (FEOF(str)) {
		clearerr(str->fp);
		cell tmp;
		make_int(&tmp, -1);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = ch;
	cell tmp;
	make_int(&tmp, ch);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

static USE_RESULT pl_status fn_iso_peek_byte_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,in_byte_or_var);

	if (strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "input,stream");

	if (!str->binary) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,text_stream");
	}

	if (str->at_end_of_file && (str->eof_action == eof_action_error)) {
		cell tmp;
		make_int(&tmp, n);
		tmp.flags |= FLAG_INT_HEX;
		return throw_error(q, &tmp, q->st.curr_frame, "permission_error", "input,past_end_of_stream");
	}

	if (!str->ungetch && str->p) {
		if (str->p->srcptr && *str->p->srcptr) {
			int ch = peek_char_utf8((const char*)str->p->srcptr);
			str->ungetch = ch;
		}
	}

	int ch = str->ungetch ? str->ungetch : net_getc(str);

	if (q->is_task && !feof(str->fp) && ferror(str->fp)) {
		clearerr(str->fp);
		return do_yield_0(q, 1);
	}

	if (FEOF(str)) {
		clearerr(str->fp);
		cell tmp;
		make_int(&tmp, -1);
		return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	}

	str->ungetch = ch;
	cell tmp;
	make_int(&tmp, ch);
	return unify(q, p1, p1_ctx, &tmp, q->st.curr_frame);
}

int new_stream(prolog *pl)
{
	for (int i = 0; i < MAX_STREAMS; i++) {
		if (!pl->streams[i].fp && !pl->streams[i].ignore) {
			memset(&pl->streams[i], 0, sizeof(stream));
			return i;
		}
	}

	return -1;
}

int get_stream(query *q, cell *p1)
{
	if (is_atom(p1)) {
		int n = get_named_stream(q->pl, GET_STR(q, p1), LEN_STR(q, p1));

		if (n < 0)
			return -1;

		return n;
	}

	if (!(p1->flags&FLAG_INT_STREAM))
		return -1;

	if (!q->pl->streams[get_int(p1)].fp)
		return -1;

	return get_smallint(p1);
}

static USE_RESULT pl_status fn_iso_current_input_1(query *q)
{
	GET_FIRST_ARG(pstr,any);

	if (is_variable(pstr)) {
		cell tmp;
		make_int(&tmp, q->pl->current_input);
		tmp.flags |= FLAG_INT_STREAM | FLAG_INT_HEX;
		set_var(q, pstr, pstr_ctx, &tmp, q->st.curr_frame);
		return pl_success;
	}

	if (!is_stream(pstr))
		return throw_error(q, pstr, q->st.curr_frame, "domain_error", "stream");

	int n = get_stream(q, pstr);
	return n == q->pl->current_input ? pl_success : pl_failure;
}

static USE_RESULT pl_status fn_iso_current_output_1(query *q)
{
	GET_FIRST_ARG(pstr,any);

	if (is_variable(pstr)) {
		cell tmp;
		make_int(&tmp, q->pl->current_output);
		tmp.flags |= FLAG_INT_STREAM | FLAG_INT_HEX;
		set_var(q, pstr, pstr_ctx, &tmp, q->st.curr_frame);
		return pl_success;
	}

	if (!is_stream(pstr))
		return throw_error(q, pstr, q->st.curr_frame, "domain_error", "stream");

	int n = get_stream(q, pstr);
	return n == q->pl->current_output ? pl_success : pl_failure;
}

static USE_RESULT pl_status fn_iso_set_input_1(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];

	if (strcmp(str->mode, "read") && strcmp(str->mode, "update"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "input,stream");

	q->pl->current_input = n;
	return pl_success;
}

static USE_RESULT pl_status fn_iso_set_output_1(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];

	if (!strcmp(str->mode, "read"))
		return throw_error(q, pstr, q->st.curr_frame, "permission_error", "output,stream");

	q->pl->current_output = n;
	return pl_success;
}

static USE_RESULT pl_status fn_iso_set_stream_position_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,any);

	if (!str->repo)
		return throw_error(q, p1, p1_ctx, "permission_error", "reposition,stream");

	if (!is_smallint(p1))
		return throw_error(q, p1, p1_ctx, "domain_error", "stream_position");

	off_t pos = get_smallint(p1);

	if (fseeko(str->fp, pos, SEEK_SET))
		return throw_error(q, p1, p1_ctx, "domain_error", "position");

	return pl_success;
}

static USE_RESULT pl_status fn_read_term_from_chars_3(query *q)
{
	GET_FIRST_ARG(p_chars,any);
	GET_NEXT_ARG(p_term,any);
	GET_NEXT_ARG(p_opts,list_or_nil);
	int n = 3;
	stream *str = &q->pl->streams[n];
	char *src = NULL;
	size_t len;
	bool has_var, is_partial;

	if (is_atom(p_chars) && !is_string(p_chars)) {
		if (!strcmp(GET_STR(q, p_chars), "[]")) {
			cell tmp;
			make_literal(&tmp, g_eof_s);
			return unify(q, p_term, p_term_ctx, &tmp, q->st.curr_frame);
		} else
			return throw_error(q, p_chars, p_chars_ctx, "type_error", "character");
	} else if (is_string(p_chars)) {
		len = LEN_STR(q, p_chars);
		src = malloc(len+1+1);		// +1 is to allow adding a '.'
		may_ptr_error(src);
		memcpy(src, GET_STR(q, p_chars), len);
		src[len] = '\0';
	} else if (!check_list(q, p_chars, p_chars_ctx, &is_partial, NULL)) {
		return throw_error(q, p_chars, p_chars_ctx, "type_error", "list");
	} else if ((len = scan_is_chars_list2(q, p_chars, p_chars_ctx, false, &has_var, &is_partial)) > 0) {
		if (!len)
			return throw_error(q, p_chars, p_chars_ctx, "type_error", "character");

		src = chars_list_to_string(q, p_chars, p_chars_ctx, len);
	} else {
		if (has_var)
			return throw_error(q, p_chars, p_chars_ctx, "instantiation_error", "variable");

		return throw_error(q, p_chars, p_chars_ctx, "type_error", "character");
	}

	if (!str->p) {
		str->p = create_parser(q->st.m);
		str->p->flag = q->st.m->flag;
		str->p->fp = str->fp;
	} else
		reset(str->p);

	char *save_src = src;
	str->p->srcptr = src;
	src = eat_space(str->p);

	if (!src || !*src) {
		free(save_src);
		cell tmp;
		make_literal(&tmp, g_eof_s);
		return unify(q, p_term, p_term_ctx, &tmp, q->st.curr_frame);
	}

	const char *end_ptr = src + strlen(src) - 1;

	while (isspace(*end_ptr) && (end_ptr != src))
		end_ptr--;

	if (src[strlen(src)-1] != '.')
		strcat(src, ".");

	q->p->no_fp = true;
	pl_status ok = do_read_term(q, str, p_term, p_term_ctx, p_opts, p_opts_ctx, src);
	q->p->no_fp = false;
	free(save_src);
	destroy_parser(str->p);
	str->p = NULL;

	if (ok != pl_success)
		return pl_failure;

	return ok;
}

static USE_RESULT pl_status fn_read_term_from_atom_3(query *q)
{
	GET_FIRST_ARG(p_chars,any);
	GET_NEXT_ARG(p_term,any);
	GET_NEXT_ARG(p_opts,list_or_nil);
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	char *src;
	size_t len;

	if (is_cstring(p_chars)) {
		len = LEN_STR(q, p_chars);
		src = malloc(len+1+1);	// final +1 is for look-ahead
		may_ptr_error(src);
		memcpy(src, GET_STR(q, p_chars), len);
		src[len] = '\0';
	} else if ((len = scan_is_chars_list(q, p_chars, p_chars_ctx, false)) > 0) {
		if (!len)
			return throw_error(q, p_chars, p_chars_ctx, "type_error", "atom");

		src = chars_list_to_string(q, p_chars, p_chars_ctx, len);
	} else
		return throw_error(q, p_chars, p_chars_ctx, "type_error", "atom");

	const char *end_ptr = src + strlen(src) - 1;

	while (isspace(*end_ptr) && (end_ptr != src))
		end_ptr--;

	if (src[strlen(src)-1] != '.')
		strcat(src, ".");

	q->p->no_fp = true;
	pl_status ok = do_read_term(q, str, p_term, p_term_ctx, p_opts, p_opts_ctx, src);
	q->p->no_fp = false;
	free(src);
	return ok;
}

static USE_RESULT pl_status fn_write_term_to_atom_3(query *q)
{
	GET_FIRST_ARG(p_chars,atom_or_var);
	GET_NEXT_ARG(p_term,any);
	GET_NEXT_ARG(p2,list_or_nil);
	cell *vnames = NULL;
	pl_idx_t vnames_ctx = 0;
	q->flag = q->st.m->flag;
	LIST_HANDLER(p2);

	while (is_list(p2) && !g_tpl_interrupt) {
		cell *h = LIST_HEAD(p2);
		h = deref(q, h, p2_ctx);
		pl_idx_t h_ctx = q->latest_ctx;
		parse_write_params(q, h, h_ctx, &vnames, &vnames_ctx);
		p2 = LIST_TAIL(p2);
		p2 = deref(q, p2, p2_ctx);
		p2_ctx = q->latest_ctx;
	}

	q->variable_names = vnames;
	q->variable_names_ctx = vnames_ctx;
	char *dst = print_term_to_strbuf(q, p_term, p_term_ctx, 1);
	clear_write_options(q);
	cell tmp;
	may_error(make_cstring(&tmp, dst), free(dst));
	free(dst);
	pl_status ok = unify(q, p_chars, p_chars_ctx, &tmp, q->st.curr_frame);
	unshare_cell(&tmp);
	return ok;
}

static USE_RESULT pl_status fn_write_canonical_to_atom_3(query *q)
{
	GET_FIRST_ARG(p_chars,atom_or_var);
	GET_NEXT_ARG(p_term,any);
	GET_NEXT_ARG(p2,list_or_nil);
	cell *vnames = NULL;
	pl_idx_t vnames_ctx = 0;
	q->flag = q->st.m->flag;
	LIST_HANDLER(p2);

	while (is_list(p2) && !g_tpl_interrupt) {
		cell *h = LIST_HEAD(p2);
		h = deref(q, h, p2_ctx);
		pl_idx_t h_ctx = q->latest_ctx;
		parse_write_params(q, h, h_ctx, &vnames, &vnames_ctx);
		p2 = LIST_TAIL(p2);
		p2 = deref(q, p2, p2_ctx);
		p2_ctx = q->latest_ctx;
	}

	char *dst = print_canonical_to_strbuf(q, p_term, p_term_ctx, 1);
	clear_write_options(q);
	cell tmp;
	may_error(make_cstring(&tmp, dst), free(dst));
	free(dst);
	pl_status ok = unify(q, p_chars, p_chars_ctx, &tmp, q->st.curr_frame);
	unshare_cell(&tmp);
	return ok;
}

static USE_RESULT pl_status fn_write_term_to_chars_3(query *q)
{
	GET_FIRST_ARG(p_chars,atom_or_var);
	GET_NEXT_ARG(p_term,any);
	GET_NEXT_ARG(p2,list_or_nil);
	cell *vnames = NULL;
	pl_idx_t vnames_ctx = 0;
	q->flag = q->st.m->flag;
	LIST_HANDLER(p2);

	while (is_list(p2) && !g_tpl_interrupt) {
		cell *h = LIST_HEAD(p2);
		h = deref(q, h, p2_ctx);
		pl_idx_t h_ctx = q->latest_ctx;
		parse_write_params(q, h, h_ctx, &vnames, &vnames_ctx);
		p2 = LIST_TAIL(p2);
		p2 = deref(q, p2, p2_ctx);
		p2_ctx = q->latest_ctx;
	}

	q->variable_names = vnames;
	q->variable_names_ctx = vnames_ctx;
	char *dst = print_term_to_strbuf(q, p_term, p_term_ctx, 1);
	clear_write_options(q);
	cell tmp;
	may_error(make_string(&tmp, dst), free(dst));
	free(dst);
	pl_status ok = unify(q, p_chars, p_chars_ctx, &tmp, q->st.curr_frame);
	unshare_cell(&tmp);
	return ok;
}

static USE_RESULT pl_status fn_write_canonical_to_chars_3(query *q)
{
	GET_FIRST_ARG(p_chars,atom_or_var);
	GET_NEXT_ARG(p_term,any);
	GET_NEXT_ARG(p2,list_or_nil);
	cell *vnames = NULL;
	pl_idx_t vnames_ctx = 0;
	q->flag = q->st.m->flag;
	LIST_HANDLER(p2);

	while (is_list(p2) && !g_tpl_interrupt) {
		cell *h = LIST_HEAD(p2);
		h = deref(q, h, p2_ctx);
		pl_idx_t h_ctx = q->latest_ctx;
		parse_write_params(q, h, h_ctx, &vnames, &vnames_ctx);
		p2 = LIST_TAIL(p2);
		p2 = deref(q, p2, p2_ctx);
		p2_ctx = q->latest_ctx;
	}

	char *dst = print_canonical_to_strbuf(q, p_term, p_term_ctx, 1);
	clear_write_options(q);
	cell tmp;
	may_error(make_string(&tmp, dst), free(dst));
	free(dst);
	pl_status ok = unify(q, p_chars, p_chars_ctx, &tmp, q->st.curr_frame);
	unshare_cell(&tmp);
	return ok;
}

static USE_RESULT pl_status fn_edin_redo_1(query *q)
{
	GET_FIRST_ARG(p1,integer);
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	if (isatty(fileno(str->fp)) && !str->did_getc && !str->ungetch) {
		fprintf(str->fp, "%s", PROMPT);
		fflush(str->fp);
	}

	for (;;) {
		str->did_getc = true;
		int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);
		str->ungetch = 0;

		if (feof(str->fp)) {
			str->did_getc = false;
			break;
		} else if (ch == '\n')
			str->did_getc = false;

		if (ch == get_int(p1))
			break;
	}

	return pl_success;
}

static USE_RESULT pl_status fn_edin_redo_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];
	GET_NEXT_ARG(p1,integer);

	if (isatty(fileno(str->fp)) && !str->did_getc && !str->ungetch) {
		fprintf(str->fp, "%s", PROMPT);
		fflush(str->fp);
	}

	for (;;) {
		str->did_getc = true;
		int ch = str->ungetch ? str->ungetch : xgetc_utf8(net_getc, str);
		str->ungetch = 0;

		if (feof(str->fp)) {
			str->did_getc = false;
			break;
		} else if (ch == '\n')
			str->did_getc = false;

		if (ch == get_int(p1))
			break;
	}

	return pl_success;
}

static USE_RESULT pl_status fn_edin_tab_1(query *q)
{
	GET_FIRST_ARG(p1_tmp,any);
	cell p1 = eval(q, p1_tmp);

	if (!is_integer(&p1))
		return throw_error(q, &p1, p1_tmp_ctx, "type_error", "integer");

	int n = q->pl->current_output;
	stream *str = &q->pl->streams[n];

	for (int i = 0; i < get_int(&p1); i++)
		fputc(' ', str->fp);

	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_edin_tab_2(query *q)
{
	GET_FIRST_ARG(pstr,stream);
	GET_FIRST_ARG(p1_tmp,any);
	cell p1 = eval(q, p1_tmp);

	if (!is_integer(&p1))
		return throw_error(q, &p1, p1_tmp_ctx, "type_error", "integer");

	int n = get_stream(q, pstr);
	stream *str = &q->pl->streams[n];

	for (int i = 0; i < get_int(&p1); i++)
		fputc(' ', str->fp);

	return !ferror(str->fp);
}

static USE_RESULT pl_status fn_edin_seen_0(query *q)
{
	int n = q->pl->current_input;
	stream *str = &q->pl->streams[n];

	if (n <= 2)
		return pl_success;

	if ((str->fp != stdin)
		&& (str->fp != stdout)
		&& (str->fp != stderr))
		fclose(str->fp);

	free(str->filename);
	free(str->mode);
	free(str->name);
	memset(str, 0, sizeof(stream));
	q->pl->current_input = 0;
	return pl_success;
}

static USE_RESULT pl_status fn_edin_told_0(query *q)
{
	int n = q->pl->current_output;
	stream *str = &q->pl->streams[n];

	if (n <= 2)
		return pl_success;

	if ((str->fp != stdin)
		&& (str->fp != stdout)
		&& (str->fp != stderr))
		fclose(str->fp);

	free(str->filename);
	free(str->mode);
	free(str->name);
	memset(str, 0, sizeof(stream));
	q->pl->current_output = 0;
	return pl_success;
}

static USE_RESULT pl_status fn_edin_seeing_1(query *q)
{
	GET_FIRST_ARG(p1,variable);
	char *name = q->pl->current_input==0?"user":q->pl->streams[q->pl->current_input].name;
	cell tmp;
	may_error(make_cstring(&tmp, name));
	set_var(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	unshare_cell(&tmp);
	return pl_success;
}

static USE_RESULT pl_status fn_edin_telling_1(query *q)
{
	GET_FIRST_ARG(p1,variable);
	char *name =q->pl->current_output==1?"user":q->pl->streams[q->pl->current_output].name;
	cell tmp;
	may_error(make_cstring(&tmp, name));
	set_var(q, p1, p1_ctx, &tmp, q->st.curr_frame);
	unshare_cell(&tmp);
	return pl_success;
}

const struct builtins g_files_bifs[] =
{
#ifndef SANDBOX
	{"open", 4, fn_iso_open_4, NULL, false},
	{"close", 1, fn_iso_close_1, NULL, false},
	{"close", 2, fn_iso_close_2, NULL, false},

#ifndef _WIN32
	{"popen", 4, fn_popen_4, "+atom,+atom,-stream,+list", false},
#endif
#endif

	{"read_term", 2, fn_iso_read_term_2, NULL, false},
	{"read_term", 3, fn_iso_read_term_3, NULL, false},
	{"read", 1, fn_iso_read_1, NULL, false},
	{"read", 2, fn_iso_read_2, NULL, false},
	{"write_canonical", 1, fn_iso_write_canonical_1, NULL, false},
	{"write_canonical", 2, fn_iso_write_canonical_2, NULL, false},
	{"write_term", 2, fn_iso_write_term_2, NULL, false},
	{"write_term", 3, fn_iso_write_term_3, NULL, false},
	{"writeq", 1, fn_iso_writeq_1, NULL, false},
	{"writeq", 2, fn_iso_writeq_2, NULL, false},
	{"write", 1, fn_iso_write_1, NULL, false},
	{"write", 2, fn_iso_write_2, NULL, false},
	{"nl", 0, fn_iso_nl_0, NULL, false},
	{"nl", 1, fn_iso_nl_1, NULL, false},
	{"at_end_of_stream", 0, fn_iso_at_end_of_stream_0, NULL, false},
	{"at_end_of_stream", 1, fn_iso_at_end_of_stream_1, NULL, false},
	{"set_stream_position", 2, fn_iso_set_stream_position_2, NULL, false},
	{"flush_output", 0, fn_iso_flush_output_0, NULL, false},
	{"flush_output", 1, fn_iso_flush_output_1, NULL, false},
	{"put_char", 1, fn_iso_put_char_1, NULL, false},
	{"put_char", 2, fn_iso_put_char_2, NULL, false},
	{"put_code", 1, fn_iso_put_code_1, NULL, false},
	{"put_code", 2, fn_iso_put_code_2, NULL, false},
	{"put_byte", 1, fn_iso_put_byte_1, NULL, false},
	{"put_byte", 2, fn_iso_put_byte_2, NULL, false},
	{"get_char", 1, fn_iso_get_char_1, NULL, false},
	{"get_char", 2, fn_iso_get_char_2, NULL, false},
	{"get_code", 1, fn_iso_get_code_1, NULL, false},
	{"get_code", 2, fn_iso_get_code_2, NULL, false},
	{"get_byte", 1, fn_iso_get_byte_1, NULL, false},
	{"get_byte", 2, fn_iso_get_byte_2, NULL, false},
	{"peek_char", 1, fn_iso_peek_char_1, NULL, false},
	{"peek_char", 2, fn_iso_peek_char_2, NULL, false},
	{"peek_code", 1, fn_iso_peek_code_1, NULL, false},
	{"peek_code", 2, fn_iso_peek_code_2, NULL, false},
	{"peek_byte", 1, fn_iso_peek_byte_1, NULL, false},
	{"peek_byte", 2, fn_iso_peek_byte_2, NULL, false},
	{"current_input", 1, fn_iso_current_input_1, NULL, false},
	{"current_output", 1, fn_iso_current_output_1, NULL, false},
	{"set_input", 1, fn_iso_set_input_1, NULL, false},
	{"set_output", 1, fn_iso_set_output_1, NULL, false},
	{"stream_property", 2, fn_iso_stream_property_2, NULL, false},
	{"read_term_from_atom", 3, fn_read_term_from_atom_3, "+atom,?term,+list", false},
	{"read_term_from_chars", 3, fn_read_term_from_chars_3, "+chars,?term,+list", false},
	{"write_term_to_atom", 3, fn_write_term_to_atom_3, "?atom,?term,+list", false},
	{"write_canonical_to_atom", 3, fn_write_canonical_to_chars_3, "?atom,?term,+list", false},
	{"write_term_to_chars", 3, fn_write_term_to_chars_3, "?chars,?term,+list", false},
	{"write_canonical_to_chars", 3, fn_write_canonical_to_chars_3, "?chars,?term,+list", false},

	// Edinburgh...

	{"seeing", 1, fn_edin_seeing_1, "-name", false},
	{"telling", 1, fn_edin_telling_1, "-name", false},
	{"seen", 0, fn_edin_seen_0, NULL, false},
	{"told", 0, fn_edin_told_0, NULL, false},
	{"redo", 1, fn_edin_redo_1, "+integer", false},
	{"redo", 2, fn_edin_redo_2, "+stream,+integer", false},
	{"tab", 1, fn_edin_tab_1, "+integer", false},
	{"tab", 2, fn_edin_tab_2, "+stream,+integer", false},


	{0}
};

