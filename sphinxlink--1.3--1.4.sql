-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION sphinxlink UPDATE TO '1.4'" to load this file. \quit

DROP FUNCTION IF EXISTS sphinx_query(text, text);
DROP FUNCTION IF EXISTS sphinx_meta(conname text, OUT varname text, OUT value text);
DROP FUNCTION IF EXISTS sphinx_query_params(text, integer, text);

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

