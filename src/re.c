#include "re.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define MAX_UNICODE 0x10FFFF

// --- Negated range iterator ---

typedef struct {
  size_t sz;
  Range* ranges;
  size_t idx;
} NegRangeState;

static NegRangeState neg_state;

static int range_cmp(const void* a, const void* b) {
  const Range* ra = a;
  const Range* rb = b;
  if (ra->start != rb->start) {
    return ra->start < rb->start ? -1 : 1;
  }
  return ra->end < rb->end ? -1 : (ra->end > rb->end ? 1 : 0);
}

void re_neg_ranges(NegRangeIter* iter, size_t sz, Range* ranges) {
  if (sz > 0) {
    qsort(ranges, sz, sizeof(Range), range_cmp);
  }
  neg_state.sz = sz;
  neg_state.ranges = ranges;
  neg_state.idx = 0;

  // find the first gap
  int32_t pos = 0;
  while (neg_state.idx < sz && ranges[neg_state.idx].start <= pos) {
    if (ranges[neg_state.idx].end >= pos) {
      pos = ranges[neg_state.idx].end + 1;
    }
    neg_state.idx++;
  }

  if (neg_state.idx >= sz) {
    // gap from pos to MAX_UNICODE
    if (pos <= MAX_UNICODE) {
      iter->start = pos;
      iter->end = MAX_UNICODE;
      iter->done = false;
    } else {
      iter->done = true;
    }
  } else {
    // gap from pos to ranges[idx].start - 1
    iter->start = pos;
    iter->end = ranges[neg_state.idx].start - 1;
    iter->done = false;
  }
}

void re_neg_next(NegRangeIter* iter) {
  if (iter->done) {
    return;
  }

  // advance past the range that ended the previous gap
  int32_t pos;
  if (neg_state.idx < neg_state.sz) {
    pos = neg_state.ranges[neg_state.idx].end + 1;
    neg_state.idx++;
  } else {
    iter->done = true;
    return;
  }

  // skip overlapping/adjacent ranges
  while (neg_state.idx < neg_state.sz && neg_state.ranges[neg_state.idx].start <= pos) {
    if (neg_state.ranges[neg_state.idx].end >= pos) {
      pos = neg_state.ranges[neg_state.idx].end + 1;
    }
    neg_state.idx++;
  }

  if (pos > MAX_UNICODE) {
    iter->done = true;
    return;
  }

  if (neg_state.idx >= neg_state.sz) {
    iter->start = pos;
    iter->end = MAX_UNICODE;
    iter->done = false;
  } else {
    iter->start = pos;
    iter->end = neg_state.ranges[neg_state.idx].start - 1;
    iter->done = false;
  }
}

// --- Re builder ---

typedef struct {
  int32_t start_state;
  int32_t cur_state;
  int32_t* branch_ends;
  int32_t nbranch_ends;
  int32_t branch_ends_cap;
} GroupFrame;

struct Re {
  Aut* aut;
  int32_t next_state;
  GroupFrame* stack;
  int32_t stack_sz;
  int32_t stack_cap;
};

static int32_t alloc_state(Re* re) { return re->next_state++; }

static GroupFrame* top(Re* re) {
  assert(re->stack_sz > 0);
  return &re->stack[re->stack_sz - 1];
}

static void push_frame(Re* re, int32_t start, int32_t cur) {
  if (re->stack_sz == re->stack_cap) {
    re->stack_cap = re->stack_cap ? re->stack_cap * 2 : 8;
    re->stack = realloc(re->stack, (size_t)re->stack_cap * sizeof(GroupFrame));
  }
  re->stack[re->stack_sz++] = (GroupFrame){
      .start_state = start,
      .cur_state = cur,
      .branch_ends = NULL,
      .nbranch_ends = 0,
      .branch_ends_cap = 0,
  };
}

static void save_branch_end(GroupFrame* f, int32_t state) {
  if (f->nbranch_ends == f->branch_ends_cap) {
    f->branch_ends_cap = f->branch_ends_cap ? f->branch_ends_cap * 2 : 4;
    f->branch_ends = realloc(f->branch_ends, (size_t)f->branch_ends_cap * sizeof(int32_t));
  }
  f->branch_ends[f->nbranch_ends++] = state;
}

Re* re_new(Aut* aut) {
  Re* re = calloc(1, sizeof(Re));
  re->aut = aut;
  re->next_state = 0;
  // state 0 is the initial state; push the implicit root frame
  int32_t s0 = alloc_state(re);
  (void)s0;
  push_frame(re, 0, 0);
  return re;
}

void re_del(Re* re) {
  if (!re) {
    return;
  }
  for (int32_t i = 0; i < re->stack_sz; i++) {
    free(re->stack[i].branch_ends);
  }
  free(re->stack);
  free(re);
}

void re_range(Re* re, int32_t cp_start, int32_t cp_end) {
  GroupFrame* f = top(re);
  int32_t s = alloc_state(re);
  aut_transition(re->aut, (TransitionDef){f->cur_state, s, cp_start, cp_end}, (DebugInfo){0, 0});
  f->cur_state = s;
}

void re_lparen(Re* re) {
  GroupFrame* f = top(re);
  int32_t start = f->cur_state;
  int32_t branch = alloc_state(re);
  aut_epsilon(re->aut, start, branch, 0);
  push_frame(re, start, branch);
}

void re_fork(Re* re) {
  GroupFrame* f = top(re);
  save_branch_end(f, f->cur_state);
  int32_t branch = alloc_state(re);
  aut_epsilon(re->aut, f->start_state, branch, 0);
  f->cur_state = branch;
}

void re_seq(Re* re, ...) {
  va_list ap;
  va_start(ap, re);
  for (;;) {
    int32_t cp = va_arg(ap, int32_t);
    if (cp < 0) {
      break;
    }
    re_range(re, cp, cp);
  }
  va_end(ap);
}

void re_rparen(Re* re) {
  assert(re->stack_sz > 1);
  GroupFrame* f = top(re);
  save_branch_end(f, f->cur_state);

  int32_t exit_state = alloc_state(re);
  for (int32_t i = 0; i < f->nbranch_ends; i++) {
    aut_epsilon(re->aut, f->branch_ends[i], exit_state, 0);
  }

  free(f->branch_ends);
  re->stack_sz--;

  GroupFrame* parent = top(re);
  parent->cur_state = exit_state;
}

void re_action(Re* re, int32_t action_id) {
  GroupFrame* f = top(re);
  int32_t s = alloc_state(re);
  aut_epsilon(re->aut, f->cur_state, s, action_id);
  f->cur_state = s;
}
