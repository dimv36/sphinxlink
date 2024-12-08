/*
 * sphinxlink.c
 *
 * Functions returning results from a remote SphinxSearch (ManticoreSearch) server
 *
 * Author: Dmitry Voronin <carriingfate92@yandex.ru>
 *
 * contrib/sphinxlink/sphinxlink.c
 * Copyright (c) 2017 - 2022, Dmitry Voronin
 * ALL RIGHTS RESERVED;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */
#include "postgres.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "funcapi.h"
#include <sphinxlink.h>

PG_MODULE_MAGIC;

#define MAXHOSTLEN 1024
#define NUMCONN 32

#define safe_free(_ptr, _freed) \
do { \
	if ((_ptr) && (_freed)) \
		pfree((_ptr)); \
	(_ptr) = NULL; \
} while (0)

/* Global Module Structures */
typedef struct remoteConn
{
	MYSQL	   *conn;				/* Hold the remote connection */
	int			port;				/* Sphinx port for connection */
	char		host[MAXHOSTLEN];	/* Host for connection */
} remoteConn;


typedef struct storeInfo
{
	FunctionCallInfo fcinfo;
	Tuplestorestate *tuplestore;
	AttInMetadata *attinmeta;
	MemoryContext tmpcontext;
	char	  **cstrs;
} storeInfo;


typedef struct remoteConnHashEnt
{
	char		name[NAMEDATALEN];
	remoteConn *rconn;
} remoteConnHashEnt;


typedef struct sphinx_meta_ctx
{
	MYSQL_RES	*res;
} sphinx_meta_ctx;


#define SPHINXLINK_INIT \
do { \
	if (!pconn) \
	{ \
		pconn = (remoteConn *) MemoryContextAlloc(TopMemoryContext, sizeof(remoteConn)); \
		pconn->conn = NULL; \
		pconn->port = 0; \
		pconn->host[0] = '\0'; \
	} \
} while (0)


#define SPHINXLINK_GETCONN \
do { \
	conname = text_to_cstring(tconname); \
	rconn = getConnectionByName(conname); \
	if (rconn) \
		conn = rconn->conn; \
	if (!conn && conname) \
	{ \
		ereport(ERROR, \
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST), \
				 errmsg("connection \"%s\" is not available", conname))); \
	} \
} while (0)


/* Static functions declaration */
static TupleDesc createTemplateTupleDescImpl(int nargs);
static void prepTuplestoreResult(FunctionCallInfo fcinfo);
static void materializeQueryResult(FunctionCallInfo fcinfo, MYSQL *conn, const char *sql, const char *match_clause);
static void storeRow(volatile storeInfo *sinfo, MYSQL_ROW row, unsigned int nfields, bool first);
static bool storeQueryResult(volatile storeInfo *sinfo, MYSQL *conn, const char *sql, const char *match_clause);
static remoteConn *getConnectionByName(const char *name);
static HTAB *createConnHash(void);
static void createNewConnection(const char *name, const char *host, const int port);
static bool connectionExists(const char *name);
static void deleteConnection(const char *name);
static char *toMyDatabaseEncoding(const char *value);
static char *toUTF8Encoding(const char *value);

/* Module variables declaration */
static remoteConn *pconn = NULL;
static HTAB *remoteConnHash = NULL;


PG_FUNCTION_INFO_V1(sphinx_connect);
Datum
sphinx_connect(PG_FUNCTION_ARGS)
{
	text	   *tconname = PG_GETARG_TEXT_PP(0);
	text	   *thost = PG_GETARG_TEXT_PP(1);
	char	   *conname = NULL;
	char	   *host = NULL;
	int			port;

	SPHINXLINK_INIT;

	conname = text_to_cstring(tconname);
	host = text_to_cstring(thost);
	port = PG_GETARG_INT32(2);

	PG_FREE_IF_COPY(tconname, 0);
	PG_FREE_IF_COPY(thost, 1);

	if (connectionExists(conname))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("duplicate connection name")));

	createNewConnection(conname, host, port);

	PG_RETURN_TEXT_P(cstring_to_text("OK"));
}


PG_FUNCTION_INFO_V1(sphinx_disconnect);
Datum
sphinx_disconnect(PG_FUNCTION_ARGS)
{
	text	   *tconname = PG_GETARG_TEXT_PP(0);
	char	   *conname = NULL;
	remoteConn *rconn = NULL;
	MYSQL	   *conn = NULL;

	SPHINXLINK_INIT;
	SPHINXLINK_GETCONN;

	mysql_close(conn);
	conn = NULL;
	if (rconn)
	{
		deleteConnection(conname);
		pfree(rconn);
	}
	else
		pconn->conn = NULL;

	PG_FREE_IF_COPY(tconname, 0);

	PG_RETURN_TEXT_P(cstring_to_text("OK"));
}


PG_FUNCTION_INFO_V1(sphinx_connections);
Datum
sphinx_connections(PG_FUNCTION_ARGS)
{
	FuncCallContext	   *funcctx;
	int					call_cntr;
	int					max_calls;
	TupleDesc			tupdesc;
	AttInMetadata	   *attinmeta;
	char			   *values[3];

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext	oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* total number of tuples to be returned */
		funcctx->max_calls = remoteConnHash ? hash_get_num_entries(remoteConnHash) : 0;

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));

		/*
		 * generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		tupdesc = createTemplateTupleDescImpl(3);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "connname",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "host",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "port",
						   INT4OID, -1, 0);

		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls)		/* do when there is more left to send */
	{
		HeapTuple	tuple;
		Datum		result;
		remoteConnHashEnt *entry;
		HASH_SEQ_STATUS status;
		int			i = 0;
		int			j = 0;

		hash_seq_init(&status, remoteConnHash);
		while ((entry = (remoteConnHashEnt *) hash_seq_search(&status)) != NULL)
		{
			if (i == call_cntr)
				break;
			i++;
		}

		/*
		 * Prepare a values array for building the returned tuple.
		 * This should be an array of C strings which will
		 * be processed later by the type input functions.
		 */
		values[j++] = psprintf("%s", entry->name);
		values[j++] = psprintf("%s", entry->rconn->host);
		values[j++] = psprintf("%d", entry->rconn->port);

		hash_seq_term(&status);

		/* build a tuple */
		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else		/* do when there is no more left */
	{
		SRF_RETURN_DONE(funcctx);
	}
}


PG_FUNCTION_INFO_V1(sphinx_query);
Datum
sphinx_query(FunctionCallInfo fcinfo)
{
	MYSQL	   *volatile conn = NULL;
	remoteConn *rconn = NULL;
	char	   *match_clause = NULL;
	char	   *sql = NULL;

	prepTuplestoreResult(fcinfo);

	SPHINXLINK_INIT;

	if (((PG_NARGS() == 3) ||
		 (PG_NARGS() == 4)) && (get_fn_expr_argtype(fcinfo->flinfo, 1) == INT4OID))
	{
		/* text, int, text, text OR text, int, text */
		StringInfoData	conntmppl;
		char		   *host = text_to_cstring(PG_GETARG_TEXT_PP(0));
		int				port = PG_GETARG_INT64(1);

		initStringInfo(&conntmppl);

		appendStringInfo(&conntmppl, "sph-%s-%d", host, port);

		if (!(rconn = getConnectionByName(conntmppl.data)))
		{
			createNewConnection(conntmppl.data, host, port);
			rconn = getConnectionByName(conntmppl.data);
		}
		if (strcmp(rconn->host, host) || (rconn->port != port))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("connection with name \"%s\" already exists, but creadentials are different", conntmppl.data)));
		else
			conn = rconn->conn;

		sql = text_to_cstring(PG_GETARG_TEXT_PP(2));
		if (PG_NARGS() == 4)
			match_clause = text_to_cstring(PG_GETARG_TEXT_PP(3));
	}
	else if (((PG_NARGS() == 2) ||
			  (PG_NARGS() == 3)) && (get_fn_expr_argtype(fcinfo->flinfo, 1) == TEXTOID))
	{
		text	   *tconname = PG_GETARG_TEXT_PP(0);
		char	   *conname = NULL;

		SPHINXLINK_GETCONN;

		sql = text_to_cstring(PG_GETARG_TEXT_PP(1));

		if (PG_NARGS() == 3)
			match_clause = text_to_cstring(PG_GETARG_TEXT_PP(2));

		PG_FREE_IF_COPY(tconname, 0);
	}

	PG_TRY();
	{
		materializeQueryResult(fcinfo, conn, sql, match_clause);
	}
	PG_CATCH();
	{
		PG_RE_THROW();
	}

	PG_END_TRY();

	return (Datum) 0;
}


/*
 * Verify function caller can handle a tuplestore result, and set up for that.
 *
 * Note: if the caller returns without actually creating a tuplestore, the
 * executor will treat the function result as an empty set.
 */
void
prepTuplestoreResult(FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* check to see if query supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* let the executor know we're sending back a tuplestore */
	rsinfo->returnMode = SFRM_Materialize;

	/* caller must fill these to return a non-empty result */
	rsinfo->setResult = NULL;
	rsinfo->setDesc = NULL;
}


/*
 * Execute query, and send any result rows to sinfo->tuplestore.
 */
static bool
storeQueryResult(volatile storeInfo *sinfo,
				 MYSQL *conn,
				 const char *sql,
				 const char *match_clause)
{
	bool		first = true;
	unsigned int nfields = 0;
	MYSQL_RES   *res;
	const char  *query = NULL;
	int			ret = 0;

	if (match_clause)
	{
		StringInfoData	buff;
		char		   *pos = NULL;
		char		   *escaped = NULL;

		initStringInfo(&buff);

		if ((pos = strstr(sql, "MATCH(?)")))
		{
			int			length = strlen(match_clause);
			int			encoded_length = 0;

			appendBinaryStringInfo(&buff, sql, (pos - sql));

			escaped = (char *) palloc(length * 2 + 1);

			if ((encoded_length = mysql_real_escape_string(conn, escaped, match_clause, length)) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Could not escape clause \"%s\"", match_clause)));
			appendStringInfo(&buff, "MATCH('%s')", escaped);
			pos += 8;
			if (pos)
				appendStringInfoString(&buff, pos);
			pfree(escaped);
		}
		query = buff.data;
	}
	else
		query = sql;

	if ((ret = mysql_query(conn, toUTF8Encoding(query))))
		ereport(ERROR,
				(errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
				 errmsg("Could not execute query: %s", mysql_error(conn))));

	res = mysql_store_result(conn);
	nfields = mysql_num_fields(res);

	for (;;)
	{
		MYSQL_ROW	row = NULL;

		CHECK_FOR_INTERRUPTS();

		if (!(row = mysql_fetch_row(res)))
			break;

		/* if empty resultset, fill tuplestore header */
		storeRow(sinfo, row, nfields, first);
		if (first)
			first = false;
	}
	if (res)
		mysql_free_result(res);

	return true;
}


/*
 * Send single row to sinfo->tuplestore.
 */
static void
storeRow(volatile storeInfo *sinfo, MYSQL_ROW row, unsigned int nfields, bool first)
{
	HeapTuple		tuple;
	MemoryContext	oldcontext;
	unsigned int	i;

	if (first)
	{
		/* Prepare for new result set */
		ReturnSetInfo *rsinfo = (ReturnSetInfo *) sinfo->fcinfo->resultinfo;
		TupleDesc	tupdesc;

		/*
		 * It's possible to get more than one result set if the query string
		 * contained multiple SQL commands.  In that case, we follow PQexec's
		 * traditional behavior of throwing away all but the last result.
		 */
		if (sinfo->tuplestore)
			tuplestore_end(sinfo->tuplestore);
		sinfo->tuplestore = NULL;

		/* get a tuple descriptor for our result type */
		switch (get_call_result_type(sinfo->fcinfo, NULL, &tupdesc))
		{
			case TYPEFUNC_COMPOSITE:
				/* success */
				break;
			case TYPEFUNC_RECORD:
				/* failed to determine actual type of RECORD */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function returning record called in context "
								"that cannot accept type record")));
				break;
			default:
				/* result type isn't composite */
				elog(ERROR, "return type must be a row type");
				break;
		}

		/* make sure we have a persistent copy of the tupdesc */
		tupdesc = CreateTupleDescCopy(tupdesc);

		/* check result and tuple descriptor have the same number of columns */
		if (nfields != tupdesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("remote query result rowtype does not match "
							"the specified FROM clause rowtype")));

		/* Prepare attinmeta for later data conversions */
		sinfo->attinmeta = TupleDescGetAttInMetadata(tupdesc);

		/* Create a new, empty tuplestore */
		oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
		sinfo->tuplestore = tuplestore_begin_heap(true, false, work_mem);
		rsinfo->setResult = sinfo->tuplestore;
		rsinfo->setDesc = tupdesc;
		MemoryContextSwitchTo(oldcontext);

		/* Done if empty resultset */
		if (!row)
			return;

		/*
		 * Set up sufficiently-wide string pointers array; this won't change
		 * in size so it's easy to preallocate.
		 */
		if (sinfo->cstrs)
			pfree(sinfo->cstrs);
		sinfo->cstrs = (char **) palloc(nfields * sizeof(char *));
	}

	/*
	 * Do the following work in a temp context that we reset after each tuple.
	 * This cleans up not only the data we have direct access to, but any
	 * cruft the I/O functions might leak.
	 */
	oldcontext = MemoryContextSwitchTo(sinfo->tmpcontext);

	/*
	 * Fill cstrs with null-terminated strings of column values.
	 */
	for (i = 0; i < nfields; i++)
	{
		char *value = row[i];

		if (!value)
			sinfo->cstrs[i] = NULL;
		else
			sinfo->cstrs[i] = toMyDatabaseEncoding(value);
	}

	/* Convert row to a tuple, and add it to the tuplestore */
	tuple = BuildTupleFromCStrings(sinfo->attinmeta, sinfo->cstrs);

	tuplestore_puttuple(sinfo->tuplestore, tuple);

	/* Clean up */
	MemoryContextSwitchTo(oldcontext);
	MemoryContextReset(sinfo->tmpcontext);
}



/*
 * Execute the given SQL command and store its results into a tuplestore
 * to be returned as the result of the current function.
 *
 * This is equivalent to PQexec followed by materializeResult, but we make
 * use of libpq's single-row mode to avoid accumulating the whole result
 * inside libpq before it gets transferred to the tuplestore.
 */
static void
materializeQueryResult(FunctionCallInfo fcinfo,
					   MYSQL *conn,
					   const char *sql,
					   const char *match_clause)
{
	volatile storeInfo sinfo;

	/* initialize storeInfo to empty */
	memset((void *) &sinfo, 0, sizeof(sinfo));
	sinfo.fcinfo = fcinfo;

	PG_TRY();
	{
		/* Create short-lived memory context for data conversions */
		sinfo.tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
												 "sphinxlink temporary context",
												 ALLOCSET_DEFAULT_SIZES);

		/* execute query, collecting any tuples into the tuplestore */
		if (!storeQueryResult(&sinfo, conn, sql, match_clause))
		{
			char	   *err = pstrdup(mysql_error(conn));

			ereport(ERROR,
					(errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
					 errmsg("Error when send query to Sphinx: %s", err)));
		}

		/* clean up data conversion short-lived memory context */
		if (sinfo.tmpcontext != NULL)
			MemoryContextDelete(sinfo.tmpcontext);
		sinfo.tmpcontext = NULL;
	}
	PG_CATCH();
	{
		PG_RE_THROW();
	}
	PG_END_TRY();
}


TupleDesc
createTemplateTupleDescImpl(int nargs)
{
#if (PG_VERSION_NUM >= 120000)
	return CreateTemplateTupleDesc(nargs);
#else
	return CreateTemplateTupleDesc(nargs, false);
#endif
}


remoteConn *
getConnectionByName(const char *name)
{
	remoteConnHashEnt *hentry;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), false);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash,
											   key, HASH_FIND, NULL);

	if (hentry)
		return hentry->rconn;

	return NULL;
}


HTAB *
createConnHash(void)
{
	HASHCTL		ctl;

	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(remoteConnHashEnt);

	return hash_create("Remote Con hash", NUMCONN, &ctl, HASH_ELEM);
}


void
createNewConnection(const char *name,
					const char *host,
					const int port)
{
	remoteConnHashEnt *hentry;
	bool			found;
	char		   *key;
	MYSQL		   *conn = NULL;
	remoteConn	   *rconn = NULL;
	int				reconnect = 1;
	my_bool			disabled = 0;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	conn = mysql_init(NULL);
	if (!conn)
	{
		safe_free(rconn, true);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to initialise MySQL connection object")));
	}

	/* Sphinx only works with UTF8, so make connection with it */
	mysql_options(conn, MYSQL_SET_CHARSET_NAME, "UTF8");
	mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);
	/* MDEV-31857: enable MYSQL_OPT_SSL_VERIFY_SERVER_CERT by default */
	mysql_options(conn, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &disabled);

	if (!mysql_real_connect(conn, host, NULL, NULL, NULL, port, NULL, 0))
	{
		char	   *msg;

		msg = pstrdup(mysql_error(conn));
		mysql_close(conn);
		safe_free(rconn, true);

		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("failed to connect to Sphinx: %s", msg)));
	}

	/* create hash entry */
	rconn = (remoteConn *) MemoryContextAlloc(TopMemoryContext,
											  sizeof(remoteConn));

	rconn->conn = conn;
	snprintf(rconn->host, MAXHOSTLEN - 1, "%s", host);
	rconn->port = port;

	/* add it to hash map */
	key = pstrdup(name);
	truncate_identifier(key, strlen(key), true);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash, key,
											   HASH_ENTER, &found);

	hentry->rconn = rconn;
	strlcpy(hentry->name, name, sizeof(hentry->name));
}


bool
connectionExists(const char *name)
{
	bool		found;
	char	   *key;

	if (!remoteConnHash)
	{
		remoteConnHash = createConnHash();
		return false;
	}

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), true);
	hash_search(remoteConnHash, key,
				HASH_FIND, &found);
	return found;
}


void
deleteConnection(const char *name)
{
	remoteConnHashEnt *hentry;
	bool		found;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), false);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash,
											   key, HASH_REMOVE, &found);

	if (!hentry)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("undefined connection name")));
}


char *
toMyDatabaseEncoding(const char *value)
{
	int		encoding = GetDatabaseEncoding();
	char   *encoded = NULL;

	if (encoding == PG_UTF8)
		return (char *) value;

	encoded = (char *) pg_do_encoding_conversion((unsigned char *) value,
												 strlen(value),
												 PG_UTF8,
												 encoding);
	return encoded;
}

char *
toUTF8Encoding(const char *value)
{
	int		encoding = GetDatabaseEncoding();
	char   *encoded = NULL;

	if ((encoding == PG_UTF8) || (pg_get_client_encoding() != PG_UTF8))
		return (char *) value;
	encoded = (char *) pg_do_encoding_conversion((unsigned char *) value,
												 strlen(value),
												 encoding,
												 PG_UTF8);
	return encoded;
}
