-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION sphinxlink" to load this file. \quit

CREATE FUNCTION sphinx_connect(text, text DEFAULT '127.0.0.1', int DEFAULT 9306)
RETURNS text
AS 'MODULE_PATHNAME','sphinx_connect'
LANGUAGE C STRICT;

CREATE FUNCTION sphinx_disconnect(text)
RETURNS text
AS 'MODULE_PATHNAME', 'sphinx_disconnect'
LANGUAGE C STRICT;

CREATE FUNCTION sphinx_connections(OUT conname text, OUT host text, OUT port integer)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'sphinx_connections'
LANGUAGE C STRICT;

CREATE FUNCTION sphinx_query(text, text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'sphinx_query'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION sphinx_query(text, text, text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'sphinx_query'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION sphinx_query_params(text, integer, text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'sphinx_query'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION sphinx_query_params(text, integer, text, text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'sphinx_query'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION sphinx_meta(conname text)
RETURNS TABLE (varname text, value text)
AS
$$
BEGIN
  RETURN QUERY
    SELECT * FROM sphinx_query(conname, 'SHOW META') AS ss (varname text, value text);
END;
$$
LANGUAGE plpgsql;

CREATE FUNCTION sphinx_meta(host text, port integer)
RETURNS TABLE (varname text, value text)
AS
$$
BEGIN
  RETURN QUERY
    SELECT * FROM sphinx_query_params(host, port, 'SHOW META') AS ss (varname text, value text);
END;
$$
LANGUAGE plpgsql;
