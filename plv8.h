#ifndef _PLV8_H_
#define _PLV8_H_

#ifdef __cplusplus
extern "C"{
#endif

#include "postgres.h"
#include "fmgr.h"

Datum plv8_call_handler(PG_FUNCTION_ARGS);
Datum plv8_call_validator(PG_FUNCTION_ARGS);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _PLV8_H
