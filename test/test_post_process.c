#include "../src/parse.h"
#include "../src/post_process.h"
#include "../src/ustr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

static bool _parse(ParseState* ps, const char* cstr) {
  if (!cstr) {
    return parse_nest(ps, NULL);
  }
  size_t len = strlen(cstr);
  char* u = ustr_new(len, cstr);
  bool ok = parse_nest(ps, u);
  ustr_del(u);
  return ok;
}

// --- Duplicate tags ---

static const char DUP_TAG_NEST[] = "[[vpa]]\n"
                                   "%ignore @space @comment\n"
                                   "main = { /[a-z]+/ @tok_a /[0-9]+/ @tok_b *noise }\n"
                                   "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                   "[[peg]]\n"
                                   "main = item+\n"
                                   "item = [\n"
                                   "  @tok_a : dup\n"
                                   "  @tok_b : dup\n"
                                   "]\n";

TEST(test_duplicate_tag_error) {
  ParseState* ps = parse_state_new();
  bool ok = _parse(ps, DUP_TAG_NEST);
  assert(ok);
  ok = pp_auto_tag_branches(ps) && pp_check_duplicate_tags(ps);
  assert(!ok);
  assert(parse_has_error(ps));
  assert(strstr(parse_get_error(ps), "dup") != NULL);
  parse_state_del(ps);
}

// --- Missing PEG main ---

static const char NO_PEG_MAIN_NEST[] = "[[vpa]]\n"
                                       "%ignore @space @comment\n"
                                       "main = { *noise }\n"
                                       "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                       "[[peg]]\n"
                                       "helper = @nl*\n";

TEST(test_missing_peg_main) {
  ParseState* ps = parse_state_new();
  bool ok = _parse(ps, NO_PEG_MAIN_NEST);
  assert(ok);
  ok = pp_validate(ps);
  assert(!ok);
  assert(parse_has_error(ps));
  parse_state_del(ps);
}

int main(void) {
  printf("test_post_process:\n");

  RUN(test_duplicate_tag_error);
  RUN(test_missing_peg_main);

  printf("all ok\n");
  return 0;
}
