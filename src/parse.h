#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SCOPE_START,

  SCOPE_MAIN,
  SCOPE_VPA,
  SCOPE_SCOPE,
  SCOPE_LIT_SCOPE,
  SCOPE_PEG,
  SCOPE_BRANCHES,
  SCOPE_PEG_TAG,
  SCOPE_RE,
  SCOPE_RE_REF,
  SCOPE_CHARCLASS,
  SCOPE_STR,

  SCOPE_COUNT
} ScopeId;

typedef enum {
  ACTION_START = SCOPE_COUNT,

  ACTION_IGNORE,
  ACTION_BEGIN,
  ACTION_END,
  ACTION_UNPARSE,
  ACTION_UNPARSE_END,
  ACTION_FAIL,
  ACTION_STR_CHECK_END, // .str_check_end

  // composite: since lexer api only accepts single action_id, multiple actions must be combined
  ACTION_SET_QUOTE_BEGIN,        // .set_quote .begin
  ACTION_RE_TAG_BEGIN,           // @re_tag .begin
  ACTION_CHARCLASS_BEGIN_BEGIN,  // @charclass_begin .begin

  ACTION_COUNT
} ActionId;

typedef enum {
  LIT_START = ACTION_COUNT,

  LIT_IGNORE,  // "%ignore"
  LIT_EFFECT,  // "%effect"
  LIT_DEFINE,  // "%define"
  LIT_EQ,      // "="
  LIT_OR,      // "|"
  LIT_INTERLACE_BEGIN, // "<"
  LIT_INTERLACE_END,   // ">"
  LIT_QUESTION, // "?"
  LIT_PLUS,     // "+"
  LIT_STAR,     // "*"
  LIT_LPAREN,   // "("
  LIT_RPAREN,   // ")"

  LIT_COUNT
} LitId;

typedef enum {
  TOK_START = LIT_COUNT,

  // shared tokens
  TOK_NL,

  // scope: vpa
  TOK_TOK_ID,
  TOK_HOOK_BEGIN,
  TOK_HOOK_END,
  TOK_HOOK_FAIL,
  TOK_HOOK_UNPARSE,
  TOK_VPA_ID,
  TOK_MODULE_ID,
  TOK_USER_HOOK_ID,
  TOK_STATE_ID,
  TOK_RE_FRAG_ID,

  // shared by re, charclass, str
  TOK_CODEPOINT,
  TOK_C_ESCAPE,
  TOK_PLAIN_ESCAPE,
  TOK_CHAR,

  // str scope
  TOK_STR_START,

  // re scope
  TOK_RE_TAG,
  TOK_RE_DOT,
  TOK_RE_SPACE_CLASS,
  TOK_RE_WORD_CLASS,
  TOK_RE_DIGIT_CLASS,
  TOK_RE_HEX_CLASS,
  TOK_RE_BOF,
  TOK_RE_EOF,

  // re_ref scope
  TOK_RE_REF,

  // charclass scope
  TOK_CHARCLASS_BEGIN,
  TOK_RANGE_SEP,

  // peg scope
  TOK_PEG_ID,
  TOK_PEG_TOK_ID,
  TOK_TAG_ID,

  TOK_COUNT
} TokenId;

#include "peg.h"
#include "token_chunk.h"
#include "vpa.h"

// --- Parser state ---

typedef struct {
  int32_t off;
  int32_t len;
} StrSpan;

typedef struct {
  char* name; // fragment name (e.g. "ID")
  ReIr re;    // regex IR for the fragment
} ReFragment;

typedef struct {
  const char* src;
  int32_t src_len;

  TokenTree* tree;
  TokenChunk* read_chunk; // chunk being parsed (cursor target)
  int32_t tpos;

  ReIr* re_irs;           // darray
  StrSpan* str_spans;     // darray (StrSpan*)
  ReFragment* re_frags;   // darray

  VpaRule* vpa_rules;     // darray
  KeywordEntry* keywords; // darray
  IgnoreSet ignores;
  EffectDecl* effects; // darray
  PegRule* peg_rules;  // darray

  char error[512];
} ParseState;

typedef void* DStr; // string builder (darray of char)

// --- Public API ---

bool parse_nest(ParseState* ps, const char* src);

ParseState* parse_state_new(void);
void parse_state_del(ParseState* ps);
const char* parse_get_error(ParseState* ps);

// --- Helper functions ---

void parse_error(ParseState* ps, const char* fmt, ...);
bool parse_has_error(ParseState* ps);
char* parse_sfmt(const char* fmt, ...);
void parse_set_str(char** dst, char* s);
