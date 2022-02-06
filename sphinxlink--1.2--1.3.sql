-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION sphinxlink UPDATE TO '1.3'" to load this file. \quit

CREATE FUNCTION sphinx_query_params(text, int, text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'sphinx_query_params'
LANGUAGE C STRICT;
