# sphinxlink

This is extension for PostgreSQL which allows you:
* Connect to SphinxSearch / ManticoreSearch;
* Execute any search queries and get results as PostgreSQL table;
* Get Meta information after each Sphinx search query.

## Building and installing

The SphinxSearch client message protocol is the same as mysql, so, you need:
* `mysql-client` development package (or `mariadb-client` development package)
* `postgresql` development package
* `gcc` compiler
* `make` utility

Typical installation procedure may look like this:
    
    $ git clone https://github.com/dimv36/sphinxlink.git
    $ cd sphinxlink
    $ make
    $ sudo USE_PGXS=1 make install
    $ psql DB -c "CREATE EXTENSION sphinxlink;"

## Functions

### Connection/Disconnection

For connection/disconnection to (from) SphinxSearch server use those functions:

    sphinx_connect(conname text, host text DEFAULT '127.0.0.1', port integer DEFAULT 9306)
    sphinx_disconnect(conname text)
    
e.g.:
 
    SELECT sphinx_connect('myconn', '192.168.1.1');
    SELECT sphinx_disconnect('myconn');
    
To get already opened connections use function `sphinx_connections()`:

    sphinx_connections(conname text, OUT host text, OUT integer port)
    
 e.g.:

    SELECT * FROM sphinx_connections();
    
### Execute queries and returning stat

If you have already opened connection, use function `sphinx_query`:

    sphinx_query(conname text, query text)
    
e.g.:

    SELECT docid FROM sphinx_query('myconn', 'SELECT docid FROM my_index WHERE MATCH(''Something&interesting'')' AS ss (docid integer);
    
If you want that extension automatically opens connection to SphinxSearch and execute query, use function `sphinx_query_params`:

    sphinx_query(host text, port integer, query text)

e.g.:

    SELECT docid FROM sphinx_query_params('192.168.1.1', 9306, 'SELECT docid FROM my_index WHERE MATCH(''Something&interesting'')' AS ss (docid integer);
 
 To get query stat, use functions `sphinx_meta()` and `sphinx_meta_params()`:
 
    sphinx_meta(conname text)
    sphinx_meta_params(host text, port integer)
    
Those function return execution stats of last query as `TABLE(varname text, value text)`
  
### Execute formatted queries

Extension also provides functions `sphinx_query()` and `sphinx_query_params()` with extra parameter `match_clause`:

    sphinx_query(conname text, query text, match_clause text)
    sphinx_query_params(host text, port integer, query text, match_clause text)
 
 Those functions have deal with parametrized queries in `query` argument, e.g.:
 
    SELECT docid FROM my_index WHERE MATCH(?)
    
 with `match_clause='Something&interesting'` will be executed as:
 
    SELECT docid FROM my_index WHERE MATCH('Something&interesting')
    
 Full example:
 
    SELECT * FROM sphinx_query('conn', 'SELECT docid FROM my_index WHERE MATCH(?)', 'Something&interesting') AS ss (docid integer);
    SELECT * FROM sphinx_query_params('127.0.0.1', 9306, 'SELECT docid FROM my_index WHERE MATCH(?)', 'Something&interesting') AS ss (docid integer);
    
## Authors
Dmitry Voronin <carriingfate92@yandex.ru>
