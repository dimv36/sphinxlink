#include "utils/rel.h"

#define list_length mysql_list_length
#define list_delete mysql_list_delete
#define list_free mysql_list_free

#include <mysql.h>
#undef list_length
#undef list_delete
#undef list_free

/* Declaration */
extern Datum sphinx_connect(PG_FUNCTION_ARGS);
extern Datum sphinx_disconnect(PG_FUNCTION_ARGS);
extern Datum sphinx_connections(PG_FUNCTION_ARGS);
extern Datum sphinx_query(PG_FUNCTION_ARGS);
extern Datum sphinx_meta(PG_FUNCTION_ARGS);
