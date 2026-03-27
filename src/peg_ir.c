// PEG IR helpers: thin abstraction over LLVM IR for packrat parser generation.

#include "peg_ir.h"

#include <stdio.h>
#include <string.h>

void peg_ir_tok(IrWriter* w, char* out, int32_t out_size, const char* token_id, const char* col) {
  char args[128];
  snprintf(args, sizeof(args), "i32 %s, i32 %s", token_id, col);
  irwriter_call_ret(w, out, out_size, "i32", "match_tok", args);
}

void peg_ir_call(IrWriter* w, char* out, int32_t out_size, const char* rule_name, const char* table, const char* col) {
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "parse_%s", rule_name);

  char col_ext[32];
  irwriter_sext(w, col_ext, sizeof(col_ext), "i32", col, "i64");

  char args[256];
  snprintf(args, sizeof(args), "ptr %s, i64 %s", table, col_ext);
  irwriter_call_ret(w, out, out_size, "i32", func_name, args);
}

void peg_ir_memo_get(IrWriter* w, char* out, int32_t out_size, const char* col_type, const char* table, const char* col,
                     int32_t field_idx, int32_t slot_idx) {
  char gep_buf[32], indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 %d, i32 %d", col, field_idx, slot_idx);
  irwriter_gep(w, gep_buf, sizeof(gep_buf), col_type, table, indices);
  irwriter_load(w, out, out_size, "i32", gep_buf);
}

void peg_ir_memo_set(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t field_idx,
                     int32_t slot_idx, const char* val) {
  char gep_buf[32], indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 %d, i32 %d", col, field_idx, slot_idx);
  irwriter_gep(w, gep_buf, sizeof(gep_buf), col_type, table, indices);
  irwriter_store(w, "i32", val, gep_buf);
}

// Backtrack stack: { [16 x i32], i32 } = { data, top }
// top starts at -1 (empty). push increments then stores, peek loads at top, pop decrements.

void peg_ir_emit_bt_defs(IrWriter* w) {
  irwriter_type_def(w, "BtStack", "{ [16 x i32], i32 }");
  irwriter_raw(w, "\n");

  irwriter_raw(w, "define internal void @backtrack_push(ptr %stack, i32 %col) {\n"
                  "entry:\n"
                  "  %top_ptr = getelementptr %BtStack, ptr %stack, i32 0, i32 1\n"
                  "  %top = load i32, ptr %top_ptr\n"
                  "  %new_top = add i32 %top, 1\n"
                  "  store i32 %new_top, ptr %top_ptr\n"
                  "  %slot = getelementptr %BtStack, ptr %stack, i32 0, i32 0, i32 %new_top\n"
                  "  store i32 %col, ptr %slot\n"
                  "  ret void\n"
                  "}\n\n");

  irwriter_raw(w, "define internal i32 @backtrack_restore(ptr %stack) {\n"
                  "entry:\n"
                  "  %top_ptr = getelementptr %BtStack, ptr %stack, i32 0, i32 1\n"
                  "  %top = load i32, ptr %top_ptr\n"
                  "  %slot = getelementptr %BtStack, ptr %stack, i32 0, i32 0, i32 %top\n"
                  "  %val = load i32, ptr %slot\n"
                  "  ret i32 %val\n"
                  "}\n\n");

  irwriter_raw(w, "define internal void @backtrack_pop(ptr %stack) {\n"
                  "entry:\n"
                  "  %top_ptr = getelementptr %BtStack, ptr %stack, i32 0, i32 1\n"
                  "  %top = load i32, ptr %top_ptr\n"
                  "  %new_top = add i32 %top, -1\n"
                  "  store i32 %new_top, ptr %top_ptr\n"
                  "  ret void\n"
                  "}\n\n");
}

void peg_ir_backtrack_push(IrWriter* w, const char* stack, const char* col) {
  char args[128];
  snprintf(args, sizeof(args), "ptr %s, i32 %s", stack, col);
  irwriter_call_void_fmt(w, "backtrack_push", args);
}

void peg_ir_backtrack_restore(IrWriter* w, char* out, int32_t out_size, const char* stack) {
  char args[64];
  snprintf(args, sizeof(args), "ptr %s", stack);
  irwriter_call_ret(w, out, out_size, "i32", "backtrack_restore", args);
}

void peg_ir_backtrack_pop(IrWriter* w, const char* stack) {
  char args[64];
  snprintf(args, sizeof(args), "ptr %s", stack);
  irwriter_call_void_fmt(w, "backtrack_pop", args);
}

void peg_ir_bit_test(IrWriter* w, char* out, int32_t out_size, const char* col_type, const char* table, const char* col,
                     int32_t seg_idx, int32_t rule_bit) {
  char gep_buf[32], indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  irwriter_gep(w, gep_buf, sizeof(gep_buf), col_type, table, indices);

  char bits[32];
  irwriter_load(w, bits, sizeof(bits), "i32", gep_buf);

  char masked[32];
  irwriter_binop_imm(w, masked, sizeof(masked), "and", "i32", bits, rule_bit);
  irwriter_icmp_imm(w, out, out_size, "ne", "i32", masked, 0);
}

void peg_ir_bit_deny(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                     int32_t rule_bit) {
  char gep_buf[32], indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  irwriter_gep(w, gep_buf, sizeof(gep_buf), col_type, table, indices);

  char bits[32];
  irwriter_load(w, bits, sizeof(bits), "i32", gep_buf);

  char cleared[32];
  irwriter_binop_imm(w, cleared, sizeof(cleared), "and", "i32", bits, ~rule_bit);
  irwriter_store(w, "i32", cleared, gep_buf);
}

void peg_ir_bit_exclude(IrWriter* w, const char* col_type, const char* table, const char* col, int32_t seg_idx,
                        int32_t rule_bit) {
  char gep_buf[32], indices[128];
  snprintf(indices, sizeof(indices), "i32 %s, i32 0, i32 %d", col, seg_idx);
  irwriter_gep(w, gep_buf, sizeof(gep_buf), col_type, table, indices);

  char bits[32];
  irwriter_load(w, bits, sizeof(bits), "i32", gep_buf);

  char kept[32];
  irwriter_binop_imm(w, kept, sizeof(kept), "and", "i32", bits, rule_bit);
  irwriter_store(w, "i32", kept, gep_buf);
}

void peg_ir_declare_externs(IrWriter* w) { irwriter_declare(w, "i32", "match_tok", "i32, i32"); }
