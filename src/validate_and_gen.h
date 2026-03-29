#pragma once

#include "parse.h"
#include "header_writer.h"
#include "irwriter.h"

bool validate_and_gen(ParseState* ps, HeaderWriter* hw, IrWriter* iw);
