// nest CLI: regex lexer generator and .nest parser compiler.

#include "header_writer.h"
#include "irwriter.h"
#include "parse.h"
#include "re.h"
#include "ustr.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Lex helpers (inlined from parse_gen.c) ---

typedef struct {
  Aut* aut;
  Re* re;
  const char* source_file;
  bool started;
  bool icase;
  bool binary;
} Lex;

#define LEX_ERR_PAREN (-1)
#define LEX_ERR_BRACKET (-2)

typedef struct {
  Lex* lex;
  int32_t* cps;
  int32_t ncp;
  int32_t pos;
  bool binary;
  bool icase;
  int32_t line;
  int32_t col;
  char* ustr_buf;
  int32_t err;
} LexParser;

static void _lp_parse_expr(LexParser* p);

static int32_t _lp_peek(LexParser* p) {
  if (p->pos >= p->ncp) {
    return -1;
  }
  return p->cps[p->pos];
}

static int32_t _lp_advance(LexParser* p) {
  if (p->pos >= p->ncp) {
    return -1;
  }
  return p->cps[p->pos++];
}

static DebugInfo _lp_di(LexParser* p) { return (DebugInfo){p->line, p->col + p->pos}; }

static void _range_add_icase(ReRange* range, int32_t start, int32_t end, bool icase) {
  re_range_add(range, start, end);
  if (!icase) {
    return;
  }
  if (start <= 'z' && end >= 'a') {
    int32_t lo = start < 'a' ? 'a' : start;
    int32_t hi = end > 'z' ? 'z' : end;
    re_range_add(range, lo - 32, hi - 32);
  }
  if (start <= 'Z' && end >= 'A') {
    int32_t lo = start < 'A' ? 'A' : start;
    int32_t hi = end > 'Z' ? 'Z' : end;
    re_range_add(range, lo + 32, hi + 32);
  }
}

static void _lp_emit_ch(LexParser* p, int32_t cp) {
  Re* re = p->lex->re;
  DebugInfo di = _lp_di(p);
  if (p->icase && ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))) {
    ReRange* range = re_range_new();
    _range_add_icase(range, cp, cp, true);
    re_append_range(re, range, di);
    re_range_del(range);
  } else {
    re_append_ch(re, cp, di);
  }
}

static void _add_class_ranges(ReRange* range, int32_t cls) {
  switch (cls) {
  case 's':
    re_range_add(range, '\t', '\r');
    re_range_add(range, ' ', ' ');
    break;
  case 'w':
    re_range_add(range, '0', '9');
    re_range_add(range, 'A', 'Z');
    re_range_add(range, '_', '_');
    re_range_add(range, 'a', 'z');
    break;
  case 'd':
    re_range_add(range, '0', '9');
    break;
  case 'h':
    re_range_add(range, '0', '9');
    re_range_add(range, 'A', 'F');
    re_range_add(range, 'a', 'f');
    break;
  }
}

static int32_t _c_escape(int32_t ch) {
  switch (ch) {
  case 'n':
    return '\n';
  case 't':
    return '\t';
  case 'r':
    return '\r';
  case '0':
    return '\0';
  case 'f':
    return '\f';
  case 'v':
    return '\v';
  default:
    return -1;
  }
}

static int32_t _lp_parse_unicode_escape(LexParser* p) {
  _lp_advance(p);
  int32_t cp = 0;
  while (_lp_peek(p) != '}' && _lp_peek(p) >= 0) {
    int32_t ch = _lp_advance(p);
    if (ch >= '0' && ch <= '9') {
      cp = cp * 16 + (ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      cp = cp * 16 + (ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      cp = cp * 16 + (ch - 'A' + 10);
    } else {
      fprintf(stderr, "nest: invalid hex char '%c' in \\u{...} escape\n", (char)ch);
      exit(1);
    }
  }
  if (_lp_peek(p) == '}') {
    _lp_advance(p);
  }
  return cp;
}

static int32_t _lp_parse_class_escape(LexParser* p, ReRange* range) {
  int32_t ch = _lp_advance(p);
  if (ch < 0) {
    return -1;
  }
  int32_t esc = _c_escape(ch);
  if (esc >= 0) {
    return esc;
  }
  switch (ch) {
  case 'u':
    return _lp_parse_unicode_escape(p);
  case 's':
  case 'w':
  case 'd':
  case 'h':
    _add_class_ranges(range, ch);
    return -1;
  default:
    return ch;
  }
}

static void _lp_parse_charclass(LexParser* p) {
  if (p->err) {
    return;
  }
  _lp_advance(p);

  bool neg = false;
  if (_lp_peek(p) == '^') {
    _lp_advance(p);
    neg = true;
  }

  ReRange* range = re_range_new();

  while (_lp_peek(p) != ']' && _lp_peek(p) >= 0 && !p->err) {
    int32_t cp;
    if (_lp_peek(p) == '\\') {
      _lp_advance(p);
      cp = _lp_parse_class_escape(p, range);
    } else {
      cp = _lp_advance(p);
    }

    if (cp >= 0 && _lp_peek(p) == '-' && p->pos + 1 < p->ncp && p->cps[p->pos + 1] != ']') {
      _lp_advance(p);
      int32_t cp2;
      if (_lp_peek(p) == '\\') {
        _lp_advance(p);
        cp2 = _lp_parse_class_escape(p, range);
      } else {
        cp2 = _lp_advance(p);
      }
      if (cp2 >= 0) {
        _range_add_icase(range, cp, cp2, p->icase);
      } else {
        _range_add_icase(range, cp, cp, p->icase);
      }
    } else if (cp >= 0) {
      _range_add_icase(range, cp, cp, p->icase);
    }
  }

  if (_lp_peek(p) != ']') {
    p->err = LEX_ERR_BRACKET;
    re_range_del(range);
    return;
  }
  _lp_advance(p);

  if (neg) {
    re_range_neg(range);
  }

  re_append_range(p->lex->re, range, _lp_di(p));
  re_range_del(range);
}

static void _lp_parse_atom(LexParser* p) {
  if (p->err) {
    return;
  }

  int32_t ch = _lp_peek(p);

  if (ch == '(') {
    _lp_advance(p);
    re_lparen(p->lex->re);
    _lp_parse_expr(p);
    if (p->err) {
      return;
    }
    if (_lp_peek(p) != ')') {
      p->err = LEX_ERR_PAREN;
      return;
    }
    _lp_advance(p);
    re_rparen(p->lex->re);
  } else if (ch == '[') {
    _lp_parse_charclass(p);
  } else if (ch == '.') {
    _lp_advance(p);
    ReRange* range = re_range_new();
    if (p->binary) {
      re_range_add(range, 0, 9);
      re_range_add(range, 11, 255);
    } else {
      re_range_add(range, 0, 9);
      re_range_add(range, 11, 0x10FFFF);
    }
    re_append_range(p->lex->re, range, _lp_di(p));
    re_range_del(range);
  } else if (ch == '\\') {
    _lp_advance(p);
    ch = _lp_peek(p);
    if (ch < 0) {
      return;
    }
    if (ch == 'a') {
      _lp_advance(p);
      re_append_ch(p->lex->re, LEX_CP_BOF, _lp_di(p));
    } else if (ch == 'z') {
      _lp_advance(p);
      re_append_ch(p->lex->re, LEX_CP_EOF, _lp_di(p));
    } else if (ch == 's' || ch == 'w' || ch == 'd' || ch == 'h') {
      _lp_advance(p);
      ReRange* range = re_range_new();
      _add_class_ranges(range, ch);
      re_append_range(p->lex->re, range, _lp_di(p));
      re_range_del(range);
    } else if (ch == 'u') {
      _lp_advance(p);
      int32_t cp = _lp_parse_unicode_escape(p);
      _lp_emit_ch(p, cp);
    } else {
      int32_t esc = _c_escape(ch);
      _lp_advance(p);
      _lp_emit_ch(p, esc >= 0 ? esc : ch);
    }
  } else {
    _lp_advance(p);
    _lp_emit_ch(p, ch);
  }
}

static void _lp_parse_quantified(LexParser* p) {
  if (p->err) {
    return;
  }

  Re* re = p->lex->re;
  Aut* aut = p->lex->aut;
  int32_t before = re_cur_state(re);

  _lp_parse_atom(p);
  if (p->err) {
    return;
  }

  int32_t ch = _lp_peek(p);
  if (ch == '?') {
    _lp_advance(p);
    aut_epsilon(aut, before, re_cur_state(re));
  } else if (ch == '+') {
    _lp_advance(p);
    aut_epsilon(aut, re_cur_state(re), before);
  } else if (ch == '*') {
    _lp_advance(p);
    aut_epsilon(aut, before, re_cur_state(re));
    aut_epsilon(aut, re_cur_state(re), before);
  }
}

static void _lp_parse_branch(LexParser* p) {
  while (!p->err) {
    int32_t ch = _lp_peek(p);
    if (ch < 0 || ch == '|' || ch == ')') {
      break;
    }
    _lp_parse_quantified(p);
  }
}

static void _lp_parse_expr(LexParser* p) {
  if (p->err) {
    return;
  }
  _lp_parse_branch(p);
  while (_lp_peek(p) == '|' && !p->err) {
    _lp_advance(p);
    re_fork(p->lex->re);
    _lp_parse_branch(p);
  }
}

static Lex* _lex_new(const char* func_name, const char* source_file_name, const char* mode) {
  Lex* l = calloc(1, sizeof(Lex));
  l->aut = aut_new(func_name, source_file_name);
  l->re = re_new(l->aut);
  l->source_file = source_file_name;
  for (const char* m = mode; *m; m++) {
    if (*m == 'i') {
      l->icase = true;
    }
    if (*m == 'b') {
      l->binary = true;
    }
  }
  re_lparen(l->re);
  return l;
}

static void _lex_del(Lex* l) {
  if (!l) {
    return;
  }
  re_del(l->re);
  aut_del(l->aut);
  free(l);
}

static int32_t _lex_add(Lex* l, const char* pattern, int32_t line, int32_t col, int32_t action_id) {
  if (l->started) {
    re_fork(l->re);
  }
  l->started = true;

  LexParser p = {0};
  p.lex = l;
  p.line = line;
  p.col = col;
  p.icase = l->icase;
  p.binary = l->binary;

  int32_t len = (int32_t)strlen(pattern);
  if (p.binary) {
    p.ncp = len;
    p.cps = malloc((size_t)len * sizeof(int32_t));
    for (int32_t i = 0; i < len; i++) {
      p.cps[i] = (uint8_t)pattern[i];
    }
  } else {
    p.ustr_buf = ustr_new((size_t)len, pattern);
    p.ncp = ustr_size(p.ustr_buf);
    if (p.ncp > 0) {
      p.cps = malloc((size_t)p.ncp * sizeof(int32_t));
      UstrIter it;
      ustr_iter_init(&it, p.ustr_buf, 0);
      for (int32_t i = 0; i < p.ncp; i++) {
        p.cps[i] = ustr_iter_next(&it);
      }
    }
  }

  _lp_parse_expr(&p);

  int32_t err = p.err;
  free(p.cps);
  if (p.ustr_buf) {
    ustr_del(p.ustr_buf);
  }

  if (err) {
    return err;
  }

  re_action(l->re, action_id);
  return action_id;
}

static void _lex_gen_func(Lex* l, IrWriter* w, bool debug_mode) {
  re_rparen(l->re);
  aut_optimize(l->aut);
  aut_gen_dfa(l->aut, w, debug_mode);
}

// --- CLI ---

static const char* _detect_triple(void) {
  FILE* p = popen("clang --print-target-triple 2>/dev/null", "r");
  if (!p) {
    return "x86_64-unknown-linux-gnu";
  }
  static char buf[128];
  if (!fgets(buf, sizeof(buf), p)) {
    pclose(p);
    return "x86_64-unknown-linux-gnu";
  }
  pclose(p);
  size_t len = strlen(buf);
  if (len > 0 && buf[len - 1] == '\n') {
    buf[len - 1] = '\0';
  }
  return buf;
}

static void _usage(void) {
  fprintf(stderr, "usage: nest <command> [options]\n"
                  "\n"
                  "commands:\n"
                  "  l    generate lexer from regex patterns\n"
                  "  c    generate parser from .nest syntax\n"
                  "\n"
                  "nest l <input> -o <output.ll> [-f <func_name>] [-m <mode>] [-t <triple>]\n"
                  "  input         file with one regex per line (action_id auto-assigned from 1)\n"
                  "  -o <file>     output LLVM IR file\n"
                  "  -f <name>     function name (default: \"lex\")\n"
                  "  -m <mode>     mode flags: i=case-insensitive, b=binary\n"
                  "  -t <triple>   target triple (default: probe clang)\n"
                  "\n"
                  "nest c <input.nest> -o <output.ll> [-t <triple>]\n"
                  "  input.nest    .nest syntax file\n"
                  "  -o <file>     output LLVM IR file\n"
                  "  -t <triple>   target triple (default: probe clang)\n");
  exit(1);
}

static int32_t _cmd_lex(int32_t argc, char** argv) {
  const char* input = NULL;
  const char* output = NULL;
  const char* func_name = "lex";
  const char* mode = "";
  const char* triple = NULL;

  int32_t i = 0;
  while (i < argc) {
    if (strcmp(argv[i], "-o") == 0) {
      if (++i >= argc) {
        _usage();
      }
      output = argv[i++];
    } else if (strcmp(argv[i], "-f") == 0) {
      if (++i >= argc) {
        _usage();
      }
      func_name = argv[i++];
    } else if (strcmp(argv[i], "-m") == 0) {
      if (++i >= argc) {
        _usage();
      }
      mode = argv[i++];
    } else if (strcmp(argv[i], "-t") == 0) {
      if (++i >= argc) {
        _usage();
      }
      triple = argv[i++];
    } else if (argv[i][0] == '-') {
      _usage();
    } else if (!input) {
      input = argv[i++];
    } else {
      _usage();
    }
  }

  if (!input || !output) {
    _usage();
  }
  if (!triple) {
    triple = _detect_triple();
  }

  FILE* fin = fopen(input, "r");
  if (!fin) {
    perror(input);
    return 1;
  }

  Lex* l = _lex_new(func_name, input, mode);
  char line[4096];
  int32_t lineno = 0;
  int32_t action_id = 1;
  while (fgets(line, sizeof(line), fin)) {
    lineno++;
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[--len] = '\0';
    }
    if (len == 0) {
      continue;
    }
    int32_t r = _lex_add(l, line, lineno, 0, action_id);
    if (r < 0) {
      fprintf(stderr, "%s:%d: parse error (%d)\n", input, lineno, r);
      fclose(fin);
      _lex_del(l);
      return 1;
    }
    action_id++;
  }
  fclose(fin);

  FILE* fout = fopen(output, "w");
  if (!fout) {
    perror(output);
    _lex_del(l);
    return 1;
  }

  IrWriter* w = irwriter_new(fout, triple);
  irwriter_start(w, input, ".");
  _lex_gen_func(l, w, false);
  irwriter_end(w);
  irwriter_del(w);
  fclose(fout);
  _lex_del(l);
  return 0;
}

static char* _read_file(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) {
    perror(path);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* buf = malloc((size_t)sz + 1);
  size_t n = fread(buf, 1, (size_t)sz, f);
  buf[n] = '\0';
  fclose(f);
  return buf;
}

static int32_t _cmd_compile(int32_t argc, char** argv) {
  const char* input = NULL;
  const char* output = NULL;
  const char* triple = NULL;

  int32_t i = 0;
  while (i < argc) {
    if (strcmp(argv[i], "-o") == 0) {
      if (++i >= argc) {
        _usage();
      }
      output = argv[i++];
    } else if (strcmp(argv[i], "-t") == 0) {
      if (++i >= argc) {
        _usage();
      }
      triple = argv[i++];
    } else if (argv[i][0] == '-') {
      _usage();
    } else if (!input) {
      input = argv[i++];
    } else {
      _usage();
    }
  }

  if (!input || !output) {
    _usage();
  }
  if (!triple) {
    triple = _detect_triple();
  }

  char* src = _read_file(input);
  if (!src) {
    return 1;
  }

  size_t out_len = strlen(output);
  char* header_path = malloc(out_len + 3);
  memcpy(header_path, output, out_len);
  if (out_len >= 3 && strcmp(output + out_len - 3, ".ll") == 0) {
    strcpy(header_path + out_len - 3, ".h");
  } else {
    strcpy(header_path + out_len, ".h");
  }

  FILE* fout = fopen(output, "w");
  if (!fout) {
    perror(output);
    free(src);
    free(header_path);
    return 1;
  }

  FILE* fhdr = fopen(header_path, "w");
  if (!fhdr) {
    perror(header_path);
    fclose(fout);
    free(src);
    free(header_path);
    return 1;
  }

  IrWriter* w = irwriter_new(fout, triple);
  irwriter_start(w, input, ".");
  HeaderWriter* hw = hw_new(fhdr);

  parse_nest(src, hw, w);

  irwriter_end(w);
  irwriter_del(w);
  hw_del(hw);
  fclose(fout);
  fclose(fhdr);
  free(src);
  free(header_path);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    _usage();
  }

  const char* cmd = argv[1];
  if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
    _usage();
  }

  if (strcmp(cmd, "l") == 0) {
    return _cmd_lex(argc - 2, argv + 2);
  } else if (strcmp(cmd, "c") == 0) {
    return _cmd_compile(argc - 2, argv + 2);
  } else {
    fprintf(stderr, "unknown command: %s\n", cmd);
    _usage();
  }
  return 1;
}
