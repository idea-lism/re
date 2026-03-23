#include "lex.h"

#include <stdio.h>
#include <stdlib.h>

#define TARGET "arm64-apple-macosx14.0.0"
#define ADD(l, mode, pat) lex_add(l, mode, pat, __LINE__)

int main(void) {
  Lex* l = lex_new("drive.c");

  ADD(l, "", "if");            // 1
  ADD(l, "", "else");          // 2
  ADD(l, "", "while");         // 3
  ADD(l, "", "[a-zA-Z_]\\w*"); // 4: identifier
  ADD(l, "", "\\d+");          // 5: number
  ADD(l, "", "\\s+");          // 6: whitespace
  ADD(l, "", "=");             // 7
  ADD(l, "", "\\+");           // 8
  ADD(l, "", "\\(");           // 9
  ADD(l, "", "\\)");           // 10
  ADD(l, "", "\\{");           // 11
  ADD(l, "", "\\}");           // 12
  ADD(l, "", ";");             // 13

  FILE* f = fopen("lex.ll", "w");
  if (!f) {
    perror("fopen");
    lex_del(l);
    return 1;
  }
  lex_gen(l, f, TARGET);
  fclose(f);
  lex_del(l);

  printf("wrote lex.ll\n");
  return 0;
}
