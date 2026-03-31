// specs/parse.md
#include "parse.h"
#include "darray.h"
#include "peg.h"
#include "re.h"
#include "re_ir.h"
#include "token_chunk.h"
#include "ustr.h"
#include "vpa.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Generated DFA lexer functions (linked from compiled nest_lex.ll) ---

typedef struct {
  int64_t state;
  int64_t action;
} LexResult;

extern LexResult lex_main(int64_t state, int64_t cp);
extern LexResult lex_vpa(int64_t state, int64_t cp);
extern LexResult lex_peg(int64_t state, int64_t cp);
extern LexResult lex_re(int64_t state, int64_t cp);
extern LexResult lex_re_ref(int64_t state, int64_t cp);
extern LexResult lex_re_str(int64_t state, int64_t cp);
extern LexResult lex_charclass(int64_t state, int64_t cp);
extern LexResult lex_keyword_str(int64_t state, int64_t cp);

#define LEX_ACTION_NOMATCH (-2)

typedef LexResult (*LexFunc)(int64_t, int64_t);

// --- Error reporting ---

bool parse_has_error(ParseState* ps) { return ps->error[0] != '\0'; }

static void _error_at(ParseState* ps, Token* t, const char* fmt, ...) {
  if (parse_has_error(ps)) {
    return;
  }
  int32_t off = 0;
  if (t && ps->tree) {
    Location loc = tc_locate(ps->tree, t->cp_start);
    off = snprintf(ps->error, sizeof(ps->error), "%d:%d: ", loc.line + 1, loc.col + 1);
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ps->error + off, sizeof(ps->error) - (size_t)off, fmt, ap);
  va_end(ap);
}

void parse_error(ParseState* ps, const char* fmt, ...) {
  if (parse_has_error(ps)) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ps->error, sizeof(ps->error), fmt, ap);
  va_end(ap);
}

// --- String allocation helpers ---

static char* _cp_strdup(const char* src, int32_t cp_start, int32_t cp_size) {
  UstrIter it = {0};
  ustr_iter_init(&it, src, cp_start);
  int32_t start_byte = it.byte_off;
  for (int32_t i = 0; i < cp_size; i++) {
    ustr_iter_next(&it);
  }
  int32_t byte_len = it.byte_off - start_byte;
  char* s = malloc((size_t)byte_len + 1);
  memcpy(s, src + start_byte, (size_t)byte_len);
  s[byte_len] = '\0';
  return s;
}

static char* _tok_strdup(ParseState* ps, Token* t) {
  return _cp_strdup(ps->src, t->cp_start, t->cp_size);
}

static char* _tok_strdup_skip(ParseState* ps, Token* t, int32_t skip) {
  return _cp_strdup(ps->src, t->cp_start + skip, t->cp_size - skip);
}

__attribute__((format(printf, 1, 2))) char* parse_sfmt(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int32_t len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char* s = malloc((size_t)len + 1);
  va_start(ap, fmt);
  vsnprintf(s, (size_t)len + 1, fmt, ap);
  va_end(ap);
  return s;
}

void parse_set_str(char** dst, char* s) {
  free(*dst);
  *dst = s;
}

// --- Pushdown automaton lexer ---

typedef struct {
  ParseState* ps;
  TokenTree* tree;
  int32_t cp_count;
  UstrIter it;
  int32_t last_quote_cp;
} LexCtx;

typedef struct {
  ScopeId id;
  LexFunc lex_fn;
} ScopeConfig;

static int32_t _next_cp(LexCtx* ctx) {
  if (ctx->it.cp_idx >= ctx->cp_count) {
    return -2;
  }
  return ustr_iter_next(&ctx->it);
}

static void _lex_scope(LexCtx* ctx, ScopeId scope_id);

// --- Decode a codepoint from a token span ---

static int32_t _decode_codepoint(const char* src, Token* t) {
  UstrIter it = {0};
  ustr_iter_init(&it, src, t->cp_start);
  const char* p = src + it.byte_off;

  if (t->tok_id == TOK_CODEPOINT) {
    return re_hex_to_codepoint(p + 2, (size_t)(t->cp_size - 3));
  }
  if (t->tok_id == TOK_C_ESCAPE && t->cp_size >= 2) {
    int32_t esc = re_c_escape(p[1]);
    return esc >= 0 ? esc : (int32_t)(unsigned char)p[1];
  }
  if (t->tok_id == TOK_PLAIN_ESCAPE && t->cp_size >= 2) {
    ustr_iter_next(&it);
    return ustr_iter_next(&it);
  }
  return ustr_iter_next(&it);
}

// --- ReIr parsing from token chunk (recursive descent) ---

typedef struct {
  ParseState* ps;
  Token* tokens;
  int32_t count;
  int32_t pos;
  bool icase;
} ReParseCtx;

static Token* _re_peek(ReParseCtx* rctx) {
  if (rctx->pos < rctx->count) {
    return &rctx->tokens[rctx->pos];
  }
  return NULL;
}

static Token* _re_next(ReParseCtx* rctx) {
  Token* t = _re_peek(rctx);
  if (t) {
    rctx->pos++;
  }
  return t;
}

static bool _re_at(ReParseCtx* rctx, int32_t id) {
  Token* t = _re_peek(rctx);
  return t && t->tok_id == id;
}

static ReIr _parse_re_expr(ReParseCtx* rctx, ReIr ir);

static ReIr _parse_charclass(ReParseCtx* rctx, ReIr ir) {
  Token* begin = _re_next(rctx);
  if (!begin) {
    return ir;
  }
  ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);

  bool neg = false;
  {
    UstrIter nit = {0};
    ustr_iter_init(&nit, rctx->ps->src, begin->cp_start);
    for (int32_t i = 0; i < begin->cp_size; i++) {
      if (ustr_iter_next(&nit) == '^') {
        neg = true;
        break;
      }
    }
  }
  if (neg) {
    ir = re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0);
  }

  while (rctx->pos < rctx->count) {
    Token* t = _re_peek(rctx);
    if (!t) {
      break;
    }
    if ((t->tok_id == TOK_CHAR || t->tok_id == TOK_CODEPOINT || t->tok_id == TOK_C_ESCAPE ||
         t->tok_id == TOK_PLAIN_ESCAPE) &&
        rctx->pos + 2 < rctx->count && rctx->tokens[rctx->pos + 1].tok_id == TOK_RANGE_SEP) {
      int32_t lo = _decode_codepoint(rctx->ps->src, t);
      rctx->pos += 2;
      Token* hi_t = _re_next(rctx);
      int32_t hi = hi_t ? _decode_codepoint(rctx->ps->src, hi_t) : lo;
      ir = re_ir_emit(ir, RE_IR_APPEND_CH, lo, hi);
    } else if (t->tok_id == TOK_CHAR || t->tok_id == TOK_CODEPOINT || t->tok_id == TOK_C_ESCAPE ||
               t->tok_id == TOK_PLAIN_ESCAPE) {
      int32_t cp = _decode_codepoint(rctx->ps->src, t);
      _re_next(rctx);
      ir = re_ir_emit(ir, RE_IR_APPEND_CH, cp, cp);
    } else {
      break;
    }
  }

  if (rctx->icase) {
    ir = re_ir_emit(ir, RE_IR_RANGE_IC, 0, 0);
  }
  ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
  return ir;
}

static ReIr _emit_shorthand_class(ReIr ir, int32_t tok_id) {
  switch (tok_id) {
  case TOK_RE_DOT:
    ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
    ir = re_ir_emit(ir, RE_IR_RANGE_NEG, 0, 0);
    ir = re_ir_emit(ir, RE_IR_APPEND_CH, '\n', '\n');
    ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
    break;
  case TOK_RE_SPACE_CLASS:
    ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_S, 0, 0);
    ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
    break;
  case TOK_RE_WORD_CLASS:
    ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_W, 0, 0);
    ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
    break;
  case TOK_RE_DIGIT_CLASS:
    ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_D, 0, 0);
    ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
    break;
  case TOK_RE_HEX_CLASS:
    ir = re_ir_emit(ir, RE_IR_RANGE_BEGIN, 0, 0);
    ir = re_ir_emit(ir, RE_IR_APPEND_GROUP_H, 0, 0);
    ir = re_ir_emit(ir, RE_IR_RANGE_END, 0, 0);
    break;
  default:
    break;
  }
  return ir;
}

static ReIr _parse_re_unit(ReParseCtx* rctx, ReIr ir) {
  Token* t = _re_peek(rctx);
  if (!t) {
    return ir;
  }

  switch (t->tok_id) {
  case TOK_RE_OPS_LPAREN:
    _re_next(rctx);
    ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
    ir = _parse_re_expr(rctx, ir);
    if (_re_at(rctx, TOK_RE_OPS_RPAREN)) {
      _re_next(rctx);
    }
    ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
    break;

  case TOK_CHARCLASS_BEGIN:
    ir = _parse_charclass(rctx, ir);
    break;

  case TOK_RE_DOT:
  case TOK_RE_SPACE_CLASS:
  case TOK_RE_WORD_CLASS:
  case TOK_RE_DIGIT_CLASS:
  case TOK_RE_HEX_CLASS:
    _re_next(rctx);
    ir = _emit_shorthand_class(ir, t->tok_id);
    break;

  case TOK_RE_BOF:
    _re_next(rctx);
    ir = re_ir_emit_ch(ir, LEX_CP_BOF);
    break;

  case TOK_RE_EOF:
    _re_next(rctx);
    ir = re_ir_emit_ch(ir, LEX_CP_EOF);
    break;

  case TOK_RE_REF: {
    _re_next(rctx);
    ir = re_ir_emit(ir, RE_IR_FRAG_REF, t->cp_start, t->cp_size);
    break;
  }

  case TOK_CHAR:
  case TOK_CODEPOINT:
  case TOK_C_ESCAPE:
  case TOK_PLAIN_ESCAPE: {
    _re_next(rctx);
    int32_t cp = _decode_codepoint(rctx->ps->src, t);
    ir = re_ir_emit(ir, rctx->icase ? RE_IR_APPEND_CH_IC : RE_IR_APPEND_CH, cp, cp);
    break;
  }

  default:
    return ir;
  }
}

static bool _is_re_unit_start(int32_t id) {
  return id == TOK_RE_OPS_LPAREN || id == TOK_CHARCLASS_BEGIN || id == TOK_RE_DOT || id == TOK_RE_SPACE_CLASS ||
         id == TOK_RE_WORD_CLASS || id == TOK_RE_DIGIT_CLASS || id == TOK_RE_HEX_CLASS || id == TOK_RE_BOF ||
         id == TOK_RE_EOF || id == TOK_RE_REF || id == TOK_CHAR || id == TOK_CODEPOINT || id == TOK_C_ESCAPE ||
         id == TOK_PLAIN_ESCAPE;
}

static ReIr _parse_re_quantified(ReParseCtx* rctx, ReIr ir) {
  int32_t unit_start = (int32_t)darray_size(ir);
  ir = _parse_re_unit(rctx, ir);
  int32_t unit_end = (int32_t)darray_size(ir);
  if (unit_start == unit_end) {
    return ir;
  }

  Token* q = _re_peek(rctx);
  if (!q) {
    return ir;
  }

  if (q->tok_id == TOK_RE_OPS_MAYBE) {
    _re_next(rctx);
    ReIrOp lparen = {RE_IR_LPAREN, 0, 0};
    ir = darray_grow(ir, darray_size(ir) + 1);
    memmove(&ir[unit_start + 1], &ir[unit_start], (size_t)(unit_end - unit_start) * sizeof(ReIrOp));
    ir[unit_start] = lparen;
    ir = re_ir_emit(ir, RE_FORK, 0, 0);
    ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  } else if (q->tok_id == TOK_RE_OPS_PLUS) {
    _re_next(rctx);
    ir = re_ir_emit(ir, RE_IR_LPAREN, 0, 0);
    for (int32_t i = unit_start; i < unit_end; i++) {
      darray_push(ir, ir[i]);
    }
    ir = re_ir_emit(ir, RE_FORK, 0, 0);
    ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  } else if (q->tok_id == TOK_RE_OPS_STAR) {
    _re_next(rctx);
    ReIrOp lparen = {RE_IR_LPAREN, 0, 0};
    ir = darray_grow(ir, darray_size(ir) + 1);
    memmove(&ir[unit_start + 1], &ir[unit_start], (size_t)(unit_end - unit_start) * sizeof(ReIrOp));
    ir[unit_start] = lparen;
    for (int32_t i = unit_start + 1; i <= unit_end; i++) {
      darray_push(ir, ir[i]);
    }
    ir = re_ir_emit(ir, RE_FORK, 0, 0);
    ir = re_ir_emit(ir, RE_IR_RPAREN, 0, 0);
  }
}

static ReIr _parse_re_expr(ReParseCtx* rctx, ReIr ir) {
  while (rctx->pos < rctx->count && _is_re_unit_start(_re_peek(rctx)->tok_id)) {
    ir = _parse_re_quantified(rctx, ir);
  }

  while (_re_at(rctx, TOK_RE_OPS_ALT)) {
    _re_next(rctx);
    ir = re_ir_emit(ir, RE_FORK, 0, 0);
    while (rctx->pos < rctx->count) {
      Token* t = _re_peek(rctx);
      if (!t || !_is_re_unit_start(t->tok_id)) {
        break;
      }
      ir = _parse_re_quantified(rctx, ir);
    }
  }
  return ir;
}

static ReIr _parse_re_tokens(ParseState* ps, Token* tokens, int32_t count, bool icase) {
  ReParseCtx rctx = {.ps = ps, .tokens = tokens, .count = count, .pos = 0, .icase = icase};
  ReIr ir = re_ir_new();
  ir = _parse_re_expr(&rctx, ir);
  return ir;
}

// --- Scope-specific lex handlers ---

static void _handle_re_scope(LexCtx* ctx, int32_t tag_start, int32_t tag_size) {
  int32_t child_idx = (int32_t)darray_size(ctx->tree->table);
  _lex_scope(ctx, SCOPE_RE);
  TokenChunk* child = &ctx->tree->table[child_idx];

  bool icase = false;
  {
    UstrIter fit = {0};
    ustr_iter_init(&fit, ctx->ps->src, tag_start);
    for (int32_t i = 0; i < tag_size; i++) {
      int32_t ch = ustr_iter_next(&fit);
      if (ch == '/') {
        break;
      }
      if (ch == 'i') {
        icase = true;
      }
    }
  }

  int32_t count = (int32_t)darray_size(child->tokens);
  ReIr ir = _parse_re_tokens(ctx->ps, child->tokens, count, icase);

  if (!ctx->ps->re_irs) {
    ctx->ps->re_irs = darray_new(sizeof(ReIr), 0);
  }
  int32_t idx = (int32_t)darray_size(ctx->ps->re_irs);
  darray_push(ctx->ps->re_irs, ir);

  tc_add(ctx->tree->current,
         (Token){.tok_id = TOK_RE_AST_BASE + idx, .cp_start = tag_start, .cp_size = tag_size});
}

static void _push_buf(DStr buf, char slice[4]) {
  char* b = (char*)buf;
  for (int32_t j = 0; slice[j]; j++) {
    darray_push(b, slice[j]);
  }
}

static DStr _parse_keyword_str(const char* src, TokenChunk* chunk) {
  DStr buf = darray_new(sizeof(char), 0);
  char* b = (char*)buf;
  int32_t n = (int32_t)darray_size(chunk->tokens);
  for (int32_t i = 0; i < n; i++) {
    Token* t = &chunk->tokens[i];
    switch (t->tok_id) {
      case TOK_CHAR: {
        UstrCpBuf slice = ustr_slice_cp(src, t->cp_start);
        _push_buf(buf, slice.buf);
        break;
      }
      case TOK_CODEPOINT: {
        UstrIter it = {0};
        ustr_iter_init(&it, src, t->cp_start + 1);
        int cp = re_hex_to_codepoint(src + it.byte_off, t->cp_size - 1);
        char slice[4] = {0};
        ustr_encode_utf8(slice, cp);
        _push_buf(buf, slice);
        break;
      }
      case TOK_C_ESCAPE: {
        UstrIter it = {0};
        ustr_iter_init(&it, src, t->cp_start + 1);
        char* b = (char*)buf;
        darray_push(b, re_c_escape(src[it.byte_off]));
        break;
      }
      case TOK_PLAIN_ESCAPE: {
        UstrCpBuf slice = ustr_slice_cp(src, t->cp_start + 1);
        _push_buf(buf, slice.buf);
        break;
      }
    }
  }
  darray_push(b, '\0');
  return buf;
}

// --- Pushdown automaton lexer ---

static void _lex_scope(LexCtx* ctx, ScopeId scope_id) {
  static const ScopeConfig configs[] = {
      [SCOPE_MAIN] = {SCOPE_MAIN, lex_main},
      [SCOPE_VPA] = {SCOPE_VPA, lex_vpa},
      [SCOPE_PEG] = {SCOPE_PEG, lex_peg},
      [SCOPE_RE] = {SCOPE_RE, lex_re},
      [SCOPE_RE_REF] = {SCOPE_RE_REF, lex_re_ref},
      [SCOPE_RE_STR] = {SCOPE_RE_STR, lex_re_str},
      [SCOPE_CHARCLASS] = {SCOPE_CHARCLASS, lex_charclass},
      [SCOPE_KEYWORD_STR] = {SCOPE_KEYWORD_STR, lex_keyword_str},
  };
  ScopeConfig cfg = configs[scope_id];

  tc_push(ctx->tree);
  ctx->tree->current->scope_id = scope_id;

  int64_t state = 0;
  int32_t last_action = 0;
  int32_t tok_cp_start = ctx->it.cp_idx;

  while (ctx->it.cp_idx < ctx->cp_count || last_action != 0) {
    int32_t cp_cp_idx = ctx->it.cp_idx;
    int32_t cp = _next_cp(ctx);

    LexResult r = cfg.lex_fn(state, cp);

    if (r.action == LEX_ACTION_NOMATCH) {
      // Rewind: the NOMATCH character belongs to the next token/scope
      ustr_iter_init(&ctx->it, ctx->ps->src, cp_cp_idx);

      if (last_action == TOK_END) {
        tc_pop(ctx->tree);
        return;
      } else if (last_action == TOK_UNPARSE_END) {
        ctx->it.cp_idx = cp_cp_idx;
        tc_pop(ctx->tree);
        return;
      } else if (last_action == TOK_IGNORE) {
        // skip
      } else if (last_action == TOK_SET_QUOTE) {
        ctx->last_quote_cp = ustr_slice_cp(ctx->ps->src, tok_cp_start).buf[0];
      } else if (last_action == TOK_STR_CHECK_END) {
        int32_t quote_cp = ctx->last_quote_cp;
        if (quote_cp == ctx->last_quote_cp) {
          tc_pop(ctx->tree);
          return;
        } else {
          tc_add(ctx->tree->current,
                 (Token){.tok_id = TOK_CHAR, .cp_start = tok_cp_start, .cp_size = cp_cp_idx - tok_cp_start});
        }
      } else if (last_action > 0 && last_action < SCOPE_COUNT) {
        if (last_action == SCOPE_KEYWORD_STR || last_action == SCOPE_RE_STR) {
          ctx->last_quote_cp = tok_cp_start;
        }
        _lex_scope(ctx, last_action);
      } else if (last_action == 0) {
        fprintf(stderr, "unreachable: NOMATCH with no last action\n");
        abort();
      } else {
        tc_add(ctx->tree->current,
               (Token){.tok_id = last_action, .cp_start = tok_cp_start, .cp_size = cp_cp_idx - tok_cp_start});
      }

      // end of main
      if (cp_cp_idx >= ctx->cp_count) {
        tc_pop(ctx->tree);
        return;
      }

      tok_cp_start = cp_cp_idx;
      last_action = 0;
      state = 0;
    } else {
      state = r.state;
      last_action = r.action;
    }
  }
  tc_pop(ctx->tree);
}

// --- Token cursor helpers (operate on ps->read_chunk) ---

static int32_t _chunk_size(ParseState* ps) { return (int32_t)darray_size(ps->read_chunk->tokens); }

static Token* _peek(ParseState* ps) {
  if (ps->tpos < _chunk_size(ps)) {
    return &ps->read_chunk->tokens[ps->tpos];
  }
  return NULL;
}

static Token* _next(ParseState* ps) {
  Token* t = _peek(ps);
  if (t) {
    ps->tpos++;
  }
  return t;
}

static void _skip_nl(ParseState* ps) {
  while (ps->tpos < _chunk_size(ps) && ps->read_chunk->tokens[ps->tpos].tok_id == TOK_NL) {
    ps->tpos++;
  }
}

static bool _at_end(ParseState* ps) { return ps->tpos >= _chunk_size(ps); }

static bool _at(ParseState* ps, int32_t id) {
  Token* t = _peek(ps);
  return t && t->tok_id == id;
}

// --- Shared VPA followup parsing ---

static bool _parse_vpa_scope_body(ParseState* ps, VpaRule* rule);

static void _parse_unit_followups(ParseState* ps, VpaUnit* unit, VpaRule* rule) {
  for (;;) {
    Token* ft = _peek(ps);
    if (!ft || ft->tok_id == TOK_NL) {
      break;
    }
    if (ft->tok_id == TOK_TOK_ID) {
      _next(ps);
      parse_set_str(&unit->name, _tok_strdup_skip(ps, ft, 1));
    } else if (ft->tok_id == TOK_HOOK_BEGIN || ft->tok_id == TOK_HOOK_END || ft->tok_id == TOK_HOOK_FAIL ||
               ft->tok_id == TOK_HOOK_UNPARSE) {
      _next(ps);
      unit->hook = ft->tok_id;
    } else if (ft->tok_id == TOK_USER_HOOK_ID) {
      _next(ps);
      parse_set_str(&unit->user_hook, _tok_strdup(ps, ft));
    } else if (ft->tok_id == TOK_SCOPE_BEGIN) {
      _next(ps);
      unit->kind = VPA_SCOPE;
      VpaRule scope_rule = {.name = strdup(rule->name), .is_scope = true};
      _parse_vpa_scope_body(ps, &scope_rule);
      unit->children = scope_rule.units;
      free(scope_rule.name);
    } else {
      break;
    }
  }
}

// --- VPA rule body: bool-returning try-functions ---

static void _add_vpa_unit(VpaRule* rule, VpaUnit unit) {
  if (!rule->units) {
    rule->units = darray_new(sizeof(VpaUnit), 0);
  }
  darray_push(rule->units, unit);
}

static bool _parse_re(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t) {
    return false;
  }
  _next(ps);
  VpaUnit unit = {.kind = VPA_REGEXP};

  int32_t ir_idx = t->tok_id - TOK_RE_AST_BASE;
  unit.re = ps->re_irs[ir_idx];
  ps->re_irs[ir_idx] = NULL;

  if (!unit.re || darray_size(unit.re) == 0) {
    _error_at(ps, t, "empty regexp");
    re_ir_free(unit.re);
    return false;
  }

  unit.binary_mode = false;
  {
    UstrIter fit = {0};
    ustr_iter_init(&fit, ps->src, t->cp_start);
    for (int32_t i = 0; i < t->cp_size; i++) {
      int32_t ch = ustr_iter_next(&fit);
      if (ch == '/') {
        break;
      }
      if (ch == 'b') {
        unit.binary_mode = true;
      }
    }
  }

  _parse_unit_followups(ps, &unit, rule);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_re_str(ParseState* ps, VpaRule* rule) {
  VpaUnit unit = {.kind = VPA_REGEXP};
  unit.re = re_ir_new();
  for (Token* t = _peek(ps); t; _next(ps)) {
    switch (t->tok_id) {
      case TOK_CHAR: {
        int32_t cp = ustr_cp_at(ps->src, t->cp_start);
        unit.re = re_ir_emit_ch(unit.re, cp);
        break;
      }
      case TOK_CODEPOINT: {
        UstrIter it = {0};
        ustr_iter_init(&it, ps->src, t->cp_start + 1);
        int32_t cp = re_hex_to_codepoint(ps->src + it.byte_off, t->cp_size - 1);
        unit.re = re_ir_emit_ch(unit.re, cp);
        break;
      }
      case TOK_C_ESCAPE: {
        UstrIter it = {0};
        ustr_iter_init(&it, ps->src, t->cp_start + 1);
        char c = re_c_escape(ps->src[it.byte_off]);
        unit.re = re_ir_emit_ch(unit.re, c);
        break;
      }
      case TOK_PLAIN_ESCAPE: {
        int32_t cp = ustr_cp_at(ps->src, t->cp_start + 1);
        unit.re = re_ir_emit_ch(unit.re, cp);
        break;
      }
    }

    t = _peek(ps);
  }

  _parse_unit_followups(ps, &unit, rule);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_vpa_ref(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t || t->tok_id != TOK_VPA_ID) {
    return false;
  }
  _next(ps);
  VpaUnit unit = {.kind = VPA_REF, .name = _tok_strdup(ps, t)};
  _parse_unit_followups(ps, &unit, rule);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_re_frag_ref(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t || t->tok_id != TOK_RE_FRAG_ID) {
    return false;
  }
  _next(ps);
  char* frag_name = _tok_strdup(ps, t);
  VpaUnit unit = {.kind = VPA_REGEXP};

  // Look up fragment
  bool found = false;
  for (int32_t i = 0; i < (int32_t)darray_size(ps->re_frags); i++) {
    if (strcmp(ps->re_frags[i].name, frag_name) == 0) {
      unit.re = re_ir_clone(ps->re_frags[i].re);
      found = true;
      break;
    }
  }
  if (!found) {
    _error_at(ps, t, "undefined fragment '%s'", frag_name);
    free(frag_name);
    return false;
  }
  free(frag_name);

  _parse_unit_followups(ps, &unit, rule);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_vpa_macro_ref(ParseState* ps, VpaRule* rule) {
  Token* t = _peek(ps);
  if (!t || t->tok_id != TOK_MACRO_ID) {
    return false;
  }
  _next(ps);
  char* base = _tok_strdup_skip(ps, t, 1);
  VpaUnit unit = {.kind = VPA_REF, .name = parse_sfmt("*%s", base)};
  free(base);
  _add_vpa_unit(rule, unit);
  return true;
}

static bool _parse_vpa_rule_body(ParseState* ps, VpaRule* rule) {
  if (_parse_re(ps, rule) || _parse_re_frag_ref(ps, rule) || _parse_re_str(ps, rule) ||
      _parse_vpa_ref(ps, rule) || _parse_vpa_macro_ref(ps, rule)) {
    return true;
  }
  _error_at(ps, _peek(ps), "unexpected token in rule body");
  return false;
}

static bool _parse_vpa_scope_body(ParseState* ps, VpaRule* rule) {
  _skip_nl(ps);
  while (!_at_end(ps) && !_at(ps, TOK_SCOPE_END)) {
    if (!_parse_vpa_rule_body(ps, rule)) {
      return false;
    }
  }
  Token* close = _peek(ps);
  if (!close || close->tok_id != TOK_SCOPE_END) {
    _error_at(ps, close, "expected '}'");
    return false;
  }
  _next(ps);
  return true;
}

static bool _parse_vpa_rule_line(ParseState* ps, VpaRule* rule) {
  while (!_at_end(ps) && !_at(ps, TOK_NL)) {
    if (!_parse_vpa_rule_body(ps, rule)) {
      return false;
    }
  }
  return true;
}

// --- VPA section: bool-returning top-level dispatchers ---

static bool _parse_vpa_rule(ParseState* ps) {
  if (!_at(ps, TOK_VPA_ID)) {
    return false;
  }
  Token* name_tok = _next(ps);
  if (!_at(ps, TOK_OPS_EQ)) {
    _error_at(ps, name_tok, "expected '=' after rule name");
    return false;
  }
  _next(ps);
  if (!ps->vpa_rules) {
    ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  }
  darray_push(ps->vpa_rules, ((VpaRule){0}));
  VpaRule* rule = &ps->vpa_rules[darray_size(ps->vpa_rules) - 1];
  rule->name = _tok_strdup(ps, name_tok);

  if (_at(ps, TOK_SCOPE_BEGIN)) {
    _next(ps);
    rule->is_scope = true;
    return _parse_vpa_scope_body(ps, rule);
  }
  return _parse_vpa_rule_line(ps, rule);
}

static bool _parse_macro_rule(ParseState* ps) {
  if (!_at(ps, TOK_MACRO_ID)) {
    return false;
  }
  Token* name_tok = _next(ps);
  if (!_at(ps, TOK_OPS_EQ)) {
    _error_at(ps, name_tok, "expected '=' after macro name");
    return false;
  }
  _next(ps);
  if (!ps->vpa_rules) {
    ps->vpa_rules = darray_new(sizeof(VpaRule), 0);
  }
  darray_push(ps->vpa_rules, ((VpaRule){0}));
  VpaRule* rule = &ps->vpa_rules[darray_size(ps->vpa_rules) - 1];
  rule->name = _tok_strdup_skip(ps, name_tok, 1);
  rule->is_macro = true;

  if (_at(ps, TOK_SCOPE_BEGIN)) {
    _next(ps);
    rule->is_scope = true;
    return _parse_vpa_scope_body(ps, rule);
  }
  return _parse_vpa_rule_line(ps, rule);
}

static bool _parse_keyword_def(ParseState* ps) {
  if (!_at(ps, TOK_DIRECTIVES_KEYWORD)) {
    return false;
  }
  Token* kw_tok = _next(ps);

  Token* group_tok = _next(ps);
  if (!group_tok || group_tok->tok_id != TOK_VPA_ID) {
    _error_at(ps, kw_tok, "expected keyword group name after %%keyword");
    return false;
  }
  char* group = _tok_strdup(ps, group_tok);

  while (!_at_end(ps) && !_at(ps, TOK_NL)) {
    Token* t = _peek(ps);
    if (t->tok_id < TOK_STR_SPAN_BASE) {
      break;
    }
    _next(ps);
    StrSpan span = ps->str_spans[t->tok_id - TOK_STR_SPAN_BASE];
    if (!ps->keywords) {
      ps->keywords = darray_new(sizeof(KeywordEntry), 0);
    }
    KeywordEntry kw = {.group = strdup(group), .lit_off = span.off, .lit_len = span.len, .src = ps->src};
    darray_push(ps->keywords, kw);
  }
  free(group);
  return true;
}

static bool _parse_ignore_decl(ParseState* ps) {
  if (!_at(ps, TOK_DIRECTIVES_IGNORE)) {
    return false;
  }
  _next(ps);
  while (!_at_end(ps) && !_at(ps, TOK_NL)) {
    Token* t = _peek(ps);
    if (t->tok_id != TOK_TOK_ID) {
      break;
    }
    _next(ps);
    if (!ps->ignores.names) {
      ps->ignores.names = darray_new(sizeof(char*), 0);
    }
    char* name = _tok_strdup_skip(ps, t, 1);
    darray_push(ps->ignores.names, name);
  }
  return true;
}

static bool _parse_effect_decl(ParseState* ps) {
  if (!_at(ps, TOK_DIRECTIVES_EFFECT)) {
    return false;
  }
  Token* effect_tok = _next(ps);

  Token* hook_tok = _next(ps);
  if (!hook_tok || hook_tok->tok_id != TOK_USER_HOOK_ID) {
    _error_at(ps, effect_tok, "expected hook name after %%effect");
    return false;
  }
  if (!_at(ps, TOK_OPS_EQ)) {
    _error_at(ps, hook_tok, "expected '=' in %%effect");
    return false;
  }
  _next(ps);

  if (!ps->effects) {
    ps->effects = darray_new(sizeof(EffectDecl), 0);
  }
  EffectDecl ed = {.hook_name = _tok_strdup(ps, hook_tok), .effects = darray_new(sizeof(int32_t), 0)};

  while (!_at_end(ps) && !_at(ps, TOK_NL)) {
    Token* t = _peek(ps);
    if (t->tok_id == TOK_OPS_PIPE) {
      _next(ps);
      continue;
    }
    if (t->tok_id == TOK_TOK_ID || t->tok_id == TOK_HOOK_BEGIN || t->tok_id == TOK_HOOK_END ||
        t->tok_id == TOK_HOOK_FAIL || t->tok_id == TOK_HOOK_UNPARSE) {
      _next(ps);
      darray_push(ed.effects, t->tok_id);
    } else {
      break;
    }
  }
  darray_push(ps->effects, ed);
  return true;
}

static bool _parse_define_decl(ParseState* ps) {
  if (!_at(ps, TOK_DIRECTIVES_DEFINE)) {
    return false;
  }
  Token* def_tok = _next(ps);

  Token* name_tok = _next(ps);
  if (!name_tok || name_tok->tok_id != TOK_RE_FRAG_ID) {
    _error_at(ps, def_tok, "expected fragment name after %%define");
    return false;
  }

  Token* re_tok = _next(ps);
  if (!re_tok || re_tok->tok_id < TOK_RE_AST_BASE ||
      re_tok->tok_id >= TOK_RE_AST_BASE + (int32_t)darray_size(ps->re_irs)) {
    _error_at(ps, name_tok, "expected regex after fragment name in %%define");
    return false;
  }

  int32_t ir_idx = re_tok->tok_id - TOK_RE_AST_BASE;
  ReFragment frag = {.name = _tok_strdup(ps, name_tok), .re = ps->re_irs[ir_idx]};
  ps->re_irs[ir_idx] = NULL;

  if (!ps->re_frags) {
    ps->re_frags = darray_new(sizeof(ReFragment), 0);
  }
  darray_push(ps->re_frags, frag);
  return true;
}

static bool _parse_vpa_section(ParseState* ps) {
  _skip_nl(ps);
  while (!_at_end(ps)) {
    Token* t = _peek(ps);
    if (!t) {
      break;
    }
    if (t->tok_id == TOK_NL) {
      ps->tpos++;
      continue;
    }
    if (t->tok_id == TOK_PEG_ID || t->tok_id == TOK_PEG_TOK_ID || t->tok_id == TOK_PEG_OPS_ASSIGN) {
      break;
    }
    if (_parse_keyword_def(ps) || _parse_ignore_decl(ps) || _parse_state_decl(ps) || _parse_effect_decl(ps) ||
        _parse_define_decl(ps) || _parse_macro_rule(ps) || _parse_vpa_rule(ps)) {
      if (parse_has_error(ps)) {
        return false;
      }
      continue;
    }
    if (parse_has_error(ps)) {
      return false;
    }
    _error_at(ps, t, "unexpected token in [[vpa]] section");
    return false;
  }
  return true;
}

// --- Recursive descent: PEG section ---

static bool _parse_peg_seq(ParseState* ps, PegUnit* seq);

static bool _parse_peg_unit(ParseState* ps, PegUnit* unit) {
  Token* t = _peek(ps);
  if (!t) {
    return true;
  }

  if (t->tok_id == TOK_PEG_ID) {
    _next(ps);
    unit->kind = PEG_ID;
    unit->name = _tok_strdup(ps, t);
  } else if (t->tok_id == TOK_PEG_TOK_ID) {
    _next(ps);
    unit->kind = PEG_TOK;
    unit->name = _tok_strdup_skip(ps, t, 1);
  } else if (t->tok_id == SCOPE_KEYWORD_STR) {
    unit->kind = PEG_KEYWORD_TOK;
    DStr kw_str = _parse_keyword_str(ps->src, ps->read_chunk);
    unit->name = strdup(kw_str);
    darray_del(kw_str);
  } else if (t->tok_id == TOK_BRANCHES_BEGIN) {
    _next(ps);
    unit->kind = PEG_BRANCHES;
    _skip_nl(ps);
    while (!_at_end(ps) && !_at(ps, TOK_BRANCHES_END)) {
      if (_at(ps, TOK_NL)) {
        ps->tpos++;
        continue;
      }
      if (!unit->children) {
        unit->children = darray_new(sizeof(PegUnit), 0);
      }
      darray_push(unit->children, ((PegUnit){0}));
      PegUnit* branch = &unit->children[darray_size(unit->children) - 1];
      branch->kind = PEG_SEQ;
      if (!_parse_peg_seq(ps, branch)) {
        return false;
      }

      Token* tag_t = _peek(ps);
      if (tag_t && tag_t->tok_id == TOK_TAG_ID) {
        _next(ps);
        char* tag_text = _tok_strdup(ps, tag_t);
        char* p = tag_text;
        if (*p == ':') {
          p++;
        }
        while (*p == ' ') {
          p++;
        }
        branch->tag = strdup(p);
        free(tag_text);
      }
    }
    Token* close = _peek(ps);
    if (!close || close->tok_id != TOK_BRANCHES_END) {
      _error_at(ps, close ? close : t, "expected ']' to close branches");
      return false;
    }
    _next(ps);
    return true;
  } else {
    return true;
  }

  // Multipliers
  Token* mt = _peek(ps);
  if (mt) {
    if (mt->tok_id == TOK_PEG_OPS_QUESTION) {
      _next(ps);
      unit->multiplier = '?';
    } else if (mt->tok_id == TOK_PEG_OPS_PLUS) {
      _next(ps);
      unit->multiplier = '+';
    } else if (mt->tok_id == TOK_PEG_OPS_STAR) {
      _next(ps);
      unit->multiplier = '*';
    }

    if (unit->multiplier == '+' || unit->multiplier == '*') {
      if (_at(ps, TOK_PEG_OPS_LT)) {
        _next(ps);
        unit->interlace = calloc(1, sizeof(PegUnit));
        unit->interlace->kind = PEG_SEQ;
        if (!_parse_peg_seq(ps, unit->interlace)) {
          return false;
        }
        unit->ninterlace = 1;
        Token* gt = _peek(ps);
        if (!gt || gt->tok_id != TOK_PEG_OPS_GT) {
          _error_at(ps, gt, "expected '>' to close interlace");
          return false;
        }
        _next(ps);
      }
    }
  }
  return true;
}

static bool _is_peg_unit_start(int32_t id) {
  return id == TOK_PEG_ID || id == TOK_PEG_TOK_ID || id == TOK_BRANCHES_BEGIN;
}

static bool _parse_peg_seq(ParseState* ps, PegUnit* seq) {
  while (!_at_end(ps)) {
    Token* t = _peek(ps);
    if (!t || !_is_peg_unit_start(t->tok_id)) {
      break;
    }
    if (!seq->children) {
      seq->children = darray_new(sizeof(PegUnit), 0);
    }
    darray_push(seq->children, ((PegUnit){0}));
    PegUnit* child = &seq->children[darray_size(seq->children) - 1];
    if (!_parse_peg_unit(ps, child)) {
      return false;
    }
  }
  return true;
}

static bool _parse_peg_rule(ParseState* ps) {
  Token* name_tok = _next(ps);
  if (!name_tok || name_tok->tok_id != TOK_PEG_ID) {
    _error_at(ps, name_tok, "expected peg rule name");
    return false;
  }
  if (!_at(ps, TOK_PEG_OPS_ASSIGN)) {
    _error_at(ps, name_tok, "expected '=' after peg rule name");
    return false;
  }
  _next(ps);
  if (!ps->peg_rules) {
    ps->peg_rules = darray_new(sizeof(PegRule), 0);
  }
  darray_push(ps->peg_rules, ((PegRule){0}));
  PegRule* rule = &ps->peg_rules[darray_size(ps->peg_rules) - 1];
  rule->name = _tok_strdup(ps, name_tok);
  rule->seq.kind = PEG_SEQ;
  return _parse_peg_seq(ps, &rule->seq);
}

static bool _parse_peg_section(ParseState* ps) {
  _skip_nl(ps);
  while (!_at_end(ps)) {
    if (_at(ps, TOK_NL)) {
      ps->tpos++;
      continue;
    }
    if (_at(ps, TOK_PEG_ID)) {
      if (!_parse_peg_rule(ps)) {
        return false;
      }
    } else {
      _error_at(ps, _peek(ps), "unexpected token in [[peg]] section");
      return false;
    }
  }
  return true;
}

// --- Free helpers ---

static void _free_vpa_unit(VpaUnit* unit) {
  re_ir_free(unit->re);
  free(unit->name);
  free(unit->user_hook);
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _free_vpa_unit(&unit->children[i]);
  }
  darray_del(unit->children);
}

static void _free_peg_unit(PegUnit* unit) {
  free(unit->name);
  free(unit->tag);
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    _free_peg_unit(&unit->children[i]);
  }
  darray_del(unit->children);
  if (unit->interlace) {
    _free_peg_unit(unit->interlace);
    free(unit->interlace);
  }
}

static void _free_state(ParseState* ps) {
  tc_tree_del(ps->tree);
  ps->tree = NULL;
  ps->src = NULL;
  ps->read_chunk = NULL;
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    free(ps->vpa_rules[i].name);
    for (int32_t j = 0; j < (int32_t)darray_size(ps->vpa_rules[i].units); j++) {
      _free_vpa_unit(&ps->vpa_rules[i].units[j]);
    }
    darray_del(ps->vpa_rules[i].units);
  }
  darray_del(ps->vpa_rules);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->peg_rules); i++) {
    free(ps->peg_rules[i].name);
    free(ps->peg_rules[i].scope);
    _free_peg_unit(&ps->peg_rules[i].seq);
  }
  darray_del(ps->peg_rules);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->re_irs); i++) {
    re_ir_free(ps->re_irs[i]);
  }
  darray_del(ps->re_irs);
  darray_del(ps->str_spans);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->re_frags); i++) {
    free(ps->re_frags[i].name);
    re_ir_free(ps->re_frags[i].re);
  }
  darray_del(ps->re_frags);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->keywords); i++) {
    free(ps->keywords[i].group);
  }
  darray_del(ps->keywords);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->states); i++) {
    free(ps->states[i].name);
  }
  darray_del(ps->states);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->effects); i++) {
    free(ps->effects[i].hook_name);
    darray_del(ps->effects[i].effects);
  }
  darray_del(ps->effects);
  for (int32_t i = 0; i < (int32_t)darray_size(ps->ignores.names); i++) {
    free(ps->ignores.names[i]);
  }
  darray_del(ps->ignores.names);
}

ParseState* parse_state_new(void) {
  ParseState* ps = calloc(1, sizeof(ParseState));
  return ps;
}

void parse_state_del(ParseState* ps) {
  if (!ps) {
    return;
  }
  _free_state(ps);
  free(ps);
}

const char* parse_get_error(ParseState* ps) {
  if (!ps) {
    return NULL;
  }
  return ps->error;
}

// --- Public API ---

bool parse_nest(ParseState* ps, const char* src) {
  if (!src) {
    parse_error(ps, "empty or null input");
    return false;
  }

  ps->src = src;
  ps->src_len = ustr_size(src);

  ps->tree = tc_tree_new(src);

  UstrIter it = {0};
  ustr_iter_init(&it, src, 0);
  LexCtx lex_ctx = {
      .ps = ps,
      .tree = ps->tree,
      .cp_count = ps->src_len,
      .it = it,
  };

  _lex_scope(&lex_ctx, SCOPE_MAIN);

  // Find VPA and PEG scope chunks
  int32_t vpa_idx = -1, peg_idx = -1;
  int32_t table_size = (int32_t)darray_size(ps->tree->table);
  for (int32_t i = 0; i < table_size; i++) {
    if (ps->tree->table[i].scope_id == SCOPE_VPA && vpa_idx == -1) {
      vpa_idx = i;
    } else if (ps->tree->table[i].scope_id == SCOPE_PEG && peg_idx == -1) {
      peg_idx = i;
    }
  }

  if (vpa_idx < 0) {
    parse_error(ps, "missing [[vpa]] section");
    return false;
  }
  if (peg_idx < 0) {
    parse_error(ps, "missing [[peg]] section");
    return false;
  }

  ps->read_chunk = &ps->tree->table[vpa_idx];
  ps->tpos = 0;

  // Pre-pass: collect %define fragments before parsing rules
  {
    int32_t saved_pos = ps->tpos;
    while (!_at_end(ps)) {
      if (_at(ps, TOK_DIRECTIVES_DEFINE)) {
        if (!_parse_define_decl(ps)) {
          return false;
        }
      } else {
        _next(ps);
      }
    }
    ps->tpos = saved_pos;
  }

  // Resolve fragment references in all ReIrs
  for (int32_t ri = 0; ri < (int32_t)darray_size(ps->re_irs); ri++) {
    ReIr ir = ps->re_irs[ri];
    if (!ir) {
      continue;
    }
    ReIr resolved = re_ir_new();
    for (int32_t oi = 0; oi < (int32_t)darray_size(ir); oi++) {
      if (ir[oi].kind == RE_IR_FRAG_REF) {
        char* frag_name = _cp_strdup(ps->src, ir[oi].start, ir[oi].end);
        bool found = false;
        for (int32_t fi = 0; fi < (int32_t)darray_size(ps->re_frags); fi++) {
          if (strcmp(ps->re_frags[fi].name, frag_name) == 0) {
            ReIr frag_ir = ps->re_frags[fi].re;
            for (int32_t fj = 0; fj < (int32_t)darray_size(frag_ir); fj++) {
              darray_push(resolved, frag_ir[fj]);
            }
            found = true;
            break;
          }
        }
        if (!found) {
          parse_error(ps, "undefined fragment '\\{%s}'", frag_name);
          free(frag_name);
          re_ir_free(resolved);
          return false;
        }
        free(frag_name);
      } else {
        darray_push(resolved, ir[oi]);
      }
    }
    re_ir_free(ir);
    ps->re_irs[ri] = resolved;
  }

  if (!_parse_vpa_section(ps)) {
    return false;
  }
  ps->read_chunk = &ps->tree->table[peg_idx];
  ps->tpos = 0;
  if (!_parse_peg_section(ps)) {
    return false;
  }
  if (!_at_end(ps)) {
    _error_at(ps, _peek(ps), "unexpected token after end of input");
    return false;
  }

  if (!ps->vpa_rules || darray_size(ps->vpa_rules) == 0) {
    parse_error(ps, "no [[vpa]] rules found");
    return false;
  }
  if (!ps->peg_rules || darray_size(ps->peg_rules) == 0) {
    parse_error(ps, "no [[peg]] rules found");
    return false;
  }

  return true;
}
