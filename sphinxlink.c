#include <dlfcn.h>

#include "postgres.h"
#include "parser/scansup.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "funcapi.h"

#include "sphinxlink.h"

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


typedef struct remoteConnHashEnt
{
	char		name[NAMEDATALEN];
	remoteConn *rconn;
} remoteConnHashEnt;


typedef struct sphinx_meta_ctx
{
	MYSQL_RES	*res;
} sphinx_meta_ctx;


#define SPHINX_INIT \
do { \
	if (!pconn) \
	{ \
		pconn = (remoteConn *) MemoryContextAlloc(TopMemoryContext, sizeof(remoteConn)); \
		pconn->conn = NULL; \
		pconn->port = 0; \
		pconn->host[0] = '\0'; \
	} \
} while (0)


#define SPHINX_GETCONN \
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
static remoteConn *getConnectionByName(const char *name);
static HTAB *createConnHash(void);
static void createNewConnection(const char *name, remoteConn *rconn);
static bool connectionExists(const char *name);
static void deleteConnection(const char *name);
static char *toMyDatabaseEncoding(const char *value, bool *freed);
static char *toUTF8Encoding(const char *value, bool *freed);


/* Module variables declaration */
static remoteConn *pconn = NULL;
static HTAB *remoteConnHash = NULL;


PG_FUNCTION_INFO_V1(sphinx_connect);
Datum
sphinx_connect(PG_FUNCTION_ARGS)
{
	text	   *tconname = PG_GETARG_TEXT_PP(0);
	text	   *thost = PG_GETARG_TEXT_PP(1);
	text	   *tuser = PG_GETARG_TEXT_PP(3);
	text	   *tpassword = PG_GETARG_TEXT_PP(4);
	text	   *tdatabase = PG_GETARG_TEXT_PP(5);
	char	   *conname = NULL;
	char	   *host = NULL;
	char	   *user = NULL;
	char	   *password = NULL;
	char	   *database = NULL;
	int			port;
	MYSQL	   *conn = NULL;
	remoteConn *rconn = NULL;
	int			reconnect = 1;

	SPHINX_INIT;

	conname = text_to_cstring(tconname);
	host = text_to_cstring(thost);
	user = text_to_cstring(tuser);
	password = text_to_cstring(tpassword);
	database = text_to_cstring(tdatabase);
	port = PG_GETARG_INT32(2);

	PG_FREE_IF_COPY(tconname, 0);
	PG_FREE_IF_COPY(thost, 1);
	PG_FREE_IF_COPY(tuser, 3);
	PG_FREE_IF_COPY(tpassword, 3);
	PG_FREE_IF_COPY(tdatabase, 5);

	if (connectionExists(conname))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("duplicate connection name")));

	rconn = (remoteConn *) MemoryContextAlloc(TopMemoryContext,
											  sizeof(remoteConn));

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

	if (!mysql_real_connect(conn, host, user, password, database, port, NULL, 0))
	{
		char	   *msg;

		msg = pstrdup(mysql_error(conn));
		mysql_close(conn);
		safe_free(rconn, true);

		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("failed to connect to Sphinx: %s", msg)));
	}

	rconn->conn = conn;
	snprintf(rconn->host, MAXHOSTLEN - 1, "%s", host);
	rconn->port = port;
	createNewConnection(conname, rconn);

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

	SPHINX_INIT;
	SPHINX_GETCONN;

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
		tupdesc = CreateTemplateTupleDesc(3, false);
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
sphinx_query(PG_FUNCTION_ARGS)
{
	text			   *tconname = PG_GETARG_TEXT_PP(0);
	text			   *tquery = PG_GETARG_TEXT_PP(1);
	char			   *conname = NULL;
	remoteConn		   *rconn = NULL;
	MYSQL			   *conn = NULL;
	MYSQL_RES		   *res = NULL;
	char			   *query = NULL;
	int					ntuples;
	int					nfields;
	TupleDesc			tupdesc;
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bool				freed;
	char			   *encoded_query = NULL;

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

	SPHINX_INIT;
	SPHINX_GETCONN;
	query = text_to_cstring(tquery);
	encoded_query = toUTF8Encoding(query, &freed);

	PG_FREE_IF_COPY(tconname, 0);
	PG_FREE_IF_COPY(tquery, 1);

	if (mysql_query(conn, encoded_query) != 0)
	{
		char	   *err = pstrdup(mysql_error(conn));

		safe_free(encoded_query, freed);
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("Error when send query to Sphinx: %s", err)));
	}
	safe_free(encoded_query, freed);
	if ((res = mysql_store_result(conn)) == NULL)
	{
		char	    *err = pstrdup(mysql_error(conn));

		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("Error storing results: %s", err)));
	}
	ntuples = mysql_num_rows(res);
	nfields = mysql_num_fields(res);

	switch (get_call_result_type(fcinfo, NULL, &tupdesc))
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

	/*
	 * check result and tuple descriptor have the same number of columns
	 */
	if (nfields != tupdesc->natts)
	{
		if (!ntuples)
			PG_RETURN_NULL();

		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("remote query result rowtype does not match "
						"the specified FROM clause rowtype")));
	}

	if (ntuples > 0)
	{
		AttInMetadata	   *attinmeta;
		Tuplestorestate	   *tupstore;
		MYSQL_ROW			row;
		char			  **values;
		MemoryContext		oldcontext;

		attinmeta = TupleDescGetAttInMetadata(tupdesc);

		oldcontext = MemoryContextSwitchTo(
									rsinfo->econtext->ecxt_per_query_memory);
		tupstore = tuplestore_begin_heap(true, false, work_mem);
		rsinfo->setDesc = tupdesc;
		rsinfo->setResult = tupstore;
		MemoryContextSwitchTo(oldcontext);

		values = (char **) palloc(nfields * sizeof(char *));

		/* put all tuples into the tuplestore */
		while ((row = mysql_fetch_row(res)))
		{
			int			i;
			HeapTuple	tuple;

			for (i = 0; i < nfields; i++)
			{
				char *value = row[i];
				char *encvalue = NULL;

				if (value == NULL)
					values[i] = NULL;
				else
				{
					encvalue = toMyDatabaseEncoding(value, &freed);
					values[i] = pstrdup(encvalue);
				}
			}

			/* build the tuple and put it into the tuplestore. */
			tuple = BuildTupleFromCStrings(attinmeta, values);

			/* Free results */
			for (i = 0; i < nfields; i++)
				pfree(values[i]);

			tuplestore_puttuple(tupstore, tuple);
		}

		pfree(values);

		if (res)
			mysql_free_result(res);

		/* clean up and return the tuplestore */
		tuplestore_donestoring(tupstore);
	}
	return (Datum) 0;
}


PG_FUNCTION_INFO_V1(sphinx_meta);
Datum
sphinx_meta(PG_FUNCTION_ARGS)
{
	FuncCallContext	   *funcctx;
	int					call_cntr;
	int					max_calls;
	TupleDesc			tupdesc;
	AttInMetadata	   *attinmeta;
	text			   *tconname = PG_GETARG_TEXT_PP(0);
	char			   *conname = NULL;
	remoteConn		   *rconn = NULL;
	MYSQL			   *conn = NULL;
	sphinx_meta_ctx	   *fctx = NULL;

	SPHINX_INIT;
	SPHINX_GETCONN;

	PG_FREE_IF_COPY(tconname, 0);

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext	oldcontext;
		MYSQL_RES	   *res = NULL;
		int				ntuples = 0;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (mysql_query(conn, "SHOW META;") != 0)
		{
			char	   *err = pstrdup(mysql_error(conn));
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("Error when send query to Sphinx: %s", err)));
		}
		if ((res = mysql_store_result(conn)) == NULL)
		{
			char	    *err = pstrdup(mysql_error(conn));
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("Error storing results: %s", err)));
		}
		ntuples = mysql_num_rows(res);

		fctx = palloc(sizeof(sphinx_meta_ctx));
		fctx->res = res;

		funcctx->user_fctx = fctx;

		/* total number of tuples to be returned */
		funcctx->max_calls = ntuples;

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
		tupdesc = CreateTemplateTupleDesc(2, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "varname",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "value",
						   TEXTOID, -1, 0);

		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	attinmeta = funcctx->attinmeta;
	fctx = (sphinx_meta_ctx *) funcctx->user_fctx;

	if (call_cntr < max_calls)		/* do when there is more left to send */
	{
		char	  **values;
		HeapTuple	tuple;
		Datum		result;
		MYSQL_ROW	row;
		char	   *encvalue = NULL;
		bool		freed;

		if ((row = mysql_fetch_row(fctx->res)))
		{
			/*
			 * Prepare a values array for building the returned tuple.
			 * This should be an array of C strings which will
			 * be processed later by the type input functions.
			 */
			values = (char **) palloc(2 * sizeof(char *));

			encvalue = toMyDatabaseEncoding(row[0], &freed);
			values[0] = (char *) pstrdup(encvalue);
			safe_free(encvalue, freed);

			encvalue = toMyDatabaseEncoding(row[1], &freed);
			values[1] = (char *) pstrdup(encvalue);
			safe_free(encvalue, freed);

			tuple = BuildTupleFromCStrings(attinmeta, values);

			/* make the tuple into a datum */
			result = HeapTupleGetDatum(tuple);

			/* clean up (this is not really necessary) */
			pfree(values[0]);
			pfree(values[1]);
			pfree(values);

			PG_FREE_IF_COPY(conname, 0);

			SRF_RETURN_NEXT(funcctx, result);
		}
		SRF_RETURN_NEXT(funcctx, (Datum) 0);
	}
	else		/* do when there is no more left */
	{
		if (fctx->res)
			mysql_free_result(fctx->res);
		SRF_RETURN_DONE(funcctx);
	}
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
createNewConnection(const char *name, remoteConn *rconn)
{
	remoteConnHashEnt *hentry;
	bool		found;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

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
toMyDatabaseEncoding(const char *value, bool *freed)
{
	int		encoding = GetDatabaseEncoding();
	char   *encoded = NULL;

	if (encoding == PG_UTF8)
	{
		*freed = false;
		return (char *) value;
	}
	encoded = (char *) pg_do_encoding_conversion((unsigned char *)value,
												 strlen(value),
												 PG_UTF8,
												 encoding);
	*freed = true;
	return encoded;
}

char *
toUTF8Encoding(const char *value, bool *freed)
{
	int		encoding = GetDatabaseEncoding();
	char   *encoded = NULL;

	if ((encoding == PG_UTF8) || (pg_get_client_encoding() != PG_UTF8))
	{
		*freed = false;
		return (char *)value;
	}
	encoded = (char *) pg_do_encoding_conversion((unsigned char *)value,
												 strlen(value),
												 encoding,
												 PG_UTF8);
	*freed = true;
	return encoded;
}
