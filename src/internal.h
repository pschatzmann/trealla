#pragma once

#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <assert.h>
#include <unistd.h>

#ifndef USE_OPENSSL
#define USE_OPENSSL 0
#endif

#ifndef USE_THREADS
#define USE_THREADS 0
#endif

typedef intmax_t pl_int_t;
typedef uintmax_t pl_uint_t;
typedef uint32_t pl_idx_t;

#define PL_INT_MAX INTMAX_MAX
#define PL_INT_MIN INTMAX_MIN

#if (__STDC_VERSION__ >= 201112L) && USE_THREADS
#include <stdatomic.h>
#define atomic_t _Atomic
#else
#define atomic_t volatile
#endif

#ifdef _WIN32
#define PATH_SEP_CHAR '\\'
#define NEWLINE_MODE "dos"
#else
#define PATH_SEP_CHAR '/'
#define NEWLINE_MODE "posix"
#endif

#include "map.h"
#include "trealla.h"
#include "cdebug.h"
#include "imath/imath.h"

#ifdef _WIN32
char *realpath(const char *path, char resolved_path[PATH_MAX]);
#endif

static const unsigned INITIAL_NBR_CELLS = 100;		// cells

typedef enum {
	pl_halt    =  0,
	pl_abort   =  0,
	pl_yield   =  0,
	pl_cycle   =  0,
	pl_error   =  0,
	pl_failure =  0,
	pl_success =  1,
} pl_status;

extern unsigned g_string_cnt, g_literal_cnt;

// Sentinel Value
#define ERR_IDX (~(pl_idx_t)0)
#define IDX_MAX (ERR_IDX-1)
#define ERR_CYCLE_CMP -2

#define MAX_SMALL_STRING ((sizeof(void*)*2)-1)
#define MAX_VAR_POOL_SIZE 4000
#define MAX_ARITY UCHAR_MAX
#define MAX_QUEUES 16
#define MAX_STREAMS 1024
#define MAX_MODULES 1024
//#define MAX_DEPTH 9999
#define MAX_DEPTH 6000			// Clang stack size needs this small
#define MAX_IGNORES 64000

#define STREAM_BUFLEN 1024

#define MAX_OF(a,b) (a) > (b) ? (a) : (b)
#define MIN_OF(a,b) (a) < (a) ? (a) : (b)

#define GET_CHOICE(i) (q->choices+(i))
#define GET_CURR_CHOICE() GET_CHOICE(q->cp?q->cp-1:q->cp)

#define GET_FRAME(i) (q->frames+(i))
#define GET_FIRST_FRAME() GET_FRAME(0)
#define GET_CURR_FRAME() GET_FRAME(q->st.curr_frame)

#define GET_SLOT(f,i) ((i) < (f)->nbr_slots ? 			\
	(q->slots+(f)->base_slot_nbr+(i)) : 				\
	(q->slots+(f)->overflow+((i)-(f)->nbr_slots)) 		\
	)

#define GET_FIRST_SLOT(f) GET_SLOT(f, 0)

// Primary type...

#define is_empty(c) ((c)->tag == TAG_EMPTY)
#define is_variable(c) ((c)->tag == TAG_VAR)
#define is_literal(c) ((c)->tag == TAG_LITERAL)
#define is_cstring(c) ((c)->tag == TAG_CSTR)
#define is_integer(c) ((c)->tag == TAG_INT)
#define is_real(c) ((c)->tag == TAG_REAL)
#define is_indirect(c) ((c)->tag == TAG_PTR)
#define is_end(c) ((c)->tag == TAG_END)

// Derived type...

#define is_iso_atom(c) ((is_literal(c) || is_cstring(c)) && !(c)->arity)
#define is_iso_list(c) (is_literal(c) && ((c)->arity == 2) && ((c)->val_off == g_dot_s))

#define get_real(c) (c)->val_real
#define set_real(c,v) (c)->val_real = (v)
#define get_smallint(c) (c)->val_int
#define set_smallint(c,v) { (c)->val_int = (v); }
#define get_int(c) (c)->val_int

#define neg_bigint(c) (c)->val_bigint->ival.sign = MP_NEG;
#define neg_smallint(c) (c)->val_int = -llabs((c)->val_int)
#define neg_real(c) (c)->val_real = -fabs((c)->val_real)

#define is_zero(c) (is_bigint(c) ?							\
	mp_int_compare_zero(&(c)->val_bigint->ival) == 0 :		\
	is_integer(c) ? get_smallint(c) == 0 :					\
	is_real(c) ? get_real(c) == 0.0 : false)

#define is_negative(c) (is_bigint(c) ?						\
	(c)->val_bigint->ival.sign == MP_NEG :					\
	is_integer(c) ? get_smallint(c) < 0 :					\
	is_real(c) ? get_real(c) < 0.0 : false)

#define is_positive(c) (is_bigint(c) ?						\
	mp_int_compare_zero(&(c)->val_bigint->ival) > 0 :		\
	is_integer(c) ? get_smallint(c) > 0 :					\
	is_real(c) ? get_real(c) > 0.0 : false)

#define is_gt(c,n) (get_smallint(c) > (n))
#define is_ge(c,n) (get_smallint(c) >= (n))
#define is_eq(c,n) (get_smallint(c) == (n))
#define is_ne(c,n) (get_smallint(c) != (n))
#define is_le(c,n) (get_smallint(c) <= (n))
#define is_lt(c,n) (get_smallint(c) < (n))

#define is_smallint(c) (is_integer(c) && !((c)->flags & FLAG_MANAGED))
#define is_bigint(c) (is_integer(c) && ((c)->flags & FLAG_MANAGED))
#define is_atom(c) ((is_literal(c) && !(c)->arity) || is_cstring(c))
#define is_string(c) (is_cstring(c) && ((c)->flags & FLAG_CSTR_STRING))
#define is_managed(c) ((c)->flags & FLAG_MANAGED)
#define is_blob(c) (is_cstring(c) && ((c)->flags & FLAG_CSTR_BLOB))
#define is_list(c) (is_iso_list(c) || is_string(c))
#define is_static(c) (is_blob(c) && ((c)->flags & FLAG_STATIC))
#define is_strbuf(c) (is_blob(c) && !((c)->flags & FLAG_STATIC))
#define is_nil(c) (is_literal(c) && !(c)->arity && ((c)->val_off == g_nil_s))
#define is_quoted(c) ((c)->flags & FLAG_CSTR_QUOTED)
#define is_fresh(c) ((c)->flags & FLAG_VAR_FRESH)
#define is_anon(c) ((c)->flags & FLAG_VAR_ANON)
#define is_builtin(c) ((c)->flags & FLAG_BUILTIN)
#define is_function(c) ((c)->flags & FLAG_FUNCTION)
#define is_tail_recursive(c) ((c)->flags & FLAG_TAIL_REC)
#define is_temporary(c) ((c)->flags & FLAG_VAR_TEMPORARY)
#define is_ref(c) ((c)->flags & FLAG_VAR_REF)
#define is_op(c) (c->flags & 0xE000)

typedef struct {
	int64_t refcnt;
	size_t len;
	char cstr[];
} strbuf;

typedef struct {
	int64_t refcnt;
	mpz_t ival;
} bigint;

#define SET_STR(c,s,n,off) {									\
	strbuf *strb = malloc(sizeof(strbuf) + (n) + 1);			\
	may_ptr_error(strb);										\
	memcpy(strb->cstr, s, n); 									\
	strb->cstr[n] = 0;											\
	strb->len = n;												\
	strb->refcnt = 1;											\
	g_string_cnt++;												\
	(c)->val_strb = strb;										\
	(c)->strb_off = off;										\
	(c)->strb_len = n;											\
	(c)->flags |= FLAG_MANAGED | FLAG_CSTR_BLOB;				\
	}

#define _GET_STR(pl,c) 											\
	( !is_cstring(c) ? ((pl)->pool + (c)->val_off)				\
	: is_strbuf(c) ? ((c)->val_strb->cstr + (c)->strb_off)		\
	: is_static(c) ? (c)->val_str								\
	: (char*)(c)->val_chr										\
	)

#define _LEN_STR(pl,c) 											\
	( !is_cstring(c) ? strlen((pl)->pool + (c)->val_off)		\
	: is_strbuf(c) ? (c)->strb_len								\
	: is_static(c) ? (c)->str_len								\
	: (c)->chr_len												\
	)

#define _CMP_SLICE(pl,c,str,len) slicecmp(_GET_STR(pl, c), _LEN_STR(pl, c), str, len)
#define _CMP_SLICE2(pl,c,str) slicecmp2(_GET_STR(pl, c), _LEN_STR(pl, c), str)
#define _CMP_SLICES(pl,c1,c2) slicecmp(_GET_STR(pl, c1), _LEN_STR(pl, c1), _GET_STR(pl, c2), _LEN_STR(pl, c2))
#define _DUP_SLICE(pl,c) slicedup(_GET_STR(pl, c), _LEN_STR(pl, c))

#define LEN_STR_UTF8(c) substrlen_utf8(GET_STR(q, c), LEN_STR(q, c))
#define GET_STR(x,c) _GET_STR((x)->pl, c)
#define LEN_STR(x,c) _LEN_STR((x)->pl, c)
#define GET_POOL(x,off) ((x)->pl->pool + (off))
#define CMP_SLICE(x,c,str,len) _CMP_SLICE((x)->pl, c, str, len)
#define CMP_SLICE2(x,c,str) _CMP_SLICE2((x)->pl, c, str)
#define CMP_SLICES(x,c1,c2) _CMP_SLICES((x)->pl, c1, c2)
#define DUP_SLICE(x,c) _DUP_SLICE((x)->pl, c)

// If changing the order of these: see unify.c dispatch table

enum {
	TAG_EMPTY=0,
	TAG_VAR=1,
	TAG_LITERAL=2,
	TAG_CSTR=3,
	TAG_INT=4,
	TAG_REAL=5,
	TAG_PTR=6,
	TAG_END=7
};

enum {
	FLAG_INT_HEX=1<<0,					// used with TAG_INT
	FLAG_INT_OCTAL=1<<1,				// used with TAG_INT
	FLAG_INT_BINARY=1<<2,				// used with TAG_INT
	FLAG_INT_STREAM=1<<3,				// used with TAG_INT

	FLAG_CSTR_BLOB=1<<0,				// used with TAG_CSTR
	FLAG_CSTR_STRING=1<<1,				// used with TAG_CSTR
	FLAG_CSTR_QUOTED=1<<2,				// used with TAG_CSTR

	FLAG_VAR_FIRST_USE=1<<0,			// used with TAG_VAR
	FLAG_VAR_ANON=1<<1,					// used with TAG_VAR
	FLAG_VAR_FRESH=1<<2,				// used with TAG_VAR
	FLAG_VAR_TEMPORARY=1<<3,			// used with TAG_VAR
	FLAG_VAR_REF=1<<4,					// used with TAG_VAR

	FLAG_SPARE1=1<<6,
	FLAG_SPARE2=1<<7,
	FLAG_BUILTIN=1<<8,
	FLAG_STATIC=1<<9,
	FLAG_MANAGED=1<<10,					// any reflist-counted object
	FLAG_TAIL_REC=1<<11,
	FLAG_FUNCTION=1<<12,

	FLAG_END=1<<13
};

// The OP types are stored in the high 3 bits of the flag (13-15)

#define	OP_FX 1
#define	OP_FY 2
#define	OP_XF 3
#define	OP_YF 4
#define	OP_YFX 5
#define	OP_XFX 6
#define	OP_XFY 7

#define IS_PREFIX(op) (((op) == OP_FX) || ((op) == OP_FY))
#define IS_POSTFIX(op) (((op) == OP_XF) || ((op) == OP_YF))
#define IS_INFIX(op) (((op) == OP_XFX) || ((op) == OP_XFY) || ((op) == OP_YFX))

#define is_prefix(c) IS_PREFIX(GET_OP(c))
#define is_postfix(c) IS_POSTFIX(GET_OP(c))
#define is_infix(c) IS_INFIX(GET_OP(c))

#define is_fx(c) (GET_OP(c) == OP_FX)
#define is_fy(c) (GET_OP(c) == OP_FY)
#define is_xf(c) (GET_OP(c) == OP_XF)
#define is_yf(c) (GET_OP(c) == OP_YF)
#define is_yfx(c) (GET_OP(c) == OP_YFX)
#define is_xfx(c) (GET_OP(c) == OP_XFX)
#define is_xfy(c) (GET_OP(c) == OP_XFY)

#define SET_OP(c,op) (CLR_OP(c), (c)->flags |= (((uint16_t)(op)) << 13))
#define CLR_OP(c) ((c)->flags &= ~((uint16_t)(0xF) << 13))
#define GET_OP(c) (((c)->flags >> 13) & 0xF)
#define IS_OP(c) (GET_OP(c) != 0 ? true : false)

typedef struct module_ module;
typedef struct query_ query;
typedef struct predicate_ predicate;
typedef struct db_entry_ db_entry;
typedef struct cell_ cell;
typedef struct clause_ clause;
typedef struct trail_ trail;
typedef struct frame_ frame;
typedef struct parser_ parser;
typedef struct page_ page;
typedef struct stream_ stream;
typedef struct slot_ slot;
typedef struct choice_ choice;
typedef struct prolog_state_ prolog_state;
typedef struct prolog_flags_ prolog_flags;
typedef struct cycle_info_ cycle_info;
typedef struct reflist_ reflist;

// Using a fixed-size cell allows having arrays of cells, which is
// basically what a Term is. A compound is a variable length array of
// cells, the length specified by 'nbr_cells' field in the 1st cell.
// A cell is a tagged union.
// The size should be 24 bytes (oops, now it is 32 bytes)

struct cell_ {
	uint8_t tag;
	uint8_t arity;
	uint16_t flags;
	pl_idx_t nbr_cells;

	union {

		void *val_dummy[3];

		struct {
			pl_int_t val_int;
		};

		struct {
			bigint *val_bigint;
		};

		struct {
			double val_real;
		};

		struct {
			cell *val_ptr;
		};

		struct {
			cell *val_ret;
			uint32_t cgen;				// choice generation
			uint16_t mod_id;
		};

		struct {
			char val_chr[MAX_SMALL_STRING];
			uint8_t	chr_len;
		};

		struct {
			strbuf *val_strb;
			uint32_t strb_off;			// slice offset
			uint32_t strb_len;			// slice length
		};

		struct {
			char *val_str;
			uint64_t str_len;			// slice_length
		};

		struct {
			union {
				pl_status (*fn)(query*);
				predicate *match;
				uint16_t priority;		// used in parsing operators

				struct {
					cell *tmp_attrs;	// used with TAG_VAR in copy_term
					pl_idx_t tmp_ctx;	// used with TAG_VAR in copy_term
					pl_idx_t ref_ctx;	// used with TAG_VAR & refs
				};
			};

			uint32_t val_off;			// used with TAG_VAR & TAG_LITERAL
			uint32_t var_nbr;			// used with TAG_VAR
		};

		struct {
			cell *attrs;				// used with TAG_EMPTY in slot
			pl_idx_t attrs_ctx;			// to set attributes on a var
			uint32_t spare5;
		};
	};
};

typedef struct {
	uint64_t u1, u2;
} uuid;

struct clause_ {
	uint64_t ugen_created, ugen_erased;
	pl_idx_t nbr_cells, cidx;
	uint32_t nbr_vars, nbr_temporaries;
	bool is_first_cut:1;
	bool is_cut_only:1;
	bool arg1_is_unique:1;
	bool arg2_is_unique:1;
	bool arg3_is_unique:1;
	bool is_unique:1;
	bool is_fact:1;
	bool is_complex:1;
	bool is_tail_rec:1;
	cell cells[];
};

struct db_entry_ {
	predicate *owner;
	db_entry *prev, *next, *dirty;
	const char *filename;
	uint64_t db_id;
	uuid u;
	clause cl;
};

struct predicate_ {
	predicate *prev, *next;
	db_entry *head, *tail;
	module *m;
	map *idx, *idx_save;
	db_entry *dirty_list;
	cell key;
	uint64_t cnt, ref_cnt, db_id;
	bool is_prebuilt:1;
	bool is_public:1;
	bool is_dynamic:1;
	bool is_meta_predicate:1;
	bool is_persist:1;
	bool is_multifile:1;
	bool is_discontiguous:1;
	bool is_abolished:1;
	bool is_noindex:1;
	bool is_check_directive:1;
	bool is_processed:1;
};

struct builtins {
	const char *name;
	unsigned arity;
	pl_status (*fn)(query*);
	const char *help;
	bool function;
};

typedef struct {
	const char *name;
	unsigned specifier;
	unsigned priority;
} op_table;

struct trail_ {
	cell *attrs;
	pl_idx_t ctx, attrs_ctx;
	uint32_t var_nbr;
};

struct slot_ {
	cell c;
	pl_idx_t ctx;
	uint16_t mgen;
	bool mark:1;
};

struct frame_ {
	cell *prev_cell;
	module *m;
	uint64_t ugen;
	pl_idx_t prev_frame, base_slot_nbr, overflow;
	uint32_t nbr_slots, nbr_vars, cgen;
	bool is_complex:1;
	bool is_last:1;
};

enum { eof_action_eof_code, eof_action_error, eof_action_reset };

struct stream_ {
	FILE *fp;
	char *mode, *filename, *name, *data, *src;
	void *sslptr;
	parser *p;
	char srcbuf[STREAM_BUFLEN];
	size_t data_len, alloc_nbytes;
	int ungetch, srclen;
	uint8_t level, eof_action;
	bool ignore:1;
	bool at_end_of_file:1;
	bool bom:1;
	bool repo:1;
	bool binary:1;
	bool did_getc:1;
	bool socket:1;
	bool nodelay:1;
	bool nonblock:1;
	bool udp:1;
	bool ssl:1;
	bool domain:1;
};

struct prolog_state_ {
	cell *curr_cell;
	db_entry *curr_clause, *curr_clause2;
	miter *f_iter;
	predicate *pr, *pr2;
	module *m;
	miter *iter;
	double prob;
	pl_idx_t curr_frame, fp, hp, tp, sp;
	uint32_t curr_page;
	uint8_t qnbr;
	bool definite:1;
	bool arg1_is_ground:1;
	bool arg2_is_ground:1;
	bool arg3_is_ground:1;
};

struct choice_ {
	prolog_state st;
	uint64_t ugen;
	pl_idx_t v1, v2, overflow;
	uint32_t nbr_slots, nbr_vars, cgen, frame_cgen;
	bool is_tail_rec:1;
	bool catchme_retry:1;
	bool catchme_exception:1;
	bool barrier:1;
	bool call_barrier:1;
	bool soft_cut:1;
	bool did_cleanup:1;
	bool register_cleanup:1;
	bool register_term:1;
	bool block_catcher:1;
	bool catcher:1;
};

struct page_ {
	page *next;
	cell *heap;
	pl_idx_t hp, max_hp_used, h_size;
	unsigned nbr;
};

enum q_retry { QUERY_OK=0, QUERY_RETRY=1, QUERY_EXCEPTION=2 };
enum unknowns { UNK_FAIL=0, UNK_ERROR=1, UNK_WARNING=2, UNK_CHANGEABLE=3 };
enum occurs { OCCURS_CHECK_FALSE=0, OCCURS_CHECK_TRUE=1, OCCURS_CHECK_ERROR = 2 };

struct prolog_flags_ {
	enum occurs occurs_check;
	enum unknowns unknown;
	bool double_quote_codes:1;
	bool double_quote_chars:1;
	bool double_quote_atom:1;
	bool character_escapes:1;
	bool char_conversion:1;
	bool not_strict_iso:1;
	bool debug:1;
};

struct query_ {
	query *prev, *next, *parent;
	module *save_m, *current_m;
	prolog *pl;
	parser *p;
	frame *frames;
	slot *slots;
	choice *choices;
	trail *trails;
	cell *tmp_heap, *last_arg, *exception, *variable_names;
	cell *queue[MAX_QUEUES], *tmpq[MAX_QUEUES];
	bool ignores[MAX_IGNORES];
	page *pages;
	slot *save_e;
	db_entry *dirty_list;
	cycle_info *info1, *info2;
	map *vars;
	cell accum;
	mpz_t tmp_ival;
	prolog_state st;
	uint64_t tot_goals, tot_backtracks, tot_retries, tot_matches, tot_tcos;
	uint64_t step, qid;
	uint64_t time_started, get_started;
	uint64_t time_cpu_started, time_cpu_last_started;
	uint64_t tmo_msecs;
	unsigned max_depth, tab_idx, varno, tab0_varno;
	int nv_start;
	pl_idx_t cp, tmphp, latest_ctx, popp, variable_names_ctx;
	pl_idx_t frames_size, slots_size, trails_size, choices_size;
	pl_idx_t max_choices, max_frames, max_slots, max_trails, before_hook_tp;
	pl_idx_t h_size, tmph_size, tot_heaps, tot_heapsize, undo_lo_tp, undo_hi_tp;
	pl_idx_t q_size[MAX_QUEUES], tmpq_size[MAX_QUEUES], qp[MAX_QUEUES];
	uint32_t cgen;
	uint16_t mgen;
	uint8_t nv_mask[MAX_ARITY];
	prolog_flags flags;
	enum q_retry retry;
	int8_t halt_code;
	int8_t quoted;
	bool parens:1;
	bool last_thing_was_symbol:1;
	bool in_attvar_print:1;
	bool lists_ok:1;
	bool autofail:1;
	bool is_oom:1;
	bool is_redo:1;
	bool run_hook:1;
	bool in_hook:1;
	bool do_dump_vars:1;
	bool is_dump_vars:1;
	bool status:1;
	bool resume:1;
	bool no_tco:1;
	bool check_unique:1;
	bool has_vars:1;
	bool error:1;
	bool did_throw:1;
	bool trace:1;
	bool creep:1;
	bool eval:1;
	bool yielded:1;
	bool is_task:1;
	bool nl:1;
	bool fullstop:1;
	bool ignore_ops:1;
	bool numbervars:1;
	bool halt:1;
	bool abort:1;
	bool cycle_error:1;
	bool spawned:1;
	bool run_init:1;
	bool varnames:1;
	bool listing:1;
	bool in_commit:1;
	bool did_quote:1;
};

struct parser_ {
	struct {
		char var_pool[MAX_VAR_POOL_SIZE];
		unsigned var_used[MAX_ARITY];
		const char *var_name[MAX_ARITY];
	} vartab;

	prolog *pl;
	FILE *fp;
	module *m;
	clause *cl;
	char *token, *save_line, *srcptr, *error_desc, *tmpbuf;
	cell v;
	size_t token_size, n_line, toklen, pos_start, tmpbuf_size;
	prolog_flags flags;
	unsigned depth, read_term;
	unsigned nesting_parens, nesting_braces, nesting_brackets;
	int quote_char, line_nbr, line_nbr_start;
	unsigned nbr_vars;
	int8_t dq_consing;
	bool error;
	bool was_consing:1;
	bool was_string:1;
	bool did_getline:1;
	bool already_loaded:1;
	bool do_read_term:1;
	bool string:1;
	bool run_init:1;
	bool directive:1;
	bool consulting:1;
	bool internal:1;
	bool one_shot:1;
	bool start_term:1;
	bool end_of_term:1;
	bool comment:1;
	bool is_quoted:1;
	bool is_variable:1;
	bool is_op:1;
	bool skip:1;
	bool command:1;
	bool last_close:1;
	bool no_fp:1;
	bool args:1;
	bool symbol:1;
};

struct loaded_file {
	struct loaded_file *next;
	char *filename;
	bool is_loaded:1;
};

#define MAX_MODULES_USED 64

struct module_ {
	module *next, *orig;
	prolog *pl;
	module *used[MAX_MODULES_USED];
	query *tasks;
	const char *filename, *name;
	predicate *head, *tail;
	parser *p;
	FILE *fp;
	map *index, *nbs, *ops, *defops;
	struct loaded_file *loaded_files;
	unsigned id, idx_used, indexing_threshold;
	prolog_flags flags;
	bool user_ops:1;
	bool prebuilt:1;
	bool use_persist:1;
	bool make_public:1;
	bool loaded_properties:1;
	bool loaded_ops:1;
	bool loading:1;
	bool error:1;
};

typedef struct {
	pl_idx_t ctx, val_off;
	unsigned var_nbr, cnt;
	bool is_anon;
} var_item;

struct prolog_ {
	stream streams[MAX_STREAMS];
	module *modmap[MAX_MODULES];
	module *modules;
	module *system_m, *user_m, *curr_m, *dcgs;
	parser *p;
	var_item *tabs;
	struct { pl_idx_t tab1[MAX_IGNORES], tab2[MAX_IGNORES]; };
	map *symtab, *funtab, *keyval;
	char *pool;
	size_t pool_offset, pool_size, tabs_size;
	uint64_t s_last, s_cnt, seed, ugen;
	unsigned next_mod_id;
	uint8_t current_input, current_output, current_error;
	int8_t halt_code, opt;
	bool is_redo:1;
	bool halt:1;
	bool status:1;
	bool did_dump_vars:1;
	bool quiet:1;
	bool stats:1;
	bool noindex:1;
	bool iso_only:1;
	bool trace:1;
};

extern pl_idx_t g_empty_s, g_pair_s, g_dot_s, g_cut_s, g_nil_s, g_true_s, g_fail_s;
extern pl_idx_t g_anon_s, g_neck_s, g_eof_s, g_lt_s, g_false_s, g_once_s;
extern pl_idx_t g_gt_s, g_eq_s, g_sys_elapsed_s, g_sys_queue_s, g_braces_s;
extern pl_idx_t g_sys_stream_property_s, g_unify_s, g_on_s, g_off_s, g_sys_var_s;
extern pl_idx_t g_call_s, g_braces_s, g_plus_s, g_minus_s, g_post_unify_hook_s;
extern pl_idx_t g_sys_soft_cut_s;

extern unsigned g_cpu_count;

#define share_cell(c) if (is_managed(c)) share_cell_(c)
#define unshare_cell(c) if (is_managed(c)) unshare_cell_(c)

inline static void share_cell_(const cell *c)
{
	if (is_strbuf(c))
		(c)->val_strb->refcnt++;
	else if (is_bigint(c))
		(c)->val_bigint->refcnt++;
}

inline static void unshare_cell_(const cell *c)
{
	if (is_strbuf(c)) {
		if (--(c)->val_strb->refcnt == 0) {
			free((c)->val_strb);
			g_string_cnt--;
		}
	} else if (is_bigint(c)) {
		if (--(c)->val_bigint->refcnt == 0)	{
			mp_int_clear(&(c)->val_bigint->ival);
			free((c)->val_bigint);
		}
	}
}

#define copy_cells(dst, src, nbr_cells) memcpy(dst, src, sizeof(cell)*(nbr_cells))

inline static pl_idx_t safe_copy_cells(cell *dst, const cell *src, pl_idx_t nbr_cells)
{
	memcpy(dst, src, sizeof(cell)*nbr_cells);

	for (pl_idx_t i = 0; i < nbr_cells; i++) {
		share_cell(src);
		src++;
	}

	return nbr_cells;
}

inline static void chk_cells(const cell *src, pl_idx_t nbr_cells)
{
	for (pl_idx_t i = 0; i < nbr_cells; i++) {
		unshare_cell(src);
		src++;
	}
}

#define LIST_HANDLER(l) cell l##_h_tmp; cell l##_t_tmp
#define LIST_HEAD(l) list_head(l, &l##_h_tmp)
#define LIST_TAIL(l) list_tail(l, &l##_t_tmp)

cell *list_head(cell *l, cell *tmp);
cell *list_tail(cell *l, cell *tmp);

enum clause_type {DO_CLAUSE, DO_RETRACT, DO_STREAM_RETRACT, DO_RETRACTALL};

size_t formatted(char *dst, size_t dstlen, const char *src, int srclen, bool dq);
char *slicedup(const char *s, size_t n);
int slicecmp(const char *s1, size_t len1, const char *s2, size_t len2);
unsigned count_bits(const uint8_t *mask, unsigned bit);
uint64_t get_time_in_usec(void);
uint64_t cpu_time_in_usec(void);
char *relative_to(const char *basefile, const char *relfile);
size_t sprint_int(char *dst, size_t size, pl_int_t n, int base);
void format_property(module *m, char *tmpbuf, size_t buflen, const char *name, unsigned arity, const char *type);
const char *dump_key(const void *k, const void *v, const void *p);

#define slicecmp2(s1,l1,s2) slicecmp(s1,l1,s2,strlen(s2))

// A string builder...

typedef struct {
	char *buf, *dst;
	size_t size;
} astring;

#define ASTRING(pr) astring pr##_buf;									\
	pr##_buf.size = 0;													\
	pr##_buf.buf = NULL;												\
	pr##_buf.dst = pr##_buf.buf;

#define ASTRING_alloc(pr,len) astring pr##_buf; 						\
	pr##_buf.size = len;												\
	pr##_buf.buf = malloc((len)+1);										\
	ensure(pr##_buf.buf);												\
	pr##_buf.dst = pr##_buf.buf;										\
	*pr##_buf.dst = '\0';

#define ASTRING_strlen(pr) (pr##_buf.dst - pr##_buf.buf)

#define ASTRING_trim(pr,ch) {											\
	if (ASTRING_strlen(pr)) {											\
		if (pr##_buf.dst[-1] == (ch)) 									\
			*--pr##_buf.dst = '\0';										\
	}																	\
}

#define ASTRING_trim_all(pr,ch) {										\
	while (ASTRING_strlen(pr)) {										\
		if (pr##_buf.dst[-1] != (ch)) 									\
			break;														\
		*--pr##_buf.dst = '\0';											\
	}																	\
}

#define ASTRING_trim_ws(pr) {											\
	while (ASTRING_strlen(pr)) {										\
		if (!isspace(pr##_buf.dst[-1]))									\
			break;														\
		*--pr##_buf.dst = '\0';											\
	}																	\
}

#define ASTRING_check(pr,len) {											\
	size_t rem = pr##_buf.size - ASTRING_strlen(pr);					\
	if ((size_t)((len)+1) >= rem) {										\
		size_t offset = ASTRING_strlen(pr);								\
		pr##_buf.buf = realloc(pr##_buf.buf, (pr##_buf.size += ((len)-rem)) + 1); \
		ensure(pr##_buf.buf);											\
		pr##_buf.dst = pr##_buf.buf + offset;							\
	}																	\
}

#define ASTRING_strcat(pr,s) ASTRING_strcatn(pr,s,strlen(s))

#define ASTRING_strcatn(pr,s,len) {										\
	ASTRING_check(pr, len);												\
	memcpy(pr##_buf.dst, s, len);										\
	pr##_buf.dst += len;												\
	*pr##_buf.dst = '\0';												\
}

#define ASTRING_sprintf(pr,fmt,...) {									\
	size_t len = snprintf(NULL, 0, fmt, __VA_ARGS__);					\
	ASTRING_check(pr, len);												\
	sprintf(pr##_buf.dst, fmt, __VA_ARGS__);							\
	pr##_buf.dst += len;												\
	*pr##_buf.dst = '\0';												\
}

#define ASTRING_cstr(pr) pr##_buf.buf ? pr##_buf.buf : ""
#define ASTRING_free(pr) { free(pr##_buf.buf); pr##_buf.buf = NULL; }
