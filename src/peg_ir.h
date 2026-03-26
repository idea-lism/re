#pragma once

#include "irwriter.h"

#include <stdint.h>

int32_t peg_ir_call(IrWriter* w, char* buf, int32_t buf_size, const char* rule_name, const char* table,
                    const char* col);

int32_t peg_ir_tok(IrWriter* w, char* buf, int32_t buf_size, const char* tok_id, const char* col);

int32_t peg_ir_load_slot(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                         int32_t slot_idx);

int32_t peg_ir_store_slot(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                          int32_t slot_idx, const char* value);

int32_t peg_ir_load_bits(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                         int32_t seg_idx);

int32_t peg_ir_store_bits(IrWriter* w, char* buf, int32_t buf_size, const char* table, const char* col,
                          int32_t seg_idx, const char* value);

void peg_ir_push(IrWriter* w, const char* col);
void peg_ir_pop(IrWriter* w, char* buf, int32_t buf_size);
void peg_ir_peek(IrWriter* w, char* buf, int32_t buf_size);
void peg_ir_jump(IrWriter* w, const char* label);
void peg_ir_iffail(IrWriter* w, const char* result, const char* label);

int32_t peg_ir_alloca(IrWriter* w, char* buf, int32_t buf_size, const char* ty);
int32_t peg_ir_load(IrWriter* w, char* buf, int32_t buf_size, const char* ty, const char* ptr);
void peg_ir_store(IrWriter* w, const char* ty, const char* val, const char* ptr);
int32_t peg_ir_add(IrWriter* w, char* buf, int32_t buf_size, const char* lhs, const char* rhs);
int32_t peg_ir_select(IrWriter* w, char* buf, int32_t buf_size, const char* cond, const char* ty, const char* true_val,
                      const char* false_val);
