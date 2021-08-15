#include <stdlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "internal.h"
#include "history.h"
#include "parser.h"
#include "module.h"
#include "prolog.h"
#include "query.h"
#include "builtins.h"
#include "heap.h"
#include "utf8.h"
#include "cdebug.h"

#define Trace if (q->trace /*&& !consulting*/) trace_call

static const unsigned INITIAL_NBR_HEAP = 8000;		// cells
static const unsigned INITIAL_NBR_QUEUE = 1000;		// cells

static const unsigned INITIAL_NBR_GOALS = 1000;
static const unsigned INITIAL_NBR_SLOTS = 32000;
static const unsigned INITIAL_NBR_CHOICES = 1000;
static const unsigned INITIAL_NBR_TRAILS = 1000;

int g_tpl_interrupt = 0;

typedef enum { CALL, EXIT, REDO, NEXT, FAIL } box_t;

// Note: when in commit there is a provisional choice point
// that we should skip over, hence the '2' ...

static bool any_choices(query *q, frame *g, bool in_commit)
{
	if (q->cp < (in_commit ? 2 : 1))
		return false;

	idx_t curr_choice = q->cp - (in_commit ? 2 : 1);
	const choice *ch = GET_CHOICE(curr_choice);
	return ch->cgen >= g->cgen ? true : false;
}

static void trace_call(query *q, cell *c, idx_t c_ctx, box_t box)
{
	if (!c || !c->fn || is_empty(c))
		return;

#if 0
	if (is_builtin(c))
		return;
#endif

	if (box == CALL)
		box = q->retry?REDO:q->resume?NEXT:CALL;

	const char *src = GET_STR(q, c);

	if (!strcmp(src, ",") || !strcmp(src, ";") || !strcmp(src, "->"))
		return;

	fprintf(stderr, " [%llu] ", (unsigned long long)q->step++);
	fprintf(stderr, "%s ",
		box == CALL ? "CALL" :
		box == EXIT ? "EXIT" :
		box == REDO ? "REDO" :
		box == NEXT ? (isatty(2)?"\e[32mNEXT\e[0m" : "NEXT") :
		box == FAIL ? (isatty(2) ? "\e[31mFAIL\e[0m" : "FAIL") :
		"????");

#if DEBUG
	frame *g = GET_CURR_FRAME();
	fprintf(stderr, "{f(%u:v=%u:s=%u):ch%u:tp%u:cp%u:fp%u:sp%u:hp%u} ",
		q->st.curr_frame, g->nbr_vars, g->nbr_slots, any_choices(q, g, false),
		q->st.tp, q->cp, q->st.fp, q->st.sp, q->st.hp);
#endif

	int save_depth = q->max_depth;
	q->max_depth = 1000;
	print_term(q, stderr, c, c_ctx, -1);
	fprintf(stderr, "\n");
	fflush(stderr);
	q->max_depth = save_depth;

	if (q->creep)
		sleep(1);
}

static USE_RESULT pl_status check_trail(query *q)
{
	if (q->st.tp > q->max_trails) {
		q->max_trails = q->st.tp;

		if (q->st.tp >= q->trails_size) {
			idx_t new_trailssize = alloc_grow((void**)&q->trails, sizeof(trail), q->st.tp, q->trails_size*3/2);
			if (!new_trailssize) {
				q->error = true;
				return pl_error;
			}

			q->trails_size = new_trailssize;
		}
	}

	return pl_success;
}

static USE_RESULT pl_status check_choice(query *q)
{
	if (q->cp > q->max_choices) {
		q->max_choices = q->cp;

		if (q->cp >= q->choices_size) {
			idx_t new_choicessize = alloc_grow((void**)&q->choices, sizeof(choice), q->cp, q->choices_size*3/2);
			if (!new_choicessize) {
				q->error = true;
				return pl_error;
			}

			q->choices_size = new_choicessize;
		}
	}

	return pl_success;
}

static USE_RESULT pl_status check_frame(query *q)
{
	if (q->st.fp > q->max_frames) {
		q->max_frames = q->st.fp;

		if (q->st.fp >= q->frames_size) {
			idx_t new_framessize = alloc_grow((void**)&q->frames, sizeof(frame), q->st.fp, q->frames_size*3/2);
			if (!new_framessize) {
				q->error = true;
				return pl_error;
			}

			q->frames_size = new_framessize;
		}
	}

	return pl_success;
}

static USE_RESULT pl_status check_slot(query *q, unsigned cnt)
{
	idx_t nbr = q->st.sp + cnt;

	if (nbr > q->max_slots) {
		q->max_slots = q->st.sp;

		if (nbr >= q->slots_size) {
			idx_t new_slotssize = alloc_grow((void**)&q->slots, sizeof(slot), nbr, (nbr*3/2));
			if (!new_slotssize) {
				q->error = true;
				return pl_error;
			}

			memset(q->slots+q->slots_size, 0, sizeof(slot)*(new_slotssize-q->slots_size));
			q->slots_size = new_slotssize;
		}
	}

	return pl_success;
}

struct ref_ {
	ref *next;
	idx_t var_nbr, ctx;
};

inline static bool is_in_ref_list(cell *c, idx_t c_ctx, ref *rlist)
{
	while (rlist && !g_tpl_interrupt) {
		if ((c->var_nbr == rlist->var_nbr)
			&& (c_ctx == rlist->ctx))
			return true;

		rlist = rlist->next;
	}

	return false;
}

static bool is_cyclic_term_internal(query *q, cell *p1, idx_t p1_ctx, ref *list)
{
	if (!is_structure(p1))
		return false;

	idx_t nbr_cells = p1->nbr_cells - 1;
	p1++;

	while (nbr_cells) {
		if (is_variable(p1)) {
			if (is_in_ref_list(p1, p1_ctx, list))
				return q->cycle_error = true;

			ref *nlist = malloc(sizeof(ref));
			nlist->next = list;
			nlist->var_nbr = p1->var_nbr;
			nlist->ctx = p1_ctx;

			cell *c = deref(q, p1, p1_ctx);
			idx_t c_ctx = q->latest_ctx;

			if (is_cyclic_term_internal(q, c, c_ctx, nlist)) {
				free(nlist);
				return true;
			}

			free(nlist);
		}

		nbr_cells--;
		p1++;
	}

	return false;
}

bool is_cyclic_term(query *q, cell *p1, idx_t p1_ctx)
{
	q->cycle_error = false;
	return is_cyclic_term_internal(q, p1, p1_ctx, NULL);
}

static void next_key(query *q)
{
	if (q->st.iter) {
		if (!m_nextkey(q->st.iter, (void**)&q->st.curr_clause)) {
			q->st.curr_clause = NULL;
			q->st.iter = NULL;
		}
	} else
		q->st.curr_clause = q->st.curr_clause->next;
}

static bool is_next_key(query *q)
{
	if (q->st.iter) {
		if (m_is_nextkey(q->st.iter))
			return true;
	} else if (q->st.curr_clause->next)
		return true;

	return false;
}

void add_to_dirty_list(query *q, clause *cl)
{
	if (!retract_from_db(q->st.m, cl))
		return;

	cl->dirty = q->dirty_list;
	q->dirty_list = cl;
}

static void purge_dirty_list(query *q)
{
	int cnt = 0;

	while (q->dirty_list) {
		clause *cl = q->dirty_list;
		q->dirty_list = cl->dirty;

		if (cl->prev)
			cl->prev->next = cl->next;

		if (cl->next)
			cl->next->prev = cl->prev;

		if (cl->owner->head == cl)
			cl->owner->head = cl->next;

		if (cl->owner->tail == cl)
			cl->owner->tail = cl->prev;

		clear_rule(&cl->r);
		free(cl);
		cnt++;
	}

	//if (cnt) printf("Info: query purged %d retracted items\n", cnt);
}

bool is_valid_list(query *q, cell *p1, idx_t p1_ctx, bool allow_partials)
{
	if (!is_list(p1) && !is_nil(p1))
		return false;

	LIST_HANDLER(p1);

	while (is_list(p1) && !g_tpl_interrupt) {
		LIST_HEAD(p1);
		p1 = LIST_TAIL(p1);
		p1 = deref(q, p1, p1_ctx);
		p1_ctx = q->latest_ctx;
	}

	return is_nil(p1) || (allow_partials && is_variable(p1));
}

bool is_valid_list_up_to(query *q, cell *p1, idx_t p1_ctx, bool allow_partials, int n)
{
	if (!is_list(p1) && !is_nil(p1))
		return false;

	LIST_HANDLER(p1);

	while (is_list(p1) && !g_tpl_interrupt) {
		LIST_HEAD(p1);

		if (!n)
			return true;

		p1 = LIST_TAIL(p1);
		p1 = deref(q, p1, p1_ctx);
		p1_ctx = q->latest_ctx;

		n--;
	}

	return is_nil(p1) || (allow_partials && is_variable(p1));
}

size_t scan_is_chars_list(query *q, cell *l, idx_t l_ctx, bool allow_codes)
{
	idx_t save_ctx = q ? q->latest_ctx : l_ctx;
	size_t is_chars_list = 0;
	LIST_HANDLER(l);
	int cnt = 0;

	while (is_iso_list(l) && !is_cyclic_term(q, l, l_ctx)
		&& (q->st.m->flag.double_quote_chars || allow_codes)
		&& !g_tpl_interrupt) {
		cell *h = LIST_HEAD(l);
		cell *c = q ? deref(q, h, l_ctx) : h;

		if (is_integer(c) && !allow_codes) {
			is_chars_list = 0;
			break;
		} else if (!is_integer(c) && !is_atom(c)) {
			is_chars_list = 0;
			break;
		}

		if (is_integer(c)) {
			int ch = get_integer(c);
			char tmp[20];
			put_char_utf8(tmp, ch);
			size_t len = len_char_utf8(tmp);
			is_chars_list += len;
		} else {
			const char *src = GET_STR(q, c);
			size_t len = len_char_utf8(src);

			if (len != LEN_STR(q, c)) {
				is_chars_list = 0;
				break;
			}

			is_chars_list += len;
		}

		l = LIST_TAIL(l);
		l = q ? deref(q, l, l_ctx) : l;
		if (q) l_ctx = q->latest_ctx;
		cnt++;
	}

	if (is_variable(l))
		is_chars_list = 0;
	else if (is_string(l))
		;
	else if (!is_literal(l) || (l->val_off != g_nil_s))
		is_chars_list = 0;

	if (q) q->latest_ctx = save_ctx;
	return is_chars_list;
}

static void unwind_trail(query *q, const choice *ch)
{
	while (q->st.tp > ch->st.tp) {
		const trail *tr = q->trails + --q->st.tp;

		if (ch->pins) {
			if (ch->pins & (1 << tr->var_nbr))
				continue;
		}

		const frame *g = GET_FRAME(tr->ctx);
		slot *e = GET_SLOT(g, tr->var_nbr);
		//unshare_cell(&e->c); // FIXME
		e->c.tag = TAG_EMPTY;
		e->c.attrs = tr->attrs;
		e->c.attrs_ctx = tr->attrs_ctx;
	}
}

void undo_me(query *q)
{
	const choice *ch = GET_CURR_CHOICE();
	unwind_trail(q, ch);
}

void try_me(const query *q, unsigned nbr_vars)
{
	frame *g = GET_FRAME(q->st.fp);
	g->nbr_slots = nbr_vars;
	g->nbr_vars = nbr_vars;
	g->ctx = q->st.sp;
	slot *e = GET_SLOT(g, 0);

	for (unsigned i = 0; i < nbr_vars; i++, e++) {
		unshare_cell(&e->c);
		e->c.tag = TAG_EMPTY;
		e->c.attrs = NULL;
	}
}

static void trim_heap(query *q, const choice *ch)
{
	for (arena *a = q->arenas; a;) {
		if (a->nbr <= ch->st.arena_nbr)
			break;

		for (idx_t i = 0; i < a->max_hp_used; i++) {
			cell *c = a->heap + i;
			unshare_cell(c);
			c->tag = TAG_EMPTY;
			c->attrs = NULL;
		}

		arena *save = a;
		q->arenas = a = a->next;
		free(save->heap);
		free(save);
	}

	const arena *a = q->arenas;

	for (idx_t i = ch->st.hp; a && (i < a->max_hp_used) && (i < q->st.hp); i++) {
		cell *c = a->heap + i;
		unshare_cell(c);
		c->tag = TAG_EMPTY;
		c->attrs = NULL;
	}
}

idx_t drop_choice(query *q)
{
	idx_t curr_choice = --q->cp;
	return curr_choice;
}

bool retry_choice(query *q)
{
	if (!q->cp)
		return false;

	idx_t curr_choice = drop_choice(q);
	const choice *ch = GET_CHOICE(curr_choice);
	unwind_trail(q, ch);

	// TO-DO: Watch for stack, make non-recursive...

	if (ch->catchme_exception || ch->soft_cut || ch->did_cleanup)
		return retry_choice(q);

	trim_heap(q, ch);
	m_done(q->st.iter);
	q->st = ch->st;
	q->save_m = NULL;		// maybe move q->save_m to q->st.save_m

	frame *g = GET_CURR_FRAME();
	g->ugen = ch->ugen;
	g->cgen = ch->orig_cgen;
	g->nbr_vars = ch->nbr_vars;
	g->nbr_slots = ch->nbr_slots;
	g->overflow = ch->overflow;
	return true;
}

static frame *make_frame(query *q, unsigned nbr_vars)
{
	idx_t new_frame = q->st.fp++;
	frame *g = GET_FRAME(new_frame);
	g->prev_frame = q->st.curr_frame;
	g->prev_cell = q->st.curr_cell;
	g->cgen = ++q->st.cgen;
	g->overflow = 0;

	q->st.sp += nbr_vars;
	q->st.curr_frame = new_frame;
	g = GET_FRAME(q->st.curr_frame);
	return g;
}

static void trim_trail(query *q)
{
	if (q->undo_hi_tp)
		return;

	if (!q->cp) {
		q->st.tp = 0;
		return;
	}

	idx_t tp;

	if (q->cp) {
		const choice *ch = GET_CURR_CHOICE();
		tp = ch->st.tp;
	} else
		tp = 0;

	while (q->st.tp > tp) {
		const trail *tr = q->trails + q->st.tp - 1;

		if (tr->ctx != q->st.curr_frame)
			break;

		q->st.tp--;
	}
}

static void reuse_frame(query *q, unsigned nbr_vars)
{
	const frame *newg = GET_FRAME(q->st.fp);
	frame *g = GET_CURR_FRAME();
	slot *e = GET_SLOT(g, 0);

	for (unsigned i = 0; i < g->nbr_vars; i++, e++) {
		unshare_cell(&e->c);
		e->c.tag = TAG_EMPTY;
		e->c.attrs = NULL;
	}

	g->cgen = newg->cgen;
	g->nbr_slots = nbr_vars;
	g->nbr_vars = nbr_vars;
	g->overflow = 0;

	const choice *ch = GET_CURR_CHOICE();
	q->st.sp = ch->st.sp;

	const slot *from = GET_SLOT(newg, 0);
	slot *to = GET_SLOT(g, 0);
	memmove(to, from, sizeof(slot)*nbr_vars);
	q->st.sp = g->ctx + nbr_vars;
	q->tot_tcos++;
}

static bool check_slots(const query *q, frame *g, rule *r)
{
	if (g->nbr_vars != r->nbr_vars)
		return false;

	for (unsigned i = 0; i < g->nbr_vars; i++) {
		const slot *e = GET_SLOT(g, i);

		if (is_indirect(&e->c))
			return false;

		if (is_string(&e->c))
			return false;
	}

	return true;
}

static void commit_me(query *q, rule *r)
{
	frame *g = GET_CURR_FRAME();
	g->m = q->st.m;
	q->st.m = q->st.curr_clause->owner->m;
	q->st.iter = NULL;
	bool last_match = r->first_cut || !is_next_key(q);
	bool recursive = is_tail_recursive(q->st.curr_cell);
	bool tco = !q->no_tco && recursive && !any_choices(q, g, true);
	bool slots_ok = check_slots(q, g, r);
	choice *ch = GET_CURR_CHOICE();

#if 0
	printf("*** tco=%d, q->no_tco=%d, last_match=%d, rec=%d, any_choices=%d, check_slots=%d\n",
		tco, q->no_tco, last_match, recursive, any_choices(q, g, true), check_slots(q, g, r));
#endif

	if (tco && slots_ok && q->st.m->pl->opt)
		reuse_frame(q, r->nbr_vars);
	else
		g = make_frame(q, r->nbr_vars);

	if (last_match) {
		drop_choice(q);
		trim_trail(q);
	} else {
		ch->st.curr_clause = q->st.curr_clause;
		ch->cgen = g->cgen;
	}

	q->st.curr_cell = get_body(r->cells);
	//memset(q->nv_mask, 0, MAX_ARITY);
}

void stash_me(query *q, rule *r, bool last_match)
{
	idx_t cgen = q->st.cgen;

	if (last_match)
		drop_choice(q);
	else {
		choice *ch = GET_CURR_CHOICE();
		ch->st.curr_clause2 = q->st.curr_clause2;
		cgen = ++q->st.cgen;
		ch->cgen = cgen;
	}

	unsigned nbr_vars = r->nbr_vars;
	idx_t new_frame = q->st.fp++;
	frame *g = GET_FRAME(new_frame);
	g->prev_frame = q->st.curr_frame;
	g->prev_cell = NULL;
	g->cgen = cgen;
	g->overflow = 0;

	q->st.sp += nbr_vars;
}

pl_status make_choice(query *q)
{
	may_error(check_frame(q));
	may_error(check_choice(q));

	frame *g = GET_CURR_FRAME();
	idx_t curr_choice = q->cp++;
	choice *ch = GET_CHOICE(curr_choice);
	memset(ch, 0, sizeof(choice));
	ch->ugen = g->ugen;
	ch->orig_cgen = ch->cgen = g->cgen;
	ch->st = q->st;

	may_error(check_slot(q, MAX_ARITY));
	ch->nbr_vars = g->nbr_vars;
	ch->nbr_slots = g->nbr_slots;
	ch->overflow = g->overflow;

	return pl_success;
}

// A barrier is used when making a call/1, it sets a
// new choice generation so that cuts are contained...

pl_status make_barrier(query *q)
{
	may_error(make_choice(q));
	frame *g = GET_CURR_FRAME();
	choice *ch = GET_CURR_CHOICE();
	ch->cgen = g->cgen = ++q->st.cgen;
	ch->barrier = true;
	return pl_success;
}

pl_status make_catcher(query *q, enum q_retry retry)
{
	may_error(make_barrier(q));
	choice *ch = GET_CURR_CHOICE();

	if (retry == QUERY_RETRY)
		ch->catchme_retry = true;
	else if (retry == QUERY_EXCEPTION)
		ch->catchme_exception = true;

	return pl_success;
}

void cut_me(query *q, bool local_cut, bool soft_cut)
{
	frame *g = GET_CURR_FRAME();

	while (q->cp) {
		choice *ch = GET_CURR_CHOICE();

		while (soft_cut) {
			if (ch->barrier && (ch->cgen == g->cgen)) {
				ch->soft_cut = true;
				g->cgen--;
				return;
			}

			ch--;
		}

		if (!local_cut && ch->barrier && (ch->cgen == g->cgen))
			break;

		if (ch->cgen < g->cgen)
			break;

		q->cp--;

		if (ch->chk_is_det) {
			extern void do_cleanup(query *q, cell *p1);
			ch->chk_is_det = false;

			while (--ch) {
				if (ch->register_cleanup) {
					if (ch->did_cleanup)
						break;

					ch->did_cleanup = true;
					cell *c = ch->st.curr_cell;
					//c = deref(q, c, ch->st.curr_frame);
					cell *p1 = deref(q, c+1, ch->st.curr_frame);
					cell *tmp = deep_copy_to_heap(q, p1, ch->st.curr_frame, true, false);
					do_cleanup(q, tmp);
					break;
				}

				q->cp--;
			}

			break;
		}

#if 0
		if (ch->tail_rec) {
			printf("*** here2\n");
			frame *g_prev = GET_FRAME(g->prev_frame);
			g->prev_frame = g_prev->prev_frame;
			g->prev_cell = g_prev->prev_cell;
			*g_prev = *g;
			q->st.curr_frame--;
			q->st.fp--;
			q->tot_tcos++;
		}
#endif
	}

	if (!q->cp && !q->undo_hi_tp)
		q->st.tp = 0;
}

// Continue to next rule in body

static void proceed(query *q)
{
	q->st.curr_cell += q->st.curr_cell->nbr_cells;
	frame *g = GET_CURR_FRAME();

	while (q->st.curr_cell && is_end(q->st.curr_cell)) {
		if (q->st.curr_cell->cgen != ERR_IDX)
			g->cgen = q->st.curr_cell->cgen;

		if (q->st.curr_cell->mod_nbr != q->st.m->id)
			q->st.m = find_module_id(q->st.m->pl, q->st.curr_cell->mod_nbr);

		q->st.curr_cell = q->st.curr_cell->val_ptr;
	}
}

// Reached end of body, return to previous frame

static bool resume_frame(query *q)
{
	if (!q->st.curr_frame)
		return false;

	frame *g = GET_CURR_FRAME();

#if 0
	rule *r = &q->st.curr_clause->r;

	if ((q->st.curr_frame == (q->st.fp-1))
		&& q->st.m->pl->opt && r->tail_rec
		&& !any_choices(q, g, false) && check_slots(q, g, r))
		q->st.fp--;
#endif

	q->st.curr_cell = g->prev_cell;
	q->st.curr_frame = g->prev_frame;
	g = GET_CURR_FRAME();
	q->st.m = g->m;
	return true;
}

void make_indirect(cell *tmp, cell *c)
{
	tmp->tag = TAG_INDIRECT;
	tmp->nbr_cells = 1;
	tmp->arity = 0;
	tmp->flags = 0;
	tmp->val_ptr = c;
}

unsigned create_vars(query *q, unsigned cnt)
{
	frame *g = GET_CURR_FRAME();

	if (!cnt)
		return g->nbr_vars;

	unsigned var_nbr = g->nbr_vars;

	if (check_slot(q, cnt) != pl_success)
		return 0;

	if ((g->ctx + g->nbr_slots) >= q->st.sp) {
		g->nbr_slots += cnt;
		q->st.sp = g->ctx + g->nbr_slots;
	} else if (!g->overflow) {
		g->overflow = q->st.sp;
		q->st.sp += cnt;
	} else if ((g->overflow + (g->nbr_vars - g->nbr_slots)) == q->st.sp) {
		q->st.sp += cnt;
	} else {
		idx_t save_overflow = g->overflow;
		g->overflow = q->st.sp;
		idx_t cnt2 = g->nbr_vars - g->nbr_slots;
		memmove(q->slots+g->overflow, q->slots+save_overflow, sizeof(slot)*cnt2);
		q->st.sp += cnt2 + cnt;
	}

	for (unsigned i = 0; i < cnt; i++) {
		slot *e = GET_SLOT(g, g->nbr_vars+i);
		e->c.tag = TAG_EMPTY;
		e->c.attrs = NULL;
	}

	g->nbr_vars += cnt;
	return var_nbr;
}

static void add_trail(query *q, idx_t c_ctx, unsigned c_var_nbr, cell *attrs, idx_t attrs_ctx)
{
	if (check_trail(q) != pl_success) {
		q->error = pl_error;
		return;
	}

	trail *tr = q->trails + q->st.tp++;
	tr->ctx = c_ctx;
	tr->var_nbr = c_var_nbr;
	tr->attrs = attrs;
	tr->attrs_ctx = attrs_ctx;
}

void set_var(query *q, const cell *c, idx_t c_ctx, cell *v, idx_t v_ctx)
{
	const frame *g = GET_FRAME(c_ctx);
	slot *e = GET_SLOT(g, c->var_nbr);
	e->ctx = v_ctx;
	cell *attrs = NULL;
	idx_t attrs_ctx = 0;

	if (is_empty(&e->c)) {
		attrs = e->c.attrs;
		attrs_ctx = e->c.attrs_ctx;
	} else
		attrs = NULL;

	if (is_structure(v))
		make_indirect(&e->c, v);
	else {
		e->c = *v;
		share_cell(v);
	}

	if (attrs) {
		if (is_variable(v)) {
			const frame *g = GET_FRAME(v_ctx);
			slot *e = GET_SLOT(g, v->var_nbr);

			if (!e->c.attrs) {
				e->c.attrs = attrs;
				e->c.attrs_ctx = attrs_ctx;

				if (q->cp)
					add_trail(q, v_ctx, v->var_nbr, NULL, 0);
			} else
				q->has_attrs = true;
		} else
			q->has_attrs = true;
	}

	if (q->cp)
		add_trail(q, c_ctx, c->var_nbr, attrs, attrs_ctx);
}

void reset_var(query *q, const cell *c, idx_t c_ctx, cell *v, idx_t v_ctx)
{
	const frame *g = GET_FRAME(c_ctx);
	slot *e = GET_SLOT(g, c->var_nbr);

	while (is_variable(&e->c)) {
		c = &e->c;
		c_ctx = e->ctx;
		g = GET_FRAME(c_ctx);
		e = GET_SLOT(g, c->var_nbr);
	}

	e->ctx = v_ctx;

	if (v->arity && !is_string(v))
		make_indirect(&e->c, v);
	else {
		e->c = *v;
		share_cell(v);
	}
}

static bool unify_structure(query *q, cell *p1, idx_t p1_ctx, cell *p2, idx_t p2_ctx, unsigned depth)
{
	if (p1->arity != p2->arity)
		return false;

	if (p1->val_off != p2->val_off)
		return false;

	unsigned arity = p1->arity;
	p1++; p2++;

	while (arity--) {
		cell *c1 = deref(q, p1, p1_ctx);
		idx_t c1_ctx = q->latest_ctx;
		cell *c2 = deref(q, p2, p2_ctx);
		idx_t c2_ctx = q->latest_ctx;

		if (!unify_internal(q, c1, c1_ctx, c2, c2_ctx, depth+1))
			return false;

		p1 += p1->nbr_cells;
		p2 += p2->nbr_cells;
	}

	return true;
}

static bool unify_list(query *q, cell *p1, idx_t p1_ctx, cell *p2, idx_t p2_ctx, unsigned depth)
{
	LIST_HANDLER(p1);
	LIST_HANDLER(p2);

	while (is_list(p1) && is_list(p2) && !g_tpl_interrupt) {
		cell *h1 = LIST_HEAD(p1);
		cell *h2 = LIST_HEAD(p2);

		cell *c1 = deref(q, h1, p1_ctx);
		idx_t c1_ctx = q->latest_ctx;
		cell *c2 = deref(q, h2, p2_ctx);
		idx_t c2_ctx = q->latest_ctx;

		if (!unify_internal(q, c1, c1_ctx, c2, c2_ctx, depth+1))
			return false;

		p1 = LIST_TAIL(p1);
		p2 = LIST_TAIL(p2);

		p1 = deref(q, p1, p1_ctx);
		p1_ctx = q->latest_ctx;
		p2 = deref(q, p2, p2_ctx);
		p2_ctx = q->latest_ctx;
	}

	return unify_internal(q, p1, p1_ctx, p2, p2_ctx, depth+1);
}

static bool unify_integer(__attribute__((unused)) query *q, cell *p1, cell *p2)
{
	if (is_bigint(p1) && is_bigint(p2))
		return !mp_int_compare(&p1->val_bigint->ival, &p2->val_bigint->ival);

	if (is_bigint(p1) && is_integer(p2))
		return !mp_int_compare_value(&p1->val_bigint->ival, p2->val_int);

	if (is_bigint(p2) && is_integer(p1))
		return !mp_int_compare_value(&p2->val_bigint->ival, p1->val_int);

	if (is_integer(p2))
		return (get_integer(p1) == get_integer(p2));

	return false;
}

static bool unify_real(__attribute__((unused)) query *q, cell *p1, cell *p2)
{
	if (is_real(p2))
		return get_real(p1) == get_real(p2);

	return false;
}

static bool unify_literal(query *q, cell *p1, cell *p2)
{
	if (is_literal(p2))
		return p1->val_off == p2->val_off;

	if (is_cstring(p2) && (LEN_STR(q, p1) == LEN_STR(q, p2)))
		return !memcmp(GET_STR(q, p2), GET_POOL(q, p1->val_off), LEN_STR(q, p1));

	return false;
}

static bool unify_cstring(query *q, cell *p1, cell *p2)
{
	if (is_cstring(p2) && (LEN_STR(q, p1) == LEN_STR(q, p2)))
		return !memcmp(GET_STR(q, p1), GET_STR(q, p2), LEN_STR(q, p1));

	if (is_literal(p2) && (LEN_STR(q, p1) == LEN_STR(q, p2)))
		return !memcmp(GET_STR(q, p1), GET_POOL(q, p2->val_off), LEN_STR(q, p1));

	return false;
}

struct dispatch {
	uint8_t tag;
	bool (*fn)(query*, cell*, cell*);
};

static const struct dispatch g_disp[] =
{
	{TAG_EMPTY, NULL},
	{TAG_VARIABLE, NULL},
	{TAG_LITERAL, unify_literal},
	{TAG_CSTRING, unify_cstring},
	{TAG_INTEGER, unify_integer},
	{TAG_REAL, unify_real},
	{0}
};

bool unify_internal(query *q, cell *p1, idx_t p1_ctx, cell *p2, idx_t p2_ctx, unsigned depth)
{
	if (!depth)
		q->cycle_error = false;

	if (depth >= MAX_DEPTH) {
		q->cycle_error = true;
		return true;
	}

	if (p1_ctx == q->st.curr_frame)
		q->no_tco = true;

	if (is_variable(p1) && is_variable(p2)) {
		if (p2_ctx > p1_ctx)
			set_var(q, p2, p2_ctx, p1, p1_ctx);
		else if (p2_ctx < p1_ctx)
			set_var(q, p1, p1_ctx, p2, p2_ctx);
		else if (p2->var_nbr != p1->var_nbr)
			set_var(q, p2, p2_ctx, p1, p1_ctx);

		return true;
	}

	if (is_variable(p1)) {
		set_var(q, p1, p1_ctx, p2, p2_ctx);
		return true;
	}

	if (is_variable(p2)) {
		set_var(q, p2, p2_ctx, p1, p1_ctx);
		return true;
	}

	if (is_string(p1) && is_string(p2))
		return unify_cstring(q, p1, p2);

	if (is_list(p1) && is_list(p2))
		return unify_list(q, p1, p1_ctx, p2, p2_ctx, depth+1);

	if (p1->arity || p2->arity)
		return unify_structure(q, p1, p1_ctx, p2, p2_ctx, depth+1);

	return g_disp[p1->tag].fn(q, p1, p2);
}

static bool check_update_view(const frame *g, const clause *c)
{
	if (c->r.ugen_created > g->ugen)
		return false;

	if (c->r.ugen_erased && (c->r.ugen_erased <= g->ugen))
		return false;

	return true;
}

// Match HEAD :- BODY.

USE_RESULT pl_status match_rule(query *q, cell *p1, idx_t p1_ctx)
{
	if (!q->retry) {
		cell *head = deref(q, get_head(p1), p1_ctx);
		cell *c = head;

		if (!is_literal(c)) {
			// For now convert it to a literal
			idx_t off = index_from_pool(q->st.m->pl, GET_STR(q, c));
			may_idx_error(off);
			unshare_cell(c);
			c->tag = TAG_LITERAL;
			c->val_off = off;
			c->flags = 0;
			c->arity = 0;
		}

		predicate *pr = search_predicate(q->st.m, head);

		if (!pr) {
			bool found = false;

			if (get_builtin(q->st.m->pl, GET_STR(q, head), head->arity, &found), found)
				return throw_error(q, head, "permission_error", "modify,static_procedure");

			q->st.curr_clause2 = NULL;
			return false;
		}

		if (!pr->is_dynamic)
			return throw_error(q, head, "permission_error", "modify,static_procedure");

		q->st.curr_clause2 = pr->head;
		frame *g = GET_FRAME(q->st.curr_frame);
		g->ugen = q->st.m->pl->ugen;
	} else {
		q->st.curr_clause2 = q->st.curr_clause2->next;
	}

	if (!q->st.curr_clause2)
		return pl_failure;

	may_error(make_choice(q));
	cell *p1_body = deref(q, get_logical_body(p1), p1_ctx);
	cell *orig_p1 = p1;
	const frame *g = GET_FRAME(q->st.curr_frame);

	for (; q->st.curr_clause2; q->st.curr_clause2 = q->st.curr_clause2->next) {
		if (!check_update_view(g, q->st.curr_clause2))
			continue;

		rule *r = &q->st.curr_clause2->r;
		cell *c = r->cells;
		bool needs_true = false;
		p1 = orig_p1;
		cell *c_body = get_logical_body(c);

		if (p1_body && is_variable(p1_body) && !c_body) {
			p1 = deref(q, get_head(p1), p1_ctx);
			c = get_head(c);
			needs_true = true;
		}

		try_me(q, r->nbr_vars);
		q->tot_matches++;

		if (unify_structure(q, p1, p1_ctx, c, q->st.fp, 0)) {
			int ok;

			if (needs_true) {
				p1_body = deref(q, p1_body, p1_ctx);
				idx_t p1_body_ctx = q->latest_ctx;
				cell tmp = (cell){0};
				tmp.tag = TAG_LITERAL;
				tmp.nbr_cells = 1;
				tmp.val_off = g_true_s;
				ok = unify(q, p1_body, p1_body_ctx, &tmp, q->st.curr_frame);
			} else
				ok = pl_success;

			return ok;
		}

		undo_me(q);
	}

	drop_choice(q);
	return pl_failure;
}

// Match HEAD.
// Match HEAD :- true.

USE_RESULT pl_status match_clause(query *q, cell *p1, idx_t p1_ctx, enum clause_type is_retract)
{
	if (!q->retry) {
		cell *c = p1;

		if (!is_literal(c)) {
			// For now convert it to a literal
			idx_t off = index_from_pool(q->st.m->pl, GET_STR(q, c));
			may_idx_error(off);
			unshare_cell(c);
			c->tag = TAG_LITERAL;
			c->val_off = off;
			c->flags = 0;
		}

		predicate *pr = search_predicate(q->st.m, p1);

		if (!pr) {
			bool found = false;

			if (get_builtin(q->st.m->pl, GET_STR(q, p1), p1->arity, &found), found) {
				if (is_retract != DO_CLAUSE)
					return throw_error(q, p1, "permission_error", "modify,static_procedure");
				else
					return throw_error(q, p1, "permission_error", "access,private_procedure");
			}

			q->st.curr_clause2 = NULL;
			return pl_failure;
		}

		if (!pr->is_dynamic) {
			if (is_retract != DO_CLAUSE)
				return throw_error(q, p1, "permission_error", "modify,static_procedure");
			else
				return throw_error(q, p1, "permission_error", "access,private_procedure");
		}

		q->st.curr_clause2 = pr->head;
		frame *g = GET_FRAME(q->st.curr_frame);
		g->ugen = q->st.m->pl->ugen;
	} else {
		q->st.curr_clause2 = q->st.curr_clause2->next;
	}

	if (!q->st.curr_clause2)
		return pl_failure;

	may_error(make_choice(q));
	const frame *g = GET_FRAME(q->st.curr_frame);

	for (; q->st.curr_clause2; q->st.curr_clause2 = q->st.curr_clause2->next) {
		if (!check_update_view(g, q->st.curr_clause2))
			continue;

		rule *r = &q->st.curr_clause2->r;
		cell *head = get_head(r->cells);
		cell *body = get_logical_body(r->cells);

		// Retract(HEAD) should ignore rules (and directives)

		if ((is_retract == DO_RETRACT) && body)
			continue;

		try_me(q, r->nbr_vars);
		q->tot_matches++;

		if (unify_structure(q, p1, p1_ctx, head, q->st.fp, 0))
			return pl_success;

		undo_me(q);
	}

	drop_choice(q);
	return pl_failure;
}

#define DUMP_KEYS 0

#if DUMP_KEYS
static const char *dump_key(const void *p1, const void *p)
{
	query *q = (query*)p;
	const cell *c = (cell*)p1;
	static char tmpbuf[1024];
	print_term_to_buf(q, tmpbuf, sizeof(tmpbuf), c, q->st.curr_frame, 0, false, 0);
	return tmpbuf;
}
#endif

static USE_RESULT pl_status match_head(query *q)
{
	if (!q->retry) {
		cell *c = q->st.curr_cell;
		predicate *pr;

		if (is_literal(c)) {
			pr = c->match;
		} else {
			// For now convert it to a literal
			idx_t off = index_from_pool(q->st.m->pl, GET_STR(q, c));
			may_idx_error(off);
			unshare_cell(c);
			c->tag = TAG_LITERAL;
			c->val_off = off;
			c->flags = 0;
			pr = NULL;
		}

		if (!pr) {
			pr = search_predicate(q->st.m, c);
			q->save_m = q->st.m;

			if (!pr) {
				if (!is_end(c) && !(is_literal(c) && !strcmp(GET_STR(q, c), "initialization")))
					if (q->st.m->flag.unknown == 1)
						return throw_error(q, c, "existence_error", "procedure");
					else
						return pl_failure;
				else
					q->error = true;

				return pl_error;
			}

			c->match = pr;
		}

		if (pr->idx1) {
			cell *key = deep_clone_to_tmp(q, c, q->st.curr_frame);
			cell *p1 = NULL, *p2 = NULL;

			if (key->arity) {
				p1 = key + 1;

				if (key->arity > 1) {
					p2 = p1 + p1->nbr_cells;

					if (is_variable(p2))
						p2 = NULL;
				} else {
					if (is_variable(p1))
						p1 = NULL;
				}
			}

			if (p1 && is_variable(p1))
				p1 = NULL;

			if (p1 || p2) {
#if DUMP_KEYS
				sl_dump(pr->idx1, dump_key, q);
				sl_dump(pr->idx2, dump_key, q);
#endif
				map *idx = p2 ? pr->idx2 : pr->idx1;
				q->st.iter = m_findkey(idx, key);
				next_key(q);
			} else
				q->st.curr_clause = pr->head;
		} else
			q->st.curr_clause = pr->head;

		frame *g = GET_FRAME(q->st.curr_frame);
		g->ugen = q->st.m->pl->ugen;
	} else
		next_key(q);

	if (!q->st.curr_clause)
		return pl_failure;

	may_error(make_choice(q));
	const frame *g = GET_FRAME(q->st.curr_frame);

	for (; q->st.curr_clause; next_key(q)) {
		if (!check_update_view(g, q->st.curr_clause))
			continue;

		rule *r = &q->st.curr_clause->r;
		cell *head = get_head(r->cells);
		try_me(q, r->nbr_vars);
		q->tot_matches++;
		q->no_tco = false;

		if (unify_structure(q, q->st.curr_cell, q->st.curr_frame, head, q->st.fp, 0)) {
			if (q->error)
				return pl_error;

			commit_me(q, r);
			return pl_success;
		}

		undo_me(q);
	}

	drop_choice(q);
	return pl_failure;
}

static cell *check_duplicate_result(query *q, unsigned orig, cell *orig_c, idx_t orig_ctx, cell *tmp)
{
	return orig_c;

	parser *p = q->p;
	frame *g = GET_FRAME(0);
	cell *c = orig_c;

	for (unsigned i = 0; i < p->nbr_vars; i++) {
		if (i >= orig)
			break;

		slot *e = GET_SLOT(g, i);

		if (is_empty(&e->c))
			continue;

		q->latest_ctx = 0;

		if (is_indirect(&e->c)) {
			c = e->c.val_ptr;
			q->latest_ctx = e->ctx;
		} else
			c = deref(q, &e->c, e->ctx);

		if (!is_variable(orig_c) && is_variable(c))
			continue;

		if (unify(q, c, q->latest_ctx, orig_c, orig_ctx)) {
			tmp->tag = TAG_VARIABLE;
			tmp->nbr_cells = 1;
			tmp->val_off = index_from_pool(q->st.m->pl, p->vartab.var_name[i]);
			tmp->arity = 0;
			tmp->flags = 0;
			tmp->var_nbr = 0;
			q->latest_ctx = orig_ctx;
			return tmp;
		}
	}

	q->latest_ctx = orig_ctx;
	return orig_c;
}

static void dump_vars(query *q, bool partial)
{
	parser *p = q->p;
	frame *g = GET_FRAME(0);
	int any = 0;

	q->is_dump_vars = true;

	for (unsigned i = 0; i < p->nbr_vars; i++) {
		if (!strcmp(p->vartab.var_name[i], "_"))
			continue;

		slot *e = GET_SLOT(g, i);

		if (is_empty(&e->c))
			continue;

		cell *c = deref(q, &e->c, e->ctx);

		if (is_indirect(&e->c)) {
			c = e->c.val_ptr;
			q->latest_ctx = e->ctx;
		}

		if (any)
			fprintf(stdout, ",\n");

		fprintf(stdout, "%s = ", p->vartab.var_name[i]);
		bool parens = false;
		cell tmp;
		c = check_duplicate_result(q, i, c, q->latest_ctx, &tmp);

		// If priority >= '=' then put in parens...

		if (is_structure(c)) {
			unsigned pri = find_op(q->st.m, GET_STR(q, c), GET_OP(c));
			if (pri >= 700) parens = true;
		}

		if (is_atom(c) && !is_string(c) && search_op(q->st.m, GET_STR(q, c), NULL, false) && !IS_OP(c))
			parens = true;

		if (parens) fputc('(', stdout);
		idx_t c_ctx = q->latest_ctx;

		if (is_cyclic_term(q, c, c_ctx))
			print_term(q, stdout, c, c_ctx, 0);
		else
			print_term(q, stdout, c, c_ctx, -2);

		if (parens) fputc(')', stdout);
		any++;
	}

	q->is_dump_vars = false;

	if (any && !partial) {
		fprintf(stdout, ".\n");
		fflush(stdout);
	}

	q->st.m->pl->did_dump_vars = any;
}

static int check_interrupt(query *q)
{
	signal(SIGINT, &sigfn);
	g_tpl_interrupt = 0;

	for (;;) {
		printf("\nAction (a)bort, (f)ail, (c)ontinue, (t)race, c(r)eep, (e)xit: ");
		fflush(stdout);
		int ch = history_getch();
		printf("%c\n", ch);

		if (ch == 't') {
			q->trace = !q->trace;
			return 0;
		}

		if (ch == 'r') {
			q->trace = true;
			q->creep = true;
			return 0;
		}

		if (ch == 'c')
			return 0;

		if (ch == 'f')
			return -1;

		if (ch == 'a') {
			q->abort = true;
			return 1;
		}

		if (ch == 'e') {
			signal(SIGINT, NULL);
			q->halt = true;
			return 1;
		}
	}
}

static bool check_redo(query *q)
{
	if (q->do_dump_vars && q->cp) {
		dump_vars(q, true);

		if (!q->st.m->pl->did_dump_vars)
			printf("true");
	}

	fflush(stdout);

	for (;;) {
		printf(" ");
		fflush(stdout);
		int ch = history_getch();

		if ((ch == 'h') || (ch == '?')) {
			printf("Action (a)bort, (e)xit, (r)edo:\n");
			fflush(stdout);
			continue;
		}

		if ((ch == 'r') || (ch == ' ') || (ch == ';')) {
			printf("\n; ");
			fflush(stdout);
			q->retry = QUERY_RETRY;
			break;
		}

		if ((ch == '\n') || (ch == 'a')) {
			printf("\n; ...\n");
			q->abort = true;
			return true;
		}

		if (ch == 'e') {
			signal(SIGINT, NULL);
			q->error = q->halt = true;
			return true;
		}
	}

	return false;
}

static bool any_outstanding_choices(query *q)
{
	if (!q->cp)
		return false;

	choice *ch = GET_CURR_CHOICE();

	for (;;) {
		if (!ch->register_cleanup)
			break;

		ch--;
		q->cp--;
	}

	return q->cp > 0;
}

pl_status start(query *q)
{
	q->yielded = false;
	bool done = false;

	while (!done && !q->error) {
		if (g_tpl_interrupt) {
			int ok = check_interrupt(q);

			switch (ok) {
				case 1:
					return pl_success;
				case -1:
					q->retry = true;
				default:
					continue;
			}
		}

		if (q->retry) {
			Trace(q, q->st.curr_cell, q->st.curr_frame, FAIL);

			if (!retry_choice(q))
				break;
		}

		if (is_variable(q->st.curr_cell)) {
			if (!fn_call_0(q, q->st.curr_cell))
				continue;
		}

		q->tot_goals++;
		q->did_throw = false;
		q->save_tp = q->st.tp;
		q->has_attrs = false;
		Trace(q, q->st.curr_cell, q->st.curr_frame, CALL);
		cell *save_cell = q->st.curr_cell;
		idx_t save_ctx = q->st.curr_frame;

		if (q->st.curr_cell->flags&FLAG_BUILTIN) {
			if (!q->st.curr_cell->fn) {					// NO-OP
				q->tot_goals--;
				q->st.curr_cell++;
				continue;
			}

			if (!q->st.curr_cell->fn(q)) {
				q->retry = QUERY_RETRY;

				if (q->yielded)
					break;

				q->tot_retries++;
				continue;
			}

			if (q->has_attrs && !q->in_hook)
				may_error(do_post_unification_hook(q));

			proceed(q);
		} else if (is_iso_list(q->st.curr_cell)) {
			consultall(q->st.m->p, q->st.curr_cell);
			proceed(q);
		} else {
			if (!is_callable(q->st.curr_cell)) {
				DISCARD_RESULT throw_error(q, q->st.curr_cell, "type_error", "callable");
			} else if (match_head(q) != pl_success) {
				q->retry = QUERY_RETRY;
				q->tot_retries++;
				continue;
			}

			if (q->has_attrs && !q->in_hook)
				may_error(do_post_unification_hook(q));
		}

		Trace(q, save_cell, save_ctx, EXIT);
		q->resume = false;
		q->retry = QUERY_OK;

		while (!q->st.curr_cell || is_end(q->st.curr_cell)) {
			if (!resume_frame(q)) {

				while (q->cp) {
					choice *ch = GET_CURR_CHOICE();

					if (!ch->barrier)
						break;

					q->cp--;
				}

				if (any_outstanding_choices(q) && q->p && !q->run_init) {
					if (!check_redo(q))
						break;

					return pl_success;
				}

				done = q->status = true;
				break;
			}

			q->resume = true;
			proceed(q);
		}
	}

	if (!q->p)
		return pl_success;

	if (q->halt)
		q->error = false;
	else if (q->do_dump_vars && !q->abort && q->status)
		dump_vars(q, false);
	else
		q->st.m->pl->did_dump_vars = false;

	return pl_success;
}

uint64_t cpu_time_in_usec(void)
{
	struct timespec now;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
	return (uint64_t)(now.tv_sec * 1000 * 1000) + (now.tv_nsec / 1000);
}

uint64_t get_time_in_usec(void)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	return (uint64_t)(now.tv_sec * 1000 * 1000) + (now.tv_nsec / 1000);
}

pl_status execute(query *q, rule *r)
{
	q->st.m->pl->did_dump_vars = false;
	q->st.curr_cell = r->cells;
	q->st.sp = r->nbr_vars;
	q->st.fp = 1;
	q->abort = false;
	q->cycle_error = false;

	frame *g = q->frames + q->st.curr_frame;
	g->nbr_vars = r->nbr_vars;
	g->nbr_slots = r->nbr_vars;
	g->ugen = ++q->st.m->pl->ugen;
	pl_status ret = start(q);
	m_done(q->st.iter);
	return ret;
}

void destroy_query(query *q)
{
	purge_dirty_list(q);

	while (q->st.qnbr > 0) {
		free(q->tmpq[q->st.qnbr]);
		q->tmpq[q->st.qnbr] = NULL;
		q->st.qnbr--;
	}

	for (arena *a = q->arenas; a;) {
		for (idx_t i = 0; i < a->max_hp_used; i++) {
			cell *c = a->heap + i;
			unshare_cell(c);
			c->tag = TAG_EMPTY;
		}

		arena *save = a;
		a = a->next;
		free(save->heap);
		free(save);
	}

	for (int i = 0; i < MAX_QUEUES; i++) {
		for (idx_t j = 0; j < q->qp[i]; j++) {
			cell *c = q->queue[i]+j;
			unshare_cell(c);
			c->tag = TAG_EMPTY;
		}

		free(q->queue[i]);
	}

	slot *e = q->slots;

	for (idx_t i = 0; i < q->st.sp; i++, e++)
		unshare_cell(&e->c);

	mp_int_clear(&q->tmp_ival);
	free(q->trails);
	free(q->choices);
	free(q->slots);
	free(q->frames);
	free(q->tmp_heap);
	free(q);
}

query *create_query(module *m, bool is_task)
{
	static atomic_t uint64_t g_query_id = 0;

	query *q = calloc(1, sizeof(query));
	ensure(q);
	q->qid = g_query_id++;
	q->pl = m->pl;
	q->st.m = m;
	q->trace = m->pl->trace;
	q->flag = m->flag;
	q->time_started = q->get_started = get_time_in_usec();
	q->cpu_last_started = q->cpu_started = cpu_time_in_usec();
	mp_int_init(&q->tmp_ival);

	// Allocate these now...

	q->frames_size = is_task ? INITIAL_NBR_GOALS/10 : INITIAL_NBR_GOALS;
	q->slots_size = is_task ? INITIAL_NBR_SLOTS/10 : INITIAL_NBR_SLOTS;
	q->choices_size = is_task ? INITIAL_NBR_CHOICES/10 : INITIAL_NBR_CHOICES;
	q->trails_size = is_task ? INITIAL_NBR_TRAILS/10 : INITIAL_NBR_TRAILS;

	bool error = false;
	CHECK_SENTINEL(q->frames = calloc(q->frames_size, sizeof(frame)), NULL);
	CHECK_SENTINEL(q->slots = calloc(q->slots_size, sizeof(slot)), NULL);
	CHECK_SENTINEL(q->choices = calloc(q->choices_size, sizeof(choice)), NULL);
	CHECK_SENTINEL(q->trails = calloc(q->trails_size, sizeof(trail)), NULL);

	// Allocate these later as needed...

	q->h_size = is_task ? INITIAL_NBR_HEAP/10 : INITIAL_NBR_HEAP;
	q->tmph_size = is_task ? INITIAL_NBR_CELLS/10 : INITIAL_NBR_CELLS;

	for (int i = 0; i < MAX_QUEUES; i++)
		q->q_size[i] = is_task ? INITIAL_NBR_QUEUE/10 : INITIAL_NBR_QUEUE;

	if (error) {
		destroy_query (q);
		q = NULL;
	}

	return q;
}

query *create_sub_query(query *q, cell *curr_cell)
{
	query *subq = create_query(q->st.m, true);
	if (!subq) return NULL;
	subq->parent = q;
	subq->st.fp = 1;
	subq->is_task = true;

	cell *tmp = clone_to_heap(subq, 0, curr_cell, 1); //cehteh: checkme
	idx_t nbr_cells = tmp->nbr_cells;
	make_end(tmp+nbr_cells);
	subq->st.curr_cell = tmp;

	frame *gsrc = GET_FRAME(q->st.curr_frame);
	frame *gdst = subq->frames;
	gdst->nbr_vars = gsrc->nbr_vars;
	slot *e = GET_SLOT(gsrc, 0);

	for (unsigned i = 0; i < gsrc->nbr_vars; i++, e++) {
		cell *c = deref(q, &e->c, e->ctx);
		cell tmp = (cell){0};
		tmp.tag = TAG_VARIABLE;
		tmp.var_nbr = i;
		tmp.val_off = g_anon_s;
		set_var(subq, &tmp, 0, c, q->latest_ctx);
	}

	subq->st.sp = gsrc->nbr_vars;
	return subq;
}

