#include "peg_ir.h"

#include <stdio.h>

int32_t peg_ir_call(IrWriter* w, char* buf, int32_t buf_size, const char* rule_name, const char* table,
                    const char* col) {
  (void)w;
  (void)buf;
  (void)buf_size;
  (void)rule_name;
  (void)table;
  (void)col;
  return 0;
}

int32_t peg_ir_tok(IrWriter* w, char* buf, int32_t buf_size, const char* tok_id, const char* col) {
  (void)w;
  (void)buf;
  (void)buf_size;
  (void)tok_id;
  (void)col;
  return 0;
}

int32_t peg_ir_load_slot(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                         int32_t slot_idx) {
  (void)w;
  (void)buf;
  (void)buf_size;
  (void)table;
  (void)col;
  (void)slot_idx;
  return 0;
}

int32_t peg_ir_store_slot(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                          int32_t slot_idx, const char* value) {
  (void)w;
  (void)buf;
  (void)buf_size;
  (void)table;
  (void)col;
  (void)slot_idx;
  (void)value;
  return 0;
}

int32_t peg_ir_load_bits(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                         int32_t seg_idx) {
  (void)w;
  (void)buf;
  (void)buf_size;
  (void)table;
  (void)col;
  (void)seg_idx;
  return 0;
}

int32_t peg_ir_store_bits(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                          int32_t seg_idx, const char* value) {
  (void)w;
  (void)buf;
  (void)buf_size;
  (void)table;
  (void)col;
  (void)seg_idx;
  (void)value;
  return 0;
}
