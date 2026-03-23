#include "lex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void _usage(const char* prog) {
  fprintf(stderr, "usage: %s [-m mode] [-t target_triple] input output.ll\n", prog);
  exit(1);
}

int main(int32_t argc, char** argv) {
  const char* mode = "";
  const char* target = "";
  const char* input = NULL;
  const char* output = NULL;

  int32_t i = 1;
  while (i < argc) {
    if (strcmp(argv[i], "-m") == 0) {
      if (++i >= argc) {
        _usage(argv[0]);
      }
      mode = argv[i++];
    } else if (strcmp(argv[i], "-t") == 0) {
      if (++i >= argc) {
        _usage(argv[0]);
      }
      target = argv[i++];
    } else if (argv[i][0] == '-') {
      _usage(argv[0]);
    } else if (!input) {
      input = argv[i++];
    } else if (!output) {
      output = argv[i++];
    } else {
      _usage(argv[0]);
    }
  }

  if (!input || !output || !target[0]) {
    _usage(argv[0]);
  }

  FILE* fin = fopen(input, "r");
  if (!fin) {
    perror(input);
    return 1;
  }

  Lex* l = lex_new(input, mode);
  char line[4096];
  int32_t lineno = 0;
  while (fgets(line, sizeof(line), fin)) {
    lineno++;
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[--len] = '\0';
    }
    if (len == 0) {
      continue;
    }
    int32_t r = lex_add(l, line, lineno);
    if (r < 0) {
      fprintf(stderr, "%s:%d: parse error (%d)\n", input, lineno, r);
      fclose(fin);
      lex_del(l);
      return 1;
    }
  }
  fclose(fin);

  FILE* fout = fopen(output, "w");
  if (!fout) {
    perror(output);
    lex_del(l);
    return 1;
  }

  lex_gen(l, fout, target);
  fclose(fout);
  lex_del(l);
  return 0;
}
