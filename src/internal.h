#pragma once

#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <assert.h>
#include <sys/param.h>

#ifndef USE_OPENSSL
#define USE_OPENSSL 0
#endif

#ifndef USE_THREADS
#define USE_THREADS 0
#endif

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define snprintf _snprintf
#define chdir _chdir
#define getpid _getpid
#define PATH_SEP "\\"
#define PATH_SEP_CHAR '\\'
#define NEWLINE_MODE "dos"
#else
#include <unistd.h>
#define PATH_SEP "/"
#define PATH_SEP_CHAR '/'
#define NEWLINE_MODE "posix"
#endif

typedef intmax_t int_t;
typedef uintmax_t uint_t;

#define INT_T_MAX INTMAX_MAX
#define INT_T_MIN INTMAX_MIN

#if (__STDC_VERSION__ >= 201112L) && USE_THREADS
#include <stdatomic.h>
#define atomic_t _Atomic
#else
#define atomic_t volatile
#endif

typedef uint32_t idx_t;

#include "map.h"
#include "trealla.h"
#include "cdebug.h"
#include "imath/imath.h"

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

// Sentinel Value
#define ERR_IDX (~(idx_t)0)
#define IDX_MAX (ERR_IDX-1)
#define ERR_CYCLE_CMP -2

#define MAX_SMALL_STRING (sizeof(void*)*2)
#define MAX_VAR_POOL_SIZE 1000
#define MAX_ARITY UCHAR_MAX
#define MAX_QUEUES 16
#define MAX_STREAMS 1024
//#define MAX_DEPTH 9999
#define MAX_DEPTH 3000			// Clang stack size needs this small
#define STREAM_BUFLEN 1024

#define GET_CHOICE(i) (q->choices+(i))
#define GET_CURR_CHOICE() GET_CHOICE(q->cp-1)

#define GET_FRAME(i) (q->frames+(i))
#define GET_CURR_FRAME() GET_FRAME(q->st.curr_frame)

#define GET_SLOT(g,i) ((i) < g->nbr_slots ? (q->slots+g->ctx+(i)) : (q->slots+g->overflow+((i)-g->nbr_slots)))

// Primary type...

#define is_empty(c) ((c)->tag == TAG_EMPTY)
#define is_variable(c) ((c)->tag == TAG_VARIABLE)
#define is_literal(c) ((c)->tag == TAG_LITERAL)
#define is_cstring(c) ((c)->tag == TAG_CSTRING)
#define is_integer(c) ((c)->tag == TAG_INTEGER)
#define is_real(c) ((c)->tag == TAG_REAL)
#define is_indirect(c) ((c)->tag == TAG_INDIRECT)
#define is_end(c) ((c)->tag == TAG_END)

// Derived type...

#define is_iso_atom(c) ((is_literal(c) || is_cstring(c)) && !(c)->arity)
#define is_iso_list(c) (is_literal(c) && ((c)->arity == 2) && ((c)->val_off == g_dot_s))
#define is_cons_list(c) (is_iso_list(c) && is_variable(c+2))

#define get_real(c) (c)->val_real
#define set_real(c,v) (c)->val_real = (v)

#define get_smallint(c) (c)->val_int
#define set_smallint(c,v) { (c)->val_int = (v); }

#define get_integer(c) (c)->val_int

#define neg_bigint(c) (c)->val_bigint->ival.sign = MP_NEG;
#define neg_smallint(c) (c)->val_int = -llabs((c)->val_int)
#define neg_real(c) (c)->val_real = -fabs((c)->val_real)

#define is_negative(c) (is_bigint(c) ? (c)->val_bigint->ival.sign == MP_NEG : get_smallint(c) < 0)
#define is_positive(c) (is_bigint(c) ? (c)->val_bigint->ival.sign != MP_NEG : get_smallint(c) > 0)

#define is_gt(c,n) (get_smallint(c) > (n))
#define is_ge(c,n) (get_smallint(c) >= (n))
#define is_eq(c,n) (get_smallint(c) == (n))
#define is_ne(c,n) (get_smallint(c) != (n))
#define is_le(c,n) (get_smallint(c) <= (n))
#define is_lt(c,n) (get_smallint(c) < (n))

#define is_smallint(c) (is_integer(c) && !((c)->flags & FLAG_MANAGED))
#define is_bigint(c) (is_integer(c) && (c)->flags & FLAG_MANAGED)
#define is_atom(c) ((is_literal(c) && !(c)->arity) || is_cstring(c))
#define is_string(c) (is_cstring(c) && (c)->flags & FLAG_STRING)
#define is_managed(c) ((c)->flags & FLAG_MANAGED)
#define is_blob(c) (is_cstring(c) && (c)->flags & FLAG_BLOB)
#define is_list(c) (is_iso_list(c) || is_string(c))
#define is_static(c) (is_blob(c) && ((c)->flags & FLAG_STATIC))
#define is_strbuf(c) (is_blob(c) && !((c)->flags & FLAG_STATIC))
#define is_nil(c) (is_literal(c) && !(c)->arity && ((c)->val_off == g_nil_s))
#define is_quoted(c) ((c)->flags & FLAG2_QUOTED)
#define is_fresh(c) ((c)->flags & FLAG2_FRESH)
#define is_anon(c) ((c)->flags & FLAG2_ANON)
#define is_builtin(c) ((c)->flags & FLAG_BUILTIN)
#define is_function(c) ((c)->flags & FLAG_FUNCTION)
#define is_tail(c) ((c)->flags & FLAG_TAIL)
#define is_tail_recursive(c) ((c)->flags & FLAG_TAIL_REC)
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
	(c)->val_strb = strb;										\
	(c)->strb_off = off;										\
	(c)->strb_len = n;											\
	(c)->flags |= FLAG_MANAGED | FLAG_BLOB;						\
	}

#define _GET_STR(pl,c) 											\
	( !is_cstring(c) ? ((pl)->pool + (c)->val_off)				\
	: is_strbuf(c) ? ((c)->val_strb->cstr + (c)->strb_off)		\
	: is_static(c) ? (c)->val_str								\
	: ((char*)(c)->val_chr)										\
	)

#define _LEN_STR(pl,c) 											\
	( !is_cstring(c) ? strlen((pl)->pool + (c)->val_off)		\
	: is_strbuf(c) ? (c)->strb_len								\
	: is_static(c) ? (c)->str_len								\
	: strlen((c)->val_chr)										\
	)

#define LEN_STR_UTF8(c) strlen_utf8(GET_STR(q, c))
#define GET_STR(x,c) _GET_STR((x)->pl, c)
#define LEN_STR(x,c) _LEN_STR((x)->pl, c)
#define GET_POOL(x,off) ((x)->pl->pool + (off))

// If changing the order of these: see runtime.c dispatch table

enum {
	TAG_EMPTY=0,
	TAG_VARIABLE=1,
	TAG_LITERAL=2,
	TAG_CSTRING=3,
	TAG_INTEGER=4,
	TAG_REAL=5,
	TAG_INDIRECT=6,
	TAG_END=7
};

enum {
	FLAG_BUILTIN=1<<0,
	FLAG_HEX=1<<1,						// used with TAG_INTEGER
	FLAG_OCTAL=1<<2,					// used with TAG_INTEGER
	FLAG_BINARY=1<<3,					// used with TAG_INTEGER
	FLAG_STREAM=1<<4,					// used with TAG_INTEGER
	FLAG_TAIL_REC=1<<5,
	FLAG_TAIL=1<<6,
	FLAG_BLOB=1<<7,						// used with TAG_CSTRING
	FLAG_STRING=1<<8,					// used with TAG_CSTRING
	FLAG_STATIC=1<<9,
	FLAG_MANAGED=1<<10,					// any ref-counted object
	FLAG_FUNCTION=1<<11,

	FLAG_SPARE1=1<<12,

	FLAG2_PROCESSED=FLAG_SPARE1,		// used by bagof
	FLAG2_FIRST_USE=FLAG_HEX,			// used with TAG_VARIABLE
	FLAG2_ANON=FLAG_OCTAL,				// used with TAG_VARIABLE
	FLAG2_FRESH=FLAG_BINARY,			// used with TAG_VARIABLE
	FLAG2_QUOTED=FLAG_OCTAL,			// used with TAG_CSTRING

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

#define CELL_PREFIX(c) IS_PREFIX(GET_OP(c))
#define CELL_POSTFIX(c) IS_POSTFIX(GET_OP(c))
#define CELL_INFIX(c) IS_INFIX(GET_OP(c))

#define IS_FX(c) (GET_OP(c) == OP_FX)
#define IS_FY(c) (GET_OP(c) == OP_FY)
#define IS_XF(c) (GET_OP(c) == OP_XF)
#define IS_YF(c) (GET_OP(c) == OP_YF)
#define IS_YFX(c) (GET_OP(c) == OP_YFX)
#define IS_XFX(c) (GET_OP(c) == OP_XFX)
#define IS_XFY(c) (GET_OP(c) == OP_XFY)

#define SET_OP(c,op) (CLR_OP(c), (c)->flags |= (((uint16_t)(op)) << 13))
#define CLR_OP(c) ((c)->flags &= ~((uint16_t)(0xF) << 13))
#define GET_OP(c) (((c)->flags >> 13) & 0xF)
#define IS_OP(c) (GET_OP(c) != 0 ? true : false)

typedef struct module_ module;
typedef struct query_ query;
typedef struct predicate_ predicate;
typedef struct clause_ clause;
typedef struct cell_ cell;
typedef struct parser_ parser;

// Using a fixed-size cell allows having arrays of cells, which is
// basically what a Term is. A compound is a variable length array of
// cells, the length specified by 'nbr_cells' field in the 1st cell.
// A cell is a tagged union, the size should should 24 bytes.

struct cell_ {
	uint8_t tag;
	uint8_t arity;
	uint16_t flags;
	idx_t nbr_cells;

	union {
		struct {
			int_t val_int;
			void *val_spare1;
		};

		struct {
			bigint *val_bigint;
			void *val_spare2;
		};

		struct {
			double val_real;
			void *val_spare3;
		};

		struct {
			cell *val_ptr;
			uint32_t cgen;				// choice generation
			uint32_t mod_nbr;
		};

		struct {
			char val_chr[MAX_SMALL_STRING];
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
			};

			uint32_t val_off;			// offset into pool
			uint32_t var_nbr;			// used with TAG_VAR
		};

		struct {
			cell *attrs;				// used with TAG_EMPTY in slot
			idx_t attrs_ctx;			// to set attributes on a var
		};
	};
};

extern cell* ERR_CYCLE_CELL;

typedef struct {
	uint64_t u1, u2;
} uuid;

typedef struct {
	uint64_t ugen_created, ugen_erased;
	idx_t nbr_cells, cidx;
	uint16_t nbr_vars;
	bool first_cut:1;
	bool cut_only:1;
	bool is_fact:1;
	bool persist:1;
	bool tail_rec:1;
	cell cells[];
} rule;

struct clause_ {
	predicate *owner;
	clause *prev, *next, *dirty;
	uuid u;
	rule r;
};

struct predicate_ {
	predicate *prev, *next;
	clause *head, *tail;
	module *m;
	map *idx1, *idx2, *idx_save;
	cell key;
	uint64_t cnt, refs;
	bool is_prebuilt:1;
	bool is_public:1;
	bool is_dynamic:1;
	bool is_meta_predicate:1;
	bool is_persist:1;
	bool is_multifile:1;
	bool is_discontiguous:1;
	bool is_abolished:1;
	bool is_noindex:1;
	bool check_directive:1;
};

struct builtins {
	const char *name;
	unsigned arity;
	pl_status (*fn)(query*);
	const char *help;
	bool function;
};

typedef struct {
	char *name;
	unsigned specifier;
	unsigned priority;
} op_table;

typedef struct {
	cell *attrs;
	idx_t ctx, attrs_ctx;
	uint32_t var_nbr;
} trail;

typedef struct {
	cell c;
	idx_t ctx;
} slot;

typedef struct {
	cell *prev_cell;
	module *m;
	uint64_t ugen;
	idx_t prev_frame, ctx, overflow, cgen;
	uint32_t nbr_slots;
	uint16_t nbr_vars;
} frame;

enum { eof_action_eof_code, eof_action_error, eof_action_reset };

typedef struct {
	FILE *fp;
	char *mode, *filename, *name, *data, *src;
	void *sslptr;
	parser *p;
	char srcbuf[STREAM_BUFLEN];
	size_t data_len, alloc_nbytes;
	int ungetch, srclen;
	uint8_t level, eof_action;
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
} stream;

typedef struct {
	cell *curr_cell;
	clause *curr_clause, *curr_clause2;
	miter *iter, *f_iter;
	predicate *pr;
	module *m;
	idx_t curr_frame, fp, hp, tp, sp, cgen, arena_nbr;
	uint8_t qnbr;
} prolog_state;

typedef struct {
	prolog_state st;
	uint64_t pins, ugen;
	idx_t v1, v2, cgen, orig_cgen, overflow;
	uint32_t nbr_slots;
	uint16_t nbr_vars;
	bool catchme_retry:1;
	bool catchme_exception:1;
	bool barrier:1;
	bool soft_cut:1;
	bool did_cleanup:1;
	bool register_cleanup:1;
	bool register_term:1;
	bool chk_is_det:1;
	bool tail_rec:1;
} choice;

typedef struct arena_ arena;

struct arena_ {
	arena *next;
	cell *heap;
	idx_t hp, max_hp_used, h_size;
	unsigned nbr;
};

enum q_retry { QUERY_OK=0, QUERY_RETRY=1, QUERY_EXCEPTION=2 };
enum unknowns { UNK_FAIL=0, UNK_ERROR=1, UNK_WARNING=2, UNK_CHANGEABLE=3 };

typedef struct prolog_flags_ {
	short occurs_check;
	enum unknowns unknown;
	bool double_quote_codes:1;
	bool double_quote_chars:1;
	bool double_quote_atom:1;
	bool character_escapes:1;
	bool char_conversion:1;
	bool not_strict_iso:1;
	bool debug:1;
} prolog_flags;

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
	arena *arenas;
	clause *dirty_list;
	slot *save_e;
	cell accum;
	mpz_t tmp_ival;
	prolog_state st;
	uint64_t tot_goals, tot_retries, tot_matches, tot_tcos;
	uint64_t step, qid, time_started, get_started, cpu_started, cpu_last_started;
	unsigned max_depth, tmo_msecs;
	int nv_start;
	idx_t cp, tmphp, latest_ctx, popp, variable_names_ctx, save_cp;
	idx_t frames_size, slots_size, trails_size, choices_size;
	idx_t max_choices, max_frames, max_slots, max_trails, save_tp;
	idx_t h_size, tmph_size, tot_heaps, tot_heapsize, undo_lo_tp, undo_hi_tp;
	idx_t q_size[MAX_QUEUES], tmpq_size[MAX_QUEUES], qp[MAX_QUEUES];
	uint8_t nv_mask[MAX_ARITY];
	prolog_flags flag;
	enum q_retry retry;
	int8_t halt_code;
	int8_t quoted;
	bool has_attrs:1;
	bool in_hook:1;
	bool do_dump_vars:1;
	bool is_dump_vars:1;
	bool status:1;
	bool resume:1;
	bool no_tco:1;
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
};

struct parser_ {
	struct {
		char var_pool[MAX_VAR_POOL_SIZE];
		unsigned var_used[MAX_ARITY];
		const char *var_name[MAX_ARITY];
	} vartab;

	prolog *pl;
	FILE *fp;
	module *m, *dcgs;
	rule *r;
	char *token, *save_line, *srcptr, *error_desc;
	cell v;
	size_t token_size, n_line, toklen;
	prolog_flags flag;
	unsigned line_nbr, depth, read_term;
	unsigned nesting_parens, nesting_braces, nesting_brackets;
	int quote_char;
	unsigned nbr_vars;
	int8_t dq_consing;
	bool error;
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
};

struct loaded_file {
	struct loaded_file *next;
	char filename[PATH_MAX];
};

#define MAX_MODULES_USED 64

struct module_ {
	module *next;
	prolog *pl;
	module *used[MAX_MODULES_USED];
	query *tasks;
	char *name, *filename;
	predicate *head, *tail;
	parser *p;
	FILE *fp;
	map *index, *nbs, *ops, *defops;
	struct loaded_file *loaded_files;
	unsigned id, idx_used;
	prolog_flags flag;
	bool user_ops:1;
	bool prebuilt:1;
	bool use_persist:1;
	bool make_public:1;
	bool loaded_properties:1;
	bool loaded_ops:1;
	bool loading:1;
	bool error:1;
};

struct prolog_ {
	idx_t tab1[64000];
	idx_t tab3[64000];
	idx_t tab2[64000];
	idx_t tab4[64000];
	uint8_t tab5[64000];
	module *modules;
	module *system_m, *user_m, *curr_m;
	uint64_t s_last, s_cnt, seed;
	map *symtab, *funtab, *keyval;
	char *pool;
	uint64_t ugen;
	idx_t pool_offset, pool_size, tab_idx;
	unsigned varno;
	uint8_t current_input, current_output, current_error;
	int8_t halt_code, opt;
	bool halt:1;
	bool status:1;
	bool did_dump_vars:1;
	bool quiet:1;
	bool stats:1;
	bool noindex:1;
	bool iso_only:1;
	bool trace:1;
};

extern idx_t g_empty_s, g_pair_s, g_dot_s, g_cut_s, g_nil_s, g_true_s, g_fail_s;
extern idx_t g_anon_s, g_neck_s, g_eof_s, g_lt_s, g_false_s, g_once_s;
extern idx_t g_gt_s, g_eq_s, g_sys_elapsed_s, g_sys_queue_s, g_braces_s;
extern idx_t g_stream_property_s, g_unify_s, g_on_s, g_off_s, g_sys_var_s;
extern idx_t g_call_s, g_braces_s, g_plus_s, g_minus_s, g_post_unify_hook_s;
extern stream g_streams[MAX_STREAMS];
extern unsigned g_cpu_count;

inline static void share_cell(const cell *c)
{
	if (!is_managed(c))
		return;

	if (is_strbuf(c))
		(c)->val_strb->refcnt++;
	else if (is_bigint(c))
		(c)->val_bigint->refcnt++;
}

inline static void unshare_cell(cell *c)
{
	if (!is_managed(c))
		return;

	if (is_strbuf(c)) {
		if (--(c)->val_strb->refcnt == 0) {
			free((c)->val_strb);
		}
	} else if (is_bigint(c)) {
		if (--(c)->val_bigint->refcnt == 0)	{
			mp_int_clear(&(c)->val_bigint->ival);
			free((c)->val_bigint);
		}
	}
}

inline static idx_t copy_cells(cell *dst, const cell *src, idx_t nbr_cells)
{
	memcpy(dst, src, sizeof(cell)*nbr_cells);
	return nbr_cells;
}

inline static idx_t safe_copy_cells(cell *dst, const cell *src, idx_t nbr_cells)
{
	for (idx_t i = 0; i < nbr_cells; i++) {
		share_cell(src);
		*dst++ = *src++;
	}

	return nbr_cells;
}

inline static void chk_cells(cell *src, idx_t nbr_cells)
{
	for (idx_t i = 0; i < nbr_cells; i++) {
		unshare_cell(src);
		src++;
	}
}

#define LIST_HANDLER(l) cell l##_h_tmp; cell l##_t_tmp
#define LIST_HEAD(l) list_head(l, &l##_h_tmp)
#define LIST_TAIL(l) list_tail(l, &l##_t_tmp)

extern cell *list_head(cell *l, cell *tmp);
extern cell *list_tail(cell *l, cell *tmp);

enum clause_type {DO_CLAUSE, DO_RETRACT, DO_STREAM_RETRACT, DO_RETRACTALL};

extern size_t formatted(char *dst, size_t dstlen, const char *src, int srclen, bool dq);
extern int slicecmp(const char *s1, size_t len1, const char *s2, size_t len2);
extern unsigned count_bits(const uint8_t *mask, unsigned bit);
extern uint64_t get_time_in_usec(void);
extern uint64_t cpu_time_in_usec(void);
extern char *relative_to(const char *basefile, const char *relfile);
extern size_t sprint_int(char *dst, size_t size, int_t n, int base);
extern void format_property(char *tmpbuf, size_t buflen, const char *name, unsigned arity, const char *type);

#define slicecmp2(s1,l1,s2) slicecmp(s1,l1,s2,strlen(s2))

// A string builder...

typedef struct {
	char *buf, *dst;
	size_t size;
}
 astring;

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
			 *--pr##_buf.dst = '\0';									\
	}																	\
}

#define ASTRING_trim_all(pr,ch) {										\
	while (ASTRING_strlen(pr)) {										\
		if (pr##_buf.dst[-1] != (ch)) 									\
			break;														\
		 *--pr##_buf.dst = '\0';										\
	}																	\
}

#define ASTRING_trim_ws(pr) {											\
	while (ASTRING_strlen(pr)) {										\
		if (!isspace(pr##_buf.dst[-1]))									\
			break;														\
		 *--pr##_buf.dst = '\0';										\
	}																	\
}

#define ASTRING_check(pr,len) {											\
	size_t rem = pr##_buf.size - ASTRING_strlen(pr);					\
	if (((len)+1) >= rem) {												\
		size_t offset = ASTRING_strlen(pr);								\
		pr##_buf.buf = realloc(pr##_buf.buf, (pr##_buf.size += ((len)-rem)) + 1); \
		ensure(pr##_buf.buf);											\
		pr##_buf.dst = pr##_buf.buf + offset;							\
	}																	\
}

#define ASTRING_strcat(pr,s) ASTRING_strcatn(pr,s,strlen(s))

#define ASTRING_strcatn(pr,s,len) {										\
	ASTRING_check(pr, len);												\
	memcpy(pr##_buf.dst, s, (len)+1);									\
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
