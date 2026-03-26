#include "peg_ir.h"

#include <stdio.h>

struct IrWriter {
  FILE* out;
  int reg;
  int32_t dbg_line;
  int32_t dbg_col;
  int dbg_next_id;
  int dbg_sub_id;
  int in_switch;
  int widen_ret;
  char* entry_prologue;
  void* locs;
  int dbg_file_id;
  int dbg_flags_emitted;
  int switch_dbg_id;
  const char* target_triple;
  const char* source_file;
  const char* directory;
  const char** decls;
};

int32_t peg_ir_call(IrWriter* w, char* buf, int32_t buf_size, const char* rule_name, const char* table,
                    const char* col) {
  int32_t n = snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = call i32 @parse_%s(i8* %s, i32 %s)\n", buf, rule_name, table, col);
  return n;
}

int32_t peg_ir_tok(IrWriter* w, char* buf, int32_t buf_size, const char* tok_id, const char* col) {
  int32_t n = snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = call i32 @match_tok(i32 %s, i32 %s)\n", buf, tok_id, col);
  return n;
}

int32_t peg_ir_load_slot(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                         int32_t slot_idx) {
  char ptr[32];
  snprintf(ptr, sizeof(ptr), "%%r%d", w->reg++);
  fprintf(w->out, "  %s = getelementptr inbounds [0 x i32], [0 x i32]* %s, i32 %s, i32 %d\n", ptr, table, col,
          slot_idx);

  int32_t n = snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = load i32, i32* %s\n", buf, ptr);
  return n;
}

int32_t peg_ir_store_slot(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                          int32_t slot_idx, const char* value) {
  int32_t n = snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = getelementptr inbounds [0 x i32], [0 x i32]* %s, i32 %s, i32 %d\n", buf, table, col,
          slot_idx);
  fprintf(w->out, "  store i32 %s, i32* %s\n", value, buf);
  return n;
}

int32_t peg_ir_load_bits(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                         int32_t seg_idx) {
  char ptr[32];
  snprintf(ptr, sizeof(ptr), "%%r%d", w->reg++);
  fprintf(w->out, "  %s = getelementptr inbounds [0 x i32], [0 x i32]* %s, i32 %s, i32 %d\n", ptr, table, col,
          seg_idx);

  int32_t n = snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = load i32, i32* %s\n", buf, ptr);
  return n;
}

int32_t peg_ir_store_bits(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                          int32_t seg_idx, const char* value) {
  int32_t n = snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = getelementptr inbounds [0 x i32], [0 x i32]* %s, i32 %s, i32 %d\n", buf, table, col,
          seg_idx);
  fprintf(w->out, "  store i32 %s, i32* %s\n", value, buf);
  return n;
}

void peg_ir_push(IrWriter* w, const char* col) {
  fprintf(w->out, "  call void @peg_push(i32 %s)\n", col);
}

void peg_ir_pop(IrWriter* w, char* buf, int32_t buf_size) {
  snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = call i32 @peg_pop()\n", buf);
}

void peg_ir_peek(IrWriter* w, char* buf, int32_t buf_size) {
  snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = call i32 @peg_peek()\n", buf);
}

void peg_ir_jump(IrWriter* w, const char* label) {
  fprintf(w->out, "  br label %%%s\n", label);
}

void peg_ir_iffail(IrWriter* w, const char* result, const char* label) {
  char cmp[32];
  snprintf(cmp, sizeof(cmp), "%%r%d", w->reg++);
  fprintf(w->out, "  %s = icmp slt i32 %s, 0\n", cmp, result);
  fprintf(w->out, "  br i1 %s, label %%%s, label %%next%d\n", cmp, label, w->reg);
  fprintf(w->out, "next%d:\n", w->reg++);
}

int32_t peg_ir_alloca(IrWriter* w, char* buf, int32_t buf_size, const char* ty) {
  int32_t n = snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = alloca %s\n", buf, ty);
  return n;
}

int32_t peg_ir_load(IrWriter* w, char* buf, int32_t buf_size, const char* ty, const char* ptr) {
  snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = load %s, %s* %s\n", buf, ty, ty, ptr);
  return 0;
}

void peg_ir_store(IrWriter* w, const char* ty, const char* val, const char* ptr) {
  fprintf(w->out, "  store %s %s, %s* %s\n", ty, val, ty, ptr);
}

int32_t peg_ir_add(IrWriter* w, char* buf, int32_t buf_size, const char* lhs, const char* rhs) {
  snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = add i32 %s, %s\n", buf, lhs, rhs);
  return 0;
}

int32_t peg_ir_select(IrWriter* w, char* buf, int32_t buf_size, const char* cond, const char* ty, const char* true_val,
                      const char* false_val) {
  snprintf(buf, (size_t)buf_size, "%%r%d", w->reg++);
  fprintf(w->out, "  %s = select i1 %s, %s %s, %s %s\n", buf, cond, ty, true_val, ty, false_val);
  return 0;
}
