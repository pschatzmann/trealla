#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "internal.h"
#include "query.h"
#include "heap.h"

static int accum_slot(const query *q, pl_idx_t slot_nbr, unsigned var_nbr)
{
	const void *v;

	if (m_get(q->vars, (void*)(size_t)slot_nbr, &v))
		return (unsigned)(size_t)v;

	m_set(q->vars, (void*)(size_t)slot_nbr, (void*)(size_t)var_nbr);
	return -1;
}

size_t alloc_grow(void **addr, size_t elem_size, size_t min_elements, size_t max_elements)
{
	assert(min_elements <= max_elements);
	size_t elements = max_elements;
	void *mem;

	do {
		mem = realloc(*addr, elem_size * elements);
		if (mem) break;
		elements = min_elements + (elements-min_elements)/2;
		//message("memory pressure reduce %lu to %lu", max_elements, elements);
	}
	 while (elements > min_elements);

	if (!mem)
		return 0;

	*addr = mem;
	return elements;
}

// The tmp heap is used for temporary allocations (a scratch-pad)
// for work in progress. As such it can survive a realloc() call.

cell *init_tmp_heap(query *q)
{
	if (!q->tmp_heap) {
		q->tmp_heap = malloc(q->tmph_size * sizeof(cell));
		if (!q->tmp_heap) return NULL;
		*q->tmp_heap = (cell){0};
	}

	q->tmphp = 0;
	return q->tmp_heap;
}

cell *alloc_on_tmp(query *q, pl_idx_t nbr_cells)
{
	if (((uint64_t)q->tmphp + nbr_cells) > UINT32_MAX)
		return NULL;

	pl_idx_t new_size = q->tmphp + nbr_cells;

	while (new_size >= q->tmph_size) {
		size_t elements = alloc_grow((void**)&q->tmp_heap, sizeof(cell), new_size, (new_size*3)/2);
		if (!elements) return NULL;
		q->tmph_size = elements;
	}

	cell *c = q->tmp_heap + q->tmphp;
	q->tmphp = new_size;
	return c;
}

// The heap is used for long-life allocations and a realloc() can't be
// done as it will invalidate existing pointers. Build any compounds
// first on the tmp heap, then allocate in one go here and copy in.
// When more space is need allocate a new page and keep them in the
// page list. Backtracking will garbage collect and free as needed.

cell *alloc_on_heap(query *q, pl_idx_t nbr_cells)
{
	if (((uint64_t)q->st.hp + nbr_cells) > UINT32_MAX)
		return NULL;

	if (!q->pages) {
		page *a = calloc(1, sizeof(page));
		if (!a) return NULL;
		a->next = q->pages;
		unsigned n = MAX_OF(q->h_size, nbr_cells);
		a->heap = calloc(a->h_size=n, sizeof(cell));
		if (!a->heap) { free(a); return NULL; }
		a->nbr = q->st.curr_page++;
		q->pages = a;
	}

	if ((q->st.hp + nbr_cells) >= q->pages->h_size) {
		page *a = calloc(1, sizeof(page));
		if (!a) return NULL;
		a->next = q->pages;
		unsigned n = MAX_OF(q->h_size, nbr_cells);
		a->heap = calloc(a->h_size=n, sizeof(cell));
		if (!a->heap) { free(a); return NULL; }
		a->nbr = q->st.curr_page++;
		q->pages = a;
		q->st.hp = 0;
	}

	cell *c = q->pages->heap + q->st.hp;
	q->st.hp += nbr_cells;
	q->pages->hp = q->st.hp;

	if (q->pages->hp > q->pages->max_hp_used)
		q->pages->max_hp_used = q->pages->hp;

	return c;
}

bool is_in_ref_list(cell *c, pl_idx_t c_ctx, reflist *rlist)
{
	while (rlist) {
		if ((c->var_nbr == rlist->var_nbr)
			&& (c_ctx == rlist->ctx))
			return true;

		rlist = rlist->next;
	}

	return false;
}

static bool is_in_ref_list2(cell *c, pl_idx_t c_ctx, reflist *rlist)
{
	while (rlist) {
		if ((c == rlist->ptr)
			&& (c_ctx == rlist->ctx))
			return true;

		rlist = rlist->next;
	}

	return false;
}

// FIXME: rewrite this using efficient sweep/mark methodology...

static cell *deep_copy2_to_tmp(query *q, cell *p1, pl_idx_t p1_ctx, bool copy_attrs, unsigned depth, reflist *list)
{
	if (depth >= MAX_DEPTH) {
		printf("*** OOPS %s %d\n", __FILE__, __LINE__);
		q->cycle_error = true;
		return NULL;
	}

	const pl_idx_t save_idx = tmp_heap_used(q);
	cell *save_p1 = p1;
	p1 = deref(q, p1, p1_ctx);
	p1_ctx = q->latest_ctx;

	cell *tmp = alloc_on_tmp(q, 1);
	if (!tmp) return NULL;
	copy_cells(tmp, p1, 1);

	if (!is_structure(p1)) {
		if (!is_variable(p1))
			return tmp;

		const frame *f = GET_FRAME(p1_ctx);
		const slot *e = GET_SLOT(f, p1->var_nbr);
		const pl_idx_t slot_nbr = e - q->slots;
		int var_nbr;

		if ((var_nbr = accum_slot(q, slot_nbr, q->varno)) == -1)
			var_nbr = q->varno;

		if (!q->tab_idx) {
			q->tab0_varno = var_nbr;
			q->tab_idx++;
		}

		q->varno++;
		tmp->var_nbr = var_nbr;
		tmp->flags = FLAG_VAR_FRESH;

		if (copy_attrs) {
			tmp->tmp_attrs = e->c.attrs;
			tmp->tmp_ctx = e->c.attrs_ctx;
		}

		if (is_anon(p1))
			tmp->flags |= FLAG_VAR_ANON;

		return tmp;
	}

	pl_idx_t save_p1_ctx = p1_ctx;
	bool cyclic = false;

	if (is_iso_list(p1)) {
		LIST_HANDLER(p1);

		while (is_iso_list(p1)) {
			if (g_tpl_interrupt) {
				if (check_interrupt(q))
					break;
			}

			cell *h = LIST_HEAD(p1);
			cell *c = h;
			pl_idx_t c_ctx = p1_ctx;
			c = deref(q, c, c_ctx);
			c_ctx = q->latest_ctx;

			if (is_in_ref_list2(c, c_ctx, list)) {
				cell *tmp = alloc_on_tmp(q, 1);
				if (!tmp) return NULL;
				*tmp = *h;
				tmp->var_nbr = q->tab0_varno;
				tmp->flags |= FLAG_VAR_FRESH;
				tmp->tmp_attrs = NULL;
			} else {
				reflist nlist = {0};
				nlist.next = list;
				nlist.ptr = save_p1;
				nlist.ctx = save_p1_ctx;
				cell *rec = deep_copy2_to_tmp(q, c, c_ctx, copy_attrs, depth+1, &nlist);
				if (!rec) return rec;
			}

			p1 = LIST_TAIL(p1);
			p1 = deref(q, p1, p1_ctx);
			p1_ctx = q->latest_ctx;

			reflist nlist = {0};
			nlist.next = list;
			nlist.ptr = save_p1;
			nlist.ctx = save_p1_ctx;

			if (is_in_ref_list2(p1, p1_ctx, &nlist)) {
				cell *tmp = alloc_on_tmp(q, 1);
				if (!tmp) return NULL;
				tmp->tag = TAG_VAR;
				tmp->flags = 0;
				tmp->nbr_cells = 1;
				tmp->val_off = g_anon_s;
				tmp->var_nbr = q->tab0_varno;
				tmp->flags |= FLAG_VAR_FRESH;
				tmp->tmp_attrs = NULL;
				cyclic = true;
				break;
			}

			if (is_iso_list(p1)) {
				cell *tmp = alloc_on_tmp(q, 1);
				if (!tmp) return NULL;
				copy_cells(tmp, p1, 1);
			}
		}

		if (!cyclic) {
			cell *rec = deep_copy2_to_tmp(q, p1, p1_ctx, copy_attrs, depth+1, list);
			if (!rec) return rec;
		}

		tmp = get_tmp_heap(q, save_idx);
		tmp->nbr_cells = tmp_heap_used(q) - save_idx;
		fix_list(tmp);
	} else {
		unsigned arity = p1->arity;
		p1++;

		while (arity--) {
			cell *c = p1;
			pl_idx_t c_ctx = p1_ctx;
			c = deref(q, c, c_ctx);
			c_ctx = q->latest_ctx;
			reflist nlist = {0};

			if (is_in_ref_list2(c, c_ctx, list)) {
				cell *tmp = alloc_on_tmp(q, 1);
				if (!tmp) return NULL;
				*tmp = *p1;
				tmp->var_nbr = q->tab0_varno;
				tmp->flags |= FLAG_VAR_FRESH;
				tmp->tmp_attrs = NULL;
			} else {
				nlist.next = list;
				nlist.ptr = save_p1;
				nlist.ctx = save_p1_ctx;

				cell *rec = deep_copy2_to_tmp(q, c, c_ctx, copy_attrs, depth+1, !q->lists_ok ? &nlist : NULL);
				if (!rec) return rec;
			}

			p1 += p1->nbr_cells;
		}

		tmp = get_tmp_heap(q, save_idx);
		tmp->nbr_cells = tmp_heap_used(q) - save_idx;
	}

	return tmp;
}

cell *deep_raw_copy_to_tmp(query *q, cell *p1, pl_idx_t p1_ctx)
{
	if (!init_tmp_heap(q))
		return NULL;

	frame *f = GET_CURR_FRAME();
	q->varno = f->nbr_vars;
	q->tab_idx = 0;
	q->cycle_error = false;
	reflist nlist = {0};
	nlist.ptr = p1;
	nlist.ctx = p1_ctx;
	ensure(q->vars = m_create(NULL, NULL, NULL));
	cell *rec = deep_copy2_to_tmp(q, p1, p1_ctx, false, 0, &nlist);
	m_destroy(q->vars);
	q->vars = NULL;
	if (!rec) return rec;
	return q->tmp_heap;
}

static cell *deep_copy_to_tmp_with_replacement(query *q, cell *p1, pl_idx_t p1_ctx, bool copy_attrs, cell *from, pl_idx_t from_ctx, cell *to, pl_idx_t to_ctx)
{
	if (!init_tmp_heap(q))
		return NULL;

	cell *save_p1 = p1;
	pl_idx_t save_p1_ctx = p1_ctx;
	cell *c = deref(q, p1, p1_ctx);
	pl_idx_t c_ctx = q->latest_ctx;

	if (is_iso_list(c)) {
		bool is_partial;

		if (check_list(q, c, c_ctx, &is_partial, NULL))
			q->lists_ok = true;
		else
			q->lists_ok = false;
	} else
		q->lists_ok = false;

	frame *f = GET_CURR_FRAME();
	q->varno = f->nbr_vars;
	q->tab_idx = 0;
	ensure(q->vars = m_create(NULL, NULL, NULL));
	q->cycle_error = false;
	int nbr_vars = f->nbr_vars;

	if (from && to) {
		f = GET_FRAME(from_ctx);
		const slot *e = GET_SLOT(f, from->var_nbr);
		const pl_idx_t slot_nbr = e - q->slots;

		if (!q->tab_idx) {
			q->tab0_varno = to->var_nbr;
			q->tab_idx++;
		}

		m_set(q->vars, (void*)(size_t)slot_nbr, (void*)(size_t)to->var_nbr);
	}

	if (is_variable(save_p1)) {
		const frame *f = GET_FRAME(p1_ctx);
		const slot *e = GET_SLOT(f, p1->var_nbr);
		const pl_idx_t slot_nbr = e - q->slots;

		if (!q->tab_idx) {
			q->tab0_varno = q->varno;
			q->tab_idx++;
		}

		m_set(q->vars, (void*)(size_t)slot_nbr, (void*)(size_t)q->varno);
		q->varno++;
	}

	if (is_variable(p1)) {
		p1 = deref(q, p1, p1_ctx);
		p1_ctx = q->latest_ctx;
	}

	reflist nlist = {0};
	nlist.ptr = c;
	nlist.ctx = c_ctx;
	cell *rec = deep_copy2_to_tmp(q, c, c_ctx, copy_attrs, 0, !q->lists_ok ? &nlist : NULL);
	q->lists_ok = false;
	m_destroy(q->vars);
	q->vars = NULL;
	if (!rec) return rec;
	int cnt = q->varno - nbr_vars;

	if (cnt) {
		if (!create_vars(q, cnt)) {
			DISCARD_RESULT throw_error(q, p1, p1_ctx, "resource_error", "stack");
			return NULL;
		}
	}

	if (is_variable(save_p1)) {
		cell tmp;
		tmp = *save_p1;
		tmp.var_nbr = q->tab0_varno;
		unify(q, &tmp, q->st.curr_frame, rec, q->st.curr_frame);
	}

	if (!copy_attrs)
		return get_tmp_heap_start(q);

	c = get_tmp_heap_start(q);

	for (pl_idx_t i = 0; i < rec->nbr_cells; i++, c++) {
		if (is_variable(c) && is_fresh(c) && c->tmp_attrs) {
			slot *e = GET_SLOT(f, c->var_nbr);
			e->c.attrs = c->tmp_attrs;
			e->c.attrs_ctx = c->tmp_ctx;
		}
	}

	return get_tmp_heap_start(q);
}

cell *deep_copy_to_tmp(query *q, cell *p1, pl_idx_t p1_ctx, bool copy_attrs)
{
	return deep_copy_to_tmp_with_replacement(q, p1, p1_ctx, copy_attrs, NULL, 0, NULL, 0);
}

cell *deep_copy_to_heap(query *q, cell *p1, pl_idx_t p1_ctx, bool copy_attrs)
{
	cell *tmp = deep_copy_to_tmp(q, p1, p1_ctx, copy_attrs);
	if (!tmp) return tmp;

	cell *tmp2 = alloc_on_heap(q, tmp->nbr_cells);
	if (!tmp2) return NULL;
	safe_copy_cells(tmp2, tmp, tmp->nbr_cells);
	return tmp2;
}

cell *deep_copy_to_heap_with_replacement(query *q, cell *p1, pl_idx_t p1_ctx, bool copy_attrs, cell *from, pl_idx_t from_ctx, cell *to, pl_idx_t to_ctx)
{
	cell *tmp = deep_copy_to_tmp_with_replacement(q, p1, p1_ctx, copy_attrs, from, from_ctx, to, to_ctx);
	if (!tmp) return NULL;
	cell *tmp2 = alloc_on_heap(q, tmp->nbr_cells);
	if (!tmp2) return NULL;
	safe_copy_cells(tmp2, tmp, tmp->nbr_cells);
	return tmp2;
}

static cell *deep_clone2_to_tmp(query *q, cell *p1, pl_idx_t p1_ctx, unsigned depth, reflist *list)
{
	if (depth >= MAX_DEPTH) {
		printf("*** OOPS %s %d\n", __FILE__, __LINE__);
		q->cycle_error = true;
		return NULL;
	}

	pl_idx_t save_idx = tmp_heap_used(q);
	cell *save_p1 = p1;
	pl_idx_t save_p1_ctx = p1_ctx;
	p1 = deref(q, p1, p1_ctx);
	p1_ctx = q->latest_ctx;
	cell *tmp = alloc_on_tmp(q, 1);
	if (!tmp) return NULL;
	copy_cells(tmp, p1, 1);

	if (is_variable(tmp) && !is_ref(tmp)) {
		tmp->flags |= FLAG_VAR_REF;
		tmp->ref_ctx = p1_ctx;
	}

	if (!is_structure(p1))
		return tmp;

	bool cyclic = false;

	if (is_iso_list(p1)) {
		LIST_HANDLER(p1);

		while (is_iso_list(p1)) {
			if (g_tpl_interrupt) {
				if (check_interrupt(q))
					break;
			}

			cell *h = LIST_HEAD(p1);
			cell *c = deref(q, h, p1_ctx);
			pl_idx_t c_ctx = q->latest_ctx;

			if (is_in_ref_list2(c, c_ctx, list)) {
				cell *tmp = alloc_on_tmp(q, 1);
				if (!tmp) return NULL;
				*tmp = *h;
			} else {
				reflist nlist = {0};
				nlist.next = list;
				nlist.ptr = save_p1;
				nlist.ctx = save_p1_ctx;
				cell *rec = deep_clone2_to_tmp(q, c, c_ctx, depth+1, &nlist);
				if (!rec) return rec;
			}

			p1 = LIST_TAIL(p1);
			p1 = deref(q, p1, p1_ctx);
			p1_ctx = q->latest_ctx;

			reflist nlist = {0};
			nlist.next = list;
			nlist.ptr = save_p1;
			nlist.ctx = save_p1_ctx;

			if (is_in_ref_list2(p1, p1_ctx, &nlist)) {
				cell *tmp = alloc_on_tmp(q, 1);
				if (!tmp) return NULL;
				*tmp = *save_p1;
				cyclic = true;
				break;
			}

			if (is_iso_list(p1)) {
				cell *tmp = alloc_on_tmp(q, 1);
				if (!tmp) return NULL;
				copy_cells(tmp, p1, 1);
			}
		}

		if (!cyclic) {
			cell *rec = deep_clone2_to_tmp(q, p1, p1_ctx, depth+1, list);
			if (!rec) return rec;
		}

		tmp = get_tmp_heap(q, save_idx);
		tmp->nbr_cells = tmp_heap_used(q) - save_idx;
		return tmp;
	}

	unsigned arity = p1->arity;
	p1++;

	while (arity--) {
		cell *c = deref(q, p1, p1_ctx);
		pl_idx_t c_ctx = q->latest_ctx;
		reflist nlist = {0};

		if (is_in_ref_list2(c, c_ctx, list)) {
			cell *tmp = alloc_on_tmp(q, 1);
			if (!tmp) return NULL;
			*tmp = *p1;
		} else {
			nlist.next = list;
			nlist.ptr = save_p1;
			nlist.ctx = save_p1_ctx;

			cell *rec = deep_clone2_to_tmp(q, c, c_ctx, depth+1, &nlist);
			if (!rec) return rec;
		}

		p1 += p1->nbr_cells;
	}

	tmp = get_tmp_heap(q, save_idx);
	tmp->nbr_cells = tmp_heap_used(q) - save_idx;
	return tmp;
}

cell *deep_clone_to_tmp(query *q, cell *p1, pl_idx_t p1_ctx)
{
	if (!init_tmp_heap(q))
		return NULL;

	q->cycle_error = false;
	reflist nlist = {0};
	nlist.ptr = p1;
	nlist.ctx = p1_ctx;

	cell *rec = deep_clone2_to_tmp(q, p1, p1_ctx, 0, &nlist);
	if (!rec) return rec;
	return q->tmp_heap;
}

cell *deep_clone_to_heap(query *q, cell *p1, pl_idx_t p1_ctx)
{
	p1 = deep_clone_to_tmp(q, p1, p1_ctx);
	if (!p1) return p1;
	cell *tmp = alloc_on_heap(q, p1->nbr_cells);
	if (!tmp) return NULL;
	safe_copy_cells(tmp, p1, p1->nbr_cells);
	return tmp;
}

cell *clone2_to_tmp(query *q, cell *p1)
{
	cell *tmp = alloc_on_tmp(q, p1->nbr_cells);
	if (!tmp) return NULL;
	cell *src = p1, *dst = tmp;

	for (pl_idx_t i = 0; i < p1->nbr_cells; i++, dst++, src++) {
		*dst = *src;

		if (!is_variable(src) || is_ref(src))
			continue;

		dst->flags |= FLAG_VAR_REF;
		dst->ref_ctx = q->st.curr_frame;
	}

	return tmp;
}

cell *clone_to_tmp(query *q, cell *p1)
{
	if (!init_tmp_heap(q)) return NULL;
	return clone2_to_tmp(q, p1);
}

cell *clone_to_heap(query *q, bool prefix, cell *p1, pl_idx_t suffix)
{
	cell *tmp = alloc_on_heap(q, (prefix?1:0)+p1->nbr_cells+suffix);
	if (!tmp) return NULL;
	frame *f = GET_CURR_FRAME();

	if (prefix) {
		// Needed for follow() to work
		*tmp = (cell){0};
		tmp->tag = TAG_EMPTY;
		tmp->nbr_cells = 1;
		tmp->flags = FLAG_BUILTIN;
	}

	cell *src = p1, *dst = tmp+(prefix?1:0);

	for (pl_idx_t i = 0; i < p1->nbr_cells; i++, dst++, src++) {
		*dst = *src;
		share_cell(src);

		if (!is_variable(src) || is_ref(src))
			continue;

		dst->flags |= FLAG_VAR_REF;
		dst->ref_ctx = q->st.curr_frame;
	}

	return tmp;
}

cell *alloc_on_queuen(query *q, int qnbr, const cell *c)
{
	if (!q->queue[qnbr]) {
		q->queue[qnbr] = calloc(q->q_size[qnbr], sizeof(cell));
		ensure(q->queue[qnbr]);
	}

	while ((q->qp[qnbr]+c->nbr_cells) >= q->q_size[qnbr]) {
		q->q_size[qnbr] += q->q_size[qnbr] / 2;
		q->queue[qnbr] = realloc(q->queue[qnbr], sizeof(cell)*q->q_size[qnbr]);
		ensure(q->queue[qnbr]);
	}

	cell *dst = q->queue[qnbr] + q->qp[qnbr];
	q->qp[qnbr] += safe_copy_cells(dst, c, c->nbr_cells);
	return dst;
}

void fix_list(cell *c)
{
	pl_idx_t cnt = c->nbr_cells;

	while (is_iso_list(c)) {
		c->nbr_cells = cnt;
		c = c + 1;					// skip .
		cnt -= 1 + c->nbr_cells;
		c = c + c->nbr_cells;		// skip head
	}
}

// Defer check until end_list()

void allocate_list(query *q, const cell *c)
{
	if (!init_tmp_heap(q)) return;
	append_list(q, c);
}

// Defer check until end_list()

void append_list(query *q, const cell *c)
{
	cell *tmp = alloc_on_tmp(q, 1+c->nbr_cells);
	if (!tmp) return;
	tmp->tag = TAG_LITERAL;
	tmp->nbr_cells = 1 + c->nbr_cells;
	tmp->val_off = g_dot_s;
	tmp->arity = 2;
	tmp->flags = 0;
	tmp++;
	copy_cells(tmp, c, c->nbr_cells);
}

USE_RESULT cell *end_list(query *q)
{
	cell *tmp = alloc_on_tmp(q, 1);
	if (!tmp) return NULL;
	tmp->tag = TAG_LITERAL;
	tmp->nbr_cells = 1;
	tmp->val_off = g_nil_s;
	tmp->arity = tmp->flags = 0;
	pl_idx_t nbr_cells = tmp_heap_used(q);

	tmp = alloc_on_heap(q, nbr_cells);
	if (!tmp) return NULL;
	safe_copy_cells(tmp, get_tmp_heap(q, 0), nbr_cells);
	tmp->nbr_cells = nbr_cells;
	fix_list(tmp);
	return tmp;
}

bool search_tmp_list(query *q, cell *v)
{
	cell *tmp = get_tmp_heap(q, 0);
	pl_idx_t nbr_cells = tmp_heap_used(q);


	if (!tmp || !tmp_heap_used(q))
		return false;

	for (pl_idx_t i = 0; i < nbr_cells; i++, tmp++) {
		if (tmp->var_nbr == v->var_nbr)
			return true;
	}

	return false;
}

