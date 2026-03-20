#pragma once

#include "aut.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct Re Re;

typedef struct {
  int32_t start;
  int32_t end;
} Range;

typedef struct {
  int32_t start;
  int32_t end;
  bool done;
} NegRangeIter;

Re* re_new(Aut* aut);
void re_del(Re* re);

void re_neg_ranges(NegRangeIter* iter, size_t sz, Range* ranges);
void re_neg_next(NegRangeIter* iter);

void re_range(Re* re, int32_t cp_start, int32_t cp_end);
void re_lparen(Re* re);
void re_fork(Re* re);
void re_seq(Re* re, ...);
void re_rparen(Re* re);
void re_action(Re* re, int32_t action_id);
