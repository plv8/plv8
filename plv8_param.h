#ifndef _PLV8_PARAM_H_
#define _PLV8_PARAM_H_

extern "C" {
#include "postgres.h"

/*
 * Variable SPI parameter is since 9.0.  Avoid include files in prior versions,
 * as they contain C++ keywords.
 */
#include "nodes/params.h"
#include "parser/parse_node.h"

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

// plv8_param.cc
extern void plv8_variable_param_setup(ParseState *pstate, void *arg);
extern ParamListInfo plv8_setup_variable_paramlist(plv8_param_state *parstate,
							  Datum *values, char *nulls);

#endif	// _PLV8_PARAM_H_
