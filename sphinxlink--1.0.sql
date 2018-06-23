-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION sphinxlink" to load this file. \quit

CREATE FUNCTION sphinx_connect(text, text DEFAULT '127.0.0.1', int DEFAULT 9306, text DEFAULT NULL, text DEFAULT NULL, text DEFAULT NULL)
RETURNS text
AS 'MODULE_PATHNAME','sphinx_connect'
LANGUAGE C;

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
LANGUAGE C STRICT;

CREATE FUNCTION sphinx_meta(conname text, OUT varname text, OUT value text)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'sphinx_meta'
LANGUAGE C STRICT;
