// type-checking pass, which also does late identifier resolution
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "hashtable.h"
#include "ast_field.h"

#include <stdlib.h> // strtof

// TRACE_TEMPLATE_EXPANSION: define to trace details about template instantiation
//#define TRACE_TEMPLATE_EXPANSION

#define trace(fmt, va...) \
  _trace(opt_trace_typecheck, 4, "TC", "%*s" fmt, a->traceindent*2, "", ##va)

#define tracex(fmt, va...) \
  _trace(opt_trace_typecheck, 4, "TC", fmt, ##va)

#ifdef TRACE_TEMPLATE_EXPANSION
  #define trace_tplexp(fmt, va...) \
    _trace(opt_trace_typecheck, 6, "TX", "%*s" fmt, ctx->traceindent*2, "", ##va)
#else
  #define trace_tplexp(fmt, args...) ((void)0)
#endif


typedef struct {
  sym_t          name;      // available name of decl
  sym_t nullable othername; // alternate name
  node_t*        decl;
} didyoumean_t;


// typecheck_t
typedef struct {
  compiler_t*     compiler;
  pkg_t*          pkg;
  memalloc_t      ma;        // compiler->ma
  memalloc_t      ast_ma;    // compiler->ast_ma
  scope_t         scope;
  err_t           err;
  fun_t* nullable fun;       // current function
  type_t*         typectx;
  ptrarray_t      typectxstack;
  ptrarray_t      nspath;
  map_t           postanalyze;    // set of nodes to analyze at the very end (keys only)
  map_t           tmpmap;
  map_t           typeidmap;      // typeid_t => type_t*
  map_t           templateimap;   // typeid_t => usertype_t*
  buf_t           tmpbuf;
  bool            reported_error; // true if an error diagnostic has been reported
  u32             pubnest;        // NF_VIS_PUB nesting level
  u32             templatenest;   // NF_TEMPLATE nesting level

  // didyoumean tracks names that we might want to consider for help messages
  // when an identifier can not be resolved
  array_type(didyoumean_t) didyoumean;

  #if DEBUG
    int traceindent;
  #endif
} typecheck_t;


static const char* fmtnode(u32 bufindex, const void* nullable n);


static const char* fmtkind(const void* node) {
  const node_t* n = node;
  if (n->kind == EXPR_ID && ((idexpr_t*)n)->ref)
    n = ((idexpr_t*)n)->ref;
  switch (n->kind) {
    case EXPR_BINOP: switch (((binop_t*)n)->op) {
      case OP_EQ:   // ==
      case OP_NEQ:  // !=
      case OP_LT:   // <
      case OP_GT:   // >
      case OP_LTEQ: // <=
      case OP_GTEQ: // >=
        return "comparison";
    }
  }
  return nodekind_fmt(n->kind);
}


#ifdef DEBUG
  #define trace_node(a, msg, np) ({ \
    const node_t* __n = *(const node_t**)(np); \
    trace("%s%-14s: %s", (msg), nodekind_name(__n->kind), fmtnode(0, __n)); \
  })

  typedef struct {
    typecheck_t*   a;
    const node_t** np;
    const char*    msg;
  } nodetrace_t;

  static void _trace_cleanup(nodetrace_t* nt) {
    if (!opt_trace_typecheck)
      return;
    typecheck_t* a = nt->a;
    a->traceindent--;
    type_t* t = NULL;
    const node_t* n = *nt->np;
    if (node_isexpr(n)) {
      t = asexpr(n)->type;
    } else if (node_istype(n)) {
      t = (type_t*)n;
    }
    if (t &&
        ( t == type_unknown ||
          t->kind == TYPE_UNRESOLVED) )
    {
      trace("\e[1;31m%s type not resolved (%s)\e[0m",
        nodekind_name(n->kind), fmtnode(0, n));
    }
    trace("%s%-14s => %s %s", nt->msg, nodekind_name(n->kind),
      t ? nodekind_name(t->kind) : "NULL",
      t ? fmtnode(0, t) : "");
  }

  #define TRACE_NODE(a, msg, np) \
    trace_node((a), (msg), (np)); \
    (a)->traceindent++; \
    nodetrace_t __nt __attribute__((__cleanup__(_trace_cleanup),__unused__)) = \
      {(a), (const node_t**)(np), (msg)};

#else
  #define trace_node(a,msg,n) ((void)0)
  #define TRACE_NODE(a,msg,n) ((void)0)
#endif


#define CHECK_ONCE(node) \
  ( ((node)->flags & NF_CHECKED) == 0 && ((node)->flags |= NF_CHECKED) )


static void incuse(void* node) {
  node_t* n = node;
  n->nuse++;
  if (n->kind == EXPR_ID && ((idexpr_t*)n)->ref)
    incuse(((idexpr_t*)n)->ref);
}

#define use(node) (incuse(node), (node))


bool type_isowner(const type_t* t) {
  // TODO: consider computing this once during typecheck and then just setting
  // a nodeflag e.g. NF_OWNER, and rewriting type_isowner to just check for that flag.
  t = type_isopt(t) ? ((opttype_t*)t)->elem : t;
  return (
    (t->flags & (NF_DROP | NF_SUBOWNERS)) ||
    type_isptr(t) ||
    ( t->kind == TYPE_ALIAS && type_isowner(((aliastype_t*)t)->elem) )
  );
}


// unwrap_id returns node->ref if node is an ID
static node_t* unwrap_id(void* node) {
  node_t* n = node;
  while (n->kind == EXPR_ID)
    n = assertnotnull(((idexpr_t*)n)->ref);
  return n;
}


// unwrap_alias unwraps aliases
// e.g. "MyMyT" => "MyT" => "T"
static type_t* unwrap_alias(type_t* t) {
  while (t->kind == TYPE_ALIAS)
    t = assertnotnull(((aliastype_t*)t)->elem);
  return t;
}

static const type_t* unwrap_alias_const(const type_t* t) {
  while (t->kind == TYPE_ALIAS)
    t = assertnotnull(((aliastype_t*)t)->elem);
  return t;
}


// unwrap_ptr unwraps optional, ref and ptr
// e.g. "?&T" => "&T" => "T"
type_t* type_unwrap_ptr(type_t* t) {
  assertnotnull(t);
  for (;;) switch (t->kind) {
    case TYPE_OPTIONAL: t = assertnotnull(((opttype_t*)t)->elem); break;
    case TYPE_REF:
    case TYPE_MUTREF:   t = assertnotnull(((reftype_t*)t)->elem); break;
    case TYPE_PTR:      t = assertnotnull(((ptrtype_t*)t)->elem); break;
    default:            return t;
  }
}


// unwrap_ptr_and_alias unwraps optional, ref, ptr and alias
// e.g. "&MyT" => "MyT" => "T"
static type_t* unwrap_ptr_and_alias(type_t* t) {
  assertnotnull(t);
  for (;;) switch (t->kind) {
    case TYPE_REF:
    case TYPE_MUTREF:   t = assertnotnull(((reftype_t*)t)->elem); break;
    case TYPE_PTR:      t = assertnotnull(((ptrtype_t*)t)->elem); break;
    case TYPE_ALIAS:    t = assertnotnull(((aliastype_t*)t)->elem); break;
    default:            return t;
  }
}


static type_t* concrete_type(const compiler_t* c, type_t* t) {
  for (;;) switch (t->kind) {
  case TYPE_ALIAS: t = assertnotnull(((aliastype_t*)t)->elem); break;
  case TYPE_INT:   t = c->inttype; break;
  case TYPE_UINT:  t = c->uinttype; break;
  default:         return t;
  }
}


// type_iscompatible:  value of type x can be read as type y or vice versa (eg "x + y").
// type_isassignable:  value of type y can be assigned to local of type x.
// type_isequivalent:  value of type x and y are equivalent (sans any aliases.)
// type_isconvertible: value of type src can be converted to type dst.
static bool _type_compat(
  const compiler_t* c, const type_t* x, const type_t* y, bool assignment);
static bool type_compat(
  const compiler_t* c, const type_t* x, const type_t* y, bool assignment)
{
  return x == y || _type_compat(c, x, y, assignment);
}
static bool type_isequivalent(const compiler_t* c, const type_t* x, const type_t* y) {
  return x == y || concrete_type(c, (type_t*)x) == concrete_type(c, (type_t*)y);
}
static bool type_isassignable( const compiler_t* c, const type_t* x, const type_t* y) {
  return type_compat(c, x, y, true);
}
static bool type_iscompatible(const compiler_t* c, const type_t* x, const type_t* y) {
  return type_compat(c, x, y, false);
}

static const type_t* type_compat_unwrap(
  const compiler_t* c, const type_t* t, bool may_deref)
{
  for (;;) switch (t->kind) {
    case TYPE_ALIAS: t = assertnotnull(((aliastype_t*)t)->elem); break;
    case TYPE_INT:   t = c->inttype; break;
    case TYPE_UINT:  t = c->uinttype; break;

    case TYPE_REF:
    case TYPE_MUTREF:
      if (!may_deref)
        return t;
      may_deref = false;
      t = ((reftype_t*)t)->elem;
      break;

    default:
      return t;
  }
}

static bool _type_compat(
  const compiler_t* c, const type_t* x, const type_t* y, bool assignment)
{
  assertnotnull(x);
  assertnotnull(y);

  x = type_compat_unwrap(c, x, /*may_deref*/!assignment);
  y = type_compat_unwrap(c, y, /*may_deref*/!assignment);

  #if 0 && DEBUG
  {
    dlog("_type_compat (assignment=%d)", assignment);
    buf_t* buf = (buf_t*)&c->diagbuf;
    buf_clear(buf);
    node_fmt(buf, (node_t*)x, 0);
    dlog("  x = %s", buf->chars);
    buf_clear(buf);
    node_fmt(buf, (node_t*)y, 0);
    dlog("  y = %s", buf->chars);
  }
  #endif

  if (x == y)
    return true;

  switch (x->kind) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
      // note: we do allow "T = &T" (e.g. "var y &int; var x int = y")
      // of non-owning types, even though may be a little confusing sometimes.
      if (assignment)
        y = type_compat_unwrap(c, y, /*may_deref*/true);
      return x == y;

    case TYPE_STRUCT:
      // note that at this point, x != y
      if (assignment)
        y = type_compat_unwrap(c, y, /*may_deref*/true);
      return x == y && !type_isowner(x);

    case TYPE_PTR:
      // *T <= *T
      // &T <= *T
      return (
        type_isptrlike(y) &&
        type_compat(c, ((ptrtype_t*)x)->elem, ((ptrtype_t*)y)->elem, assignment) );

    case TYPE_OPTIONAL: {
      // ?T <= T
      // ?T <= ?T
      const opttype_t* d = (opttype_t*)x;
      if (y->kind == TYPE_OPTIONAL)
        y = ((opttype_t*)y)->elem;
      return type_compat(c, d->elem, y, assignment);
    }

    case TYPE_REF:
    case TYPE_MUTREF: {
      // &T    <= &T
      // mut&T <= &T
      // mut&T <= mut&T
      // &T    x= mut&T
      // &T    <= *T
      // mut&T <= *T
      const reftype_t* l = (reftype_t*)x;
      if (y->kind == TYPE_PTR) {
        // e.g. "&T <= *T"
        return type_compat(c, l->elem, ((ptrtype_t*)y)->elem, assignment);
      }
      const reftype_t* r = (reftype_t*)y;
      // e.g. "&T <= &T"
      bool l_ismut = l->kind == TYPE_MUTREF;
      bool r_ismut = r->kind == TYPE_MUTREF;
      return (
        type_isref(y) &&
        (r_ismut == l_ismut || r_ismut || !l_ismut) &&
        type_compat(c, l->elem, r->elem, assignment) );
    }

    case TYPE_SLICE:
    case TYPE_MUTSLICE: {
      // &[T]    <= &[T]
      // &[T]    <= mut&[T]
      // mut&[T] <= mut&[T]
      //
      // &[T]    <= &[T N]
      // &[T]    <= mut&[T N]
      // mut&[T] <= mut&[T N]
      const slicetype_t* l = (slicetype_t*)x;
      bool l_ismut = l->kind == TYPE_MUTSLICE;
      switch (y->kind) {
        case TYPE_SLICE:
        case TYPE_MUTSLICE: {
          const slicetype_t* r = (slicetype_t*)y;
          bool r_ismut = r->kind == TYPE_MUTSLICE;
          return (
            (r_ismut == l_ismut || r_ismut || !l_ismut) &&
            type_compat(c, l->elem, r->elem, assignment) );
        }
        case TYPE_REF:
        case TYPE_MUTREF: {
          bool r_ismut = y->kind == TYPE_MUTREF;
          const arraytype_t* r = (arraytype_t*)((reftype_t*)y)->elem;
          return (
            r->kind == TYPE_ARRAY &&
            (r_ismut == l_ismut || r_ismut || !l_ismut) &&
            type_compat(c, l->elem, r->elem, assignment) );
        }
      }
      return false;
    }

    case TYPE_ARRAY: {
      // [T N] <= [T N]
      const arraytype_t* l = (arraytype_t*)x;
      const arraytype_t* r = (arraytype_t*)y;
      return (
        r->kind == TYPE_ARRAY &&
        l->len == r->len &&
        type_compat(c, l->elem, r->elem, assignment) );
    }
  }

  return false;
}


bool type_isconvertible(const type_t* dst, const type_t* src) {
  dst = unwrap_alias_const(assertnotnull(dst));
  src = unwrap_alias_const(assertnotnull(src));

  if (type_isref(dst))
    dst = ((reftype_t*)dst)->elem;
  if (type_isref(src))
    src = ((reftype_t*)src)->elem;

  if (dst == src || (type_isprim(dst) && type_isprim(src)))
    return true;

  return false;
}


static void seterr(typecheck_t* a, err_t err) {
  if (!a->err)
    a->err = err;
}


static bool noerror(typecheck_t* a) {
  return (!a->err) & (compiler_errcount(a->compiler) == 0);
}


static const char* fmtnode(u32 bufindex, const void* nullable n) {
  buf_t* buf = tmpbuf_get(bufindex);
  err_t err = node_fmt(buf, n, /*depth*/0);
  safecheck(err == 0);
  return buf->chars;
}


inline static locmap_t* locmap(typecheck_t* a) {
  return &a->compiler->locmap;
}


// const origin_t to_origin(typecheck_t*, T origin)
// where T is one of: origin_t | loc_t | node_t* (default)
#define to_origin(a, origin) ({ \
  __typeof__(origin) __tmp1 = origin; \
  __typeof__(origin)* __tmp = &__tmp1; \
  const origin_t __origin = _Generic(__tmp, \
          origin_t*:  *(origin_t*)__tmp, \
    const origin_t*:  *(origin_t*)__tmp, \
          loc_t*:     origin_make(locmap(a), *(loc_t*)__tmp), \
    const loc_t*:     origin_make(locmap(a), *(loc_t*)__tmp), \
          default:    ast_origin(locmap(a), *(node_t**)__tmp) \
  ); \
  __origin; \
})


// void diag(typecheck_t*, T origin, diagkind_t diagkind, const char* fmt, ...)
// where T is one of: origin_t | loc_t | node_t* | expr_t*
#define diag(a, origin, diagkind, fmt, args...) \
  report_diag((a)->compiler, to_origin((a), (origin)), (diagkind), (fmt), ##args)

#define error(a, origin, fmt, args...) \
  ((a)->reported_error = true, diag((a), origin, DIAG_ERR, (fmt), ##args))
#define warning(a, origin, fmt, args...)  diag(a, origin, DIAG_WARN, (fmt), ##args)
#define help(a, origin, fmt, args...)     diag(a, origin, DIAG_HELP, (fmt), ##args)


static void out_of_mem(typecheck_t* a) {
  error(a, (origin_t){0}, "out of memory");
  seterr(a, ErrNoMem);
}


#define mknode(a, TYPE, kind)  ( (TYPE*)_mknode((a), sizeof(TYPE), (kind)) )


static node_t* _mknode(typecheck_t* a, usize size, nodekind_t kind) {
  node_t* n = ast_mknode(a->ast_ma, size, kind);
  if (!n)
    return out_of_mem(a), last_resort_node;
  return n;
}


static void transfer_nuse_to_wrapper(void* wrapper_node, void* wrapee_node) {
  node_t* wrapper = wrapper_node;
  node_t* wrapee = wrapee_node;
  wrapper->nuse = wrapee->nuse;
  wrapee->nuse -= (u32)!!wrapee->nuse;
}


static reftype_t* mkreftype(typecheck_t* a, type_t* elem, bool ismut) {
  reftype_t* t = mknode(a, reftype_t, ismut ? TYPE_MUTREF : TYPE_REF);
  t->flags = elem->flags & NF_CHECKED;
  t->size = a->compiler->target.ptrsize;
  t->align = t->size;
  t->elem = elem;
  transfer_nuse_to_wrapper(t, elem);
  return t;
}


static expr_t* mkderef(typecheck_t* a, expr_t* refval, loc_t loc) {
  unaryop_t* n = mknode(a, unaryop_t, EXPR_DEREF);
  n->op = OP_MUL;
  n->flags = refval->flags & (NF_RVALUE | NF_CHECKED);
  n->loc = loc;
  n->expr = refval;
  transfer_nuse_to_wrapper(n, refval);
  switch (refval->type->kind) {
    case TYPE_PTR:
    case TYPE_REF:
    case TYPE_MUTREF:
      n->type = ((ptrtype_t*)refval->type)->elem;
      break;
    default:
      n->type = type_void;
      assertf(0, "unexpected %s", nodekind_name(refval->type->kind));
  }
  return (expr_t*)n;
}


static expr_t* mkretexpr(typecheck_t* a, expr_t* value, loc_t loc) {
  retexpr_t* n = mknode(a, retexpr_t, EXPR_RETURN);
  n->flags = value->flags & NF_CHECKED;
  value->flags |= NF_RVALUE;
  n->loc = loc;
  n->value = value;
  n->type = value->type;
  transfer_nuse_to_wrapper(n, value);
  return (expr_t*)n;
}


// intern_usertype interns *tp in c->typeidmap.
// Returns true if *tp was added
static bool intern_usertype(typecheck_t* a, usertype_t** tp) {
  assert(nodekind_isusertype((*tp)->kind));

  typeid_t typeid = typeid_intern((type_t*)*tp);

  usertype_t** p = (usertype_t**)map_assign_ptr(&a->typeidmap, a->ma, typeid);
  if UNLIKELY(!p)
    return out_of_mem(a), false;

  if (*p) {
    if (*tp != *p) {
      // update caller's tp argument with existing type
      trace("[intern_usertype] dedup %s#%p %s",
        nodekind_name((*p)->kind), *p, fmtnode(0, *p));
      assert((*p)->kind == (*tp)->kind);
      *tp = *p;
    }
    return false;
  }

  // add type (or no-op, if *tp==*p)
  *p = *tp;

  #if 0
  if (opt_trace_typecheck) {
    buf_t tmpbuf = buf_make(a->ma);
    buf_appendrepr(&tmpbuf, typeid, typeid_len(typeid));
    trace("[intern_usertype] add %s#%p %s (typeid='%.*s')",
      nodekind_name((*tp)->kind), *tp, fmtnode(0, *tp),
      (int)tmpbuf.len, tmpbuf.chars);
    buf_dispose(&tmpbuf);
  }
  #else
    trace("[intern_usertype] add %s#%p %s",
      nodekind_name((*tp)->kind), *tp, fmtnode(0, *tp));
  #endif

  return true;
}


// true if constructing a type t has no side effects
static bool type_cons_no_side_effects(const type_t* t) { switch (t->kind) {
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_INT:
  case TYPE_U8:
  case TYPE_U16:
  case TYPE_U32:
  case TYPE_U64:
  case TYPE_UINT:
  case TYPE_F32:
  case TYPE_F64:
    return true;

  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_OPTIONAL:
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
  case TYPE_ARRAY:
    // all ptrtype_t types
    return type_cons_no_side_effects(((ptrtype_t*)t)->elem);

  case TYPE_ALIAS:
    return type_cons_no_side_effects(((aliastype_t*)t)->elem);

  // TODO: other types. E.g. check fields of struct
  default:
    dlog("TODO %s %s", __FUNCTION__, nodekind_name(t->kind));
    return false;
}}


// expr_no_side_effects returns true if materializing n has no side effects.
// I.e. if removing n has no effect on the semantic of any other code outside it.
//
// TODO: consider using a cached nodeflag for this.
// It can be a pretty expensive operation when n is a block, "if" or similar
// arbitrarily nested AST.
// Note that we only use this function during and after (cgen) typecheck,
// so caching should be safe. (Caching nodeflags during parsing is risky.)
//
bool expr_no_side_effects(const expr_t* n) { switch (n->kind) {
  case EXPR_ID:
  case EXPR_BOOLLIT:
  case EXPR_INTLIT:
  case EXPR_FLOATLIT:
    return true;

  case EXPR_MEMBER:
    return expr_no_side_effects(((member_t*)n)->recv);

  case EXPR_FIELD:
  case EXPR_PARAM:
  case EXPR_LET:
  case EXPR_VAR: {
    const local_t* local = (local_t*)n;
    return (
      type_cons_no_side_effects(local->type) &&
      ( !local->init || expr_no_side_effects(local->init) )
    );
  }

  case EXPR_ARRAYLIT: {
    const arraylit_t* alit = (arraylit_t*)n;
    bool no_side_effects = type_cons_no_side_effects(alit->type);
    for (u32 i = 0; no_side_effects && i < alit->values.len; i++)
      no_side_effects &= expr_no_side_effects((expr_t*)alit->values.v[i]);
    return no_side_effects;
  }

  case EXPR_BLOCK: {
    const block_t* block = (block_t*)n;
    bool no_side_effects = true;
    for (u32 i = 0; no_side_effects && i < block->children.len; i++)
      no_side_effects &= expr_no_side_effects((expr_t*)block->children.v[i]);
    return no_side_effects;
  }

  case EXPR_BINOP:
    return expr_no_side_effects(((binop_t*)n)->right) &&
           expr_no_side_effects(((binop_t*)n)->left);

  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP: {
    const unaryop_t* op = (unaryop_t*)n;
    if (op->op == OP_INC || op->op == OP_DEC)
      return false;
    return expr_no_side_effects(op->expr);
  }

  case EXPR_IF: {
    // TODO: consider thenb to have no side effects when cond is a constant
    // expression evaluating to "false".
    //   E.g. here "x++" has side effects but is never evaluated,
    //   thus the "thenb" has no side effects (since it's never visited):
    //     if 3 > 5 { x++ }
    //   However, note that in this example the "if" does have side effects,
    //   because "thenb" cause side effects:
    //     if 3 > 5 { x++ } else { x-- }
    const ifexpr_t* ife = (ifexpr_t*)n;
    return expr_no_side_effects(ife->cond) &&
           expr_no_side_effects((expr_t*)ife->thenb) &&
           (ife->elseb == NULL || expr_no_side_effects((expr_t*)ife->elseb));
  }

  case EXPR_RETURN:
    return ((retexpr_t*)n)->value == NULL ||
           expr_no_side_effects(((retexpr_t*)n)->value);

  case EXPR_CALL:
    return false;

  case EXPR_FUN: {
    const fun_t* fn = (fun_t*)n;
    const funtype_t* ft = (funtype_t*)fn->type;
    if (!ft)
      return false; // incomplete
    // check parameter initializers, e.g. "fun f(x=sideeffect())"
    for (u32 i = 0; i < ft->params.len; i++) {
      const local_t* param = (local_t*)ft->params.v[i];
      if (param->init && !expr_no_side_effects(param->init))
        return false;
    }
    if (fn->body)
      return expr_no_side_effects((expr_t*)fn->body);
    return false;
  }

  // TODO: other kinds
  default:
    dlog("TODO %s %s", __FUNCTION__, nodekind_name(n->kind));
    return false;
}}


static void error_incompatible_types(
  typecheck_t* a, const void* nullable origin_node, const type_t* x, const type_t* y)
{
  const char* in_descr = origin_node ? fmtkind(origin_node) : NULL;
  error(a, origin_node, "incompatible types %s and %s%s%s",
    fmtnode(0, x), fmtnode(1, y), in_descr ? " in " : "", in_descr);
}


static void error_unassignable_type(
  typecheck_t* a, const void* dst_expr, const expr_t* src)
{
  const expr_t* dst = dst_expr;
  const expr_t* origin = dst;
  if (node_islocal((node_t*)dst)) {
    const local_t* local = (local_t*)dst;
    if (loc_line(assertnotnull(local->init)->loc))
      origin = local->init;
  }

  // check if the source's type has been narrowed, e.g. from optional check
  if (
    (src->flags & NF_NARROWED) ||
    (src->kind == EXPR_ID && ((idexpr_t*)src)->ref &&
      (((idexpr_t*)src)->ref->flags & NF_NARROWED)))
  {
    error(a, src, "optional value %s is empty here", fmtnode(0, src));
    return;
  }

  type_t* srctype = src->type;

  // check if destination is a narrowed local
  if ((dst->flags & NF_NARROWED) && srctype->kind == TYPE_OPTIONAL) {
    // instead of "cannot assign value of type ?int to binding of type i8"
    // say        "cannot assign value of type int to binding of type i8"
    // for e.g.
    //   fun example(a ?int)
    //     if let x i8 = a   <——
    //
    srctype = ((opttype_t*)srctype)->elem;
  }

  error(a, origin, "cannot assign value of type %s to %s of type %s",
    fmtnode(0, srctype), fmtkind(dst), fmtnode(1, dst->type));
}


static void typectx_push(typecheck_t* a, type_t* t) {
  // if (t->kind == TYPE_UNKNOWN)
  //   t = type_void;
  trace("typectx [%u] %s -> %s",
    a->typectxstack.len, fmtnode(0, a->typectx), fmtnode(1, t));
  if UNLIKELY(!ptrarray_push(&a->typectxstack, a->ma, a->typectx))
    out_of_mem(a);
  a->typectx = t;
}

static void typectx_pop(typecheck_t* a) {
  assert(a->typectxstack.len > 0);
  type_t* t = ptrarray_pop(&a->typectxstack);
  trace("typectx [%u] %s <- %s",
    a->typectxstack.len, fmtnode(1, t), fmtnode(0, a->typectx));
  a->typectx = t;
}


static void enter_scope(typecheck_t* a) {
  if (!scope_push(&a->scope, a->ma))
    out_of_mem(a);
  trace("enter scope #%u", scope_level(&a->scope));
}


static void leave_scope(typecheck_t* a) {
  trace("leave scope #%u", scope_level(&a->scope));
  scope_pop(&a->scope);
}


static void enter_ns(typecheck_t* a, void* node) {
  if UNLIKELY(!ptrarray_push(&a->nspath, a->ma, node))
    out_of_mem(a);
}


static void leave_ns(typecheck_t* a) {
  ptrarray_pop(&a->nspath);
}


static node_t* nullable lookup(typecheck_t* a, sym_t name) {
  assert(name != sym__);
  node_t* n = scope_lookup(&a->scope, name, U32_MAX);
  trace("lookup \"%s\" in scope => %s", name, n ? nodekind_name(n->kind) : "(null)");
  if (!n) {
    if (!( n = pkg_def_get(a->pkg, name) )) {
      trace("lookup \"%s\" in pkg => (null)", name);
      return NULL;
    }
    trace("lookup \"%s\" in pkg => %s", name, nodekind_name(n->kind));

    // mark the node as being used across translations units of the same package
    node_upgrade_visibility(n, NF_VIS_PKG);

    // // define in current scope to reduce number of package lookups
    // if (!scope_define(&a->scope, a->ma, name, n))
    //   out_of_mem(a);
  }
  return use(n);
}


static void define(typecheck_t* a, sym_t name, void* n) {
  if (name == sym__)
    return;

  trace("define \"%s\" => %s (%s)", name, fmtnode(0, n),
    node_isexpr(n) ? fmtnode(1, ((expr_t*)n)->type) : "");

  #if DEBUG
    node_t* existing = scope_lookup(&a->scope, name, 0);
    if UNLIKELY(existing) {
      error(a, n, "duplicate definition \"%s\"", name);
      if (loc_line(existing->loc))
        warning(a, existing, "\"%s\" previously defined here", name);
      assertf(0, "duplicate definition \"%s\"", name);
    }
  #endif

  if (!scope_define(&a->scope, a->ma, name, n))
    out_of_mem(a);
}


static void _type(typecheck_t* a, type_t** tp);
static void stmt(typecheck_t* a, stmt_t* n);
static void exprp(typecheck_t* a, expr_t** np);
#define expr(a, n)  exprp(a, (expr_t**)&(n))

inline static void type(typecheck_t* a, type_t** tp) {
  if ( *tp != type_unknown && ((*tp)->flags & NF_CHECKED) == 0 )
    _type(a, tp);
}


static void implicit_rvalue_deref(typecheck_t* a, const type_t* ltype, expr_t** rvalp) {
  expr_t* rval = *rvalp;
  ltype = unwrap_alias_const(ltype);
  const type_t* rtype = unwrap_alias(rval->type);

  // dlog("%s ltype: %s (kind %s, isreflike %d), rtype: %s (kind %s, isreflike %d)",
  //   __FUNCTION__,
  //   fmtnode(0, ltype), nodekind_name(ltype->kind), type_isreflike(ltype),
  //   fmtnode(1, rtype), nodekind_name(rtype->kind), type_isreflike(rtype) );

  if (!type_isreflike(ltype) && type_isreflike(rtype))
    *rvalp = mkderef(a, rval, rval->loc);
}


static bool name_is_reserved(sym_t name) {
  return (
    *name == CO_ABI_GLOBAL_PREFIX[0] &&
    strlen(name) >= strlen(CO_ABI_GLOBAL_PREFIX) &&
    memcmp(CO_ABI_GLOBAL_PREFIX, name, strlen(CO_ABI_GLOBAL_PREFIX)) == 0 );
}


static bool report_unused(typecheck_t* a, const void* expr_node) {
  assert(node_isexpr(expr_node));
  const expr_t* n = expr_node;

  switch (n->kind) {
    case EXPR_FIELD:
    case EXPR_PARAM:
    case EXPR_LET:
    case EXPR_VAR: {
      local_t* var = (local_t*)n;
      if (var->name != sym__ && !name_is_reserved(var->name) && noerror(a)) {
        warning(a, var->nameloc, "unused %s %s", fmtkind(n), var->name);
        return true;
      }
      return false;
    }
    case EXPR_IF:
      if ((n->flags & NF_RVALUE) == 0)
        return false;
      break; // report
    default:
      if (!expr_no_side_effects(n))
        return false;
      break; // report
  }

  if (noerror(a)) {
    warning(a, n, "unused %s %s", fmtkind(n), fmtnode(0, n));
    return true;
  }
  return false;
}


static void block_noscope(typecheck_t* a, block_t* n) {
  TRACE_NODE(a, "", &n);

  u32 count = n->children.len;
  stmt_t** stmtv = (stmt_t**)n->children.v;

  if (count == 0) {
    n->type = type_void;
    return;
  }

  // if block is rvalue, last expression is the block's value, analyzed separately
  u32 stmt_end = count;
  stmt_end -= (u32)( (n->flags & NF_RVALUE) && stmtv[count-1]->kind != EXPR_RETURN );

  for (u32 i = 0; i < stmt_end; i++) {
    stmt_t* cn = stmtv[i];
    stmt(a, cn);

    if (cn->kind == EXPR_RETURN) {
      // mark remaining expressions as unused
      // note: parser reports diagnostics about unreachable code
      for (i++; i < count; i++)
        ((node_t*)stmtv[i])->nuse = 0;
      stmt_end = count; // avoid rvalue branch later on
      n->type = ((expr_t*)cn)->type;
      n->flags |= NF_EXIT;
      break;
    }
  }

  // we are done if block is not an rvalue or contains an explicit "return" statement
  if (stmt_end == count)
    goto end;

  // if the block is rvalue, treat last entry as implicitly-returned expression
  expr_t* lastexpr = (expr_t*)stmtv[stmt_end];
  assert(nodekind_isexpr(lastexpr->kind));
  lastexpr->flags |= NF_RVALUE;

  exprp(a, (expr_t**)&stmtv[stmt_end]);
  lastexpr = (expr_t*)stmtv[stmt_end]; // reload; expr might have edited

  incuse(lastexpr);
  n->type = lastexpr->type;

end:
  // report unused expressions
  for (u32 i = 0; i < stmt_end; i++) {
    stmt_t* cn = stmtv[i];
    if UNLIKELY(cn->nuse == 0 && nodekind_isexpr(cn->kind)) {
      if (report_unused(a, cn))
        break; // stop after the first reported diagnostic
    }
  }
}


static void block(typecheck_t* a, block_t* n) {
  enter_scope(a);
  block_noscope(a, n);
  leave_scope(a);
}


static void this_type(typecheck_t* a, local_t* local) {
  type_t* recvt = local->type;
  // pass certain types by value instead of pointer when access is read-only
  if (!local->ismut) {
    if (nodekind_isprimtype(recvt->kind)) // e.g. int, i32
      return;
    if (recvt->kind == TYPE_STRUCT) {
      // small structs
      structtype_t* st = (structtype_t*)recvt;
      u64 maxsize = (u64)a->compiler->target.ptrsize * 2;
      if ((u32)st->align <= a->compiler->target.ptrsize && st->size <= maxsize)
        return;
    }
  }
  // pointer type
  reftype_t* t = mkreftype(a, recvt, local->ismut);
  local->type = (type_t*)t;
}


static void local(typecheck_t* a, local_t* n) {
  assertf(n->nuse == 0 || n->name != sym__, "'_' local that is somehow used");

  type(a, &n->type);

  if (n->init) {
    typectx_push(a, n->type);
    exprp(a, &n->init);
    typectx_pop(a);

    if (n->type == type_unknown || n->type->kind == TYPE_UNRESOLVED) {
      n->type = n->init->type;
    } else {
      type_t* rtype = n->init->type;
      if ((n->flags & NF_NARROWED) && n->type != type_void) {
        // handle type narrowed local, e.g.
        //   fun example(a ?int)
        //     if let x = a  <—— "let x" is of type "int" but a is "?int"
        //       x           <—— refs to "let x" are "int"
        //
        assert(rtype->kind == TYPE_OPTIONAL);
        rtype = ((opttype_t*)rtype)->elem;
      }
      if UNLIKELY(!type_isassignable(a->compiler, n->type, rtype)) {
        error_unassignable_type(a, n, n->init);
      } else {
        implicit_rvalue_deref(a, n->type, &n->init);
      }
    }
  }

  if (n->isthis)
    this_type(a, n);

  // // field, param and var are mutable (but "let" isn't; see local_var)
  // n->flags |= NF_MUT;

  if UNLIKELY(
    (n->type == type_void || n->type == type_unknown) &&
    (n->flags & NF_NARROWED) == 0)
  {
    error(a, n, "cannot define %s of type void", fmtkind(n));
  }

  if (n->name == sym__ && type_isowner(n->type)) {
    // owners require var names for ownership tracking
    // FIXME: this is a pretty janky hack which is rooted in the fact that
    //        IR-based ownership analysis tracks variable _names_.
    char buf[strlen("__co_varFFFFFFFFFFFFFFFF")+1];
    n->name = sym_snprintf(buf, sizeof(buf), "__co_var%lx", (unsigned long)n);
  }
}


static void local_var(typecheck_t* a, local_t* n) {
  assert(nodekind_isvar(n->kind));
  bool need_def = (n->flags & NF_UNKNOWN) || n->type == type_unknown;
  local(a, n);
  // n->flags = COND_FLAG(n->flags, NF_MUT, n->kind == EXPR_VAR);
  if (need_def || scope_istoplevel(&a->scope))
    define(a, n->name, n);
}


// check_local can be called directly, bypassing the general expr() function
static void check_local(typecheck_t* a, local_t* n) {
  if CHECK_ONCE(n) {
    #ifdef DEBUG
      trace("%s \"%s\" :", nodekind_name(n->kind), n->name);
      a->traceindent++;
    #endif

    local(a, n);

    #ifdef DEBUG
      a->traceindent--;
    #endif
  }
  trace("%s \"%s\" => %s %s",
    nodekind_name(n->kind), n->name,
    nodekind_name(n->kind), fmtnode(0, n));
}


static void structtype(typecheck_t* a, structtype_t** tp) {
  structtype_t* st = *tp;

  if (!st->nsparent)
    st->nsparent = a->nspath.v[a->nspath.len - 1];

  u8  align = 0;
  u64 size = 0;

  enter_ns(a, st);

  for (u32 i = 0; i < st->fields.len; i++) {
    local_t* f = (local_t*)st->fields.v[i];

    check_local(a, f);
    assertnotnull(f->type);

    if (type_isowner(f->type)) {
      // note: this is optimistic; types aren't marked "DROP" until a
      // custom drop function is implemented, so at this point f->type
      // may be "not owner" since we haven't visited it's drop function yet.
      // e.g.
      //   type A {}
      //   type B { a A }         <—— currently checking B
      //   fun A.drop(mut this){} <—— not yet visited
      // For this reason, we add struct types to a.postanalyze later on.
      st->flags |= NF_SUBOWNERS;
    }

    type_t* t = concrete_type(a->compiler, f->type);
    f->offset = ALIGN2(size, t->align);
    size = f->offset + t->size;
    align = MAX(align, t->align); // alignment of struct is max alignment of fields

    // // check for internal types leaking from public ones
    // if UNLIKELY(a->pubnest && (f->type->flags & NF_VIS_PUB) == 0 &&
    //             f->type->kind != TYPE_PLACEHOLDER)
    // {
    //   error(a, f, "internal type %s of field %s in public struct",
    //     fmtnode(0, f->type), f->name);
    //   help(a, f->type, "mark %s `pub`", fmtnode(0, f->type));
    // }
  }

  leave_ns(a);

  st->align = align;
  st->size = ALIGN2(size, (u64)align);

  // if (st->flags & NF_TEMPLATEI) {
  if (!intern_usertype(a, (usertype_t**)tp))
    return;
  // }

  if (!(st->flags & NF_SUBOWNERS)) {
    if UNLIKELY(!map_assign_ptr(&a->postanalyze, a->ma, *tp))
      out_of_mem(a);
  }
}


static void arraytype_calc_size(typecheck_t* a, arraytype_t* at) {
  if (at->len == 0) {
    // type darray<T> {cap, len uint; rawptr T ptr }
    at->align = MAX(a->compiler->target.ptrsize, a->compiler->target.intsize);
    at->size = a->compiler->target.intsize*2 + a->compiler->target.ptrsize;
    return;
  }
  u64 size;
  if (check_mul_overflow(at->len, at->elem->size, &size)) {
    error(a, at, "array constant too large; overflows uint (%s)",
      fmtnode(0, a->compiler->uinttype));
    return;
  }
  at->align = at->elem->align;
  at->size = size;
}


static void arraytype(typecheck_t* a, arraytype_t** tp) {
  arraytype_t* at = *tp;

  type(a, &at->elem);

  if (type_isowner(at->elem))
    at->flags |= NF_SUBOWNERS;

  if (at->lenexpr) {
    typectx_push(a, type_uint);
    expr(a, at->lenexpr);
    typectx_pop(a);

    if (compiler_errcount(a->compiler) > 0)
      return;

    // note: comptime_eval_uint has already reported the error when returning false
    if (!comptime_eval_uint(a->compiler, at->lenexpr, /*flags*/0, &at->len))
      return;

    if UNLIKELY(at->len == 0 && compiler_errcount(a->compiler) == 0)
      error(a, at, "zero length array");
  }

  // check for internal types leaking from public ones
  if UNLIKELY(a->pubnest) {
    // if ((at->elem->flags & NF_VIS_PUB) == 0) {
    //   error(a, at, "public array type of internal subtype %s",
    //     fmtnode(0, at->elem));
    //   help(a, at->elem, "mark %s `pub`", fmtnode(0, at->elem));
    // }
    node_set_visibility((node_t*)at, NF_VIS_PUB);
  }

  //assertf(at->_typeid == NULL, "%s", fmtnode(0, at));
  arraytype_calc_size(a, at);
  intern_usertype(a, (usertype_t**)tp);
}


static void funtype1(typecheck_t* a, funtype_t** np, type_t* thistype) {
  funtype_t* ft = *np;
  typectx_push(a, thistype);
  for (u32 i = 0; i < ft->params.len; i++) {
    check_local(a, (local_t*)ft->params.v[i]);

    // check for internal types leaking from public function
    local_t* param = (local_t*)ft->params.v[i];
    if UNLIKELY(a->pubnest && (param->type->flags & NF_VIS_PUB) == 0) {
      error(a, param, "parameter of internal type %s in public function",
        fmtnode(0, param->type));
      help(a, param->type, "mark %s `pub`", fmtnode(0, param->type));
    }
  }
  type(a, &ft->result);
  typectx_pop(a);
  // TODO: consider NOT interning function types with parameters that have initializers
  intern_usertype(a, (usertype_t**)np);
}


static void funtype(typecheck_t* a, funtype_t** np) {
  return funtype1(a, np, type_unknown);
}


static type_t* check_retval(typecheck_t* a, const void* originptr, expr_t*nullable* np) {
  assertnotnull(a->fun);
  funtype_t* ft = (funtype_t*)a->fun->type;

  type_t* t;
  if (*np) {
    use(*np);
    exprp(a, np);
    expr_t* n = *np;
    t = n->type;
  } else {
    t = type_void;
  }

  if UNLIKELY(!type_isassignable(a->compiler, ft->result, t)) {
    const node_t* origin = originptr;
    if (ft->result == type_void) {
      error(a, origin, "function %s%sdoes not return a value",
        a->fun->name ? a->fun->name : "",
        a->fun->name ? " " : "");
    } else {
      if (t == type_void) {
        loc_t loc = origin->loc;
        if (origin->kind == EXPR_BLOCK)
          loc = ((block_t*)origin)->endloc;
        error(a, loc, "missing return value");
      } else if (t != type_unknown || !a->reported_error) {
        error(a, origin, "invalid function result type: %s", fmtnode(0, t));
      }
      if (loc_line(a->fun->resultloc) && (t != type_unknown || !a->reported_error)) {
        help(a, a->fun->resultloc, "function %s%sreturns %s",
          (a->fun->name ? a->fun->name : ""), (a->fun->name ? " " : ""),
          fmtnode(1, ft->result));
      }
    }
  }

  if (*np) {
    implicit_rvalue_deref(a, ft->result, np);
    return (*np)->type;
  }
  return type_void;
}


static void main_fun(typecheck_t* a, fun_t* n) {
  a->pkg->mainfun = n;

  funtype_t* ft = (funtype_t*)n->type;

  // there should be no input parameters
  if UNLIKELY(ft->params.len > 0) {
    error(a, fun_params_origin(locmap(a), n),
      "special \"main\" function should not accept any input parameters");
  }

  // there should be no output result
  if UNLIKELY(ft->result != type_void) {
    error(a, n->resultloc, "special \"main\" function should not return a result");
  }
}


static void fun(typecheck_t* a, fun_t* n) {
  fun_t* outer_fun = a->fun;
  a->fun = n;
  a->pubnest += (u32)!!(n->flags & NF_VIS_PUB);

  if (n->recvt) {
    // type function
    type(a, &n->recvt);
    if (!n->nsparent)
      n->nsparent = (node_t*)n->recvt;
    enter_ns(a, n->recvt);
  } else {
    // plain function
    if (!n->nsparent) {
      n->nsparent = a->nspath.v[a->nspath.len - 1];
      if (n->name)
        define(a, n->name, n);
    }
  }

  // first, check function type
  if CHECK_ONCE(n->type) {
    type_t* thistype = n->recvt ? n->recvt : type_unknown;
    funtype1(a, (funtype_t**)&n->type, thistype);
  }

  funtype_t* ft = (funtype_t*)n->type;
  assert(ft->kind == TYPE_FUN);

  // parameters
  if (ft->params.len > 0) {
    enter_scope(a);
    for (u32 i = 0; i < ft->params.len; i++) {
      local_t* param = (local_t*)ft->params.v[i];
      if CHECK_ONCE(param) {
        expr(a, param);
      } else if (n->body && param->name != sym__) {
        // Must define in scope, even if we have checked param already.
        // This can happen because multiple functions with the same signatue
        // may share one funtype_t, which holds the parameters.
        define(a, param->name, param);
      }
    }
  }

  // result type
  type(a, &ft->result);

  // check signature of special "drop" function.
  // basically a "poor person's drop trait."
  if (n->recvt && n->name == sym_drop) {
    bool ok = false;
    if (ft->result == type_void && ft->params.len == 1) {
      local_t* param0 = (local_t*)ft->params.v[0];
      ok = param0->type->kind == TYPE_MUTREF;
      if (ok)
        n->recvt->flags |= NF_DROP;
    }
    if (!ok)
      error(a, n, "invalid signature of \"drop\" function, expecting (mut this)void");
  }

  // body
  if (n->body) {
    // If the function returns a value, mark the block as rvalue.
    // This causes block_noscope() to treat the last expression specially.
    n->body->flags = COND_FLAG(n->body->flags, NF_RVALUE, ft->result != type_void);

    // visit body
    enter_ns(a, n);
      typectx_push(a, ft->result);
        block(a, n->body);
      typectx_pop(a);
    leave_ns(a);

    // handle implicit return
    if (ft->result != type_void && (n->body->flags & NF_EXIT) == 0) {
      // function body should return a value, but block does not contain "return";
      // the last expression is converted to a "return" statement.
      if UNLIKELY(n->body->children.len == 0) {
        // error will be reported by check_retval
        expr_t* lastexpr = NULL;
        check_retval(a, n->body, &lastexpr);
      } else {
        expr_t** lastexpr = (expr_t**)&n->body->children.v[n->body->children.len - 1];
        check_retval(a, *lastexpr, lastexpr);
        *lastexpr = mkretexpr(a, *lastexpr, (*lastexpr)->loc);
      }
    }

    // is this the "main" function?
    if (ast_is_main_fun(n))
      main_fun(a, n);
  } else {
    node_upgrade_visibility((node_t*)n, NF_VIS_PKG);
  }

  if (n->recvt)
    leave_ns(a);

  if (ft->params.len > 0)
    scope_pop(&a->scope);

  a->pubnest -= (u32)!!(n->flags & NF_VIS_PUB);
  a->fun = outer_fun;
}


static bool type_narrow_error_find_local(expr_t* x, local_t** lp, op_t* op) {
  switch (x->kind) {
  case EXPR_VAR:
  case EXPR_LET:
    if (!*lp && (x->flags & NF_NARROWED))
      *lp = (local_t*)x;
    break;
  case EXPR_PREFIXOP:
    if (((unaryop_t*)x)->op == OP_NOT && !*op)
      *op = ((unaryop_t*)x)->op;
    break;
  case EXPR_BINOP:
    if (((binop_t*)x)->op == OP_LOR && !*op)
      *op = ((binop_t*)x)->op;
    break;
  }
  if (*lp && *op)
    return true;
  ast_childit_t it = ast_childit_const((node_t*)x);
  for (const node_t* cn; (cn = ast_childit_const_next(&it));) {
    if (!node_isexpr(cn))
      continue;
    if (type_narrow_error_find_local((expr_t*)cn, lp, op))
      return true;
  }
  return false;
}


static bool type_narrow_error_localdef_mix(compiler_t* c, expr_t* cond) {
  local_t* l = NULL;
  op_t op = 0;
  type_narrow_error_find_local(cond, &l, &op);
  assertnotnull(l);
  assert(op > 0);
  report_diag(c, ast_origin(&c->locmap, (node_t*)l), DIAG_ERR,
    "cannot use type-narrowing %s definition with '%s' operation",
    l->kind == EXPR_VAR ? "var" : "let", op_fmt(op));
  return false;
}


static bool type_narrow_cond1(
  compiler_t* c, memalloc_t ast_ma, scope_t* scope, u32* flags, expr_t* x)
{
  // negation is a little tricky
  // e.g.
  //   fun example1(a, b ?int)
  //     if !(a && !b) { /* a is void, b is int */ }
  //     else          { /* a is int, b is void */ }
  //   fun example2(a, b ?int)
  //     if (a && !b)  { /* a is int, b is void */ }
  //     else          { /* a is void, b is int */ }
  //
  // We temporarily mark optional-type expressions with NF_MARK1 to denote "negative".
  // E.g. !a sets NF_MARK1 on a, !!a sets NF_MARK1 on a and then clears it.
  //
  // TODO: handle mutually negated identifier, e.g.
  //   fun example2(a ?int)
  //     if (a || !a) { /* a is void */ }
  //     else         { /* a is void */ }
  //
  switch (x->kind) {

  case EXPR_PREFIXOP: {
    unaryop_t* n = (unaryop_t*)x;
    if (n->op != OP_NOT)
      break;
    *flags |= 1; // has complex op
    //dlog(">> %s [%s] %s", nodekind_name(n->kind), fmtnode(0, n->type), fmtnode(1, n));
    u32 scope_len = scope->len;
    if (!type_narrow_cond1(c, ast_ma, scope, flags, n->expr))
      return false;
    for (u32 i = scope->len; i > scope_len;) {
      --i; //sym_t name = scope->ptr[--i];
      node_t* n2 = scope->ptr[--i];
      if (n2->flags & NF_NARROWED) {
        // toggle "negative" flag
        n2->flags ^= NF_MARK1;
        //dlog("- negating definition \"%s\" %s", name, fmtnode(0, n2));
      }
    }
    break;
  }

  case EXPR_BINOP: {
    binop_t* n = (binop_t*)x;
    if (n->op != OP_LAND && n->op != OP_LOR)
      break;
    if (n->op == OP_LOR)
      *flags |= 1; // has complex op
    //dlog(">> %s [%s] %s", nodekind_name(n->kind), fmtnode(0, n->type), fmtnode(1, n));
    if (!type_narrow_cond1(c, ast_ma, scope, flags, n->left))
      return false;
    if (!type_narrow_cond1(c, ast_ma, scope, flags, n->right))
      return false;
    break;
  }

  case EXPR_ID: {
    idexpr_t* n = (idexpr_t*)x;
    if (n->type->kind != TYPE_OPTIONAL || (n->flags & NF_NARROWED)) {
      // NF_NARROWED = already narrowed by previous pass
      break;
    }
    n->flags |= NF_NARROWED;
    //dlog(">> %s [%s] %s", nodekind_name(n->kind), fmtnode(0, n->type), fmtnode(1, n));
    local_t* n2 = scope_lookup(scope, n->name, /*maxdepth*/0);
    if (!n2 || n2->kind != n->kind || !(n2->flags & NF_NARROWED)) {
      safecheck(node_islocal(n->ref));
      n2 = ast_clone_node(ast_ma, (local_t*)assertnotnull(n->ref));
      if (!n2) return false;
      n2->flags |= NF_NARROWED;
      tracex("define \"%s\" => %s (%s)", n2->name, fmtnode(0, n2), fmtnode(1, n2->type));
      if (!scope_define(scope, c->ma, n2->name, n2))
        return false;
    }
    //else dlog("existing \"%s\" %s", n->name, fmtnode(0, n2));
    break;
  }

  case EXPR_VAR:
  case EXPR_LET: {
    local_t* n = (local_t*)x;
    if ((n->flags & NF_NARROWED) || // already narrowed by previous pass
        n->type->kind == TYPE_UNKNOWN ||
        ( n->type->kind != TYPE_OPTIONAL &&
          ( n->init == NULL ||
            ( n->init->type &&
              n->init->type->kind != TYPE_OPTIONAL &&
              n->init->type->kind != TYPE_UNKNOWN
            )
          )
        ) )
    {
      break;
    }
    *flags |= 2; // has local definition
    n->flags |= NF_NARROWED | NF_MARK2;
    //dlog(">> %s [%s] %s", nodekind_name(n->kind), fmtnode(0, n->type), fmtnode(1, n));
    tracex("define \"%s\" => %s (%s)", n->name, fmtnode(0, n), fmtnode(1, n->type));
    if (!scope_define(scope, c->ma, n->name, n))
      return false;
    break;
  }

  default: {
    if (x->type->kind != TYPE_OPTIONAL)
      break;
    //dlog(">> %s [%s] %s", nodekind_name(x->kind), fmtnode(0, x->type), fmtnode(1, x));
    break;
  }
  }
  return true;
}


bool type_narrow_cond(
  compiler_t*           c,
  memalloc_t            ast_ma,
  scope_t*              scope,
  nodearray_t* nullable elsedefs,
  expr_t*               cond)
{
  u32 scope_len = scope->len;
  u32 flags = 0;

  if (!type_narrow_cond1(c, ast_ma, scope, &flags, cond))
    return false;

  if UNLIKELY(flags == (1 | 2))
    return type_narrow_error_localdef_mix(c, cond);

  if (elsedefs && scope->len > scope_len) {
    if (!nodearray_reserve_exact(elsedefs, c->ma, scope->len - scope_len))
      return false;
  }

  for (u32 i = scope->len; i > scope_len;) {
    sym_t name = scope->ptr[--i];
    expr_t* n = scope->ptr[--i];
    if ((n->flags & NF_NARROWED) == 0)
      continue;

    bool isneg = (n->flags & NF_MARK1) != 0;
    bool islocal = (n->flags & NF_MARK2) != 0;
    n->flags &= ~(NF_MARK1 | NF_MARK2);

    // optional type is found either on the local or the initializer. e.g.
    //   fun example(a ?int)
    //     if let x = a ...  // local's type is opttype_t
    //     if let x int = a  // local's type is int; use init type
    //
    type_t* oktype = n->type;
    if (oktype->kind != TYPE_OPTIONAL) {
      assert(node_islocal((node_t*)n));
      oktype = assertnotnull(((local_t*)n)->init)->type;
      assertf(oktype->kind == TYPE_OPTIONAL, "%s", nodekind_name(oktype->kind));
    }
    oktype = ((opttype_t*)oktype)->elem;

    if (islocal) {
      // check assignable type of local definition
      local_t* var = (local_t*)n;
      if (var->type->kind == TYPE_UNRESOLVED) {
        // Type is not yet known; must retain the type in this case.
        // This can only happen during the first pass of type_narrow_cond run by parser.
        oktype = var->type;
      } else if UNLIKELY(
        var->type != type_unknown &&
        !type_isassignable(c, var->type, oktype))
      {
        report_diag(c, ast_origin(&c->locmap, (node_t*)var->init), DIAG_ERR,
          "cannot assign value of type %s to %s of type %s",
          fmtnode(0, oktype), fmtkind(var), fmtnode(1, var->type));
      }
    } else if (elsedefs) {
      // Add the inverse to the "else" definitions.
      // E.g. "(x ?int) if x ..." => "x int" in "then" branch, "x void" in "else" branch
      expr_t* n2 = ast_clone_node(ast_ma, n);
      if (!n2) return false;
      // narrow type of n2 (inversely)
      n2->type = isneg ? oktype : type_void;
      elsedefs->v[elsedefs->len++] = (node_t*)n2;
      dlog("type_narrow 'else' \"%s\" %s (%c)", name, fmtnode(0, n2), isneg?'+':'-');
    }

    // narrow type of n
    n->type = isneg ? type_void : oktype;
    dlog("type_narrow 'then' \"%s\" %s (%c)", name, fmtnode(0, n), isneg?'-':'+');
  }

  return true;
}


bool type_narrow_elsedefs(compiler_t* c, scope_t* scope, const nodearray_t* elsedefs) {
  for (u32 i = 0; i < elsedefs->len; i++) {
    expr_t* n = (expr_t*)elsedefs->v[i];
    sym_t name;
    if (nodekind_islocal(n->kind)) {
      name = ((local_t*)n)->name;
    } else {
      assert_nodekind(n, EXPR_ID);
      name = ((idexpr_t*)n)->name;
    }
    if (!scope_define(scope, c->ma, name, n))
      return false;
  }
  return true;
}


static void ifexpr(typecheck_t* a, ifexpr_t* n) {
  bool cond_has_unkn = n->cond->flags & NF_UNKNOWN;

  // enter "then" scope
  enter_scope(a);

  // condition
  assert(n->cond->flags & NF_RVALUE);
  use(n->cond);
  expr(a, n->cond);

  nodearray_t elsedefs = {0};
  if (cond_has_unkn) {
    nodearray_t* elsedefsp = n->elseb ? &elsedefs : NULL;
    if (!type_narrow_cond(a->compiler, a->ast_ma, &a->scope, elsedefsp, n->cond)) {
      nodearray_dispose(&elsedefs, a->ma);
      return;
    }
  }

  if UNLIKELY(
    !(n->cond->flags & NF_NARROWED) &&
    !type_isbool(n->cond->type) &&
    !type_isopt(n->cond->type) )
  {
    return error(a, n->cond, "conditional is not a boolean nor an optional type");
  }

  // "then" branch
  n->thenb->flags |= (n->flags & NF_RVALUE); // "then" block is rvalue if "if" is
  block_noscope(a, n->thenb);
  leave_scope(a);

  // "else" branch
  if (n->elseb) {
    // visit "else" branch
    enter_scope(a);
    if (!type_narrow_elsedefs(a->compiler, &a->scope, &elsedefs))
      return out_of_mem(a);
    n->elseb->flags |= (n->flags & NF_RVALUE); // "else" block is rvalue if "if" is
    block_noscope(a, n->elseb);
    leave_scope(a);
  }

  nodearray_dispose(&elsedefs, a->ma);

  // if (type_narrowed_binding) {
  //   expr_t* dst = n->cond;
  //   while (dst->kind == EXPR_ID && node_isexpr(((idexpr_t*)dst)->ref))
  //     dst = (expr_t*)((idexpr_t*)dst)->ref;
  //   dst->nuse += type_narrowed_binding->nuse;
  // }

  // unless the "if" is used as an rvalue, we are done
  if ((n->flags & NF_RVALUE) == 0) {
    n->type = type_void;
    return;
  }

  if (n->elseb && n->elseb->type != type_void) {
    // "if ... else" => T
    n->type = n->thenb->type;
    if UNLIKELY(!type_isassignable(a->compiler, n->thenb->type, n->elseb->type)) {
      // TODO: type union
      if (n->thenb->type->kind != TYPE_UNKNOWN && n->elseb->type->kind != TYPE_UNKNOWN) {
        const char* t1 = fmtnode(0, n->thenb->type);
        const char* t2 = fmtnode(1, n->elseb->type);
        error(a, n->elseb, "incompatible types %s and %s in \"if\" branches", t1, t2);
      }
    }
  } else {
    // "if" => ?T
    n->type = n->thenb->type;
    if (n->type->kind != TYPE_OPTIONAL) {
      opttype_t* t = mknode(a, opttype_t, TYPE_OPTIONAL);
      t->elem = n->type;
      n->type = (type_t*)t;
    }
  }
}


static didyoumean_t* didyoumean_add(
  typecheck_t* a, sym_t name, node_t* decl, sym_t nullable othername)
{
  didyoumean_t* dym = array_alloc(didyoumean_t, (array_t*)&a->didyoumean, a->ma, 1);
  if UNLIKELY(!dym) {
    if (a->didyoumean.len > 0)
      return &a->didyoumean.v[0];
    static didyoumean_t last_resort = {0};
    return &last_resort;
  }
  dym->name = name;
  dym->othername = othername;
  dym->decl = decl;
  return dym;
}


typedef struct {
  sym_t         name;
  const node_t* n;
  int           edit_dist;
} fuzzyent_t;

typedef struct {
  sym_t      name; // name that we are looking for
  memalloc_t ma;
  array_type(fuzzyent_t) entries;
} fuzzy_t;


static int fuzzyent_cmp(const fuzzyent_t* a, const fuzzyent_t* b, void* ctx) {
  return a->name == b->name ? 0 : a->name < b->name ? -1 : 1;
}


// return 1 if added, 0 if already registered, <0 on error (an err_t)
static int fuzzy_add_candidate(fuzzy_t* fz, sym_t name, const node_t* n) {
  fuzzyent_t lookup = { .name = name };
  fuzzyent_t* ent = array_sortedset_assign(
    fuzzyent_t, (array_t*)&fz->entries, fz->ma, &lookup,
    (array_sorted_cmp_t)fuzzyent_cmp, NULL);

  if UNLIKELY(!ent) // OOM
    return ErrNoMem;

  if (ent->name != NULL) {
    // skip definitions which we've seen, which are shadowed. E.g.
    // type A {}
    // fun foo() {
    //   let A = 3
    //   let B = AA  // here, "type A" is shadowed by "let A" (which we visit first)
    // }
    return 0;
  }

  ent->name = name;
  ent->n = n;
  return 1;
}


static bool fuzzy_visit_scope(const void* key, const void* value, void* ctx) {
  fuzzy_t* fz = ctx;
  // return true to keep iterating
  // fuzzy_add_candidate returns <0 on error
  return fuzzy_add_candidate(fz, key, value) >= 0;
}


static int levenshtein_dist(
  const char* astr, int alen,
  const char* bstr, int blen,
  int* d, int i, int j)
{
  if (d[i*(blen+1) + j] >= 0)
    return d[i*(blen+1) + j];
  int x;
  if (i == alen) {
    x = blen - j;
  } else if (j == blen) {
    x = alen - i;
  } else if (astr[i] == bstr[j]) {
    x = levenshtein_dist(astr, alen, bstr, blen, d, i + 1, j + 1);
  } else {
    x = levenshtein_dist(astr, alen, bstr, blen, d, i + 1, j + 1);
    int y;
    if ((y = levenshtein_dist(astr, alen, bstr, blen, d, i, j + 1)) < x) x = y;
    if ((y = levenshtein_dist(astr, alen, bstr, blen, d, i + 1, j)) < x) x = y;
    x++;
  }
  return d[i*(blen+1) + j] = x;
}


static int levenshtein(const char* astr, int alen, const char* bstr, int blen, int* d) {
  for (int i = 0; i < (alen + 1) * (blen + 1); i++)
    d[i] = -1;
  return levenshtein_dist(astr, alen, bstr, blen, d, 0, 0);
}


static int fuzzy_sort_cmp(const fuzzyent_t* a, const fuzzyent_t* b, fuzzy_t* fz) {
  return a->edit_dist - b->edit_dist;
}


static bool fuzzy_sort(fuzzy_t* fz) {
  bool ok = true;
  int namelen = strlen(fz->name);
  int dmcap = namelen * 2;

  // allocate memory for edit distance cache
  mem_t dm = mem_alloc(fz->ma, ((usize)dmcap+1) * ((usize)dmcap+1) * sizeof(int));
  if (!dm.p)
    return false;

  for (u32 i = 0; i < fz->entries.len; i++) {
    int ent_namelen = strlen(fz->entries.v[i].name);

    if UNLIKELY(ent_namelen >= dmcap) {
      dmcap = ent_namelen + 1;
      usize newsize = (((usize)namelen + 1) * ((usize)ent_namelen + 1)) * sizeof(int);
      if (!mem_resize(fz->ma, &dm, newsize)) {
        ok = false;
        break;
      }
    }

    fz->entries.v[i].edit_dist = levenshtein(
      fz->name, namelen, fz->entries.v[i].name, ent_namelen, dm.p);
  }

  mem_free(fz->ma, &dm);

  if (!ok)
    return false;

  // sort from shortest edit distance to longest
  co_qsort(fz->entries.v, fz->entries.len, sizeof(fz->entries.v[0]),
    (co_qsort_cmp)fuzzy_sort_cmp, fz);

  return true;
}


static void unknown_identifier(typecheck_t* a, idexpr_t* n) {
  sym_t name = n->name;

  error(a, n, "unknown identifier \"%s\"", name);

  // try to find an exact match in didyoumean
  u32 nsuggestions = 0;
  for (u32 i = 0; i < a->didyoumean.len; i++) {
    didyoumean_t* dym = &a->didyoumean.v[i];
    if (dym->name == name || dym->othername == name) {
      help(a, dym->decl, "did you mean \"%s\"", dym->name);
      nsuggestions++;
    }
  }

  if (nsuggestions > 0)
    return;

  // suggest fuzzy matches
  u32 maxdepth = U32_MAX;
  fuzzy_t fz = { .name = name, .ma = a->ma };
  scope_iterate(&a->scope, maxdepth, fuzzy_visit_scope, &fz);
  if (fuzzy_sort(&fz)) {
    // note: fuzzy_sort returns false if memory allocation failed, which we ignore
    // for (u32 i = 0; i < fz.entries.len; i++) {
    //   dlog("fz.entries.v[%u] = %s (%d)",
    //     i, fz.entries.v[i].name, fz.entries.v[i].edit_dist);
    // }
    int max_edit_dist = 2;
    if (fz.entries.len > 0 && fz.entries.v[0].edit_dist <= max_edit_dist)
      help(a, fz.entries.v[0].n, "did you mean \"%s\"", fz.entries.v[0].name);
  }

  array_dispose(fuzzyent_t, (array_t*)&fz.entries, fz.ma);
}


static void idexpr(typecheck_t* a, idexpr_t* n) {
  if (!n->ref || (n->flags & NF_UNKNOWN)) {
    n->ref = lookup(a, n->name);
    if UNLIKELY(!n->ref)
      return unknown_identifier(a, n);
  }

  assertf(n->ref->kind != NODE_IMPORTID && n->ref->kind != STMT_IMPORT,
    "unresolved import '%s'", ((importid_t*)n->ref)->name);

  expr(a, n->ref);

  // // mutability of ref extends to id
  // n->flags |= (n->ref->flags & NF_MUT);

  if (node_istype(n->ref)) {
    n->type = (type_t*)n->ref;
    type(a, &n->type);
  } else if ((n->flags & NF_NARROWED) && n->type->kind == TYPE_OPTIONAL) {
    assert(((opttype_t*)n->type)->elem->kind != TYPE_UNKNOWN);
  } else {
    n->type = asexpr(n->ref)->type;
  }
}


static void nsexpr(typecheck_t* a, nsexpr_t* n) {
  panic("TODO nsexpr");
}


static void retexpr(typecheck_t* a, retexpr_t* n) {
  if UNLIKELY(!a->fun)
    return error(a, n, "return outside of function");
  n->type = check_retval(a, n, &n->value);
}


static bool check_assign_to_member(typecheck_t* a, member_t* m) {
  // check mutability of receiver
  assertnotnull(m->recv->type);
  switch (m->recv->type->kind) {

  case TYPE_STRUCT:
    // assignment to non-ref "this", e.g. "fun Foo.bar(this Foo) { this = Foo() }"
    if UNLIKELY(
      m->recv->kind == EXPR_ID &&
      ((idexpr_t*)m->recv)->ref->kind == EXPR_PARAM &&
      ((local_t*)((idexpr_t*)m->recv)->ref)->isthis)
    {
      error(a, m->recv, "assignment to immutable struct %s", fmtnode(0, m->recv));
      return false;
    }
    return true;

  case TYPE_REF:
    error(a, m->recv, "assignment to immutable reference %s", fmtnode(0, m->recv));
    return false;

  default:
    return true;
  }
}


static bool check_assign_to_id(typecheck_t* a, idexpr_t* id) {
  node_t* target = id->ref;
  if (!target) // target is NULL when "id" is undefined
    return false;
  switch (target->kind) {
  case EXPR_ID:
    // this happens when trying to assign to a type-narrowed local
    // e.g. "var a ?int; if a { a = 3 }"
    error(a, id, "cannot assign to type-narrowed binding \"%s\"", id->name);
    return true;
  case EXPR_VAR:
    return true;
  case EXPR_PARAM:
    if (!((local_t*)target)->isthis)
      return true;
    FALLTHROUGH;
  default:
    error(a, id, "cannot assign to %s \"%s\"", fmtkind(target), id->name);
    return false;
  }
}


static bool check_assign(typecheck_t* a, expr_t* target) {
  switch (target->kind) {
    case EXPR_ID:
      return check_assign_to_id(a, (idexpr_t*)target);
    case EXPR_MEMBER:
      return check_assign_to_member(a, (member_t*)target);
    case EXPR_DEREF: {
      // dereference target, e.g. "var x &int ; *x = 3"
      type_t* t = ((unaryop_t*)target)->expr->type;
      if (t->kind == TYPE_REF) {
        const char* s = fmtnode(0, t);
        error(a, target, "cannot assign via immutable reference of type %s", s);
        return false;
      }
      if (t->kind == TYPE_MUTREF || t->kind == TYPE_PTR)
        return true;
      break;
    }
  }
  error(a, target, "cannot assign to %s", fmtkind(target));
  return false;
}


static void assign(typecheck_t* a, binop_t* n) {
  if (n->left->kind == EXPR_ID && ((idexpr_t*)n->left)->name == sym__) {
    // "_ = expr"
    typectx_push(a, n->left->type);
    expr(a, n->right);
    use(n->right);
    typectx_pop(a);

    n->type = n->right->type;
    return;
  }

  expr(a, n->left);
  use(n->left);

  typectx_push(a, n->left->type);
  expr(a, n->right);
  use(n->right);
  typectx_pop(a);

  n->type = n->left->type;

  if UNLIKELY(!type_isassignable(a->compiler, n->left->type, n->right->type))
    error_unassignable_type(a, n, n->right);

  check_assign(a, n->left);
}


static bool type_has_binop(const compiler_t* c, const type_t* t, op_t op) {
  t = concrete_type(c, (type_t*)t);
  switch (t->kind) {
    case TYPE_BOOL:
    case TYPE_OPTIONAL:
      switch (op) {
        case OP_LAND:       // &&
        case OP_LOR:        // ||
        case OP_EQ:         // ==
        case OP_NEQ:        // !=
        case OP_ASSIGN:     // =
          return true;
        default:
          return false;
      }
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
      switch (op) {
        case OP_ADD:        // +
        case OP_SUB:        // -
        case OP_MUL:        // *
        case OP_DIV:        // /
        case OP_MOD:        // %
        case OP_AND:        // &
        case OP_OR:         // |
        case OP_XOR:        // ^
        case OP_SHL:        // <<
        case OP_SHR:        // >>
        case OP_LAND:       // &&
        case OP_LOR:        // ||
        case OP_EQ:         // ==
        case OP_NEQ:        // !=
        case OP_LT:         // <
        case OP_GT:         // >
        case OP_LTEQ:       // <=
        case OP_GTEQ:       // >=
        case OP_ASSIGN:     // =
        case OP_ADD_ASSIGN: // +=
        case OP_SUB_ASSIGN: // -=
        case OP_MUL_ASSIGN: // *=
        case OP_DIV_ASSIGN: // /=
        case OP_MOD_ASSIGN: // %=
        case OP_AND_ASSIGN: // &=
        case OP_OR_ASSIGN:  // |=
        case OP_XOR_ASSIGN: // ^=
        case OP_SHL_ASSIGN: // <<=
        case OP_SHR_ASSIGN: // >>=
          return true;
        default:
          return false;
      }
    case TYPE_F32:
    case TYPE_F64:
      switch (op) {
        case OP_ADD:        // +
        case OP_SUB:        // -
        case OP_MUL:        // *
        case OP_DIV:        // /
        case OP_MOD:        // %
        case OP_LAND:       // &&
        case OP_LOR:        // ||
        case OP_EQ:         // ==
        case OP_NEQ:        // !=
        case OP_LT:         // <
        case OP_GT:         // >
        case OP_LTEQ:       // <=
        case OP_GTEQ:       // >=
        case OP_ASSIGN:     // =
        case OP_ADD_ASSIGN: // +=
        case OP_SUB_ASSIGN: // -=
        case OP_MUL_ASSIGN: // *=
        case OP_DIV_ASSIGN: // /=
        case OP_MOD_ASSIGN: // %=
          return true;
        default:
          return false;
      }
    case TYPE_STRUCT:
      switch (op) {
        case OP_EQ:         // ==
        case OP_NEQ:        // !=
        case OP_ASSIGN:     // =
          return true;
        default:
          return false;
      }
    case TYPE_REF:
    case TYPE_PTR:
      switch (op) {
        case OP_EQ:         // ==
        case OP_NEQ:        // !=
        case OP_ASSIGN:     // =
          return true;
        default:
          return false;
      }
    default:
      return op == OP_ASSIGN;
  }
}


static void error_cannot_use_as_bool(typecheck_t* a, expr_t* x) {
  error(a, x, "cannot use type %s as bool", fmtnode(0, x->type));
}


static void binop(typecheck_t* a, binop_t* n) {
  expr(a, n->left);
  use(n->left);

  typectx_push(a, n->left->type);
  expr(a, n->right);
  use(n->right);
  typectx_pop(a);

  switch (n->op) {
    case OP_EQ:
    case OP_NEQ:
    case OP_LT:
    case OP_GT:
    case OP_LTEQ:
    case OP_GTEQ:
      // e.g. "x == y"
      if UNLIKELY(!type_isequivalent(a->compiler, n->left->type, n->right->type))
        error_incompatible_types(a, n, n->left->type, n->right->type);
      n->type = type_bool;
      break;

    case OP_LAND:
      // e.g. "x && y"
      if UNLIKELY(
        (n->left->flags & NF_NARROWED) == 0 &&
        n->left->type != type_bool && n->left->type->kind != TYPE_OPTIONAL)
      {
        error_cannot_use_as_bool(a, n->left);
      }
      if UNLIKELY(
        (n->right->flags & NF_NARROWED) == 0 &&
        n->right->type != type_bool && n->right->type->kind != TYPE_OPTIONAL)
      {
        error_cannot_use_as_bool(a, n->right);
      }
      n->type = type_bool;
      break;

    case OP_LOR:
      // e.g. "x || y"
      if UNLIKELY(n->left->type != type_bool && n->left->type->kind != TYPE_OPTIONAL)
        error_cannot_use_as_bool(a, n->left);
      if UNLIKELY(n->right->type != type_bool && n->right->type->kind != TYPE_OPTIONAL)
        error_cannot_use_as_bool(a, n->right);
      n->type = type_bool;
      break;

    default: {
      // e.g. "x + y"
      type_t* lt = unwrap_alias(n->left->type);
      type_t* rt = unwrap_alias(n->right->type);
      if UNLIKELY(!type_iscompatible(a->compiler, lt, rt))
        error_incompatible_types(a, n, n->left->type, n->right->type);
      if (type_isref(lt))
        n->left = mkderef(a, n->left, n->left->loc);
      if (type_isref(rt))
        n->right = mkderef(a, n->right, n->right->loc);
      n->type = n->left->type;
    }
  }

  if UNLIKELY(!type_has_binop(a->compiler, n->left->type, n->op)) {
    error(a, n, "type %s has no '%s' operator",
      fmtnode(0, n->left->type), op_fmt(n->op));
  }
}


static void unaryop(typecheck_t* a, unaryop_t* n) {
  incuse(n->expr);
  expr(a, n->expr);

  if (n->type->kind == TYPE_UNRESOLVED || n->type == type_unknown)
    n->type = n->expr->type;

  switch (n->op) {
    case OP_REF:
    case OP_MUTREF:
      n->type = (type_t*)mkreftype(a, n->expr->type, n->op == OP_MUTREF);
      break;
    case OP_INC:
    case OP_DEC:
      // TODO: specialized check here since it's not actually assignment
      // (ownership et al)
      check_assign(a, n->expr);
      break;
    case OP_NOT:
      if UNLIKELY(
        n->expr->type->kind != TYPE_BOOL &&
        n->expr->type->kind != TYPE_OPTIONAL)
      {
        error(a, n, "type %s has no '%s' operator",
          fmtnode(0, n->expr->type), op_fmt(n->op));
      }
      n->type = type_bool;
      break;
    default:
      assertf(0, "unexpected unaryop %s", op_name(n->op));
      break;
  }
}


static void deref(typecheck_t* a, unaryop_t* n) {
  expr(a, n->expr);

  type_t* t = n->expr->type;

  if UNLIKELY(!type_isptrlike(t))
    return error(a, n, "dereferencing non-pointer value of type %s", fmtnode(0, t));

  // note: deref as store target is handled by check_assign,
  // e.g. in "var x &int ...", "*x = 3" is an error but "_ = *x" is okay if
  // type of "x" is copyable.
  n->type = ((ptrtype_t*)t)->elem;

  // check for deref of ref to non-copyable value
  if UNLIKELY(type_isref(t) && type_isowner(n->type))
    error(a, n, "cannot transfer ownership of borrowed %s", fmtnode(0, t));
}


static void floatlit(typecheck_t* a, floatlit_t* n) {
  if (a->typectx == type_f32) {
    n->type = type_f32;
    // FIXME: better way to check f32 value (than via sprintf & strtof)
    buf_t* buf = tmpbuf_get(0);
    if UNLIKELY(!buf_printf(buf, "%g", n->f64val))
      out_of_mem(a);
    float f = strtof(buf->chars, NULL);
    if UNLIKELY(f == HUGE_VALF) {
      // e.g. 1.e39
      error(a, n, "32-bit floating-point constant too large");
      n->f64val = 0.0;
    }
  } else {
    n->type = type_f64;
    if UNLIKELY(n->f64val == HUGE_VAL) {
      // e.g. 1.e309
      error(a, n, "64-bit floating-point constant too large");
      n->f64val = 0.0;
    }
  }
}


static void intlit(typecheck_t* a, intlit_t* n) {
  if (n->type != type_unknown)
    return;

  u64 isneg = 0; // TODO

  type_t* type = a->typectx;
  type_t* basetype = unwrap_alias(type);

  u64 maxval;
  u64 uintval = n->intval;
  if (isneg)
    uintval &= ~0x1000000000000000; // clear negative bit

again:
  switch (basetype->kind) {
  case TYPE_I8:   maxval = 0x7fllu+isneg; break;
  case TYPE_I16:  maxval = 0x7fffllu+isneg; break;
  case TYPE_I32:  maxval = 0x7fffffffllu+isneg; break;
  case TYPE_I64:  maxval = 0x7fffffffffffffffllu+isneg; break;
  case TYPE_U8:   maxval = 0xffllu; break;
  case TYPE_U16:  maxval = 0xffffllu; break;
  case TYPE_U32:  maxval = 0xffffffffllu; break;
  case TYPE_U64:  maxval = 0xffffffffffffffffllu; break;
  case TYPE_INT:  basetype = a->compiler->inttype; goto again;
  case TYPE_UINT: basetype = a->compiler->uinttype; goto again;
  default:
    // all other type contexts results in int, uint, i64 or u64 (depending on value)
    if (a->compiler->target.intsize == 8) {
      if (isneg) {
        type = type_int;
        maxval = 0x8000000000000000llu;
      } else if (n->intval < 0x8000000000000000llu) {
        n->type = type_int;
        return;
      } else {
        type = type_u64;
        maxval = 0xffffffffffffffffllu;
      }
    } else {
      assertf(a->compiler->target.intsize >= 4 && a->compiler->target.intsize < 8,
        "intsize %u not yet supported", a->compiler->target.intsize);
      if (isneg) {
        if (uintval <= 0x80000000llu)         { n->type = type_int; return; }
        if (uintval <= 0x8000000000000000llu) { n->type = type_i64; return; }
        // too large; trigger error report
        maxval = 0x8000000000000000llu;
        type = type_i64;
      } else {
        if (n->intval <= 0x7fffffffllu)         { n->type = type_int; return; }
        if (n->intval <= 0xffffffffllu)         { n->type = type_uint; return; }
        if (n->intval <= 0x7fffffffffffffffllu) { n->type = type_i64; return; }
        maxval = 0xffffffffffffffffllu;
        type = type_u64;
      }
    }
  }

  if UNLIKELY(uintval > maxval) {
    const char* ts = fmtnode(0, type);
    error(a, n, "integer constant overflows %s", ts);
  }

  n->type = type;
}


static void strlit(typecheck_t* a, strlit_t* n) {
  if (a->typectx == (type_t*)&a->compiler->strtype) {
    n->type = a->typectx;
    return;
  }

  arraytype_t* at = mknode(a, arraytype_t, TYPE_ARRAY);
  at->flags = NF_CHECKED;
  at->elem = type_u8;
  at->len = n->len;
  arraytype_calc_size(a, at);

  reftype_t* t = mknode(a, reftype_t, TYPE_REF);
  t->elem = (type_t*)at;

  n->type = (type_t*)t;
}


static void arraylit(typecheck_t* a, arraylit_t* n) {
  u32 i = 0;
  arraytype_t* at = (arraytype_t*)assertnotnull(a->typectx);

  if (at->kind == TYPE_ARRAY) {
    #if 0
      // eg. "var _ [int] = [1,2,3]" => "[int 3]"
      if (at->len == 0) {
        // outer array type does not have size; create sized type
        type_t* elem = at->elem;
        at = mknode(a, arraytype_t, TYPE_ARRAY);
        at->flags = NF_CHECKED;
        at->elem = elem;
        at->len = (u64)n->values.len;
        arraytype_calc_size(a, at);
      }
    #else
      if UNLIKELY(at->len > 0 && at->len < n->values.len) {
        expr_t* origin = (expr_t*)n->values.v[at->len];
        if (loc_line(origin->loc) == 0)
          origin = (expr_t*)n;
        error(a, origin, "excess value in array literal");
      }
    #endif
  } else {
    // infer the array element type based on the first value
    at = mknode(a, arraytype_t, TYPE_ARRAY);
    at->flags = NF_CHECKED;
    if UNLIKELY(n->values.len == 0) {
      at->elem = type_unknown;
      error(a, n, "cannot infer type of empty array literal; please specify its type");
      return;
    }
    typectx_push(a, type_unknown);
    exprp(a, (expr_t**)&n->values.v[i]);
    typectx_pop(a);
    at->elem = ((expr_t*)n->values.v[i])->type;
    at->len = (u64)n->values.len;
    arraytype_calc_size(a, at);
    i++; // don't visit the first value again
  }

  n->type = (type_t*)at;

  typectx_push(a, at->elem);

  for (; i < n->values.len; i++) {
    exprp(a, (expr_t**)&n->values.v[i]);
    expr_t* v = (expr_t*)n->values.v[i];
    if UNLIKELY(!type_isassignable(a->compiler, at->elem, v->type)) {
      error_unassignable_type(a, n, v);
      break;
    }
  }

  typectx_pop(a);
}


static void member_ns(typecheck_t* a, member_t* n) {
  nsexpr_t* ns = (nsexpr_t*)unwrap_id(n->recv);
  if (ns->kind != EXPR_NS) {
    error(a, n, "NOT IMPLEMENTED: namespace access via %s", nodekind_name(ns->kind));
    n->type = a->typectx;
    return;
  }

  sym_t name = n->name;
  expr_t* target = NULL;

  for (u32 i = 0; i < ns->members.len; i++) {
    if (ns->member_names[i] == name) {
      if UNLIKELY(!node_isexpr(ns->members.v[i])) {
        error(a, n, "names a %s", nodekind_fmt(ns->members.v[i]->kind));
        return;
      }
      target = (expr_t*)ns->members.v[i];
      n->target = use(target);
      n->type = target->type;
      return;
    }
  }

  // not found
  n->type = a->typectx; // avoid cascading errors

  if (ns->flags & NF_PKGNS) {
    assertnotnull(ns->pkg);
    error(a, n, "package \"%s\" has no member \"%s\"", ns->pkg->path.p, n->name);
  } else {
    const char* nsname;
    if (ns->name && ns->name != sym__) {
      nsname = ns->name;
    } else if (n->recv->kind == EXPR_ID) {
      nsname = ((idexpr_t*)n->recv)->name;
    } else {
      nsname = "";
    }
    error(a, n, "namespace %s has no member \"%s\"", nsname, n->name);
  }
}


static expr_t* nullable find_member(
  typecheck_t* a, type_t* bt, type_t* recvt, sym_t name)
{
  // note: bt has unwrap_ptr_and_alias applied, e.g. &MyMyT => T
  assert(bt->kind != TYPE_NS); // handled by find_member_ns

  // Treat the member operation as a field access.
  // If there are no matching fields of the type bt, consider type functions for bt.

  if (bt->kind == TYPE_STRUCT) {
    structtype_t* st = (structtype_t*)bt;
    for (u32 i = 0; i < st->fields.len; i++) {
      if (((local_t*)st->fields.v[i])->name == name) {
        exprp(a, (expr_t**)&st->fields.v[i]);
        return (expr_t*)st->fields.v[i];
      }
    }
  }

  // look for type function
  type_t* bt2 = type_unwrap_ptr(recvt); // e.g. ?&MyMyT => MyMyT
  fun_t* fn = typefuntab_lookup(&a->pkg->tfundefs, bt2, name);
  if (fn && CHECK_ONCE(fn)) {
    fun(a, fn);
    if (bt2 != recvt) panic("TODO check if fun is compatible with recvt");
    // TODO: check if fun is compatible with recvt, which could be for example a ref.
    // e.g. this should fail:
    //   fun Foo.bar(mut this)
    //   fun example(x &Foo)
    //     x.bar() // error: Foo.bar requires mutable receiver
  }
  return (expr_t*)fn;
}


static void error_optional_access(
  typecheck_t* a, const opttype_t* t, const expr_t* expr, const expr_t* access)
{
  error(a, expr, "optional value of type %s may not be valid", fmtnode(0, t));
  if (loc_line(access->loc)) {
    help(a, access, "check %s before access, e.g: if %s %s",
      fmtnode(0, access), fmtnode(1, access), fmtnode(2, expr));
  }
}


static void member(typecheck_t* a, member_t* n) {
  incuse(n->recv);
  expr(a, n->recv);

  // get receiver type without ref or optional
  type_t* recvt = n->recv->type; // e.g. ?&MyMyT
  type_t* recvbt = unwrap_ptr_and_alias(recvt); // e.g. &MyMyT => T

  // namespace has dedicated implementation
  if (recvbt->kind == TYPE_NS)
    return member_ns(a, n);

  // can't access members through optional
  if UNLIKELY(recvbt->kind == TYPE_OPTIONAL)
    return error_optional_access(a, (opttype_t*)recvbt, (expr_t*)n, n->recv);

  // resolve target
  typectx_push(a, type_unknown);
  expr_t* target = find_member(a, recvbt, recvt, n->name);
  typectx_pop(a);

  if (target) {
    n->target = use(target);
    n->type = target->type;
  } else {
    n->type = a->typectx; // avoid cascading errors
    if (recvt != type_unknown || !a->reported_error)
      error(a, n, "%s has no field or method \"%s\"", fmtnode(0, recvt), n->name);
  }
}


static void unsigned_index_expr(typecheck_t* a, expr_t* n, u64* constval) {
  incuse(n);

  typectx_push(a, type_uint);
  expr(a, n);
  typectx_pop(a);

  if (comptime_eval_uint(a->compiler, n, CTIME_NO_DIAG, constval)) {
    n->flags |= NF_CONST;
  } else switch (n->type->kind) {
    case TYPE_U8:
    case TYPE_UINT:
      break;
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
      // accept these types if they are convertible to uint without loss
      if (n->type->size <= a->compiler->uinttype->size)
        break;
      FALLTHROUGH;
    default:
      error(a, n, "invalid index type %s; expecting uint", fmtnode(0, n->type));
  }
}


static void subscript(typecheck_t* a, subscript_t* n) {
  incuse(n->recv);

  typectx_push(a, type_unknown);
  expr(a, n->recv);
  typectx_pop(a);

  unsigned_index_expr(a, n->index, &n->index_val);

  ptrtype_t* recvt = (ptrtype_t*)unwrap_ptr_and_alias(n->recv->type);
  n->type = a->typectx; // avoid cascading errors

  switch (recvt->kind) {
    case TYPE_ARRAY: {
      n->type = recvt->elem;
      arraytype_t* at = (arraytype_t*)recvt;
      if UNLIKELY(
        (n->index->flags & NF_CONST) && at->lenexpr && n->index_val >= at->len)
      {
        error(a, n, "out of bounds: element %llu of array %s",
          n->index_val, fmtnode(0, recvt));
      }
      break;
    }
    case TYPE_SLICE:
    case TYPE_MUTSLICE:
      n->type = recvt->elem;
      break;

    case TYPE_OPTIONAL:
      // can't subscript optional
      // eg. given
      //   fun f(x ?[int]) int { x[2] }
      // the following diagnostic is produced
      // example.co:1:24: error: optional value of type ?[int] may not be valid
      // 8 → │ fun f(x ?[int]) int { x[2] }
      //     │                         ~~~
      // example.co:1:23: help: check x before access, e.g: if x x[2]
      // 8 → │ fun f(x ?[int]) int { x[2] }
      //     │                        ~
      return error_optional_access(a, (opttype_t*)recvt, (expr_t*)n, n->recv);

    default:
      return error(a, n, "cannot index into type %s", fmtnode(0, recvt));
  }
}


static void finalize_typecons(typecheck_t* a, typecons_t** np) {
  type_t* t = (*np)->type;

  if (!type_isprim(unwrap_alias(t)))
    return;

  expr_t* expr = (*np)->expr;
  if (!expr)
    return;

  // eliminate type cast to equivalent type, e.g. "i8(3)" => "3"
  if (concrete_type(a->compiler, t) == concrete_type(a->compiler, expr->type)) {
    expr->nuse += MAX(1, (*np)->nuse) - 1;
    *(expr_t**)np = expr;
    return;
  }

  if UNLIKELY(!type_isconvertible(t, expr->type)) {
    const char* dst_s = fmtnode(0, t);
    const char* src_s = fmtnode(1, expr->type);
    error(a, *np, "cannot convert value of type %s to type %s", src_s, dst_s);
    return;
  }
}


static void typecons(typecheck_t* a, typecons_t** np) {
  typecons_t* n = *np;
  if (n->expr) {
    incuse(n->expr);
    typectx_push(a, n->type);
    expr(a, n->expr);
    typectx_pop(a);
  }
  return finalize_typecons(a, np);
}


// —————————————————————————————————————————————————————————————————————————————————
// call


static void error_field_type(typecheck_t* a, const expr_t* arg, const local_t* f) {
  const char* got = fmtnode(0, arg->type);
  const char* expect = fmtnode(1, f->type);
  const void* origin = arg;
  if (arg->kind == EXPR_PARAM)
    origin = assertnotnull(((local_t*)arg)->init);
  error(a, origin, "passing value of type %s for field \"%s\" of type %s",
    got, f->name, expect);
}


static void convert_call_to_typecons(typecheck_t* a, call_t** np, type_t* t) {
  static_assert(sizeof(typecons_t) <= sizeof(call_t), "");

  nodearray_t args = (*np)->args;
  typecons_t* tc = (typecons_t*)*np;

  tc->kind = EXPR_TYPECONS;
  tc->type = t;
  if (type_isprim(unwrap_alias(t))) {
    assert(args.len == 1);
    tc->expr = (expr_t*)args.v[0];
  } else {
    tc->args = args;
  }

  return finalize_typecons(a, (typecons_t**)np);
}


static void check_call_type_struct(typecheck_t* a, call_t* call, structtype_t* t){
  assert(call->args.len <= t->fields.len); // checked by validate_typecall_args

  u32 i = 0;
  nodearray_t args = call->args;

  // build field map
  map_t fieldmap = a->tmpmap;
  map_clear(&fieldmap);
  if UNLIKELY(!map_reserve(&fieldmap, a->ma, t->fields.len))
    return out_of_mem(a);
  for (u32 i = 0; i < t->fields.len; i++) {
    const local_t* f = (local_t*)t->fields.v[i];
    void** vp = map_assign_ptr(&fieldmap, a->ma, f->name);
    assertnotnull(vp); // map_reserve
    *vp = (void*)f;
  }

  // map arguments
  for (; i < args.len; i++) {
    expr_t* arg = (expr_t*)args.v[i];
    sym_t name = NULL;

    switch (arg->kind) {
    case EXPR_PARAM:
      name = ((local_t*)arg)->name;
      break;
    case EXPR_ID:
      name = ((idexpr_t*)arg)->name;
      break;
    default:
      error(a, arg,
        "positional argument in struct constructor; use either name:value"
        " or an identifier with the same name as the intended struct field");
      continue;
    }

    // lookup field
    void** vp = map_lookup_ptr(&fieldmap, name);
    if UNLIKELY(!vp || ((node_t*)*vp)->kind != EXPR_FIELD) {
      const char* s = fmtnode(0, t);
      if (!vp) {
        error(a, arg, "no \"%s\" field in struct %s", name, s);
      } else {
        error(a, arg, "duplicate value for field \"%s\" of struct %s", name, s);
        warning(a, *vp, "value for field \"%s\" already provided here", name);
      }
      continue;
    }

    local_t* f = *vp; // load field
    *vp = arg; // mark field name as defined, used for detecting duplicate args
    arg->flags |= NF_RVALUE;

    typectx_push(a, f->type);

    if (arg->kind == EXPR_PARAM) {
      local_t* namedarg = (local_t*)arg;
      assertnotnull(namedarg->init); // checked by parser
      exprp(a, &namedarg->init);
      namedarg->type = namedarg->init->type;
    } else {
      assert(arg->kind == EXPR_ID); // for future dumb me
      idexpr(a, (idexpr_t*)arg);
    }

    use(arg);

    typectx_pop(a);

    if UNLIKELY(!type_isassignable(a->compiler, f->type, arg->type)) {
      error_field_type(a, arg, f);
    } else {
      implicit_rvalue_deref(a, f->type, (expr_t**)&args.v[i]);
      arg = (expr_t*)args.v[i]; // reload
    }
  }

  a->tmpmap = fieldmap; // in case map grew
}


static void call_type_prim(typecheck_t* a, call_t** np, type_t* dst) {
  call_t* call = *np;
  assert(call->args.len == 1);
  expr_t* arg = (expr_t*)call->args.v[0];

  if UNLIKELY(!nodekind_isexpr(arg->kind))
    return error(a, arg, "invalid value");

  if UNLIKELY(arg->kind == EXPR_PARAM) {
    return error(a, arg, "%s type cast does not accept named arguments",
      fmtnode(0, dst));
  }

  typectx_push(a, dst);
  expr(a, arg);
  typectx_pop(a);

  use(arg);

  call->type = dst;

  return convert_call_to_typecons(a, np, dst);
}


static void error_call_type_arity(
  typecheck_t* a, call_t* call, type_t* t, u32 minargs, u32 maxargs)
{
  assert(minargs > call->args.len || call->args.len > maxargs);
  const char* typstr = fmtnode(1, t);

  const char* logical_op = "type cast";
  type_t* basetype = unwrap_alias(t);
  if (basetype->kind == TYPE_STRUCT || basetype->kind == TYPE_ARRAY)
    logical_op = "type constructor";

  if (call->args.len < minargs) {
    const void* origin = call->recv;
    if (call->args.len > 0)
      origin = call->args.v[call->args.len - 1];
    error(a, origin, "not enough arguments for %s %s, expecting%s %u",
      typstr, logical_op, minargs != maxargs ? " at least" : "", minargs);
    return;
  }

  const node_t* arg = call->args.v[maxargs];
  const char* argstr = fmtnode(0, arg);
  if (maxargs == 0) {
    // e.g. "void(x)"
    error(a, arg, "unexpected value %s; %s %s accepts no arguments",
      argstr, typstr, logical_op);
  } else {
    error(a, arg, "unexpected extra value %s in %s %s",
      argstr, typstr, logical_op);
  }
}


static bool check_call_type_arity(
  typecheck_t* a, call_t* call, type_t* t, u32 minargs, u32 maxargs)
{
  if UNLIKELY(minargs > call->args.len || call->args.len > maxargs) {
    error_call_type_arity(a, call, t, minargs, maxargs);
    return false;
  }
  return true;
}


static void call_type(typecheck_t* a, call_t** np, type_t* t) {
  call_t* call = *np;
  call->type = t;

  // unwrap alias
  type_t* origt = t; // original type
  t = unwrap_alias(t);

  switch (t->kind) {
  case TYPE_VOID: {
    // no arguments
    if UNLIKELY(!check_call_type_arity(a, call, origt, 0, 0))
      return;
    // convert to typecons
    typecons_t* tc = (typecons_t*)*np;
    tc->kind = EXPR_TYPECONS;
    tc->type = origt;
    tc->expr = NULL;
    return;
  }

  case TYPE_BOOL:
  case TYPE_INT:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_F32:
  case TYPE_F64:
    if UNLIKELY(!check_call_type_arity(a, call, origt, 1, 1))
      return;
    return call_type_prim(a, np, origt);

  case TYPE_STRUCT: {
    u32 maxargs = ((structtype_t*)t)->fields.len;
    if UNLIKELY(!check_call_type_arity(a, call, origt, 0, maxargs))
      return;
    return check_call_type_struct(a, call, (structtype_t*)t);
  }

  // TODO
  case TYPE_ARRAY:
    if UNLIKELY(!check_call_type_arity(a, call, origt, 1, U32_MAX))
      return;
    FALLTHROUGH;
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_OPTIONAL:
    trace("TODO IMPLEMENT %s", nodekind_name(t->kind));
    error(a, call->recv, "NOT IMPLEMENTED: %s", nodekind_name(t->kind));
    return;

  case TYPE_UNRESOLVED:
    // this only happens when there was a type error
    assert(compiler_errcount(a->compiler) > 0);
    return;

  default:
    assertf(0,"unexpected %s", nodekind_name(t->kind));
    return;
  }
}


static void call_fun(typecheck_t* a, call_t* call, funtype_t* ft) {
  call->type = ft->result;

  u32 paramsc = ft->params.len;
  local_t** paramsv = (local_t**)ft->params.v;
  if (paramsc > 0 && paramsv[0]->isthis) {
    paramsv++;
    paramsc--;
  }

  if UNLIKELY(call->args.len != paramsc) {
    error(a, call, "%s arguments in function call, expected %u",
      call->args.len < paramsc ? "not enough" : "too many", paramsc);
    return;
  }

  bool seen_named_arg = false;

  for (u32 i = 0; i < paramsc; i++) {
    expr_t* arg = (expr_t*)call->args.v[i];
    local_t* param = paramsv[i];


    typectx_push(a, param->type);

    if (arg->kind == EXPR_PARAM) {
      // named argument
      local_t* namedarg = (local_t*)arg;
      assertnotnull(namedarg->init); // checked by parser
      expr(a, namedarg->init);
      arg->type = namedarg->init->type;
      seen_named_arg = true;

      if UNLIKELY(namedarg->name != param->name) {
        u32 j = i;
        for (j = 0; j < paramsc; j++) {
          if (paramsv[j]->name == namedarg->name)
            break;
        }
        const char* condition = (j == paramsc) ? "unknown" : "invalid position of";
        error(a, arg, "%s named argument \"%s\", in function call %s", condition,
          namedarg->name, fmtnode(0, ft));
      }
    } else {
      // positional argument
      if UNLIKELY(seen_named_arg) {
        error(a, arg, "positional argument after named argument(s)");
        typectx_pop(a);
        break;
      }
      exprp(a, (expr_t**)&call->args.v[i]);
      arg = (expr_t*)call->args.v[i]; // reload
    }

    use(arg);

    typectx_pop(a);

    // check type
    if UNLIKELY(
      !type_isassignable(a->compiler, param->type, arg->type) &&
      param->type != type_unknown &&
      arg->type != type_unknown )
    {
      error(a, arg, "passing value of type %s to parameter of type %s",
        fmtnode(0, arg->type), fmtnode(1, param->type));
      // if (param->type->kind == TYPE_MUT &&
      //     arg->type->kind != TYPE_MUT &&
      //     (arg->kind == EXPR_ID || arg->kind == EXPR_MEMBER) &&
      //     loc_line(arg->loc))
      // {
      //   const char* name = fmtnode(0, arg);
      //   help(a, arg, "mark %s as mutable: &%s", name, name);
      // }
    } else {
      implicit_rvalue_deref(a, param->type, (expr_t**)&call->args.v[i]);
      arg = (expr_t*)call->args.v[i]; // reload
    }
  }

  if ((call->flags & NF_RVALUE) == 0 && type_isowner(call->type) && noerror(a)) {
    // return value is owner, but it is not used (call is not rvalue)
    warning(a, call, "unused result; ownership transferred from function call");
  }
}


static void call(typecheck_t* a, call_t** np) {
  call_t* n = *np;
  expr(a, n->recv);

  if (a->reported_error)
    return;


  node_t* recv = unwrap_id(n->recv);
  type_t* recvtype;

  if LIKELY(node_isexpr(recv)) {
    recvtype = ((expr_t*)recv)->type;
    if (recvtype->kind == TYPE_FUN)
      return call_fun(a, n, (funtype_t*)recvtype);
  } else if LIKELY(node_istype(recv)) {
    recvtype = (type_t*)recv;
    return call_type(a, np, recvtype);
  }

  // error: bad recv
  n->type = a->typectx; // avoid cascading errors
  if (node_isexpr(recv)) {
    error(a, n->recv, "calling an expression of type %s, expected function or type",
      fmtnode(0, ((expr_t*)recv)->type));
  } else {
    error(a, n->recv, "calling %s; expected function or type", fmtnode(0, recv));
  }
}


typedef struct {
  typecheck_t*      a;
  templateparam_t** paramv; // index in sync with args.v, count == args.len
  nodearray_t       args;
  err_t             err;
  u32               templatenest;
  #ifdef TRACE_TEMPLATE_EXPANSION
  int               traceindent;
  #endif
} instancectx_t;


static node_t* nullable instantiate_trfn(
  ast_transform_t* tr, node_t* n, void* nullable ctxp)
{
  instancectx_t* ctx = ctxp;
  UNUSED typecheck_t* a = ctx->a; // for trace

  #ifdef TRACE_TEMPLATE_EXPANSION
    trace_tplexp("%s %p %s ...", nodekind_name(n->kind), n, fmtnode(0, n));
    ctx->traceindent++;
  #endif

  node_t* n1 = n;

  if (n->kind == TYPE_PLACEHOLDER) {
    assert((n->flags & NF_TEMPLATE) == 0);

    // replace placeholder parameter with arg
    templateparam_t* templateparam = ((placeholdertype_t*)n)->templateparam;
    for (u32 i = 0; i < ctx->args.len; i++) {
      if (ctx->paramv[i] == templateparam) {
        trace_tplexp("replace template parameter %s with arg %s",
          templateparam->name, nodekind_name(ctx->args.v[i]->kind));
        // TODO: check any constraints on parameter vs arg
        n = ctx->args.v[i];
        goto visit_children;
      }
    }
    // If we get here, we're seeing an outer placeholder.
    // e.g. expanding template Foo inside template Bar replaces X with Y:
    //   type Foo<X>
    //     x X
    //   type Bar<Y>
    //     x Foo<Y>   <—
    //
    goto end;
  }

visit_children:
  ctx->templatenest += (u32)!!(n->flags & NF_TEMPLATE);
  n = ast_transform_children(tr, n, ctxp);
  ctx->templatenest -= (u32)!!(n->flags & NF_TEMPLATE);

  // if the node was replaced it means at least one placeholder was replaced
  if (n != n1) {
    // when encountering an instance inside a template we need to clear any cached
    // typeid since we may have replaced a placeholder
    if (ctx->templatenest && (n->flags & NF_TEMPLATEI)) {
      assert(node_istype(n));
      if (((type_t*)n)->_typeid)
        trace_tplexp("scrub typeid from %s#%p", nodekind_name(n->kind), n);
      ((type_t*)n)->_typeid = NULL;
    }

    // scrub "checked" and "unknown" flags, if this path of the AST was modified
    if (!nodekind_isprimtype(n->kind) && n->kind != TYPE_PLACEHOLDER) {
      if (n->flags & NF_CHECKED)
        trace_tplexp("scrub NF_CHECKED from %s#%p", nodekind_name(n->kind), n);
      n->flags &= ~(NF_CHECKED | NF_UNKNOWN);
    }
  }

end:
  #ifdef TRACE_TEMPLATE_EXPANSION
    ctx->traceindent--;
    if (n == n1) {
      trace_tplexp("%s#%p %s == (verbatim)",
        nodekind_name(n->kind), n, fmtnode(0, n));
    } else {
      trace_tplexp("%s#%p %s => %s#%p %s",
        nodekind_name(n1->kind), n1, fmtnode(0, n1),
        nodekind_name(n->kind), n, fmtnode(1, n));
    }
  #endif

  return n;
}


static void templateimap_mkkey(
  buf_t* key, const usertype_t* template, const nodearray_t* template_args)
{
  buf_append(key, (uintptr*)&template, sizeof(uintptr));
  for (u32 i = 0; i < template_args->len; i++) {
    assert(node_istype(template_args->v[i]));
    typeid_t typeid = typeid_of((type_t*)template_args->v[i]);
    buf_append(key, typeid, typeid_len(typeid));
  }
}


static void templateimap_add(
  typecheck_t* a, const usertype_t* template, usertype_t* instance)
{
  a->tmpbuf.len = 0;
  templateimap_mkkey(&a->tmpbuf, template, &instance->templateparams);

  void* v = mem_alloc(a->ma, a->tmpbuf.len).p;
  if (v) memcpy(v, a->tmpbuf.p, a->tmpbuf.len);
  void** p = map_assign(&a->templateimap, a->ma, v, a->tmpbuf.len);
  if UNLIKELY(!p || a->tmpbuf.oom || !v)
    return out_of_mem(a);

  assertf(*p == NULL, "duplicate entry %s", nodekind_name(instance->kind));
  *p = instance;

  #if 0
  if (opt_trace_typecheck) {
    buf_t tmpbuf = buf_make(a->ma);
    buf_appendrepr(&tmpbuf, a->tmpbuf.p, a->tmpbuf.len);
    trace("[templateimap_add] %s#%p %s (key '%.*s') => %s#%p %s",
      nodekind_name(template->kind), template, fmtnode(0, template),
      (int)tmpbuf.len, tmpbuf.chars,
      nodekind_name(instance->kind), instance, fmtnode(0, instance));
    buf_dispose(&tmpbuf);
  }
  #endif
}


static usertype_t* nullable templateimap_lookup(
  typecheck_t* a, const usertype_t* template, const nodearray_t* template_args)
{
  a->tmpbuf.len = 0;
  templateimap_mkkey(&a->tmpbuf, template, template_args);

  void** p = map_lookup(&a->templateimap, a->tmpbuf.p, a->tmpbuf.len);
  usertype_t* instance = p ? assertnotnull(*p) : NULL;

  #if 0
  if (opt_trace_typecheck) {
    buf_t tmpbuf = buf_make(a->ma);
    buf_appendrepr(&tmpbuf, a->tmpbuf.p, a->tmpbuf.len);
    if (instance) {
      trace("lookup_interned_usertype %s#%p %s (key '%.*s') => %s#%p %s",
        nodekind_name(template->kind), template, fmtnode(0, template),
        (int)tmpbuf.len, tmpbuf.chars,
        nodekind_name(instance->kind), instance, fmtnode(0, instance));
    } else {
      trace("lookup_interned_usertype %s#%p %s (key '%.*s') => (not found)",
        nodekind_name(template->kind), template, fmtnode(0, template),
        (int)tmpbuf.len, tmpbuf.chars);
    }
    buf_dispose(&tmpbuf);
  }
  #endif

  return instance;
}


static void instantiate_templatetype(typecheck_t* a, templatetype_t** tp) {
  templatetype_t* tt = *tp;
  usertype_t* template = tt->recv;
  assert(tt->args.len <= template->templateparams.len);

  trace("expand template %s with %u args", fmtnode(0, template), tt->args.len);
  #ifdef TRACE_TEMPLATE_EXPANSION
    for (u32 i = 0; i < tt->args.len; i++) {
      trace("  - [%u] %s %p %s => %s %p %s",
        i,
        nodekind_name(template->templateparams.v[i]->kind),
        template->templateparams.v[i],
        fmtnode(0, template->templateparams.v[i]),
        nodekind_name(tt->args.v[i]->kind),
        tt->args.v[i],
        fmtnode(1, tt->args.v[i]));
    }
    a->traceindent++;
  #endif

  // instantiation state
  instancectx_t ctx = {
    .a = a,
    .paramv = (templateparam_t**)template->templateparams.v,
    .templatenest = a->templatenest,
    #ifdef TRACE_TEMPLATE_EXPANSION
    .traceindent = a->traceindent,
    #endif
  };

  // Copy args if there are default values involved.
  // Ownership of ctx.args are eventually transferred to the instance.
  if (tt->args.len == template->templateparams.len) {
    ctx.args = tt->args;
  } else {
    if (!nodearray_reserve_exact(&ctx.args, a->ast_ma, template->templateparams.len))
      return out_of_mem(a);
    memcpy(ctx.args.v, tt->args.v, tt->args.len * sizeof(node_t*));
    ctx.args.len += tt->args.len;
    for (u32 i = ctx.args.len; i < template->templateparams.len; i++) {
      templateparam_t* tparam = (templateparam_t*)template->templateparams.v[i];
      assertnotnull(tparam->init);
      ctx.args.v[ctx.args.len++] = tparam->init;
    }
  }

  // check if there's an existing instance
  usertype_t* instance = templateimap_lookup(a, template, &ctx.args);
  if (instance) {
    trace("using existing template instance");
    *(node_t**)tp = (node_t*)instance;
    if (tt->args.len != template->templateparams.len)
      nodearray_dispose(&ctx.args, a->ast_ma);
    #ifdef DEBUG
      a->traceindent--;
    #endif
    return;
  }

  // instantiate template
  err_t err = ast_transform(
    (node_t*)template, a->ast_ma, instantiate_trfn, &ctx, (node_t**)&instance);

  // check if transformation failed (if it did, it's going to be OOM)
  if UNLIKELY(err) {
    dlog("ast_transform() failed: %s", err_str(err));
    error(a, (origin_t){0}, "%s", err_str(err));
    seterr(a, err);
    #ifdef DEBUG
      a->traceindent--;
    #endif
    return;
  }

  if (instance == (usertype_t*)template) {
    // no substitutions
    if UNLIKELY(!( instance = ast_clone_node(a->ast_ma, instance) ))
      return out_of_mem(a);
  } else {
    assertf((instance->flags & NF_CHECKED) == 0, "checked flag should be scrubbed");
  }
  assert(nodekind_isusertype(instance->kind));

  // convert instance to NF_TEMPLATEI
  instance->flags = (instance->flags & ~NF_TEMPLATE) | NF_TEMPLATEI;
  instance->templateparams = ctx.args;
  instance->_typeid = NULL; // scrub cached typeid

  // register instance (before checking, in case it refers to itself)
  templateimap_add(a, template, instance);

  // typecheck the instance
  *(node_t**)tp = (node_t*)instance;
  type(a, (type_t**)tp);

  // instance must not have been transformed.
  // We rely on this when we call templateimap_add ahead of time.
  if ((usertype_t*)*tp != instance) {
    dlog("instance was transformed: %s -> %s",
      fmtnode(0, instance), fmtnode(1, *tp));
  }
  assert((usertype_t*)*tp == instance);
  assert(nodekind_isusertype(instance->kind));

  #ifdef DEBUG
    a->traceindent--;
  #endif
}


static void templatetype(typecheck_t* a, templatetype_t** tp) {
  // Use of template, e.g. var x Foo<int>
  //                             ~~~~~~~~
  templatetype_t* tt = *tp;
  type(a, (type_t**)&tt->recv);
  usertype_t* template = tt->recv;

  // must check template, in case use preceeds definition
  type(a, (type_t**)&template);

  // count number of required template parameters
  u32 nrequired = 0;
  u32 ntotal = template->templateparams.len;
  for (u32 i = 0; i < ntotal; i++) {
    templateparam_t* tparam = (templateparam_t*)template->templateparams.v[i];
    nrequired += (u32)!tparam->init;
  }

  // stop now if we encountered errors
  if (nrequired != ntotal) {
    if (compiler_errcount(a->compiler))
      return;
  }

  // check args arity
  if UNLIKELY(tt->args.len < nrequired || tt->args.len > ntotal) {
    error(a, tt, "%s template parameters; want%s %u",
      tt->args.len > ntotal ? "too many" : "not enough",
      nrequired < ntotal ? " at least" : "",
      nrequired);
    templateparam_t** paramv = (templateparam_t**)template->templateparams.v;
    if (ntotal > 0 && paramv[0]->loc) {
      origin_t origin = origin_make(locmap(a), paramv[0]->loc);
      for (u32 i = 1; i < ntotal; i++) {
        if (paramv[i]->loc) {
          origin_t origin2 = origin_make(locmap(a), paramv[i]->loc);
          origin = origin_union(origin, origin2);
        }
      }
      help(a, origin, "template parameter%s defined here", ntotal == 1 ? "" : "s");
    }
    return;
  }

  // resolve args
  for (u32 i = 0; i < tt->args.len; i++) {
    node_t* n = tt->args.v[i];
    //dlog("[%s] check %s %p", __FUNCTION__, nodekind_name(n->kind), n);

    if (n->flags & NF_CHECKED)
      continue;

    while (n->kind == TYPE_PLACEHOLDER) {
      if (((templateparam_t*)n)->init == NULL)
        goto next;
      n->flags |= NF_CHECKED;
      n = ((templateparam_t*)n)->init;
    }

    if (nodekind_istype(n->kind)) {
      type(a, (type_t**)&n);
    } else if (nodekind_isexpr(n->kind)) {
      exprp(a, (expr_t**)&n);
    } else {
      assert_nodekind(n, TYPE_PLACEHOLDER);
    }
    next: {}
  }

  // stop now if there were errors
  if (!noerror(a))
    return;

  assert(tt == *tp);
  assert(template == (*tp)->recv);

  // actually instantiate the template, unless we are inside a template definition
  if (a->templatenest == 0)
    return instantiate_templatetype(a, tp);
}


static void placeholdertype(typecheck_t* a, placeholdertype_t** tp) {
  // e.g.
  //   type Foo<T>
  //     x T   <—— visiting T
  //       ~
  //trace("placeholdertype %s", (*tp)->templateparam->name);
  assert(a->templatenest > 0);
  //dlog("TODO templatescope");
}


static void unresolvedtype(typecheck_t* a, unresolvedtype_t** tp) {
  if ((*tp)->resolved) {
    *(type_t**)tp = (*tp)->resolved;
    return;
  }

  sym_t name = (*tp)->name;
  type_t* t = (type_t*)lookup(a, name);
  trace("resolve type \"%s\" (%p) => %s %s",
    name, name, nodekind_name(t ? t->kind : 0), t ? fmtnode(0, t) : "(null)");

  if LIKELY(t && nodekind_istype(t->kind)) {
    type(a, &t);
    t->nuse += (*tp)->nuse;
    (*tp)->resolved = t;
    *(type_t**)tp = t;

    // we must check type aliases for cycles now, since we unwrap aliases often
    // and before we have run check_typedefs.
    if (t->kind == TYPE_ALIAS && !check_typedep(a->compiler, (node_t*)t)) {
      // break cycle to prevent stack overflow in type_isowner
      ((aliastype_t*)t)->elem = type_unknown;
    }

    return;
  }

  // error beyond this point

  // not found
  if (!t) {
    error(a, *tp, "unknown type \"%s\"", name);
  } else {
    // not a type
    error(a, *tp, "%s is not a type (it's a %s)", name, fmtkind(t));
    if (loc_line(t->loc))
      help(a, t, "%s defined here", name);
  }

  // redefine as "void" in current scope to minimize repetitive errors
  if (!scope_define(&a->scope, a->ma, name, *tp))
    out_of_mem(a);
}


static void define_typedef(typecheck_t* a, typedef_t* n) {
  sym_t name;
  if (n->type->kind == TYPE_STRUCT) {
    name = assertnotnull(((structtype_t*)n->type)->name);
  } else {
    assert(n->type->kind == TYPE_ALIAS);
    name = assertnotnull(((aliastype_t*)n->type)->name);
  }
  define(a, name, (node_t*)n->type);
}


static void typedef_(typecheck_t* a, typedef_t* n) {
  a->pubnest += (u32)!!(n->flags & NF_VIS_PUB);
  type(a, &n->type);
  a->pubnest -= (u32)!!(n->flags & NF_VIS_PUB);
  define_typedef(a, n);
}


static void aliastype(typecheck_t* a, aliastype_t** tp) {
  aliastype_t* t = *tp;
  type(a, &t->elem);

  if UNLIKELY(t->elem == type_void)
    return error(a, t, "cannot alias type void");

  if (type_isowner(t->elem))
    t->flags |= NF_SUBOWNERS;

  if (!t->nsparent)
    t->nsparent = a->nspath.v[a->nspath.len - 1];

  // check for internal types leaking from public ones
  if (a->pubnest) {
    if UNLIKELY((t->elem->flags & NF_VIS_PUB) == 0) {
      error(a, t, "internal type %s in public alias %s",
        fmtnode(0, t->elem), t->name);
      help(a, t->elem, "mark %s `pub`", fmtnode(0, t->elem));
    }
    node_set_visibility((node_t*)t, NF_VIS_PUB);
  }
}


static void opttype(typecheck_t* a, opttype_t** tp) {
  opttype_t* t = *tp;
  type(a, &t->elem);
}


static void check_template(typecheck_t* a, usertype_t** tp) {
  usertype_t* t = *tp;
  assert(nodekind_isusertype(t->kind));
  for (u32 i = 0; i < t->templateparams.len; i++) {
    templateparam_t* tparam = (templateparam_t*)t->templateparams.v[i];
    if (!tparam->init)
      continue;
    if (nodekind_istype(tparam->init->kind)) {
      type(a, (type_t**)&tparam->init);
    } else if (nodekind_isexpr(tparam->init->kind)) {
      exprp(a, (expr_t**)&tparam->init);
    } else {
      assert_nodekind(tparam->init, NODE_TPLPARAM);
    }
  }
}


// end call
// —————————————————————————————————————————————————————————————————————————————————


static void _type(typecheck_t* a, type_t** tp) {
  type_t* t = *tp;

  if (t->flags & NF_CHECKED)
    return;
  t->flags |= NF_CHECKED;

  if (t->flags & NF_TEMPLATE) {
    a->templatenest++;
    check_template(a, (usertype_t**)tp);
  }

  TRACE_NODE(a, "", tp);
  switch ((enum nodekind)(*tp)->kind) {
    case TYPE_VOID:
    case TYPE_BOOL:
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_INT:
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
    case TYPE_UINT:
    case TYPE_F32:
    case TYPE_F64:
    case TYPE_NS:
    case TYPE_UNKNOWN:
      assertf(0, "%s should always be NF_CHECKED", nodekind_name((*tp)->kind));
      goto end;

    case TYPE_ARRAY: arraytype(a, (arraytype_t**)tp); goto end;
    case TYPE_FUN:   funtype(a, (funtype_t**)tp); goto end;

    case TYPE_PTR:
    case TYPE_REF:
    case TYPE_MUTREF:
    case TYPE_SLICE:
    case TYPE_MUTSLICE:
      type(a, &((ptrtype_t*)(*tp))->elem); goto end;

    case TYPE_OPTIONAL:    opttype(a, (opttype_t**)tp); goto end;
    case TYPE_STRUCT:      structtype(a, (structtype_t**)tp); goto end;
    case TYPE_ALIAS:       aliastype(a, (aliastype_t**)tp); goto end;
    case TYPE_TEMPLATE:    templatetype(a, (templatetype_t**)tp); goto end;
    case TYPE_PLACEHOLDER: placeholdertype(a, (placeholdertype_t**)tp); goto end;
    case TYPE_UNRESOLVED:  unresolvedtype(a, (unresolvedtype_t**)tp); goto end;

    // should never see these
    case NODE_BAD:
    case NODE_COMMENT:
    case NODE_UNIT:
    case NODE_IMPORTID:
    case NODE_TPLPARAM:
    case NODE_FWDDECL:
    case STMT_TYPEDEF:
    case STMT_IMPORT:
    case EXPR_FUN:
    case EXPR_BLOCK:
    case EXPR_CALL:
    case EXPR_TYPECONS:
    case EXPR_ID:
    case EXPR_NS:
    case EXPR_FIELD:
    case EXPR_PARAM:
    case EXPR_VAR:
    case EXPR_LET:
    case EXPR_MEMBER:
    case EXPR_SUBSCRIPT:
    case EXPR_PREFIXOP:
    case EXPR_POSTFIXOP:
    case EXPR_DEREF:
    case EXPR_BINOP:
    case EXPR_ASSIGN:
    case EXPR_IF:
    case EXPR_FOR:
    case EXPR_RETURN:
    case EXPR_BOOLLIT:
    case EXPR_INTLIT:
    case EXPR_FLOATLIT:
    case EXPR_STRLIT:
    case EXPR_ARRAYLIT:
      break;
  }
  assertf(0, "unexpected %s", nodekind_name((*tp)->kind));
  UNREACHABLE;

end:
  // note: must access local t here as *tp might have been updated
  a->templatenest -= (u32)!!(t->flags & NF_TEMPLATE);
}


static void stmt(typecheck_t* a, stmt_t* n) {
  if UNLIKELY(a->reported_error)
    return;
  if (n->kind == STMT_TYPEDEF) {
    if (n->flags & NF_CHECKED)
      return;
    n->flags |= NF_CHECKED;
    TRACE_NODE(a, "", &n);
    return typedef_(a, (typedef_t*)n);
  }
  assertf(node_isexpr((node_t*)n), "unexpected node %s", nodekind_name(n->kind));
  return expr(a, n);
}


static void exprp(typecheck_t* a, expr_t** np) {
  expr_t* n = *np;

  if (n->flags & NF_CHECKED)
    return;
  n->flags |= NF_CHECKED;

  assertf(node_isexpr((node_t*)n), "%s", nodekind_name(n->kind));

  if UNLIKELY(a->reported_error)
    return;

  TRACE_NODE(a, "", np);

  a->pubnest += (u32)!!(n->flags & NF_VIS_PUB);
  type(a, &n->type);
  a->pubnest -= (u32)!!(n->flags & NF_VIS_PUB);

  switch ((enum nodekind)n->kind) {
  case EXPR_FUN:       return fun(a, (fun_t*)n);
  case EXPR_IF:        return ifexpr(a, (ifexpr_t*)n);
  case EXPR_ID:        return idexpr(a, (idexpr_t*)n);
  case EXPR_NS:        return nsexpr(a, (nsexpr_t*)n);
  case EXPR_RETURN:    return retexpr(a, (retexpr_t*)n);
  case EXPR_BINOP:     return binop(a, (binop_t*)n);
  case EXPR_ASSIGN:    return assign(a, (binop_t*)n);
  case EXPR_BLOCK:     return block(a, (block_t*)n);
  case EXPR_CALL:      return call(a, (call_t**)np);
  case EXPR_TYPECONS:  return typecons(a, (typecons_t**)np);
  case EXPR_MEMBER:    return member(a, (member_t*)n);
  case EXPR_SUBSCRIPT: return subscript(a, (subscript_t*)n);
  case EXPR_DEREF:     return deref(a, (unaryop_t*)n);
  case EXPR_INTLIT:    return intlit(a, (intlit_t*)n);
  case EXPR_FLOATLIT:  return floatlit(a, (floatlit_t*)n);
  case EXPR_STRLIT:    return strlit(a, (strlit_t*)n);
  case EXPR_ARRAYLIT:  return arraylit(a, (arraylit_t*)n);

  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP:
    return unaryop(a, (unaryop_t*)n);

  case EXPR_FIELD:
  case EXPR_PARAM:
    return local(a, (local_t*)n);

  case EXPR_VAR:
  case EXPR_LET:
    return local_var(a, (local_t*)n);

  // TODO
  case EXPR_FOR:
    panic("TODO %s", nodekind_name(n->kind));
    break;

  // We should never see these kinds of nodes
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
  case NODE_IMPORTID:
  case NODE_TPLPARAM:
  case NODE_FWDDECL:
  case STMT_TYPEDEF:
  case STMT_IMPORT:
  case EXPR_BOOLLIT:
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_INT:
  case TYPE_U8:
  case TYPE_U16:
  case TYPE_U32:
  case TYPE_U64:
  case TYPE_UINT:
  case TYPE_F32:
  case TYPE_F64:
  case TYPE_ARRAY:
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_OPTIONAL:
  case TYPE_STRUCT:
  case TYPE_ALIAS:
  case TYPE_NS:
  case TYPE_UNKNOWN:
  case TYPE_TEMPLATE:
  case TYPE_PLACEHOLDER:
  case TYPE_UNRESOLVED:
    break;
  }
  assertf(0, "unexpected node %s", nodekind_name(n->kind));
  UNREACHABLE;
}


static void postanalyze_any(typecheck_t* a, node_t* n);


static void postanalyze_dependency(typecheck_t* a, void* np) {
  node_t* n = np;
  if (n->kind != TYPE_STRUCT)
    return;
  void** vp = map_assign_ptr(&a->postanalyze, a->ma, n);
  if UNLIKELY(!vp)
    return out_of_mem(a);
  if (*vp == (void*)1)
    return;
  *vp = (void*)1;
  postanalyze_any(a, n);
}


static void postanalyze_structtype(typecheck_t* a, structtype_t* st) {
  for (u32 i = 0; i < st->fields.len; i++) {
    local_t* f = (local_t*)st->fields.v[i];
    postanalyze_dependency(a, f->type);
    if (type_isowner(f->type))
      st->flags |= NF_SUBOWNERS;
  }
}


static void postanalyze_any(typecheck_t* a, node_t* n) {
  trace("postanalyze %s#%p %s", nodekind_name(n->kind), n, fmtnode(0, n));
  switch (n->kind) {
  case TYPE_STRUCT: return postanalyze_structtype(a, (structtype_t*)n);
  case TYPE_ALIAS:  return postanalyze_any(a, (node_t*)((aliastype_t*)n)->elem);
  }
}


static void postanalyze(typecheck_t* a) {
  // Keep going until map only has "done" entries (value==1).
  // postanalyze_any may cause additions to the map.
again:
  mapent_t* e = map_it_mut(&a->postanalyze);
  while (map_itnext_mut(&a->postanalyze, &e)) {
    if (e->value == (void*)1)
      continue;
    e->value = (void*)1;
    postanalyze_any(a, (node_t*)e->key);
    goto again;
  }
}


static void report_unknown_import_member(
  typecheck_t* a, import_t* im, importid_t* imid)
{
  sym_t origname = imid->origname ? imid->origname : imid->name;
  error(a, imid->orignameloc,
    "no member \"%s\" in package \"%s\"", origname, im->pkg->path.p);
}


static void import_members(typecheck_t* a, import_t* im) {
  // e.g. import x, y as z from "foo/bar"
  // e.g. import * from "foo/bar"
  // e.g. import *, y as z from "foo/bar"
  assertnotnull(im->idlist);

  const nsexpr_t* api_ns = assertnotnull(im->pkg)->api_ns;
  assertf(api_ns, "pkg(%s)", im->pkg->path.p);
  importid_t* star_imid = NULL;

  for (importid_t* imid = im->idlist; imid; imid = imid->next_id) {
    // '*' imports are denoted by the empty name ("_")
    if (imid->name == sym__) {
      // note: parser has checked that there's only one '*' member
      star_imid = imid;
      continue;
    }

    // find member in package's API namespace
    sym_t origname = imid->origname ? imid->origname : imid->name;
    for (u32 i = 0; ; i++) {
      if UNLIKELY(i == api_ns->members.len) {
        report_unknown_import_member(a, im, imid);
        break;
      }
      if (api_ns->member_names[i] == origname) {
        // note: parser has already checked for duplicate definitions
        // dlog("importing %s as %s => %s",
        //   origname, imid->name, nodekind_name(api_ns->members.v[i]->kind));
        define(a, imid->name, api_ns->members.v[i]);
        break;
      }
    }
  }

  // we are done if there's no '*' member
  if (star_imid == NULL)
    return;

  // import everything from the package, except what has been explicitly specified
  for (u32 i = 0; i < api_ns->members.len; i++) {
    sym_t name = api_ns->member_names[i];

    // see if this member has already been explicitly imported
    bool found = false;
    for (importid_t* imid = im->idlist; imid; imid = imid->next_id) {
      sym_t origname = imid->origname ? imid->origname : imid->name;
      if (origname == name) {
        if (imid->origname) {
          // This aids the following case:
          //   example.co:2:3: error: unknown identifier "print"
          //   22 → │ print("Hello")
          //        │ ~~~~~
          //   example.co:1:12: help: did you mean "p"
          //    5 → │ print as p from "std/runtime"
          //        │          ~
          didyoumean_add(a, imid->name, (node_t*)imid, imid->origname);
        }
        found = true;
        break;
      }
    }
    if (found)
      continue;

    // Now, the parser has not checked for name collisions because it couldn't;
    // it didn't know what members the package exported (not known at parse time.)
    // So we have to check for duplicate definitions here.
    node_t* existing = scope_lookup(&a->scope, name, 0);
    if (!existing) // also look in pkg scope
      existing = pkg_def_get(a->pkg, name);
    if UNLIKELY(existing) {
      dlog("existing %s %u", nodekind_name(existing->kind), loc_line(existing->loc));
      if (scope_lookup(&a->scope, name, 0)) {
        // Collision comes from another import.
        // e.g.
        //   import a from "foo"
        //   import * from "bar" // bar exports "a"
        // Without this special case, error(existing) would report the location of
        // "a" in bar's source, which would be confusing.
        // TODO: better error message, pointing out what previous import collided.
        error(a, star_imid, "importing \"%s\" shadows previous import", name);
      } else {
        error(a, existing, "duplicate definition \"%s\"", name);
        if (loc_line(star_imid->loc)) {
          warning(a, star_imid, "\"%s\" previously imported from package \"%s\"",
            name, im->pkg->path.p);
        }
      }
    } else {
      define(a, name, api_ns->members.v[i]);
    }
  }
}


static void import(typecheck_t* a, import_t* im) {
  if (im->name != sym__) {
    // e.g. import "foo/bar" as lol
    assertnotnull(im->pkg); // should have been resolved by pkgbuild
    assertnotnull(im->pkg->api_ns);
    trace("define \"%s\" = namespace of pkg \"%s\"", im->name, im->pkg->path.p);
    define(a, im->name, im->pkg->api_ns);
  }

  if (im->idlist)
    import_members(a, im);
}


static void assign_nsparent(typecheck_t* a, node_t* n) {
  switch (n->kind) {
    case EXPR_FUN: {
      fun_t* fn = (fun_t*)n;
      if (fn->recvt) {
        // type function
        type(a, &fn->recvt);
        fn->nsparent = (node_t*)fn->recvt;
      } else {
        fn->nsparent = a->nspath.v[a->nspath.len - 1];
      }
      break;
    }
  }
}


static void define_at_unit_level(typecheck_t* a, node_t* n) {
  switch (n->kind) {
    case EXPR_FUN: {
      fun_t* fn = (fun_t*)n;
      assertnotnull(fn->name);
      define(a, fn->name, n);
      break;
    }
  }
}


err_t typecheck(
  compiler_t* c, memalloc_t ast_ma, pkg_t* pkg, unit_t** unitv, u32 unitc)
{
  typecheck_t a = {
    .compiler = c,
    .pkg = pkg,
    .ma = c->ma,
    .ast_ma = ast_ma,
    .typectx = type_void,
  };

  if (!map_init(&a.postanalyze, a.ma, 32))
    return ErrNoMem;
  if (!map_init(&a.tmpmap, a.ma, 32)) {
    a.err = ErrNoMem;
    goto end1;
  }
  if (!map_init(&a.templateimap, a.ma, 32)) {
    a.err = ErrNoMem;
    goto end2;
  }
  if (!map_init(&a.typeidmap, a.ma, 32)) {
    a.err = ErrNoMem;
    goto end3;
  }
  buf_init(&a.tmpbuf, a.ma);

  enter_scope(&a); // package

  for (u32 unit_i = 0; unit_i < unitc; unit_i++) {
    unit_t* unit = unitv[unit_i];

    enter_scope(&a);
    enter_ns(&a, unit);

    for (import_t* im = unit->importlist; im; im = im->next_import)
      import(&a, im);

    // assign parents and define
    for (u32 i = 0; i < unit->children.len; i++) {
      assign_nsparent(&a, unit->children.v[i]);
      define_at_unit_level(&a, unit->children.v[i]);
    }

    for (u32 i = 0; i < unit->children.len; i++)
      stmt(&a, (stmt_t*)unit->children.v[i]);

    leave_ns(&a);
    leave_scope(&a);
  }

  // TODO: should this run after each unit?
  postanalyze(&a);

  leave_scope(&a); // package

  ptrarray_dispose(&a.nspath, a.ma);
  ptrarray_dispose(&a.typectxstack, a.ma);
  array_dispose(didyoumean_t, (array_t*)&a.didyoumean, a.ma);
  buf_dispose(&a.tmpbuf);
  map_dispose(&a.typeidmap, a.ma);
end3:
  map_dispose(&a.templateimap, a.ma);
end2:
  map_dispose(&a.tmpmap, a.ma);
end1:
  map_dispose(&a.postanalyze, a.ma);

  return a.err;
}
