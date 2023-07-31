/*
 * This code was copied from postgres:src/backend/utils/misc/guc.c (with some changes)
 * because public postgresql headers do not expose it for some reason
 * there is GetConfigOptionName(), GetConfigOption(), GetConfigOptionFlags()
 * unfortunately none of these answer a simple question: does GUC variable exist?
 * and all of them fail hard if not provided with "missing_ok = true"
 */

#include "plv8.h"

extern "C" {
#include "utils/guc.h"
#include "utils/guc_tables.h"
}

/*
 * To allow continued support of obsolete names for GUC variables, we apply
 * the following mappings to any unrecognized name.  Note that an old name
 * should be mapped to a new one only if the new variable has very similar
 * semantics to the old.
 */
static const char *const map_old_guc_names[] = {
		/* The format is
 		 * "old_name", "new_name",
 		 */
		NULL
};

static int plv8_guc_name_compare(const char *, const char *);
static int plv8_guc_var_compare(const void *, const void *);

char *
plv8_string_option(struct config_generic *record) {
	if (record->vartype != PGC_STRING)
		elog(ERROR, "'%s' is not a string", record->name);

	auto *conf = (struct config_string *) record;
	if (*conf->variable && **conf->variable)
		return *conf->variable;
	return pstrdup("");
}

int
plv8_int_option(struct config_generic *record) {
	if (record->vartype != PGC_INT)
		elog(ERROR, "'%s' is not an int", record->name);

	auto *conf = (struct config_int *) record;
	return *conf->variable;
}

/*
 * Look up option NAME.  If it exists, return a pointer to its record,
 * else return NULL.
 */
struct config_generic *
plv8_find_option(const char *name)
{
	const char **key = &name;
	struct config_generic **res;
	int			i;

	/*
	 * By equating const char ** with struct config_generic *, we are assuming
	 * the name field is first in config_generic.
	 */
	res = (struct config_generic **) bsearch((void *) &key,
											 (void *) get_guc_variables(),
											 GetNumConfigOptions(),
											 sizeof(struct config_generic *),
											 plv8_guc_var_compare);
	/*
	 * Return NULL for placeholders,
	 * these can be safely overwritten by DefineCustomTYPEVariable() functions
	 */
	if (res) {
		if ((*res)->flags & GUC_CUSTOM_PLACEHOLDER)
			return NULL;
		return *res;
	}

	/*
	 * See if the name is an obsolete name for a variable.  We assume that the
	 * set of supported old names is short enough that a brute-force search is
	 * the best way.
	 */
	for (i = 0; map_old_guc_names[i] != NULL; i += 2)
	{
		if (plv8_guc_name_compare(name, map_old_guc_names[i]) == 0)
			return plv8_find_option(map_old_guc_names[i + 1]);
	}

	/* Unknown name */
	return NULL;
}


/*
 * comparator for qsorting and bsearching guc_variables array
 */
static int
plv8_guc_var_compare(const void *a, const void *b)
{
	const struct config_generic *confa = *(struct config_generic *const *) a;
	const struct config_generic *confb = *(struct config_generic *const *) b;

	return plv8_guc_name_compare(confa->name, confb->name);
}

/*
 * the bare comparison function for GUC names
 */
static int
plv8_guc_name_compare(const char *namea, const char *nameb)
{
	/*
	 * The temptation to use strcasecmp() here must be resisted, because the
	 * array ordering has to remain stable across setlocale() calls. So, build
	 * our own with a simple ASCII-only downcasing.
	 */
	while (*namea && *nameb)
	{
		char		cha = *namea++;
		char		chb = *nameb++;

		if (cha >= 'A' && cha <= 'Z')
			cha += 'a' - 'A';
		if (chb >= 'A' && chb <= 'Z')
			chb += 'a' - 'A';
		if (cha != chb)
			return cha - chb;
	}
	if (*namea)
		return 1;				/* a is longer */
	if (*nameb)
		return -1;				/* b is longer */
	return 0;
}
