#pragma once

#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
#define RE_API
#elif defined(__GNUC__) || defined(__clang__)
#define RE_API __attribute__((visibility("default")))
#else
#define RE_API
#endif

typedef struct Lex Lex;

#define LEX_ERR_PAREN (-1)
#define LEX_ERR_BRACKET (-2)

RE_API Lex* lex_new(const char* source_file_name, const char* mode);
RE_API void lex_del(Lex* l);
RE_API int32_t lex_add(Lex* l, const char* pattern, int32_t source_file_offset);
RE_API void lex_gen(Lex* l, FILE* f, const char* target_triple);
