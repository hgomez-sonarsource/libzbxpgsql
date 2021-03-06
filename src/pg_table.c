/*
** libzbxpgsql - A PostgreSQL monitoring module for Zabbix
** Copyright (C) 2015 - Ryan Armstrong <ryan@cavaliercoder.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "libzbxpgsql.h"

// #define  PGSQL_DISCOVER_TABLES       "SELECT table_catalog, table_schema, table_name FROM information_schema.tables"

#define PGSQL_DISCOVER_TABLES       "\
SELECT \
    c.oid \
    , current_database() \
    , n.nspname \
    , CASE c.reltablespace \
    WHEN 0 THEN (SELECT ds.spcname FROM pg_tablespace ds JOIN pg_database d ON d.dattablespace = ds.oid WHERE d.datname = current_database()) \
    ELSE (SELECT spcname FROM pg_tablespace WHERE oid = c.reltablespace) \
    END \
    , c.relname \
    ,t.typname \
    ,a.rolname \
    , CASE c.relpersistence \
        WHEN 'p' THEN 'permenant' \
        WHEN 'u' THEN 'unlogged' \
        WHEN 't' THEN 'temporary' \
        ELSE 'Unknown' \
    END \
    , (SELECT COUNT(inhparent) FROM pg_inherits WHERE inhrelid = c.oid) \
FROM pg_class c \
JOIN pg_namespace n ON c.relnamespace = n.oid \
JOIN pg_type t ON c.reltype = t.oid \
JOIN pg_roles a ON c.relowner = a.oid \
WHERE c.relkind='r'"

#define PGSQL_DISCOVER_TABLE_CHILDREN   "SELECT c.oid , c.relname, n.nspname FROM pg_inherits i JOIN pg_class c ON i.inhrelid = c.oid JOIN pg_namespace n ON c.relnamespace = n.oid WHERE i.inhparent = '%s'::regclass"

#define PGSQL_GET_TABLE_STAT_SUM    "SELECT SUM(%s) FROM pg_stat_all_tables"

#define PGSQL_GET_TABLE_STAT        "SELECT %s FROM pg_stat_all_tables WHERE relname = '%s'"

#define PGSQL_GET_TABLE_STATIO      "SELECT %s FROM pg_statio_all_tables WHERE relname = '%s'"

#define PGSQL_GET_TABLE_STATIO_SUM  "SELECT SUM(%s) FROM pg_statio_all_tables"

#define PGSQL_GET_TABLE_SIZE        "SELECT (CAST(relpages AS bigint) * 8192) FROM pg_class WHERE relkind='r' AND relname = '%s'"

#define PGSQL_GET_TABLE_SIZE_SUM    "SELECT (SUM(relpages) * 8192) FROM pg_class WHERE relkind='r'"

#define PGSQL_GET_TABLE_ROWS_SUM    "SELECT SUM(reltuples) FROM pg_class WHERE relkind='r'"

#define PGSQL_GET_TABLE_ROWS        "SELECT reltuples FROM pg_class WHERE relkind='r' AND relname = '%s'"

#define PGSQL_GET_TABLE_CHILD_COUNT "SELECT COUNT(i.inhrelid) FROM pg_inherits i WHERE i.inhparent = '%s'::regclass"

#define PGSQL_GET_CHILDREN_SIZE     "SELECT (SUM(c.relpages) * 8192) FROM pg_inherits i JOIN pg_class c ON inhrelid = c.oid WHERE i.inhparent = '%s'::regclass"

#define PGSQL_GET_CHILDREN_ROWS     "SELECT SUM(c.reltuples) FROM pg_inherits i JOIN pg_class c ON inhrelid = c.oid WHERE i.inhparent = '%s'::regclass"

/*
 * Custom key pg.table.discovery
 *
 * Returns all known Tables in a PostgreSQL database
 *
 * Parameter [0-4]:     <host,port,db,user,passwd>
 * *
 * Returns:
 * {
 *        "data":[
 *                {
 *                        "{#DATABASE}":"MyDatabase",
 *                        "{#SCHEMA}":"public",
 *                        "{#TABLESPACE}":"pg_default",
 *                        "{#TABLE}":"MyTable",
 *                        "{#TYPE}":"MyTable",
 *                        "{#OWNER}":"postgres",
 *                        "{#PERSISTENCE":"permenant|temporary",
 *                        "{#ISSUBCLASS}":"0"}]}
 */
int    PG_TABLE_DISCOVERY(AGENT_REQUEST *request, AGENT_RESULT *result)
{
    int         ret = SYSINFO_RET_FAIL;                     // Request result code
    const char  *__function_name = "PG_TABLE_DISCOVERY";    // Function name for log file
    struct      zbx_json j;                                 // JSON response for discovery rule
    
    PGconn      *conn = NULL;
    PGresult    *res = NULL;
    
    char        query[MAX_STRING_LEN] = PGSQL_DISCOVER_TABLES;
    int         i = 0, count = 0;
    
    zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);
    
    // Connect to PostreSQL
    if(NULL == (conn = pg_connect(request)))
        goto out;
    
    // Execute a query
    res = PQexec(conn, query);
    if(PQresultStatus(res) != PGRES_TUPLES_OK) {
        zabbix_log(LOG_LEVEL_ERR, "Failed to execute PostgreSQL query in %s() with: %s", __function_name, PQresultErrorMessage(res));
        goto out;
    }
    
    if(0 == (count = PQntuples(res))) {
        zabbix_log(LOG_LEVEL_DEBUG, "No results returned for query \"%s\" in %s()", query, __function_name);
    }
             
    // Create JSON array of discovered objects
    zbx_json_init(&j, ZBX_JSON_STAT_BUF_LEN);
    zbx_json_addarray(&j, ZBX_PROTO_TAG_DATA);
    
    for(i = 0; i < count; i++) {
        zbx_json_addobject(&j, NULL);        
        zbx_json_addstring(&j, "{#OID}", PQgetvalue(res, i, 0), ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(&j, "{#DATABASE}", PQgetvalue(res, i, 1), ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(&j, "{#SCHEMA}", PQgetvalue(res, i, 2), ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(&j, "{#TABLESPACE}", PQgetvalue(res, i, 3), ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(&j, "{#TABLE}", PQgetvalue(res, i, 4), ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(&j, "{#TYPE}", PQgetvalue(res, i, 5), ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(&j, "{#OWNER}", PQgetvalue(res, i, 6), ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(&j, "{#PERSISTENCE}", PQgetvalue(res, i, 7), ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(&j, "{#ISSUBCLASS}", PQgetvalue(res, i, 8), ZBX_JSON_TYPE_STRING);
        zbx_json_close(&j);         
    }
    
    // Finalize JSON response
    zbx_json_close(&j);
    SET_STR_RESULT(result, strdup(j.buffer));
    zbx_json_free(&j);
    ret = SYSINFO_RET_OK;
        
out:
    PQclear(res);
    PQfinish(conn);
    
    zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
    return ret;
}

/*
 * Custom key pg.table.children.discovery
 *
 * Returns all known child tables for the specified parent table
 *
 * Parameter [0-4]:     <host,port,db,user,passwd>
 *
 * Parameter[table]:    Parent table
 * 
 * Returns:
 * {
 *        "data":[
 *                {
 *                        "{#OID}":"12345",
 *                        "{#SCHEMA}":"public",
 *                        "{#TABLE}":"MyTable"}]}
 */
int    PG_TABLE_CHILDREN_DISCOVERY(AGENT_REQUEST *request, AGENT_RESULT *result)
{
    int         ret = SYSINFO_RET_FAIL;                             // Request result code
    const char  *__function_name = "PG_TABLE_CHILDREN_DISCOVERY";   // Function name for log file
    struct      zbx_json j;                                         // JSON response for discovery rule
    
    PGconn      *conn = NULL;
    PGresult    *res = NULL;

    char        *tablename = NULL;
    
    char        query[MAX_STRING_LEN];
    int         i = 0, count = 0;
    
    zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);
    
    // Parse parameters
    tablename = get_rparam(request, PARAM_FIRST);
    if(NULL == tablename || '\0' == *tablename) {
        zabbix_log(LOG_LEVEL_ERR, "No table name specified in %s()", __function_name);
        goto out;
    }
    
    // Build query
    zbx_snprintf(query, sizeof(query), PGSQL_DISCOVER_TABLE_CHILDREN, tablename);
    
    // Connect to PostreSQL
    if(NULL == (conn = pg_connect(request)))
        goto out;
    
    // Execute a query
    res = PQexec(conn, query);
    if(PQresultStatus(res) != PGRES_TUPLES_OK) {
        zabbix_log(LOG_LEVEL_ERR, "Failed to execute PostgreSQL query in %s() with: %s", __function_name, PQresultErrorMessage(res));
        goto out;
    }
    
    if(0 == (count = PQntuples(res))) {
        zabbix_log(LOG_LEVEL_DEBUG, "No results returned for query \"%s\" in %s()", query, __function_name);
    }
             
    // Create JSON array of discovered objects
    zbx_json_init(&j, ZBX_JSON_STAT_BUF_LEN);
    zbx_json_addarray(&j, ZBX_PROTO_TAG_DATA);
    
    for(i = 0; i < count; i++) {
        zbx_json_addobject(&j, NULL);        
        zbx_json_addstring(&j, "{#OID}", PQgetvalue(res, i, 0), ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(&j, "{#TABLE}", PQgetvalue(res, i, 1), ZBX_JSON_TYPE_STRING);
        zbx_json_addstring(&j, "{#SCHEMA}", PQgetvalue(res, i, 2), ZBX_JSON_TYPE_STRING);
        zbx_json_close(&j);         
    }
    
    // Finalize JSON response
    zbx_json_close(&j);
    SET_STR_RESULT(result, strdup(j.buffer));
    zbx_json_free(&j);
    ret = SYSINFO_RET_OK;
        
out:
    PQclear(res);
    PQfinish(conn);
    
    zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
    return ret;
}

/*
 * Custom keys pg.table.* (for each field in pg_stat_all_tables)
 *
 * Returns the requested statistic for the specified data table
 *
 * Parameter [0-4]:     <host,port,db,user,passwd>
 *
 * Parameter[table]:    table name to assess (default: all)
 *
 * Returns: u
 */
int    PG_STAT_ALL_TABLES(AGENT_REQUEST *request, AGENT_RESULT *result)
{
    int         ret = SYSINFO_RET_FAIL;                     // Request result code
    const char  *__function_name = "PG_STAT_ALL_TABLES";    // Function name for log file
    
    char        *tablename = NULL;
    char        *field;
    char        query[MAX_STRING_LEN];
    
    zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);
    
    // Get stat field from requested key name "pb.table.<field>"
    field = &request->key[9];
    
    // Build query
    tablename = get_rparam(request, PARAM_FIRST);
    if(NULL == tablename || '\0' == *tablename) {   
        zbx_snprintf(query, sizeof(query), PGSQL_GET_TABLE_STAT_SUM, field);
    }
    else {
        zbx_snprintf(query, sizeof(query), PGSQL_GET_TABLE_STAT, field, tablename);
    }
    
    // Set result
    if(0 == strncmp(field, "last_", 5)) {
        if(NULL == tablename || '\0' == *tablename) {
            // Can't do SUMs on text fields!
            zabbix_log(LOG_LEVEL_ERR, "No table specified bro, in %s", __function_name);
            goto out;
        }
    
        ret = pg_get_string(request, result, query);
    }
    else {
        ret = pg_get_int(request, result, query);
    }
    
out:
    zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
    return ret;
}

/*
 * Custom keys pg.table.* (for each field in pg_statio_all_tables)
 *
 * Returns the requested IO statistic for the specified data table
 *
 * Parameter [0-4]:     <host,port,db,user,passwd>
 *
 * Parameter[table]:    table name to assess (default: all)
 *
 * Returns: u
 */
int    PG_STATIO_ALL_TABLES(AGENT_REQUEST *request, AGENT_RESULT *result)
{
    int         ret = SYSINFO_RET_FAIL;                     // Request result code
    const char  *__function_name = "PG_STATIO_ALL_TABLES";  // Function name for log file
    
    char        *tablename = NULL;
    
    char        *field;
    char        query[MAX_STRING_LEN];
    
    zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);
    
    // Get stat field from requested key name "pb.table.<field>"
    field = &request->key[9];
    
    // Build query
    tablename = get_rparam(request, PARAM_FIRST);
    if(NULL == tablename || '\0' == *tablename)
        zbx_snprintf(query, sizeof(query), PGSQL_GET_TABLE_STATIO_SUM, field);
    else
        zbx_snprintf(query, sizeof(query), PGSQL_GET_TABLE_STATIO, field, tablename);

    ret = pg_get_int(request, result, query);
    
    zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
    return ret;
}

/*
 * Custom key pg.table.size
 *
 * Returns the disk usage in bytes for the specified data table
 *
 * Parameter [0-4]:     <host,port,db,user,passwd>
 *
 * Parameter[table]:    table name to assess (default: all)
 *
 * Parameter[include]:  table (default) | children | all
 *
 * Returns: u
 */
int    PG_TABLE_SIZE(AGENT_REQUEST *request, AGENT_RESULT *result)
{
    int         ret = SYSINFO_RET_FAIL;             // Request result code
    const char  *__function_name = "PG_TABLE_SIZE"; // Function name for log file
        
    char        query[MAX_STRING_LEN];
    char        *tablename = NULL; //, *include = NULL;
            
    zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);
    
    // Parse parameters
    tablename = get_rparam(request, PARAM_FIRST);
    // include = get_rparam(request, PARAM_FIRST + 1);
    
    // Build query
    if(NULL == tablename || '\0' == *tablename)
        zbx_snprintf(query, sizeof(query), PGSQL_GET_TABLE_SIZE_SUM);
    else
        zbx_snprintf(query, sizeof(query), PGSQL_GET_TABLE_SIZE, tablename);

    ret = pg_get_int(request, result, query);
    
    zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
    return ret;
}

/*
 * Custom key pg.table.rows
 *
 * Returns the estimated row count for the specified class (table, index, etc.)
 *
 * See: http://www.postgresql.org/docs/9.3/static/catalog-pg-class.html
 *      https://wiki.postgresql.org/wiki/Disk_Usage
 *
 * Parameter [0-4]:     <host,port,db,user,passwd>
 * 
 * Parameter[table]:    table name to assess (default: all)
 *
 * Returns: u
 */
int    PG_TABLE_ROWS(AGENT_REQUEST *request, AGENT_RESULT *result)
{
    int             ret = SYSINFO_RET_FAIL;             // Request result code
    const char          *__function_name = "PG_TABLE_ROWS"; // Function name for log file
        
    char                *tablename = NULL;
    char        query[MAX_STRING_LEN];
    
    zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);
    
    tablename = get_rparam(request, PARAM_FIRST);
    
    if(NULL == tablename || '\0' == *tablename)
    zbx_snprintf(query, sizeof(query), PGSQL_GET_TABLE_ROWS_SUM);
    else
    zbx_snprintf(query, sizeof(query), PGSQL_GET_TABLE_ROWS, tablename);

    ret = pg_get_int(request, result, query);
    
    zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
    return ret;
}

/*
 * Custom key pg.table.children
 *
 * Returns the number of tables that inherit from the specified table
 *
 * Parameter [0-4]:     <host,port,db,user,passwd>
 * 
 * Parameter[table]:    table name to assess (required)
 *
 * Returns: u
 */
int    PG_TABLE_CHILDREN(AGENT_REQUEST *request, AGENT_RESULT *result)
{
    int             ret = SYSINFO_RET_FAIL;                 // Request result code
    const char          *__function_name = "PG_TABLE_CHILDREN";     // Function name for log file
        
    char                *tablename = NULL;
    char        query[MAX_STRING_LEN];
    
    zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);
    
    tablename = get_rparam(request, PARAM_FIRST);
    if(NULL == tablename || '\0' == *tablename) {
    zabbix_log(LOG_LEVEL_ERR, "Invalid parameter count in %s(). Please specify a table name.", __function_name);
        goto out;
    }
    else
    zbx_snprintf(query, sizeof(query), PGSQL_GET_TABLE_CHILD_COUNT, tablename);

    ret = pg_get_int(request, result, query);
    
out:
    zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
    return ret;
}

/*
 * Custom key pg.table.children.size
 *
 * Returns the sum size in bytes of all tables that inherit from the specified table
 *
 * Parameter [0-4]:     <host,port,db,user,passwd>
 * 
 * Parameter[table]:    table name to assess (required)
 *
 * Returns: u
 */
int    PG_TABLE_CHILDREN_SIZE(AGENT_REQUEST *request, AGENT_RESULT *result)
{
    int             ret = SYSINFO_RET_FAIL;                 // Request result code
    const char          *__function_name = "PG_TABLE_CHILDREN_SIZE";    // Function name for log file
        
    char                *tablename = NULL;
    char        query[MAX_STRING_LEN];
    
    zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);
    
    tablename = get_rparam(request, PARAM_FIRST);
    if(NULL == tablename || '\0' == *tablename) {
    zabbix_log(LOG_LEVEL_ERR, "Invalid parameter count in %s(). Please specify a table name.", __function_name);
        goto out;
    }
    else
    zbx_snprintf(query, sizeof(query), PGSQL_GET_CHILDREN_SIZE, tablename);

    ret = pg_get_int(request, result, query);
    
out:
    zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
    return ret;
}

/*
 * Custom key pg.table.children.size
 *
 * Returns the sum size in bytes of all tables that inherit from the specified table
 *
 * Parameter [0-4]:     <host,port,db,user,passwd>
 * 
 * Parameter[table]:    table name to assess (required)
 *
 * Returns: u
 */
int    PG_TABLE_CHILDREN_TUPLES(AGENT_REQUEST *request, AGENT_RESULT *result)
{
    int             ret = SYSINFO_RET_FAIL;                 // Request result code
    const char          *__function_name = "PG_TABLE_CHILDREN_TUPLES";  // Function name for log file
        
    char                *tablename = NULL;
    char        query[MAX_STRING_LEN];
    
    zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);
    
    tablename = get_rparam(request, PARAM_FIRST);
    if(NULL == tablename || '\0' == *tablename) {
    zabbix_log(LOG_LEVEL_ERR, "Invalid parameter count in %s(). Please specify a table name.", __function_name);
        goto out;
    }
    else
    zbx_snprintf(query, sizeof(query), PGSQL_GET_CHILDREN_ROWS, tablename);

    ret = pg_get_int(request, result, query);
    
out:
    zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
    return ret;
}
