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
