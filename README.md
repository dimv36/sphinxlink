# sphinxlink

This is extension for PostgreSQL which allows you:
* Connect to SphinxSearch;
* Execute any search queries and get results as PostgreSQL table;
* Get Meta information after each Sphinx search query.

## Building and installing

The SphinxSearch client message protocol is the same as mysql, so, you need:
* mysql-client development package (or mariadb-client development package)
* postgresql development package

Typical installation procedure may look like this:
    
    $ git clone https://github.com/dimv36/sphinxlink.git
    $ cd sphinxlink
    $ make USE_PGXS=1
    $ sudo make USE_PGXS=1 install
    $ psql DB -c "CREATE EXTENSION sphinxlink;"

## HOWTO

The sphinxlink extensions provides those functions:

* sphinx_connect(conname text, host text DEFAULT '127.0.0.1', port integer DEFAULT 9306) - open connection to SphinxSearch with connection name conname;
* sphinx_query(conname text, query text) - execute query on SphinxSearch connection and get results as table in dblink style (see examples);
* sphinx_meta(conname text, OUT varname text, OUT value text) - get meta inforation about the last search query (it just executes SHOW META query on SphinxSearch and return table-based result);
* sphinx_connections(OUT conname text, OUT host text, OUT port integer) - get information about all opened connections in this session;
* sphinx_disconnect(conname text) - close connection to SphinxSearch by name conname.

## Examples

    > SELECT sphinx_connect('myconn', '192.168.1.1');
    > SELECT * FROM sphinx_query('myconn', 'SELECT weight(), mydata FROM myindex WHERE MATCH(''some&interesting'')') AS 
    (weight integer, mydata text);
    > SELECT * FROM sphinx_meta('myconn');
    > SELECT sphinx_disconnect('myconn');
    
