#ifndef _PLV8_PARAM_H_
#define _PLV8_PARAM_H_

extern "C" {
#include "postgres.h"

/*
 * Variable SPI parameter is since 9.0.  Avoid include files in prior versions,
 * as they contain C++ keywords.
 */
#include "nodes/params.h"
#if PG_VERSION_NUM >= 90000
#include "parser/parse_node.h"
#endif	// PG_VERSION_NUM >= 90000

} // extern "C"

/*
 * In variable paramter case for SPI, the type information is filled by
 * the parser in paramTypes and numParams.  MemoryContext should be given
 * by the caller to allocate the paramTypes in the right context.
 */
typedef struct plv8_param_state
{
	Oid		   *paramTypes;		/* array of parameter type OIDs */
	int			numParams;		/* number of array entries */
	MemoryContext	memcontext;
} plv8_param_state;

#if PG_VERSION_NUM >= 90000
// plv8_param.cc
extern void plv8_variable_param_setup(ParseState *pstate, void *arg);
extern ParamListInfo plv8_setup_variable_paramlist(plv8_param_state *parstate,
							  Datum *values, char *nulls);
#endif	// PG_VERSION_NUM >= 90000

#endif	// _PLV8_PARAM_H_
