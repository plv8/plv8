/*-------------------------------------------------------------------------
 *
 * plv8_param.cc : PL/v8 parameter handling.
 *
 * Copyright (c) 2009-2012, the PLV8JS Development Group.
 *-------------------------------------------------------------------------
 */
#include "plv8_param.h"
#include <limits.h>

/*
 * Variable SPI parameter is since 9.0.  Avoid include files in prior versions,
 * as they contain C++ keywords.
 */
#if PG_VERSION_NUM >= 90000

extern "C" {

#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

} // extern "C"


/*
 * In the varparams case, the caller-supplied OID array (if any) can be
 * re-palloc'd larger at need.  A zero array entry means that parameter number
 * hasn't been seen, while UNKNOWNOID means the parameter has been used but
 * its type is not yet known.
 */

static Node *plv8_variable_paramref_hook(ParseState *pstate, ParamRef *pref);
static Node *plv8_variable_coerce_param_hook(ParseState *pstate, Param *param,
						   Oid targetTypeId, int32 targetTypeMod,
						   int location);

void
plv8_variable_param_setup(ParseState *pstate, void *arg)
{
	plv8_param_state *parstate = (plv8_param_state *) arg;

	pstate->p_ref_hook_state = (void *) parstate;
	pstate->p_paramref_hook = plv8_variable_paramref_hook;
	pstate->p_coerce_param_hook = plv8_variable_coerce_param_hook;
}

static Node *
plv8_variable_paramref_hook(ParseState *pstate, ParamRef *pref)
{
	plv8_param_state *parstate = (plv8_param_state *) pstate->p_ref_hook_state;
	int			paramno = pref->number;
	Oid		   *pptype;
	Param	   *param;

	/* Check parameter number is in range */
	if (paramno <= 0 || paramno > (int) (INT_MAX / sizeof(Oid)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("there is no parameter $%d", paramno),
				 parser_errposition(pstate, pref->location)));
	if (paramno > parstate->numParams)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(parstate->memcontext);
		/* Need to enlarge param array */
		if (parstate->paramTypes)
			parstate->paramTypes = (Oid *) repalloc(parstate->paramTypes,
													paramno * sizeof(Oid));
		else
			parstate->paramTypes = (Oid *) palloc(paramno * sizeof(Oid));
		/* Zero out the previously-unreferenced slots */
		MemSet(parstate->paramTypes + parstate->numParams,
			   0,
			   (paramno - parstate->numParams) * sizeof(Oid));
		parstate->numParams = paramno;
		MemoryContextSwitchTo(oldcontext);
	}

	/* Locate param's slot in array */
	pptype = &(parstate->paramTypes)[paramno - 1];

	/* If not seen before, initialize to UNKNOWN type */
	if (*pptype == InvalidOid)
		*pptype = UNKNOWNOID;

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = *pptype;
	param->paramtypmod = -1;
#if PG_VERSION_NUM >= 90100
	param->paramcollid = get_typcollation(param->paramtype);
#endif
	param->location = pref->location;

	return (Node *) param;
}

static Node *
plv8_variable_coerce_param_hook(ParseState *pstate, Param *param,
							   Oid targetTypeId, int32 targetTypeMod,
							   int location)
{
	if (param->paramkind == PARAM_EXTERN && param->paramtype == UNKNOWNOID)
	{
		/*
		 * Input is a Param of previously undetermined type, and we want to
		 * update our knowledge of the Param's type.
		 */
		plv8_param_state *parstate =
			(plv8_param_state *) pstate->p_ref_hook_state;
		Oid		   *paramTypes = parstate->paramTypes;
		int			paramno = param->paramid;

		if (paramno <= 0 ||		/* shouldn't happen, but... */
			paramno > parstate->numParams)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PARAMETER),
					 errmsg("there is no parameter $%d", paramno),
					 parser_errposition(pstate, param->location)));

		if (paramTypes[paramno - 1] == UNKNOWNOID)
		{
			/* We've successfully resolved the type */
			paramTypes[paramno - 1] = targetTypeId;
		}
		else if (paramTypes[paramno - 1] == targetTypeId)
		{
			/* We previously resolved the type, and it matches */
		}
		else
		{
			/* Ooops */
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_PARAMETER),
					 errmsg("inconsistent types deduced for parameter $%d",
							paramno),
					 errdetail("%s versus %s",
							   format_type_be(paramTypes[paramno - 1]),
							   format_type_be(targetTypeId)),
					 parser_errposition(pstate, param->location)));
		}

		param->paramtype = targetTypeId;

		/*
		 * Note: it is tempting here to set the Param's paramtypmod to
		 * targetTypeMod, but that is probably unwise because we have no
		 * infrastructure that enforces that the value delivered for a Param
		 * will match any particular typmod.  Leaving it -1 ensures that a
		 * run-time length check/coercion will occur if needed.
		 */
		param->paramtypmod = -1;

#if PG_VERSION_NUM >= 90100
		/*
		 * This module always sets a Param's collation to be the default for
		 * its datatype.  If that's not what you want, you should be using the
		 * more general parser substitution hooks.
		 */
		param->paramcollid = get_typcollation(param->paramtype);
#endif

		/* Use the leftmost of the param's and coercion's locations */
		if (location >= 0 &&
			(param->location < 0 || location < param->location))
			param->location = location;

		return (Node *) param;
	}

	/* Else signal to proceed with normal coercion */
	return NULL;
}

ParamListInfo
plv8_setup_variable_paramlist(plv8_param_state *parstate,
							  Datum *values, char *nulls)
{
	ParamListInfo		paramLI;

	paramLI = (ParamListInfo) palloc0(sizeof(ParamListInfoData) +
							sizeof(ParamExternData) * (parstate->numParams - 1));
	paramLI->numParams = parstate->numParams;
	for(int i = 0; i < parstate->numParams; i++)
	{
		ParamExternData	   *param = &paramLI->params[i];

		param->value = values[i];
		param->isnull = nulls[i] == 'n';
		param->pflags = PARAM_FLAG_CONST;
		param->ptype = parstate->paramTypes[i];
	}

	return paramLI;
}
#endif	// PG_VERSION_NUM >= 90000
