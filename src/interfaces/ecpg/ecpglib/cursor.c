/* src/interfaces/ecpg/ecpglib/cursor.c */

/*
 * ECPG client-side cursor support routines
 */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <stdlib.h>
#include <limits.h>

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"

static void
raise_cursor_error(struct connection *con, int lineno, const char *name)
{
	con->client_side_error = true;
	ecpg_raise(lineno, ECPG_INVALID_CURSOR,
						ECPG_SQLSTATE_INVALID_CURSOR_NAME,
						(name ? name : ecpg_gettext("<empty>")));
}

/*
 * Find the cursor with the given name.
 * Optionally, return the previous one in the list.
 */
static bool
find_cursor(struct connection *con, int lineno, const char *name, bool ignore,
				struct cursor_descriptor **out,
				struct cursor_descriptor **out_prev)
{
	struct cursor_descriptor *desc = con->cursor_desc,
					*prev = NULL;
	int (* strcmp_fn)(const char *, const char *);
	bool	found = false;

	if (!name)
	{
		raise_cursor_error(con, lineno, name);
		if (out)
			*out = NULL;
		if (out_prev)
			*out_prev = NULL;
		return false;
	}

	/*
	 * This will ensure that:
	 * - quoted cursor names will be the first ones in the list ordered case-sensitively
	 * - unquoted cursor names at the end, ordered case-insensitively.
	 */
	strcmp_fn = (name[0] == '"' ? strcmp : pg_strcasecmp);

	while (desc)
	{
		int ret = strcmp_fn(desc->name, name);

		if (ret == 0)
		{
			found = true;
			break;
		}
		/*
		 * The cursor list is ordered quasi alphabetically,
		 * don't search the whole list.
		 */
		if (ret > 0)
			break;
		prev = desc;
		desc = desc->next;
	}

	if (!found)
	{
		if (!ignore)
			raise_cursor_error(con, lineno, name);
		desc = NULL;
	}

	if (out)
		*out = desc;
	if (out_prev)
		*out_prev = prev;
	return found;
}

/*
 * Add a new cursor descriptor, maintain "mostly" alphabetic order.
 * - case sensitive alphabetic order for quoted cursor names
 * - case insensitive alphabetic order for non-quoted cursor names
 */
static struct cursor_descriptor *
add_cursor(int lineno, struct connection *con,
					const char *name,
					int subxact_level,
					bool with_hold)
{
	struct cursor_descriptor *desc, *prev;

	if (!name || name[0] == '\0')
		return NULL;

	if (find_cursor(con, lineno, name, true, &desc, &prev))
		return desc;

	desc = (struct cursor_descriptor *)
			ecpg_alloc(sizeof(struct cursor_descriptor),
								lineno);
	if (!desc)
		return NULL;

	/* Copy the name */
	desc->name = ecpg_strdup(name, lineno);
	if (!desc->name)
	{
		ecpg_free(desc);
		return NULL;
	}

	desc->subxact_level = subxact_level;
	desc->with_hold = with_hold;

	if (prev)
	{
		desc->next = prev->next;
		prev->next = desc;
	}
	else
	{
		desc->next = con->cursor_desc;
		con->cursor_desc = desc;
	}

	return desc;
}

/*
 * Delete the cursor from the list of descriptors.
 */
static void
del_cursor(struct connection *con, int lineno, const char *name)
{
	struct cursor_descriptor *desc, *prev;

	if (find_cursor(con, lineno, name, true, &desc, &prev))
	{
		if (prev)
			prev->next = desc->next;
		else
			con->cursor_desc = desc->next;

		ecpg_free((void *)desc->name);
		ecpg_free(desc);
	}
}

/*
 * Commit or roll back cursors:
 * - all transactional (non-WITH HOLD) cursors are forgotten at
 *   the end of the transaction, no matter if COMMIT or ROLLBACK
 * - at RELEASE SAVEPOINT, the cursors are propagated one
 *   subtransaction level up.
 * - at ROLLBACK TO SAVEPOINT, all cursors are forgotten that were
 *   created in this subtransaction or below that level
 * - WITH HOLD cursors are kept after the transaction in case of
 *   toplevel COMMIT. The caller checks for the transaction in error
 *   and sets the rollback flag accordingly.
 */
void
ecpg_commit_cursors(int lineno, struct connection * conn,
						bool rollback, int level)
{
	struct cursor_descriptor *desc, *tmp,
					*prev = NULL;

	desc = conn->cursor_desc;
	while (desc)
	{
		/* Keep already COMMITted WITH HOLD cursors */
		if (desc->with_hold && desc->subxact_level == 0)
			;
		else if (rollback)
		{
			/*
			 * On ROLLBACK [ TO SAVEPOINT ... ] forget about
			 * every cursor DECLAREd under this level.
			 */
			if (desc->subxact_level >= level)
			{
				tmp = desc->next;
				ecpg_free(desc->name);
				ecpg_free(desc);
				desc = tmp;
				if (prev)
					prev->next = tmp;
				else
					conn->cursor_desc = tmp;
				continue;
			}
		}
		else
		{
			/*
			 * If this is a WITH HOLD cursor or we're in a
			 * subtransaction, COMMIT the cursor one level up.
			 */
			if (desc->with_hold || level > 1)
			{
				if (level > 0)
					desc->subxact_level = level - 1;
			}
			/*
			 * Otherwise (not WITH HOLD cursor AND it's the
			 * toplevel transaction) free the cursor.
			 */
			else
			{
				tmp = desc->next;
				ecpg_free(desc->name);
				ecpg_free(desc);
				desc = tmp;
				if (prev)
					prev->next = tmp;
				else
					conn->cursor_desc = tmp;
				continue;
			}
		}
		prev = desc;
		desc = desc->next;
	}
}

/*
 * Declare the cursor in the backend and create a
 * minimalistic descriptor to make tracking possible.
 * This function is called when the ECPG code OPENs the cursor.
 */
bool
ECPGopen(const int lineno, const int compat, const int force_indicator,
			const char *connection_name, const bool questionmarks,
			const bool with_hold, const char *curname, const int st, const char *query, ...)
{
	struct connection *con = ecpg_get_connection(connection_name);
	int		subxact_level;
	bool		ret;
	va_list		args;

	/*
	 * Execute the DECLARE query in the backend.
	 */
	va_start(args, query);
	ret = ecpg_do(lineno, compat, force_indicator, connection_name, questionmarks,
										st,
										query,
										args);
	va_end(args);

	if (!ret)
		return false;

	/* Finally add the cursor to the connection. */
	if (con->subxact_desc)
		subxact_level = con->subxact_desc->level;
	else
		subxact_level =
			(PQtransactionStatus(con->connection) != PQTRANS_IDLE ?
										1 : 0);
	add_cursor(lineno, con, curname, subxact_level, with_hold);

	return true;
}

/*
 * ECPGfetch
 * Execute a FETCH or MOVE statement for the application.
 *
 * This function maintains the internal cursor descriptor and
 * reduces the the network roundtrip by returning early for
 * an unknown cursor name.
 */
bool
ECPGfetch(const int lineno, const int compat, const int force_indicator,
				const char *connection_name, const bool questionmarks,
				const char *curname, const int st, const char *query, ...)
{
	struct connection  *con = ecpg_get_connection(connection_name);
	struct cursor_descriptor *cur;
	bool		ret;
	va_list		args;

	if (!find_cursor(con, lineno, curname, false, &cur, NULL))
		return false;

	va_start(args, query);
	ret = ecpg_do(lineno, compat, force_indicator, connection_name, questionmarks,
									st,
									query,
									args);
	va_end(args);

	return ret;
}

/*
 * ECPGcursor_dml
 * Called for DELETE / UPDATE ... WHERE CURRENT OF cursorname
 */
bool
ECPGcursor_dml(const int lineno, const int compat, const int force_indicator,
				const char *connection_name, const bool questionmarks,
				const char *curname, const int st, const char *query, ...)
{
	struct connection  *con = ecpg_get_connection(connection_name);
	struct cursor_descriptor *cur;
	bool		ret;
	va_list		args;

	if (!find_cursor(con, lineno, curname, false, &cur, NULL))
		return false;

	va_start(args, query);
	ret = ecpg_do(lineno, compat, force_indicator, connection_name, questionmarks,
									st, query, args);
	va_end(args);
	return ret;
}

/*
 * ECPGclose
 * Close and free a cursor.
 */
bool
ECPGclose(const int lineno, const int compat, const int force_indicator,
				const char *connection_name, const bool questionmarks,
				const char *curname, const int st, const char *query, ...)
{
	struct connection  *con = ecpg_get_connection(connection_name);
	struct cursor_descriptor *cur;
	bool		ret;
	va_list		args;

	if (!find_cursor(con, lineno, curname, false, &cur, NULL))
		return false;

	va_start(args, query);
	ret = ecpg_do(lineno, compat, force_indicator, connection_name, questionmarks,
									st, query, args);
	va_end(args);

	del_cursor(con, lineno, curname);

	return ret;
}
