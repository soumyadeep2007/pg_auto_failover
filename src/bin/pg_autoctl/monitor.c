/*
 * src/bin/pg_autoctl/monitor.c
 *	 API for interacting with the monitor
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */
#include <inttypes.h>
#include <limits.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "defaults.h"
#include "env_utils.h"
#include "log.h"
#include "monitor.h"
#include "monitor_config.h"
#include "parsing.h"
#include "pgsql.h"
#include "signals.h"
#include "string_utils.h"

#define STR_ERRCODE_OBJECT_IN_USE "55006"
#define STR_ERRCODE_EXCLUSION_VIOLATION "23P01"

#define STR_ERRCODE_SERIALIZATION_FAILURE "40001"
#define STR_ERRCODE_STATEMENT_COMPLETION_UNKNOWN "40003"
#define STR_ERRCODE_DEADLOCK_DETECTED "40P01"

#define STR_ERRCODE_CLASS_INSUFFICIENT_RESOURCES "53"
#define STR_ERRCODE_CLASS_PROGRAM_LIMIT_EXCEEDED "54"

typedef struct NodeAddressParseContext
{
	char sqlstate[SQLSTATE_LENGTH];
	NodeAddress *node;
	bool parsedOK;
} NodeAddressParseContext;

typedef struct NodeAddressArrayParseContext
{
	char sqlstate[SQLSTATE_LENGTH];
	NodeAddressArray *nodesArray;
	bool parsedOK;
} NodeAddressArrayParseContext;

typedef struct MonitorAssignedStateParseContext
{
	char sqlstate[SQLSTATE_LENGTH];
	MonitorAssignedState *assignedState;
	bool parsedOK;
} MonitorAssignedStateParseContext;

typedef struct NodeReplicationSettingsParseContext
{
	char sqlstate[SQLSTATE_LENGTH];
	int candidatePriority;
	bool replicationQuorum;
	bool parsedOK;
} NodeReplicationSettingsParseContext;

/* either "monitor" or "formation" */
#define CONNTYPE_LENGTH 10

typedef struct FormationURIParseContext
{
	char sqlstate[SQLSTATE_LENGTH];
	char connType[CONNTYPE_LENGTH];
	char connName[BUFSIZE];
	char connURI[BUFSIZE];
	bool parsedOK;
} FormationURIParseContext;

typedef struct MonitorExtensionVersionParseContext
{
	char sqlstate[SQLSTATE_LENGTH];
	MonitorExtensionVersion *version;
	bool parsedOK;
} MonitorExtensionVersionParseContext;

static int MaxHostNameSizeInNodesArray(NodeAddressArray *nodesArray);
static void prepareHostNameSeparator(char nameSeparatorHeader[],
									 int maxHostNameSize);

static bool parseNode(PGresult *result, int rowNumber, NodeAddress *node);
static void parseNodeResult(void *ctx, PGresult *result);
static void parseNodeArray(void *ctx, PGresult *result);
static void parseNodeState(void *ctx, PGresult *result);
static void parseNodeReplicationSettings(void *ctx, PGresult *result);
static void printCurrentState(void *ctx, PGresult *result);
static void printLastEvents(void *ctx, PGresult *result);
static void printFormationURI(void *ctx, PGresult *result);
static void parseCoordinatorNode(void *ctx, PGresult *result);
static void parseExtensionVersion(void *ctx, PGresult *result);

static bool prepare_connection_to_current_system_user(Monitor *source,
													  Monitor *target);

/*
 * monitor_init initializes a Monitor struct to connect to the given
 * database URL.
 */
bool
monitor_init(Monitor *monitor, char *url)
{
	log_trace("monitor_init: %s", url);

	if (!pgsql_init(&monitor->pgsql, url, PGSQL_CONN_MONITOR))
	{
		/* URL must be invalid, pgsql_init logged an error */
		return false;
	}

	return true;
}


/*
 * monitor_local_init initializes a Monitor struct to connect to the local
 * monitor postgres instance, for use from the pg_autoctl instance that manages
 * the monitor.
 */
bool
monitor_local_init(Monitor *monitor)
{
	MonitorConfig *mconfig = &(monitor->config);
	PostgresSetup *pgSetup = &(mconfig->pgSetup);
	char connInfo[MAXCONNINFO] = { 0 };

	pg_setup_get_local_connection_string(pgSetup, connInfo);

	if (!pgsql_init(&monitor->pgsql, connInfo, PGSQL_CONN_LOCAL))
	{
		/* URL must be invalid, pgsql_init logged an error */
		return false;
	}

	return true;
}


/*
 * monitor_retryable_error returns true when we may retry our query. That's
 * mostly useful to CLI entry points such as pg_autoctl enable|disable
 * maintenance where it's better if we can retry in those rare cases.
 */
bool
monitor_retryable_error(const char *sqlstate)
{
	if (strcmp(sqlstate, STR_ERRCODE_SERIALIZATION_FAILURE) == 0)
	{
		return true;
	}

	if (strcmp(sqlstate, STR_ERRCODE_STATEMENT_COMPLETION_UNKNOWN) == 0)
	{
		return true;
	}

	if (strcmp(sqlstate, STR_ERRCODE_DEADLOCK_DETECTED) == 0)
	{
		return true;
	}

	if (strncmp(sqlstate, STR_ERRCODE_CLASS_INSUFFICIENT_RESOURCES, 2) == 0)
	{
		return true;
	}

	if (strncmp(sqlstate, STR_ERRCODE_CLASS_PROGRAM_LIMIT_EXCEEDED, 2) == 0)
	{
		return true;
	}

	return false;
}


/*
 * monitor_get_nodes gets the hostname and port of all the nodes in the given
 * group.
 */
bool
monitor_get_nodes(Monitor *monitor, char *formation, int groupId,
				  NodeAddressArray *nodeArray)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		groupId == -1
		? "SELECT * FROM pgautofailover.get_nodes($1) ORDER BY node_id"
		: "SELECT * FROM pgautofailover.get_nodes($1, $2) ORDER BY node_id";
	int paramCount = 1;
	Oid paramTypes[2] = { TEXTOID, INT4OID };
	const char *paramValues[2] = { 0 };
	NodeAddressArrayParseContext parseContext = { { 0 }, nodeArray, false };

	paramValues[0] = formation;

	if (groupId > -1)
	{
		IntString myGroupIdString = intToString(groupId);

		++paramCount;
		paramValues[1] = myGroupIdString.strValue;
	}

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, parseNodeArray))
	{
		log_error("Failed to get other nodes from the monitor while running "
				  "\"%s\" with formation %s and group %d",
				  sql, formation, groupId);
		return false;
	}

	if (!parseContext.parsedOK)
	{
		log_error("Failed to get the other nodes from the monitor while "
				  "running \"%s\" with formation %s and group %d because "
				  "it returned an unexpected result. "
				  "See previous line for details.",
				  sql, formation, groupId);
		return false;
	}

	return true;
}


/*
 * monitor_print_nodes gets all the nodes in the given group and prints them
 * out to stdout in a human-friendly tabular format.
 */
bool
monitor_print_nodes(Monitor *monitor, char *formation, int groupId)
{
	NodeAddressArray nodesArray;

	if (!monitor_get_nodes(monitor, formation, groupId, &nodesArray))
	{
		/* errors have already been logged */
		return false;
	}

	(void) printNodeArray(&nodesArray);

	return true;
}


/*
 * monitor_get_other_nodes_as_json gets the hostname and port of the other node
 * in the group and prints them out in JSON format.
 */
bool
monitor_print_nodes_as_json(Monitor *monitor, char *formation, int groupId)
{
	PGSQL *pgsql = &monitor->pgsql;
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_STRING, false };

	const char *sql =
		groupId == -1
		?
		"SELECT jsonb_pretty(coalesce(jsonb_agg(row_to_json(nodes)), '[]'))"
		"  FROM pgautofailover.get_nodes($1) as nodes"
		:
		"SELECT jsonb_pretty(coalesce(jsonb_agg(row_to_json(nodes)), '[]'))"
		"  FROM pgautofailover.get_nodes($1, $2) as nodes";

	int paramCount = 1;
	Oid paramTypes[2] = { TEXTOID, INT4OID };
	const char *paramValues[2] = { 0 };

	paramValues[0] = formation;

	if (groupId > -1)
	{
		IntString myGroupIdString = intToString(groupId);

		++paramCount;
		paramValues[1] = myGroupIdString.strValue;
	}

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		log_error("Failed to get the nodes from the monitor while running "
				  "\"%s\" with formation %s and group %d",
				  sql, formation, groupId);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!context.parsedOk)
	{
		log_error("Failed to get the other nodes from the monitor while "
				  "running \"%s\" with formation %s and group %d because "
				  "it returned an unexpected result. "
				  "See previous line for details.",
				  sql, formation, groupId);
		return false;
	}

	fformat(stdout, "%s\n", context.strVal);

	return true;
}


/*
 * monitor_get_other_nodes gets the hostname and port of the other node in the
 * group.
 */
bool
monitor_get_other_nodes(Monitor *monitor,
						int myNodeId,
						NodeState currentState,
						NodeAddressArray *nodeArray)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		currentState == ANY_STATE
		?
		"SELECT * FROM pgautofailover.get_other_nodes($1) "
		"ORDER BY node_id"
		:
		"SELECT * FROM pgautofailover.get_other_nodes($1, "
		"$2::pgautofailover.replication_state) "
		"ORDER BY node_id";

	int paramCount = currentState == ANY_STATE ? 1 : 2;
	Oid paramTypes[2] = { INT4OID, TEXTOID };
	const char *paramValues[3] = { 0 };

	NodeAddressArrayParseContext parseContext = { { 0 }, nodeArray, false };

	IntString myNodeIdString = intToString(myNodeId);

	paramValues[0] = myNodeIdString.strValue;

	if (currentState != ANY_STATE)
	{
		paramValues[1] = NodeStateToString(currentState);
	}

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, parseNodeArray))
	{
		log_error("Failed to get other nodes from the monitor while running "
				  "\"%s\" with node id %d", sql, myNodeId);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!parseContext.parsedOK)
	{
		log_error("Failed to get the other nodes from the monitor while running "
				  "\"%s\" with node id %d because it returned an "
				  "unexpected result. See previous line for details.",
				  sql, myNodeId);
		return false;
	}

	return true;
}


/*
 * monitor_print_other_nodes gets the other nodes from the monitor and then
 * prints them to stdout in a human-friendly tabular format.
 */
bool
monitor_print_other_nodes(Monitor *monitor,
						  int myNodeId, NodeState currentState)
{
	NodeAddressArray otherNodesArray;

	if (!monitor_get_other_nodes(monitor, myNodeId, currentState,
								 &otherNodesArray))
	{
		/* errors have already been logged */
		return false;
	}

	(void) printNodeArray(&otherNodesArray);

	return true;
}


/*
 * monitor_print_other_node_as_json gets the hostname and port of the other
 * node in the group as a JSON string and prints it to given stream.
 */
bool
monitor_print_other_nodes_as_json(Monitor *monitor,
								  int myNodeId,
								  NodeState currentState)
{
	PGSQL *pgsql = &monitor->pgsql;
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_STRING, false };

	const char *sql =
		currentState == ANY_STATE
		?
		"SELECT jsonb_pretty(coalesce(jsonb_agg(row_to_json(nodes)), '[]'))"
		" FROM pgautofailover.get_other_nodes($1) as nodes"
		:
		"SELECT jsonb_pretty(coalesce(jsonb_agg(row_to_json(nodes)), '[]'))"
		" FROM pgautofailover.get_other_nodes($1, "
		"$3::pgautofailover.replication_state) as nodes";

	int paramCount = currentState == ANY_STATE ? 2 : 1;
	Oid paramTypes[2] = { INT4OID, TEXTOID };
	const char *paramValues[2] = { 0 };
	IntString myNodeIdString = intToString(myNodeId);

	paramValues[0] = myNodeIdString.strValue;

	if (currentState != ANY_STATE)
	{
		paramValues[1] = NodeStateToString(currentState);
	}

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		log_error("Failed to get the other nodes from the monitor while running "
				  "\"%s\" with node id %d", sql, myNodeId);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!context.parsedOk)
	{
		log_error("Failed to get the other nodes from the monitor while running "
				  "\"%s\" with node id %d because it returned an "
				  "unexpected result. See previous line for details.",
				  sql, myNodeId);
		return false;
	}

	fformat(stdout, "%s\n", context.strVal);

	return true;
}


/*
 * monitor_get_primary gets the primary node in a give formation and group.
 */
bool
monitor_get_primary(Monitor *monitor, char *formation, int groupId,
					NodeAddress *node)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql = "SELECT * FROM pgautofailover.get_primary($1, $2)";
	int paramCount = 2;
	Oid paramTypes[2] = { TEXTOID, INT4OID };
	const char *paramValues[2];
	NodeAddressParseContext parseContext = { { 0 }, node, false };
	IntString groupIdString = intToString(groupId);

	paramValues[0] = formation;
	paramValues[1] = groupIdString.strValue;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, parseNodeResult))
	{
		log_error(
			"Failed to get the primary node in the HA group from the monitor "
			"while running \"%s\" with formation \"%s\" and group ID %d",
			sql, formation, groupId);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!parseContext.parsedOK)
	{
		log_error(
			"Failed to get the primary node from the monitor while running "
			"\"%s\" with formation \"%s\" and group ID %d because it returned an "
			"unexpected result. See previous line for details.",
			sql, formation, groupId);
		return false;
	}

	/* The monitor function pgautofailover.get_primary only returns 3 fields */
	node->isPrimary = true;

	log_debug("The primary node returned by the monitor is %s:%d, with id %d",
			  node->host, node->port, node->nodeId);

	return true;
}


/*
 * monitor_get_coordinator gets the coordinator node in a given formation.
 */
bool
monitor_get_coordinator(Monitor *monitor, char *formation, NodeAddress *node)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql = "SELECT * FROM pgautofailover.get_coordinator($1)";
	int paramCount = 1;
	Oid paramTypes[1] = { TEXTOID };
	const char *paramValues[1];
	NodeAddressParseContext parseContext = { { 0 }, node, false };

	paramValues[0] = formation;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, parseCoordinatorNode))
	{
		log_error("Failed to get the coordinator node from the monitor, "
				  "while running \"%s\" with formation \"%s\".",
				  sql, formation);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!parseContext.parsedOK)
	{
		log_error("Failed to get the coordinator node from the monitor "
				  "while running \"%s\" with formation \"%s\" "
				  "because it returned an unexpected result. "
				  "See previous line for details.",
				  sql, formation);
		return false;
	}

	if (parseContext.node == NULL)
	{
		log_error("Failed to get the coordinator node from the monitor: "
				  "the monitor returned an empty result set, there's no "
				  "known available coordinator node at this time in "
				  "formation \"%s\"", formation);
		return false;
	}

	log_debug("The coordinator node returned by the monitor is %s:%d",
			  node->host, node->port);

	return true;
}


/*
 * monitor_get_most_advanced_standby finds the standby node in state REPORT_LSN
 * with the most advanced LSN position.
 */
bool
monitor_get_most_advanced_standby(Monitor *monitor,
								  char *formation, int groupId,
								  NodeAddress *node)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT * FROM pgautofailover.get_most_advanced_standby($1, $2)";
	int paramCount = 2;
	Oid paramTypes[2] = { TEXTOID, INT4OID };
	const char *paramValues[2];

	/* we expect a single entry */
	NodeAddressArray nodeArray = { 0 };
	NodeAddressArrayParseContext parseContext = { { 0 }, &nodeArray, false };

	IntString groupIdString = intToString(groupId);

	paramValues[0] = formation;
	paramValues[1] = groupIdString.strValue;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, parseNodeArray))
	{
		log_error(
			"Failed to get most advanced standby node in the HA group "
			"from the monitor while running \"%s\" with "
			"formation \"%s\" and group ID %d",
			sql, formation, groupId);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!parseContext.parsedOK || nodeArray.count != 1)
	{
		log_error(
			"Failed to get the most advanced standby node from the monitor "
			"while running \"%s\" with formation \"%s\" and group ID %d "
			"because it returned an unexpected result. "
			"See previous line for details.",
			sql, formation, groupId);
		return false;
	}

	/* copy the node we retrieved in the expected place */
	node->nodeId = nodeArray.nodes[0].nodeId;
	strlcpy(node->name, nodeArray.nodes[0].name, _POSIX_HOST_NAME_MAX);
	strlcpy(node->host, nodeArray.nodes[0].host, _POSIX_HOST_NAME_MAX);
	node->port = nodeArray.nodes[0].port;
	strlcpy(node->lsn, nodeArray.nodes[0].lsn, PG_LSN_MAXLENGTH);
	node->isPrimary = nodeArray.nodes[0].isPrimary;

	log_debug("The most advanced standby node is node %d (%s:%d)",
			  node->nodeId, node->host, node->port);

	return true;
}


/*
 * monitor_register_node performs the initial registration of a node with the
 * monitor in the given formation.
 *
 * The caller can specify a desired group ID, which will result in the node
 * being added to the group unless it is already full. If the groupId is -1,
 * the monitor will pick a group.
 *
 * The initialState can be used to indicate that the operator wants to
 * initialize the node in a specific state directly. This can be useful to add
 * a standby to an already running primary node, doing the pg_basebackup
 * directly.
 *
 * The initialState can also be used to indicate that the node is already
 * correctly initialized in a particular state. This can be useful when
 * bringing back a keeper after replacing the monitor.
 *
 * The node ID and group ID selected by the monitor, as well as the goal
 * state, are set in assignedState, which must not be NULL.
 */
bool
monitor_register_node(Monitor *monitor, char *formation,
					  char *name, char *host, int port,
					  uint64_t system_identifier,
					  char *dbname, int desiredGroupId, NodeState initialState,
					  PgInstanceKind kind, int candidatePriority, bool quorum,
					  ConnectionRetryPolicy *retryPolicy,
					  MonitorAssignedState *assignedState)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT * FROM pgautofailover.register_node($1, $2, $3, $4, $5, $6, $7, "
		"$8::pgautofailover.replication_state, $9, $10, $11)";
	int paramCount = 11;
	Oid paramTypes[11] = {
		TEXTOID, TEXTOID, INT4OID, NAMEOID, TEXTOID, INT8OID,
		INT4OID, TEXTOID, TEXTOID, INT4OID, BOOLOID
	};
	const char *paramValues[11];
	MonitorAssignedStateParseContext parseContext =
	{ { 0 }, assignedState, false };
	const char *nodeStateString = NodeStateToString(initialState);

	paramValues[0] = formation;
	paramValues[1] = host;
	paramValues[2] = intToString(port).strValue;
	paramValues[3] = dbname;
	paramValues[4] = name == NULL ? "" : name;
	paramValues[5] = intToString(system_identifier).strValue;
	paramValues[6] = intToString(desiredGroupId).strValue;
	paramValues[7] = nodeStateString;
	paramValues[8] = nodeKindToString(kind);
	paramValues[9] = intToString(candidatePriority).strValue;
	paramValues[10] = quorum ? "true" : "false";

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, parseNodeState))
	{
		if (strcmp(parseContext.sqlstate, STR_ERRCODE_OBJECT_IN_USE) == 0 &&
			!pgsql_retry_policy_expired(retryPolicy))
		{
			int sleepTimeMs =
				pgsql_compute_connection_retry_sleep_time(retryPolicy);

			log_warn("Failed to register node %s:%d in group %d of "
					 "formation \"%s\" with initial state \"%s\" "
					 "because the monitor is already registering another "
					 "standby, retrying in %d ms",
					 host, port, desiredGroupId, formation, nodeStateString,
					 sleepTimeMs);

			/* we have milliseconds, pg_usleep() wants microseconds */
			(void) pg_usleep(sleepTimeMs * 1000);

			return monitor_register_node(monitor, formation, name, host, port,
										 system_identifier,
										 dbname, desiredGroupId, initialState,
										 kind, candidatePriority, quorum,
										 retryPolicy,
										 assignedState);
		}
		else if (strcmp(parseContext.sqlstate,
						STR_ERRCODE_EXCLUSION_VIOLATION) == 0 &&
				 !pgsql_retry_policy_expired(retryPolicy))
		{
			/* *INDENT-OFF* */
			log_error("Failed to register node %s:%d in "
					  "group %d of formation \"%s\" "
					  "with system_identifier %" PRIu64 ", "
					  "because another node already exists in this group with "
					  "another system_identifier",
					  host, port, desiredGroupId, formation, system_identifier);
			/* *INDENT-ON* */

			log_info(
				"HINT: you may register a standby node from a non-existing "
				"PGDATA directory that pg_autoctl then creates for you, or "
				"PGDATA should be a copy of the current primary node such as "
				"obtained from a backup and recovery tool.");
			return false;
		}

		log_error("Failed to register node %s:%d in group %d of formation \"%s\" "
				  "with initial state \"%s\", see previous lines for details",
				  host, port, desiredGroupId, formation, nodeStateString);
		return false;
	}

	if (!parseContext.parsedOK)
	{
		log_error("Failed to register node %s:%d in group %d of formation \"%s\" "
				  "with initial state \"%s\" because the monitor returned an "
				  "unexpected result, see previous lines for details",
				  host, port, desiredGroupId, formation, nodeStateString);
		return false;
	}

	log_info("Registered node %s:%d with id %d in formation \"%s\", "
			 "group %d, state \"%s\"",
			 host, port, assignedState->nodeId,
			 formation, assignedState->groupId,
			 NodeStateToString(assignedState->state));

	return true;
}


/*
 * monitor_node_active communicates the current state of the node to the
 * monitor and puts the new goal state to assignedState, which must not
 * be NULL.
 */
bool
monitor_node_active(Monitor *monitor,
					char *formation, int nodeId,
					int groupId, NodeState currentState,
					bool pgIsRunning,
					char *currentLSN, char *pgsrSyncState,
					MonitorAssignedState *assignedState)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT * FROM pgautofailover.node_active($1, $2, $3, "
		"$4::pgautofailover.replication_state, $5, $6, $7)";
	int paramCount = 7;
	Oid paramTypes[7] = {
		TEXTOID, INT4OID, INT4OID, TEXTOID, BOOLOID, LSNOID, TEXTOID
	};
	const char *paramValues[7];
	MonitorAssignedStateParseContext parseContext =
	{ { 0 }, assignedState, false };
	const char *nodeStateString = NodeStateToString(currentState);

	paramValues[0] = formation;
	paramValues[1] = intToString(nodeId).strValue;
	paramValues[2] = intToString(groupId).strValue;
	paramValues[3] = nodeStateString;
	paramValues[4] = pgIsRunning ? "true" : "false";
	paramValues[5] = currentLSN;
	paramValues[6] = pgsrSyncState;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, parseNodeState))
	{
		log_error("Failed to get node state for node %d "
				  "in group %d of formation \"%s\" with initial state "
				  "\"%s\", replication state \"%s\", "
				  "and current lsn \"%s\", "
				  "see previous lines for details",
				  nodeId, groupId, formation, nodeStateString,
				  pgsrSyncState, currentLSN);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!parseContext.parsedOK)
	{
		log_error("Failed to get node state for node %d in group %d of formation "
				  "\"%s\" with initial state \"%s\", replication state \"%s\","
				  " and current lsn \"%s\""
				  " because the monitor returned an unexpected result, "
				  "see previous lines for details",
				  nodeId, groupId, formation, nodeStateString,
				  pgsrSyncState, currentLSN);
		return false;
	}

	return true;
}


/*
 * monitor_set_node_candidate_priority updates the monitor on the changes
 * in the node candidate priority.
 */
bool
monitor_set_node_candidate_priority(Monitor *monitor,
									int nodeid, char *hostName, int nodePort,
									int candidate_priority)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT pgautofailover.set_node_candidate_priority($1, $2, $3, $4)";
	int paramCount = 4;
	Oid paramTypes[4] = { INT4OID, TEXTOID, INT4OID, INT4OID };
	const char *paramValues[4];
	char *candidatePriorityText = intToString(candidate_priority).strValue;
	bool success = true;

	paramValues[0] = intToString(nodeid).strValue;
	paramValues[1] = hostName,
	paramValues[2] = intToString(nodePort).strValue;
	paramValues[3] = candidatePriorityText;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   NULL, NULL))
	{
		log_error("Failed to update node candidate priority on node %d"
				  " for candidate_priority: \"%s\"",
				  nodeid, candidatePriorityText);

		success = false;
	}

	return success;
}


/*
 * monitor_set_node_replication_quorum updates the monitor on the changes
 * in the node replication quorum.
 */
bool
monitor_set_node_replication_quorum(Monitor *monitor, int nodeid,
									char *hostName, int nodePort,
									bool replicationQuorum)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT pgautofailover.set_node_replication_quorum($1, $2, $3, $4)";
	int paramCount = 4;
	Oid paramTypes[4] = { INT4OID, TEXTOID, INT4OID, BOOLOID };
	const char *paramValues[4];
	char *replicationQuorumText = replicationQuorum ? "true" : "false";
	bool success = true;

	paramValues[0] = intToString(nodeid).strValue;
	paramValues[1] = hostName;
	paramValues[2] = intToString(nodePort).strValue;
	paramValues[3] = replicationQuorumText;

	if (!pgsql_execute_with_params(pgsql, sql, paramCount, paramTypes,
								   paramValues, NULL, NULL))
	{
		log_error("Failed to update node replication quorum on node %d"
				  " and replication_quorum: \"%s\"",
				  nodeid, replicationQuorumText);

		success = false;
	}

	return success;
}


/*
 * monitor_get_node_replication_settings retrieves replication settings
 * from the monitor.
 */
bool
monitor_get_node_replication_settings(Monitor *monitor, int nodeid,
									  NodeReplicationSettings *settings)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT candidatepriority, replicationquorum FROM pgautofailover.node "
		"WHERE nodeid = $1";
	int paramCount = 1;
	Oid paramTypes[1] = { INT4OID };
	const char *paramValues[1];
	NodeReplicationSettingsParseContext parseContext =
	{ { 0 }, -1, false, false };

	paramValues[0] = intToString(nodeid).strValue;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, parseNodeReplicationSettings))
	{
		log_error("Failed to retrieve node settings for node \"%d\".", nodeid);

		/* disconnect from monitor */
		pgsql_finish(&monitor->pgsql);

		return false;
	}

	/* disconnect from monitor */
	pgsql_finish(&monitor->pgsql);

	if (!parseContext.parsedOK)
	{
		return false;
	}

	settings->candidatePriority = parseContext.candidatePriority;
	settings->replicationQuorum = parseContext.replicationQuorum;

	return true;
}


/*
 * parseNodeReplicationSettings parses nore replication settings
 * from query output.
 */
static void
parseNodeReplicationSettings(void *ctx, PGresult *result)
{
	NodeReplicationSettingsParseContext *context =
		(NodeReplicationSettingsParseContext *) ctx;
	char *value = NULL;
	int errors = 0;

	if (PQntuples(result) != 1)
	{
		log_error("Query returned %d rows, expected 1", PQntuples(result));
		context->parsedOK = false;
		return;
	}

	if (PQnfields(result) != 2)
	{
		log_error("Query returned %d columns, expected 2", PQnfields(result));
		context->parsedOK = false;
		return;
	}

	value = PQgetvalue(result, 0, 0);
	if (!stringToInt(value, &context->candidatePriority))
	{
		log_error("Invalid failover candidate priority \"%s\" "
				  "returned by monitor", value);
		++errors;
	}

	value = PQgetvalue(result, 0, 1);
	if (value == NULL || ((*value != 't') && (*value != 'f')))
	{
		log_error("Invalid replication quorum \"%s\" "
				  "returned by monitor", value);
		++errors;
	}
	else
	{
		context->replicationQuorum = (*value) == 't';
	}

	if (errors > 0)
	{
		context->parsedOK = false;
		return;
	}

	/* if we reach this line, then we're good. */
	context->parsedOK = true;
}


/*
 * monitor_get_formation_number_sync_standbys retrieves number-sync-standbys
 * property for formation from the monitor. The function returns true upon
 * success.
 */
bool
monitor_get_formation_number_sync_standbys(Monitor *monitor, char *formation,
										   int *numberSyncStandbys)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT number_sync_standbys FROM pgautofailover.formation "
		"WHERE formationid = $1";
	int paramCount = 1;
	Oid paramTypes[1] = { TEXTOID };
	const char *paramValues[1];
	SingleValueResultContext parseContext = { { 0 }, PGSQL_RESULT_INT, false };
	paramValues[0] = formation;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, parseSingleValueResult))
	{
		log_error("Failed to retrieve settings for formation \"%s\".",
				  formation);

		/* disconnect from monitor */
		pgsql_finish(&monitor->pgsql);

		return false;
	}

	/* disconnect from monitor */
	pgsql_finish(&monitor->pgsql);

	if (!parseContext.parsedOk)
	{
		return false;
	}

	*numberSyncStandbys = parseContext.intVal;

	return true;
}


/*
 * monitor_set_formation_number_sync_standbys sets number-sync-standbys
 * property for formation at the monitor. The function returns true upon
 * success.
 */
bool
monitor_set_formation_number_sync_standbys(Monitor *monitor, char *formation,
										   int numberSyncStandbys)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT pgautofailover.set_formation_number_sync_standbys($1, $2)";
	int paramCount = 2;
	Oid paramTypes[2] = { TEXTOID, INT4OID };
	const char *paramValues[2];
	SingleValueResultContext parseContext = { { 0 }, PGSQL_RESULT_BOOL, false };
	paramValues[0] = formation;
	paramValues[1] = intToString(numberSyncStandbys).strValue;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, parseSingleValueResult))
	{
		log_error("Failed to update number-sync-standbys for formation \"%s\".",
				  formation);

		/* disconnect from monitor */
		pgsql_finish(&monitor->pgsql);

		return false;
	}

	if (!parseContext.parsedOk)
	{
		return false;
	}

	return parseContext.boolVal;
}


/*
 * monitor_remove calls the pgautofailover.monitor_remove function on the
 * monitor.
 */
bool
monitor_remove(Monitor *monitor, char *host, int port)
{
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_BOOL, false };
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql = "SELECT pgautofailover.remove_node($1, $2)";
	int paramCount = 2;
	Oid paramTypes[2] = { TEXTOID, INT4OID };
	const char *paramValues[2];

	paramValues[0] = host;
	paramValues[1] = intToString(port).strValue;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		log_error("Failed to remove node %s:%d from the monitor", host, port);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!context.parsedOk)
	{
		log_error("Failed to remove node %s:%d from the monitor: "
				  "could not parse monitor's result.", host, port);
		return false;
	}

	/*
	 * We ignore the return value of pgautofailover.remove_node:
	 *  - if it's true, then the node has been removed
	 *  - if it's false, then the node didn't exist in the first place
	 *
	 * The only case where we return false here is when we failed to run the
	 * pgautofailover.remove_node function on the monitor, see above.
	 */
	return true;
}


/*
 * monitor_count_groups counts how many groups we have in this formation, and
 * sets the obtained value in the groupsCount parameter.
 */
bool
monitor_count_groups(Monitor *monitor, char *formation, int *groupsCount)
{
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_INT, false };
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT count(distinct(groupid)) "
		"FROM pgautofailover.node "
		"WHERE formationid = $1";
	int paramCount = 1;
	Oid paramTypes[1] = { TEXTOID };
	const char *paramValues[1] = { formation };

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		log_error("Failed to get how many groups are in formation %s",
				  formation);
		return false;
	}

	*groupsCount = context.intVal;

	return true;
}


/*
 * monitor_perform_failover calls the pgautofailover.monitor_perform_failover
 * function on the monitor.
 */
bool
monitor_perform_failover(Monitor *monitor, char *formation, int group)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql = "SELECT pgautofailover.perform_failover($1, $2)";
	int paramCount = 2;
	Oid paramTypes[2] = { TEXTOID, INT4OID };
	const char *paramValues[2];

	paramValues[0] = formation;
	paramValues[1] = intToString(group).strValue;

	/*
	 * pgautofailover.perform_failover() returns VOID.
	 */
	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   NULL, NULL))
	{
		log_error("Failed to perform failover for formation %s and group %d",
				  formation, group);
		return false;
	}

	return true;
}


/*
 * parseNode parses a hostname and a port from the libpq result and writes
 * it to the NodeAddressParseContext pointed to by ctx.
 */
static bool
parseNode(PGresult *result, int rowNumber, NodeAddress *node)
{
	char *value = NULL;
	int length = 0;

	if (PQgetisnull(result, rowNumber, 0) ||
		PQgetisnull(result, rowNumber, 1) ||
		PQgetisnull(result, rowNumber, 2) ||
		PQgetisnull(result, rowNumber, 3))
	{
		log_error("NodeId, nodename, hostname or port returned by monitor is NULL");
		return false;
	}

	value = PQgetvalue(result, rowNumber, 0);
	node->nodeId = strtol(value, NULL, 0);
	if (node->nodeId == 0)
	{
		log_error("Invalid nodeId \"%s\" returned by monitor", value);
		return false;
	}

	value = PQgetvalue(result, rowNumber, 1);
	length = strlcpy(node->name, value, _POSIX_HOST_NAME_MAX);
	if (length >= _POSIX_HOST_NAME_MAX)
	{
		log_error("Node name \"%s\" returned by monitor is %d characters, "
				  "the maximum supported by pg_autoctl is %d",
				  value, length, _POSIX_HOST_NAME_MAX - 1);
		return false;
	}

	value = PQgetvalue(result, rowNumber, 2);

	length = strlcpy(node->host, value, _POSIX_HOST_NAME_MAX);
	if (length >= _POSIX_HOST_NAME_MAX)
	{
		log_error("Hostname \"%s\" returned by monitor is %d characters, "
				  "the maximum supported by pg_autoctl is %d",
				  value, length, _POSIX_HOST_NAME_MAX - 1);
		return false;
	}

	value = PQgetvalue(result, rowNumber, 3);

	if (!stringToInt(value, &node->port) || node->port == 0)
	{
		log_error("Invalid port number \"%s\" returned by monitor", value);
		return false;
	}

	/*
	 * pgautofailover.get_other_nodes also returns the LSN and is_primary bits
	 * of information.
	 */
	if (PQnfields(result) == 6)
	{
		/* we trust Postgres pg_lsn data type to fit in our PG_LSN_MAXLENGTH */
		value = PQgetvalue(result, rowNumber, 4);
		strlcpy(node->lsn, value, PG_LSN_MAXLENGTH);

		value = PQgetvalue(result, rowNumber, 5);
		node->isPrimary = strcmp(value, "t") == 0;
	}

	return true;
}


/*
 * parseNode parses a hostname and a port from the libpq result and writes
 * it to the NodeAddressParseContext pointed to by ctx.
 */
static void
parseNodeResult(void *ctx, PGresult *result)
{
	NodeAddressParseContext *context = (NodeAddressParseContext *) ctx;

	if (PQntuples(result) != 1)
	{
		log_error("Query returned %d rows, expected 1", PQntuples(result));
		context->parsedOK = false;
		return;
	}

	if (PQnfields(result) != 4)
	{
		log_error("Query returned %d columns, expected 3", PQnfields(result));
		context->parsedOK = false;
		return;
	}

	context->parsedOK = parseNode(result, 0, context->node);
}


/*
 * parseNode parses a hostname and a port from the libpq result and writes
 * it to the NodeAddressParseContext pointed to by ctx.
 */
static void
parseNodeArray(void *ctx, PGresult *result)
{
	bool parsedOk = true;
	int rowNumber = 0;
	NodeAddressArrayParseContext *context = (NodeAddressArrayParseContext *) ctx;

	log_debug("parseNodeArray: %d", PQntuples(result));

	/* keep a NULL entry to mark the end of the array */
	if (PQntuples(result) > NODE_ARRAY_MAX_COUNT)
	{
		log_error("Query returned %d rows, pg_auto_failover supports only up "
				  "to %d standby nodes at the moment",
				  PQntuples(result), NODE_ARRAY_MAX_COUNT);
		context->parsedOK = false;
		return;
	}

	/* pgautofailover.get_other_nodes returns 6 columns */
	if (PQnfields(result) != 6)
	{
		log_error("Query returned %d columns, expected 6", PQnfields(result));
		context->parsedOK = false;
		return;
	}

	context->nodesArray->count = PQntuples(result);

	for (rowNumber = 0; rowNumber < PQntuples(result); rowNumber++)
	{
		NodeAddress *node = &(context->nodesArray->nodes[rowNumber]);

		parsedOk = parsedOk && parseNode(result, rowNumber, node);
	}

	context->parsedOK = parsedOk;
}


/*
 * MaxHostNameSizeInNodesArray returns the greatest node name length in the
 * given array of nodes.
 */
static int
MaxHostNameSizeInNodesArray(NodeAddressArray *nodesArray)
{
	int maxHostNameSize = 0;
	int i = 0;

	for (i = 0; i < nodesArray->count; i++)
	{
		NodeAddress node = nodesArray->nodes[i];

		if (strlen(node.host) > maxHostNameSize)
		{
			maxHostNameSize = strlen(node.host);
		}
	}

	return maxHostNameSize;
}


/*
 * prepareHostNameSeparator fills in the pre-allocated given string with the
 * expected amount of dashes to use as a separator line in our tabular output.
 */
static void
prepareHostNameSeparator(char nameSeparatorHeader[], int maxHostNameSize)
{
	for (int i = 0; i <= maxHostNameSize; i++)
	{
		if (i < maxHostNameSize)
		{
			nameSeparatorHeader[i] = '-';
		}
		else
		{
			nameSeparatorHeader[i] = '\0';
			break;
		}
	}
}


/*
 * printCurrentState loops over pgautofailover.current_state() results and prints
 * them, one per line.
 */
void
printNodeArray(NodeAddressArray *nodesArray)
{
	int nodesArrayIndex = 0;
	int maxHostNameSize = 5;    /* strlen("Name") + 1, the header */

	/*
	 * Dynamically adjust our display output to the length of the longer
	 * hostname in the result set
	 */
	maxHostNameSize = MaxHostNameSizeInNodesArray(nodesArray);

	(void) printNodeHeader(maxHostNameSize);

	for (nodesArrayIndex = 0; nodesArrayIndex < nodesArray->count; nodesArrayIndex++)
	{
		NodeAddress *node = &(nodesArray->nodes[nodesArrayIndex]);

		printNodeEntry(node);
	}

	fformat(stdout, "\n");
}


/*
 * printNodeHeader pretty prints a header for a node list.
 */
void
printNodeHeader(int maxHostNameSize)
{
	char nameSeparatorHeader[BUFSIZE] = { 0 };

	(void) prepareHostNameSeparator(nameSeparatorHeader, maxHostNameSize);

	fformat(stdout, "%3s | %*s | %6s | %18s | %8s\n",
			"ID", maxHostNameSize, "Host", "Port", "LSN", "Primary?");

	fformat(stdout, "%3s-+-%*s-+-%6s-+-%18s-+-%8s\n",
			"---", maxHostNameSize, nameSeparatorHeader, "------",
			"------------------", "--------");
}


/*
 * printNodeEntry pretty prints a node.
 */
void
printNodeEntry(NodeAddress *node)
{
	fformat(stdout, "%3d | %s | %6d | %18s | %8s\n",
			node->nodeId, node->host, node->port, node->lsn,
			node->isPrimary ? "yes" : "no");
}


/*
 * parseNodeState parses a node state coming back from a call to
 * register_node or node_active.
 */
static void
parseNodeState(void *ctx, PGresult *result)
{
	MonitorAssignedStateParseContext *context =
		(MonitorAssignedStateParseContext *) ctx;
	char *value = NULL;
	int errors = 0;

	if (PQntuples(result) != 1)
	{
		log_error("Query returned %d rows, expected 1", PQntuples(result));
		context->parsedOK = false;
		return;
	}

	if (PQnfields(result) != 5)
	{
		log_error("Query returned %d columns, expected 5", PQnfields(result));
		context->parsedOK = false;
		return;
	}

	value = PQgetvalue(result, 0, 0);

	if (!stringToInt(value, &context->assignedState->nodeId))
	{
		log_error("Invalid node ID \"%s\" returned by monitor", value);
		++errors;
	}

	value = PQgetvalue(result, 0, 1);

	if (!stringToInt(value, &context->assignedState->groupId))
	{
		log_error("Invalid group ID \"%s\" returned by monitor", value);
		++errors;
	}

	value = PQgetvalue(result, 0, 2);
	context->assignedState->state = NodeStateFromString(value);
	if (context->assignedState->state == NO_STATE)
	{
		log_error("Invalid node state \"%s\" returned by monitor", value);
		++errors;
	}

	value = PQgetvalue(result, 0, 3);
	if (!stringToInt(value, &context->assignedState->candidatePriority))
	{
		log_error("Invalid failover candidate priority \"%s\" "
				  "returned by monitor", value);
		++errors;
	}

	value = PQgetvalue(result, 0, 4);
	if (value == NULL || ((*value != 't') && (*value != 'f')))
	{
		log_error("Invalid replication quorum \"%s\" "
				  "returned by monitor", value);
		++errors;
	}
	else
	{
		context->assignedState->replicationQuorum = (*value) == 't';
	}

	if (errors > 0)
	{
		context->parsedOK = false;
		return;
	}

	/* if we reach this line, then we're good. */
	context->parsedOK = true;
}


/*
 * monitor_print_state calls the function pgautofailover.current_state on the
 * monitor, and prints a line of output per state record obtained.
 */
bool
monitor_print_state(Monitor *monitor, char *formation, int group)
{
	MonitorAssignedStateParseContext context = { 0 };
	PGSQL *pgsql = &monitor->pgsql;
	char *sql = NULL;
	int paramCount = 0;
	Oid paramTypes[2];
	const char *paramValues[2];
	IntString groupStr;

	log_trace("monitor_print_state(%s, %d)", formation, group);

	switch (group)
	{
		case -1:
		{
			sql =
				"SELECT * FROM pgautofailover.current_state($1) "
				"ORDER BY node_id";

			paramCount = 1;
			paramTypes[0] = TEXTOID;
			paramValues[0] = formation;

			break;
		}

		default:
		{
			sql =
				"SELECT * FROM pgautofailover.current_state($1,$2) "
				"ORDER BY node_id";

			groupStr = intToString(group);

			paramCount = 2;
			paramTypes[0] = TEXTOID;
			paramValues[0] = formation;
			paramTypes[1] = INT4OID;
			paramValues[1] = groupStr.strValue;

			break;
		}
	}

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &printCurrentState))
	{
		log_error("Failed to retrieve current state from the monitor");
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!context.parsedOK)
	{
		log_error("Failed to parse current state from the monitor");
		return false;
	}

	return true;
}


/*
 * printCurrentState loops over pgautofailover.current_state() results and prints
 * them, one per line.
 */
static void
printCurrentState(void *ctx, PGresult *result)
{
	MonitorAssignedStateParseContext *context =
		(MonitorAssignedStateParseContext *) ctx;
	int currentTupleIndex = 0;
	int nTuples = PQntuples(result);

	int maxNameSize = 5;    /* strlen("Name") + 1, the header */
	int maxHostSize = 10;   /* strlen("Host:Port") + 1, the header */
	int maxNodeSize = 5;    /* strlen("Node") + 1, the header */

	char nameSeparatorHeader[BUFSIZE] = { 0 };
	char hostSeparatorHeader[BUFSIZE] = { 0 };
	char nodeSeparatorHeader[BUFSIZE] = { 0 };

	if (PQnfields(result) != 9)
	{
		log_error("Query returned %d columns, expected 9", PQnfields(result));
		context->parsedOK = false;
		return;
	}

	/*
	 * Dynamically adjust our display output to the length of the longer
	 * hostname in the result set
	 */
	for (currentTupleIndex = 0; currentTupleIndex < nTuples; currentTupleIndex++)
	{
		char *name = PQgetvalue(result, currentTupleIndex, 0);
		char *host = PQgetvalue(result, currentTupleIndex, 1);
		char *port = PQgetvalue(result, currentTupleIndex, 2);

		char *groupId = PQgetvalue(result, currentTupleIndex, 3);
		char *nodeId = PQgetvalue(result, currentTupleIndex, 4);

		int hostLen = strlen(host) + strlen(port) + 1; /* host:port */
		int nodeLen = strlen(groupId) + strlen(nodeId) + 1; /* 0/1 */

		if (strlen(name) > maxNameSize)
		{
			maxNameSize = strlen(name);
		}

		if (hostLen > maxHostSize)
		{
			maxHostSize = hostLen;
		}

		if (nodeLen > maxNodeSize)
		{
			maxNodeSize = nodeLen;
		}
	}

	/* prepare a nice dynamic string of '-' as a header separator */
	(void) prepareHostNameSeparator(nameSeparatorHeader, maxNameSize);
	(void) prepareHostNameSeparator(hostSeparatorHeader, maxHostSize);
	(void) prepareHostNameSeparator(nodeSeparatorHeader, maxNodeSize);

	fformat(stdout, "%*s | %*s | %*s | %17s | %17s | %8s | %6s\n",
			maxNameSize, "Name",
			maxHostSize, "Host:Port",
			maxNodeSize, "Node",
			"Current State", "Assigned State",
			"Priority", "Quorum");

	fformat(stdout, "%*s-+-%*s-+-%*s-+-%17s-+-%17s-+-%8s-+-%6s\n",
			maxNameSize, nameSeparatorHeader,
			maxHostSize, hostSeparatorHeader,
			maxNodeSize, nodeSeparatorHeader,
			"-----------------", "-----------------",
			"--------", "------");

	for (currentTupleIndex = 0; currentTupleIndex < nTuples; currentTupleIndex++)
	{
		char *name = PQgetvalue(result, currentTupleIndex, 0);
		char *host = PQgetvalue(result, currentTupleIndex, 1);
		char *port = PQgetvalue(result, currentTupleIndex, 2);
		char *groupId = PQgetvalue(result, currentTupleIndex, 3);
		char *nodeId = PQgetvalue(result, currentTupleIndex, 4);
		char *currentState = PQgetvalue(result, currentTupleIndex, 5);
		char *goalState = PQgetvalue(result, currentTupleIndex, 6);
		char *candidatePriority = PQgetvalue(result, currentTupleIndex, 7);
		char *replicationQuorum = PQgetvalue(result, currentTupleIndex, 8);

		char hostport[BUFSIZE] = { 0 };
		char composedId[BUFSIZE] = { 0 };

		sformat(hostport, sizeof(hostport), "%s:%s", host, port);
		sformat(composedId, sizeof(hostport), "%s/%s", groupId, nodeId);

		fformat(stdout, "%*s | %*s | %*s | %17s | %17s | %8s | %6s\n",
				maxNameSize, name,
				maxHostSize, hostport,
				maxNodeSize, composedId,
				currentState, goalState,
				candidatePriority, replicationQuorum);
	}
	fformat(stdout, "\n");

	context->parsedOK = true;
}


/*
 * monitor_print_state_as_json prints to given stream a single string that
 * contains the JSON representation of the current state on the monitor.
 */
bool
monitor_print_state_as_json(Monitor *monitor, char *formation, int group)
{
	SingleValueResultContext context = { 0 };
	PGSQL *pgsql = &monitor->pgsql;
	char *sql = NULL;
	int paramCount = 0;
	Oid paramTypes[2];
	const char *paramValues[2];
	IntString groupStr;

	log_trace("monitor_get_state_as_json(%s, %d)", formation, group);

	context.resultType = PGSQL_RESULT_STRING;
	context.parsedOk = false;

	switch (group)
	{
		case -1:
		{
			sql = "SELECT jsonb_pretty("
				  "coalesce(jsonb_agg(row_to_json(state)), '[]'))"
				  " FROM pgautofailover.current_state($1) as state";

			paramCount = 1;
			paramTypes[0] = TEXTOID;
			paramValues[0] = formation;

			break;
		}

		default:
		{
			sql = "SELECT jsonb_pretty("
				  "coalesce(jsonb_agg(row_to_json(state)), '[]'))"
				  "FROM pgautofailover.current_state($1,$2) as state";

			groupStr = intToString(group);

			paramCount = 2;
			paramTypes[0] = TEXTOID;
			paramValues[0] = formation;
			paramTypes[1] = INT4OID;
			paramValues[1] = groupStr.strValue;

			break;
		}
	}

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		log_error("Failed to retrieve current state from the monitor");
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!context.parsedOk)
	{
		log_error("Failed to parse current state from the monitor");
		log_error("%s", context.strVal);
		return false;
	}

	fformat(stdout, "%s\n", context.strVal);

	return true;
}


/*
 * monitor_print_last_events calls the function pgautofailover.last_events on
 * the monitor, and prints a line of output per event obtained.
 */
bool
monitor_print_last_events(Monitor *monitor, char *formation, int group, int count)
{
	MonitorAssignedStateParseContext context = { 0 };
	PGSQL *pgsql = &monitor->pgsql;
	char *sql = NULL;
	int paramCount = 0;
	Oid paramTypes[3];
	const char *paramValues[3];
	IntString countStr;
	IntString groupStr;

	log_trace("monitor_print_last_events(%s, %d, %d)", formation, group, count);

	switch (group)
	{
		case -1:
		{
			sql =
				"SELECT eventTime, formationid, nodeid, groupid, "
				"       reportedstate, goalState, description "
				"  FROM pgautofailover.last_events($1, count => $2)";

			countStr = intToString(count);

			paramCount = 2;
			paramTypes[0] = TEXTOID;
			paramValues[0] = formation;
			paramTypes[1] = INT4OID;
			paramValues[1] = countStr.strValue;

			break;
		}

		default:
		{
			sql =
				"SELECT eventTime, formationid, nodeid, groupid, "
				"       reportedstate, goalState, description "
				"  FROM pgautofailover.last_events($1,$2,$3)";

			countStr = intToString(count);
			groupStr = intToString(group);

			paramCount = 3;
			paramTypes[0] = TEXTOID;
			paramValues[0] = formation;
			paramTypes[1] = INT4OID;
			paramValues[1] = groupStr.strValue;
			paramTypes[2] = INT4OID;
			paramValues[2] = countStr.strValue;

			break;
		}
	}

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &printLastEvents))
	{
		log_error("Failed to retrieve current state from the monitor");
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!context.parsedOK)
	{
		return false;
	}

	return true;
}


/*
 * monitor_print_last_events_as_json calls the function
 * pgautofailover.last_events on the monitor, and prints the result as a JSON
 * array to the given stream (stdout, typically).
 */
bool
monitor_print_last_events_as_json(Monitor *monitor,
								  char *formation, int group,
								  int count,
								  FILE *stream)
{
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_STRING, false };
	PGSQL *pgsql = &monitor->pgsql;
	char *sql = NULL;
	int paramCount = 0;
	Oid paramTypes[3];
	const char *paramValues[3];
	IntString countStr;
	IntString groupStr;

	switch (group)
	{
		case -1:
		{
			sql = "SELECT jsonb_pretty("
				  "coalesce(jsonb_agg(row_to_json(event)), '[]'))"
				  " FROM pgautofailover.last_events($1, count => $2) as event";

			countStr = intToString(count);

			paramCount = 2;
			paramTypes[0] = TEXTOID;
			paramValues[0] = formation;
			paramTypes[1] = INT4OID;
			paramValues[1] = countStr.strValue;

			break;
		}

		default:
		{
			sql = "SELECT jsonb_pretty("
				  "coalesce(jsonb_agg(row_to_json(event)), '[]'))"
				  " FROM * FROM pgautofailover.last_events($1,$2,$3) as event";

			countStr = intToString(count);
			groupStr = intToString(group);

			paramCount = 3;
			paramTypes[0] = TEXTOID;
			paramValues[0] = formation;
			paramTypes[1] = INT4OID;
			paramValues[1] = groupStr.strValue;
			paramTypes[2] = INT4OID;
			paramValues[2] = countStr.strValue;

			break;
		}
	}

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		log_error("Failed to retrieve the last %d events from the monitor",
				  count);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!context.parsedOk)
	{
		log_error("Failed to parse %d last events from the monitor", count);
		log_error("%s", context.strVal);
		return false;
	}

	fformat(stream, "%s\n", context.strVal);

	return true;
}


/*
 * printLastEcvents loops over pgautofailover.last_events() results and prints
 * them, one per line.
 */
static void
printLastEvents(void *ctx, PGresult *result)
{
	MonitorAssignedStateParseContext *context =
		(MonitorAssignedStateParseContext *) ctx;
	int currentTupleIndex = 0;
	int nTuples = PQntuples(result);

	log_trace("printLastEvents: %d tuples", nTuples);

	if (PQnfields(result) != 7)
	{
		log_error("Query returned %d columns, expected 7", PQnfields(result));
		context->parsedOK = false;
		return;
	}

	fformat(stdout, "%30s | %10s | %6s | %19s | %19s | %s\n",
			"Event Time", "Formation", "Node",
			"Current State", "Assigned State", "Comment");
	fformat(stdout, "%30s-+-%10s-+-%6s-+-%19s-+-%19s-+-%10s\n",
			"------------------------------", "----------",
			"------", "-------------------",
			"-------------------", "----------");

	for (currentTupleIndex = 0; currentTupleIndex < nTuples; currentTupleIndex++)
	{
		char *eventTime = PQgetvalue(result, currentTupleIndex, 0);
		char *formation = PQgetvalue(result, currentTupleIndex, 1);
		char *nodeId = PQgetvalue(result, currentTupleIndex, 2);
		char *groupId = PQgetvalue(result, currentTupleIndex, 3);
		char *currentState = PQgetvalue(result, currentTupleIndex, 4);
		char *goalState = PQgetvalue(result, currentTupleIndex, 5);
		char *description = PQgetvalue(result, currentTupleIndex, 6);
		char node[BUFSIZE];

		/* for our grid alignment output it's best to have a single col here */
		sformat(node, BUFSIZE, "%s/%s", groupId, nodeId);

		fformat(stdout, "%30s | %10s | %6s | %19s | %19s | %s\n",
				eventTime, formation, node,
				currentState, goalState, description);
	}
	fformat(stdout, "\n");

	context->parsedOK = true;
}


/*
 * monitor_create_formation calls the SQL API on the monitor to create a new
 * formation of the given kind.
 */
bool
monitor_create_formation(Monitor *monitor,
						 char *formation, char *kind, char *dbname,
						 bool hasSecondary, int numberSyncStandbys)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT * FROM pgautofailover.create_formation($1, $2, $3, $4, $5)";
	int paramCount = 5;
	Oid paramTypes[5] = { TEXTOID, TEXTOID, TEXTOID, BOOLOID, INT4OID };
	const char *paramValues[5];

	paramValues[0] = formation;
	paramValues[1] = kind;
	paramValues[2] = dbname;
	paramValues[3] = hasSecondary ? "true" : "false";
	paramValues[4] = intToString(numberSyncStandbys).strValue;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   NULL, NULL))
	{
		log_error("Failed to create formation \"%s\" of kind \"%s\", "
				  "see previous lines for details.",
				  formation, kind);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);


	return true;
}


/*
 * monitor_enable_secondary_for_formation enables secondaries for the given
 * formation
 */
bool
monitor_enable_secondary_for_formation(Monitor *monitor, const char *formation)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql = "SELECT * FROM pgautofailover.enable_secondary($1)";
	int paramCount = 1;
	Oid paramTypes[1] = { TEXTOID };
	const char *paramValues[1];

	paramValues[0] = formation;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   NULL, NULL))
	{
		log_error("Failed to enable secondaries on formation \"%s\", "
				  "see previous lines for details.",
				  formation);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	return true;
}


/*
 * monitor_disable_secondary_for_formation disables secondaries for the given
 * formation. This requires no secondaries to be currently in the formation,
 * function will report an error on the monitor due to an execution error of
 * pgautofailover.disable_secondary when there are still secondaries in the
 * cluster, or more precise nodes that are not in 'sinlge' state.
 */
bool
monitor_disable_secondary_for_formation(Monitor *monitor, const char *formation)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql = "SELECT * FROM pgautofailover.disable_secondary($1)";
	int paramCount = 1;
	Oid paramTypes[1] = { TEXTOID };
	const char *paramValues[1];

	paramValues[0] = formation;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   NULL, NULL))
	{
		log_error("Failed to disable secondaries on formation \"%s\", "
				  "see previous lines for details.",
				  formation);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	return true;
}


/*
 * monitor_drop_formation calls the SQL API on the monitor to drop formation.
 */
bool
monitor_drop_formation(Monitor *monitor, char *formation)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql = "SELECT * FROM pgautofailover.drop_formation($1)";
	int paramCount = 1;
	Oid paramTypes[1] = { TEXTOID };
	const char *paramValues[1];

	paramValues[0] = formation;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   NULL, NULL))
	{
		log_error("Failed to drop formation \"%s\", "
				  "see previous lines for details.",
				  formation);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	return true;
}


/*
 * monitor_formation_uri calls the SQL API on the monitor that returns the
 * connection string that can be used by applications to connect to the
 * formation.
 */
bool
monitor_formation_uri(Monitor *monitor,
					  const char *formation,
					  const SSLOptions *ssl,
					  char *connectionString,
					  size_t size)
{
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_STRING, false };
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT formation_uri FROM pgautofailover.formation_uri($1, $2, $3, $4)";
	int paramCount = 4;
	Oid paramTypes[4] = { TEXTOID, TEXTOID, TEXTOID, TEXTOID };
	const char *paramValues[4] = { 0 };

	paramValues[0] = formation;
	paramValues[1] = ssl->sslModeStr;
	paramValues[2] = ssl->caFile;
	paramValues[3] = ssl->crlFile;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		log_error("Failed to list the formation uri for \"%s\", "
				  "see previous lines for details.",
				  formation);
		return false;
	}

	if (!context.parsedOk)
	{
		/* errors have already been logged */
		return false;
	}

	if (context.strVal == NULL || strcmp(context.strVal, "") == 0)
	{
		log_error("Formation \"%s\" currently has no nodes in group 0",
				  formation);
		return false;
	}

	strlcpy(connectionString, context.strVal, size);

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	return true;
}


/*
 * monitor_print_every_formation_uri prints a table of all our connection
 * strings: first the monitor URI itself, and then one line per formation.
 */
bool
monitor_print_every_formation_uri(Monitor *monitor, const SSLOptions *ssl)
{
	FormationURIParseContext context = { 0 };
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT 'monitor', 'monitor', $1 "
		" UNION ALL "
		"SELECT 'formation', formationid, formation_uri "
		"  FROM pgautofailover.formation, "
		"       pgautofailover.formation_uri(formation.formationid, $2, $3, $4)";

	int paramCount = 4;
	Oid paramTypes[4] = { TEXTOID, TEXTOID, TEXTOID, TEXTOID };
	const char *paramValues[4];

	paramValues[0] = monitor->pgsql.connectionString;
	paramValues[1] = ssl->sslModeStr;
	paramValues[2] = ssl->caFile;
	paramValues[3] = ssl->crlFile;

	context.parsedOK = false;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &printFormationURI))
	{
		log_error("Failed to list the formation uri, "
				  "see previous lines for details.");
		return false;
	}

	if (!context.parsedOK)
	{
		/* errors have already been logged */
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	return true;
}


/*
 * monitor_print_every_formation_uri_as_json prints all our connection strings
 * in the JSON format: first the monitor URI itself, and then one line per
 * formation.
 */
bool
monitor_print_every_formation_uri_as_json(Monitor *monitor,
										  const SSLOptions *ssl,
										  FILE *stream)
{
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_STRING, false };
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"WITH formation(type, name, uri) AS ( "
		"SELECT 'monitor', 'monitor', $1 "
		" UNION ALL "
		"SELECT 'formation', formationid, formation_uri "
		"  FROM pgautofailover.formation, "
		"       pgautofailover.formation_uri(formation.formationid, $2, $3, $4)"
		") "
		"SELECT jsonb_pretty(jsonb_agg(row_to_json(formation))) FROM formation";

	int paramCount = 4;
	Oid paramTypes[4] = { TEXTOID, TEXTOID, TEXTOID, TEXTOID };
	const char *paramValues[4];

	paramValues[0] = monitor->pgsql.connectionString;
	paramValues[1] = ssl->sslModeStr;
	paramValues[2] = ssl->caFile;
	paramValues[3] = ssl->crlFile;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		log_error("Failed to list the formation uri, "
				  "see previous lines for details.");
		return false;
	}

	if (!context.parsedOk)
	{
		/* errors have already been logged */
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	fformat(stream, "%s\n", context.strVal);

	return true;
}


/*
 * printFormationURI loops over the results of the SQL query in
 * monitor_print_every_formation_uri and outputs the result in table like
 * format.
 */
static void
printFormationURI(void *ctx, PGresult *result)
{
	FormationURIParseContext *context = (FormationURIParseContext *) ctx;
	int currentTupleIndex = 0;
	int nTuples = PQntuples(result);

	int maxFormationNameSize = 7;   /* "monitor" */
	char formationNameSeparator[BUFSIZE] = { 0 };

	log_trace("printFormationURI: %d tuples", nTuples);

	if (PQnfields(result) != 3)
	{
		log_error("Query returned %d columns, expected 3", PQnfields(result));
		context->parsedOK = false;
		return;
	}

	/*
	 * Dynamically adjust our display output to the length of the longer
	 * hostname in the result set
	 */
	for (currentTupleIndex = 0; currentTupleIndex < nTuples; currentTupleIndex++)
	{
		int size = strlen(PQgetvalue(result, currentTupleIndex, 1));

		if (size > maxFormationNameSize)
		{
			maxFormationNameSize = size;
		}
	}

	/* create the visual separator for the formation name too */
	(void) prepareHostNameSeparator(formationNameSeparator, maxFormationNameSize);

	fformat(stdout, "%10s | %*s | %s\n",
			"Type", maxFormationNameSize, "Name", "Connection String");
	fformat(stdout, "%10s-+-%*s-+-%s\n",
			"----------", maxFormationNameSize, formationNameSeparator,
			"------------------------------");

	for (currentTupleIndex = 0; currentTupleIndex < nTuples; currentTupleIndex++)
	{
		char *type = PQgetvalue(result, currentTupleIndex, 0);
		char *name = PQgetvalue(result, currentTupleIndex, 1);
		char *URI = PQgetvalue(result, currentTupleIndex, 2);

		fformat(stdout, "%10s | %*s | %s\n",
				type, maxFormationNameSize, name, URI);
	}
	fformat(stdout, "\n");

	context->parsedOK = true;
}


/*
 * monitor_synchronous_standby_names returns the value for the Postgres
 * parameter "synchronous_standby_names" to use for a given group. The setting
 * is computed on the monitor depending on the current values of the formation
 * number_sync_standbys and each node's candidate priority and replication
 * quorum properties.
 */
bool
monitor_synchronous_standby_names(Monitor *monitor,
								  char *formation, int groupId,
								  char *synchronous_standby_names, int size)
{
	PGSQL *pgsql = &monitor->pgsql;
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_STRING, false };

	const char *sql =
		"select pgautofailover.synchronous_standby_names($1, $2)";

	int paramCount = 2;
	Oid paramTypes[2] = { TEXTOID, INT4OID };
	const char *paramValues[2] = { 0 };
	IntString myGroupIdString = intToString(groupId);

	paramValues[0] = formation;
	paramValues[1] = myGroupIdString.strValue;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		log_error("Failed to get the synchronous_standby_names setting value "
				  " from the monitor for formation %s and group %d",
				  formation, groupId);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!context.parsedOk)
	{
		log_error("Failed to get the synchronous_standby_names setting value "
				  " from the monitor for formation %s and group %d,"
				  "see above for details",
				  formation, groupId);
		return false;
	}

	strlcpy(synchronous_standby_names, context.strVal, size);

	return true;
}


/*
 * monitor_set_hostname sets the hostname on the monitor, using a simple SQL
 * update command.
 */
bool
monitor_update_node_metadata(Monitor *monitor,
							 int nodeId,
							 const char *name,
							 const char *hostname,
							 int port)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT * FROM pgautofailover.update_node_metadata($1, $2, $3, $4)";
	int paramCount = 4;
	Oid paramTypes[4] = { INT8OID, TEXTOID, TEXTOID, INT4OID };
	const char *paramValues[4];

	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_BOOL, false };

	paramValues[0] = intToString(nodeId).strValue;
	paramValues[1] = name;
	paramValues[2] = hostname;
	paramValues[3] = intToString(port).strValue;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		log_error("Failed to update_node_metadata of node %d from the monitor",
				  nodeId);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!context.parsedOk)
	{
		log_error(
			"Failed to set node %d metadata on the monitor "
			"because it returned an unexpected result. "
			"See previous line for details.",
			nodeId);
		return false;
	}

	return true;
}


/*
 * monitor_set_node_system_identifier sets the node's sysidentifier column on
 * the monitor.
 */
bool
monitor_set_node_system_identifier(Monitor *monitor,
								   int nodeId,
								   uint64_t system_identifier)
{
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT * FROM pgautofailover.set_node_system_identifier($1, $2)";
	int paramCount = 2;
	Oid paramTypes[2] = { INT8OID, INT8OID };
	const char *paramValues[2];

	NodeAddress node = { 0 };
	NodeAddressParseContext parseContext = { { 0 }, &node, false };

	paramValues[0] = intToString(nodeId).strValue;
	paramValues[1] = intToString(system_identifier).strValue;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &parseContext, parseNodeResult))
	{
		log_error("Failed to set_node_system_identifier of node %d "
				  "from the monitor", nodeId);
		return false;
	}

	/* disconnect from PostgreSQL now */
	pgsql_finish(&monitor->pgsql);

	if (!parseContext.parsedOK)
	{
		log_error(
			"Failed to set node %d sysidentifier to \"%" PRIu64 "\""
																" on the monitor because it returned an unexpected result. "
																"See previous line for details.",
			nodeId, system_identifier);
		return false;
	}

	return true;
}


/*
 * parseCoordinatorNode parses a hostname and a port from the libpq result and
 * writes it to the NodeAddressParseContext pointed to by ctx. This is about
 * the same as parseNode: the only difference is that an empty result set is
 * not an error condition in parseCoordinatorNode.
 */
static void
parseCoordinatorNode(void *ctx, PGresult *result)
{
	NodeAddressParseContext *context = (NodeAddressParseContext *) ctx;
	char *value = NULL;
	int hostLength = 0;

	/* no rows, set the node to NULL, return */
	if (PQntuples(result) == 0)
	{
		context->node = NULL;
		context->parsedOK = true;
		return;
	}

	/* we have rows: we accept only one */
	if (PQntuples(result) != 1)
	{
		log_error("Query returned %d rows, expected 1", PQntuples(result));
		context->parsedOK = false;
		return;
	}

	if (PQnfields(result) != 2)
	{
		log_error("Query returned %d columns, expected 2", PQnfields(result));
		context->parsedOK = false;
		return;
	}

	if (PQgetisnull(result, 0, 0) || PQgetisnull(result, 0, 1))
	{
		log_error("Hostname or port returned by monitor is NULL");
		context->parsedOK = false;
		return;
	}

	value = PQgetvalue(result, 0, 0);
	hostLength = strlcpy(context->node->host, value, _POSIX_HOST_NAME_MAX);
	if (hostLength >= _POSIX_HOST_NAME_MAX)
	{
		log_error("Hostname \"%s\" returned by monitor is %d characters, "
				  "the maximum supported by pg_autoctl is %d",
				  value, hostLength, _POSIX_HOST_NAME_MAX - 1);
		context->parsedOK = false;
		return;
	}

	value = PQgetvalue(result, 0, 1);
	if (!stringToInt(value, &context->node->port) || context->node->port == 0)
	{
		log_error("Invalid port number \"%s\" returned by monitor", value);
		context->parsedOK = false;
	}

	context->parsedOK = true;
}


/*
 * monitor_start_maintenance calls the pgautofailover.start_maintenance(node,
 * port) on the monitor, so that the monitor assigns the MAINTENANCE_STATE at
 * the next call to node_active().
 */
bool
monitor_start_maintenance(Monitor *monitor, int nodeId,
						  ConnectionRetryPolicy *retryPolicy)
{
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_BOOL, false };
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql = "SELECT pgautofailover.start_maintenance($1)";
	int paramCount = 1;
	Oid paramTypes[1] = { INT4OID };
	const char *paramValues[1];

	paramValues[0] = intToString(nodeId).strValue;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		if (monitor_retryable_error(context.sqlstate) &&
			!pgsql_retry_policy_expired(retryPolicy))
		{
			int sleepTimeMs =
				pgsql_compute_connection_retry_sleep_time(retryPolicy);

			log_warn("Failed to start_maintenance of node %d on the monitor, "
					 "retrying in %d ms.",
					 nodeId, sleepTimeMs);

			/* we have milliseconds, pg_usleep() wants microseconds */
			(void) pg_usleep(sleepTimeMs * 1000);

			return monitor_start_maintenance(monitor, nodeId, retryPolicy);
		}

		log_error("Failed to start_maintenance of node %d from the monitor",
				  nodeId);

		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to start_maintenance of node %d from the monitor: "
				  "could not parse monitor's result.", nodeId);
		return false;
	}

	return context.boolVal;
}


/*
 * monitor_stop_maintenance calls the pgautofailover.start_maintenance(node,
 * port) on the monitor, so that the monitor assigns the CATCHINGUP_STATE at
 * the next call to node_active().
 */
bool
monitor_stop_maintenance(Monitor *monitor, int nodeId,
						 ConnectionRetryPolicy *retryPolicy)
{
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_BOOL, false };
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql = "SELECT pgautofailover.stop_maintenance($1)";
	int paramCount = 1;
	Oid paramTypes[1] = { INT4OID };
	const char *paramValues[1];

	paramValues[0] = intToString(nodeId).strValue;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		if (monitor_retryable_error(context.sqlstate) &&
			!pgsql_retry_policy_expired(retryPolicy))
		{
			int sleepTimeMs =
				pgsql_compute_connection_retry_sleep_time(retryPolicy);

			log_warn("Failed to stop_maintenance of node %d on the monitor, "
					 "retrying in %d ms.",
					 nodeId, sleepTimeMs);

			/* we have milliseconds, pg_usleep() wants microseconds */
			(void) pg_usleep(sleepTimeMs * 1000);

			return monitor_stop_maintenance(monitor, nodeId, retryPolicy);
		}

		log_error("Failed to stop_maintenance of node %d from the monitor",
				  nodeId);
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to stop_maintenance of node %d from the monitor: "
				  "could not parse monitor's result.", nodeId);
		return false;
	}

	return context.boolVal;
}


/*
 * monitor_get_notifications listens to notifications from the monitor.
 *
 * Use the select(2) facility to check if something is ready to be read on the
 * PQconn socket for us. When it's the case, return the next notification
 * message from the "state" channel. Other channel messages are sent to the log
 * directly.
 *
 * When the function returns true, it's safe for the caller to sleep, otherwise
 * it's expected that the caller keeps polling the results to drain the queue
 * of notifications received from the previous calls loop.
 */
bool
monitor_get_notifications(Monitor *monitor)
{
	PGconn *connection = monitor->pgsql.connection;
	PGnotify *notify;
	int sock;
	fd_set input_mask;

	if (connection == NULL)
	{
		log_warn("Lost connection.");
		return false;
	}

	/*
	 * It looks like we are violating modularity of the code, when we are
	 * following Postgres documentation and examples:
	 *
	 * https://www.postgresql.org/docs/current/libpq-example.html#LIBPQ-EXAMPLE-2
	 */
	sock = PQsocket(connection);

	if (sock < 0)
	{
		return false;   /* shouldn't happen */
	}
	FD_ZERO(&input_mask);
	FD_SET(sock, &input_mask);

	if (select(sock + 1, &input_mask, NULL, NULL, NULL) < 0)
	{
		/* it might be interrupted by a signal we know how to handle */
		if (asked_to_reload || asked_to_stop || asked_to_stop_fast)
		{
			return true;
		}
		else
		{
			log_warn("Failed to get monitor notifications: select(): %m");
			return false;
		}
	}

	/* Now check for input */
	PQconsumeInput(connection);
	while ((notify = PQnotifies(connection)) != NULL)
	{
		if (strcmp(notify->relname, "log") == 0)
		{
			log_info("%s", notify->extra);
		}
		else if (strcmp(notify->relname, "state") == 0)
		{
			StateNotification notification = { 0 };

			log_debug("received \"%s\"", notify->extra);

			/* the parsing scribbles on the message, make a copy now */
			strlcpy(notification.message, notify->extra, BUFSIZE);

			/* errors are logged by parse_state_notification_message */
			if (parse_state_notification_message(&notification))
			{
				log_info("New state for node %d (%s:%d): %s ➜ %s",
						 notification.nodeId,
						 notification.hostName,
						 notification.nodePort,
						 NodeStateToString(notification.reportedState),
						 NodeStateToString(notification.goalState));
			}
		}
		else
		{
			log_warn("BUG: received unknown notification on channel \"%s\": %s",
					 notify->relname, notify->extra);
		}

		PQfreemem(notify);
		PQconsumeInput(connection);
	}

	return true;
}


/*
 * monitor_wait_until_primary_applied_settings receives notifications and
 * watches for the following "apply_settings" set of transitions:
 *
 *  - primary/apply_settings
 *  - apply_settings/apply_settings
 *  - apply_settings/primary
 *  - primary/primary
 *
 * If we lose the monitor connection while watching for the transition steps
 * then we stop watching. It's a best effort attempt at having the CLI be
 * useful for its user, the main one being the test suite.
 */
bool
monitor_wait_until_primary_applied_settings(Monitor *monitor,
											const char *formation)
{
	PGconn *connection = monitor->pgsql.connection;
	bool applySettingsTransitionInProgress = false;
	bool applySettingsTransitionDone = false;

	uint64_t start = time(NULL);

	if (connection == NULL)
	{
		log_warn("Lost connection.");
		return false;
	}

	log_info("Waiting for the settings to have been applied to "
			 "the monitor and primary node");

	while (!applySettingsTransitionDone)
	{
		/* Sleep until something happens on the connection. */
		int sock;
		fd_set input_mask;
		PGnotify *notify;

		uint64_t now = time(NULL);

		if ((now - start) > PG_AUTOCTL_LISTEN_NOTIFICATIONS_TIMEOUT)
		{
			log_error("Failed to receive monitor's notifications that the "
					  "settings have been applied");
			break;
		}

		/*
		 * It looks like we are violating modularity of the code, when we are
		 * following Postgres documentation and examples:
		 *
		 * https://www.postgresql.org/docs/current/libpq-example.html#LIBPQ-EXAMPLE-2
		 */
		sock = PQsocket(connection);

		if (sock < 0)
		{
			return false;   /* shouldn't happen */
		}
		FD_ZERO(&input_mask);
		FD_SET(sock, &input_mask);

		if (select(sock + 1, &input_mask, NULL, NULL, NULL) < 0)
		{
			log_warn("select() failed: %m");
			return false;
		}

		/* Now check for input */
		PQconsumeInput(connection);
		while ((notify = PQnotifies(connection)) != NULL)
		{
			StateNotification notification = { 0 };

			uint64_t now = time(NULL);

			if ((now - start) > PG_AUTOCTL_LISTEN_NOTIFICATIONS_TIMEOUT)
			{
				/* errors are handled in the main loop */
				break;
			}

			if (strcmp(notify->relname, "state") != 0)
			{
				log_warn("%s: %s", notify->relname, notify->extra);
				continue;
			}

			log_debug("received \"%s\"", notify->extra);

			/* the parsing scribbles on the message, make a copy now */
			strlcpy(notification.message, notify->extra, BUFSIZE);

			/* errors are logged by parse_state_notification_message */
			if (!parse_state_notification_message(&notification))
			{
				log_warn("Failed to parse notification message \"%s\"",
						 notify->extra);
				continue;
			}

			/* filter notifications for our own formation */
			if (strcmp(notification.formationId, formation) != 0)
			{
				continue;
			}

			if (notification.reportedState == PRIMARY_STATE &&
				notification.goalState == APPLY_SETTINGS_STATE)
			{
				applySettingsTransitionInProgress = true;

				log_debug("step 1/4: primary node %d (%s:%d) is assigned \"%s\"",
						  notification.nodeId,
						  notification.hostName,
						  notification.nodePort,
						  NodeStateToString(notification.goalState));
			}
			else if (notification.reportedState == APPLY_SETTINGS_STATE &&
					 notification.goalState == APPLY_SETTINGS_STATE)
			{
				applySettingsTransitionInProgress = true;

				log_debug("step 2/4: primary node %d (%s:%d) reported \"%s\"",
						  notification.nodeId,
						  notification.hostName,
						  notification.nodePort,
						  NodeStateToString(notification.reportedState));
			}
			else if (notification.reportedState == APPLY_SETTINGS_STATE &&
					 notification.goalState == PRIMARY_STATE)
			{
				applySettingsTransitionInProgress = true;

				log_debug("step 3/4: primary node %d (%s:%d) is assigned \"%s\"",
						  notification.nodeId,
						  notification.hostName,
						  notification.nodePort,
						  NodeStateToString(notification.goalState));
			}
			else if (applySettingsTransitionInProgress &&
					 notification.reportedState == PRIMARY_STATE &&
					 notification.goalState == PRIMARY_STATE)
			{
				applySettingsTransitionDone = true;

				log_debug("step 4/4: primary node %d (%s:%d) reported \"%s\"",
						  notification.nodeId,
						  notification.hostName,
						  notification.nodePort,
						  NodeStateToString(notification.reportedState));
			}

			/* prepare next iteration */
			PQfreemem(notify);
			PQconsumeInput(connection);
		}
	}

	/* disconnect from monitor */
	pgsql_finish(&monitor->pgsql);

	return applySettingsTransitionDone;
}


/*
 * monitor_wait_until_some_node_reported_state receives notifications and
 * watches for a new node to be reported with the given targetState.
 *
 * If we lose the monitor connection while watching for the transition steps
 * then we stop watching. It's a best effort attempt at having the CLI be
 * useful for its user, the main one being the test suite.
 */
bool
monitor_wait_until_some_node_reported_state(Monitor *monitor,
											const char *formation,
											int groupId,
											NodeState targetState)
{
	PGconn *connection = monitor->pgsql.connection;
	NodeAddressArray nodesArray = { 0 };
	bool failoverIsDone = false;

	uint64_t start = time(NULL);

	int maxHostNameSize = 5;    /* strlen("Name") + 1, the header */
	char nameSeparatorHeader[BUFSIZE] = { 0 };

	if (connection == NULL)
	{
		log_warn("Lost connection.");
		return false;
	}

	log_info("Listening monitor notifications about state changes "
			 "in formation \"%s\" and group %d",
			 formation, groupId);
	log_info("Following table displays times when notifications are received");

	if (!monitor_get_nodes(monitor,
						   (char *) formation,
						   groupId,
						   &nodesArray))
	{
		/* ignore the error, use an educated guess for the max size */
		log_warn("Failed to get_nodes() on the monitor");
		maxHostNameSize = 25;
	}

	maxHostNameSize = MaxHostNameSizeInNodesArray(&nodesArray);
	(void) prepareHostNameSeparator(nameSeparatorHeader, maxHostNameSize);

	fformat(stdout, "%8s | %3s | %*s | %6s | %19s | %19s\n",
			"Time", "ID", maxHostNameSize, "Host", "Port",
			"Current State", "Assigned State");

	fformat(stdout, "%8s-+-%3s-+-%*s-+-%6s-+-%19s-+-%19s\n",
			"--------", "---", maxHostNameSize, nameSeparatorHeader,
			"------", "-------------------", "-------------------");

	while (!failoverIsDone)
	{
		/* Sleep until something happens on the connection. */
		int sock;
		fd_set input_mask;
		PGnotify *notify;

		uint64_t now = time(NULL);

		if ((now - start) > PG_AUTOCTL_LISTEN_NOTIFICATIONS_TIMEOUT)
		{
			log_error("Failed to receive monitor's notifications");
			break;
		}

		/*
		 * It looks like we are violating modularity of the code, when we are
		 * following Postgres documentation and examples:
		 *
		 * https://www.postgresql.org/docs/current/libpq-example.html#LIBPQ-EXAMPLE-2
		 */
		sock = PQsocket(connection);

		if (sock < 0)
		{
			log_error("Failed to get the connection socket with PQsocket()");
			return false;   /* shouldn't happen */
		}
		FD_ZERO(&input_mask);
		FD_SET(sock, &input_mask);

		if (select(sock + 1, &input_mask, NULL, NULL, NULL) < 0)
		{
			log_warn("select() failed: %m");
			return false;
		}

		/* Now check for input */
		PQconsumeInput(connection);
		while ((notify = PQnotifies(connection)) != NULL)
		{
			StateNotification notification = { 0 };

			uint64_t now = time(NULL);
			char timestring[MAXCTIMESIZE];

			if ((now - start) > PG_AUTOCTL_LISTEN_NOTIFICATIONS_TIMEOUT)
			{
				/* errors are handled in the main loop */
				break;
			}

			if (strcmp(notify->relname, "state") != 0)
			{
				log_warn("%s: %s", notify->relname, notify->extra);
				continue;
			}

			log_debug("received \"%s\"", notify->extra);

			/* the parsing scribbles on the message, make a copy now */
			strlcpy(notification.message, notify->extra, BUFSIZE);

			/* errors are logged by parse_state_notification_message */
			if (!parse_state_notification_message(&notification))
			{
				log_warn("Failed to parse notification message \"%s\"",
						 notify->extra);
				continue;
			}

			/* filter notifications for our own formation */
			if (strcmp(notification.formationId, formation) != 0 ||
				notification.groupId != groupId)
			{
				continue;
			}

			/* format the current time to be user-friendly */
			epoch_to_string(now, timestring);

			/* "Wed Jun 30 21:49:08 1993" -> "21:49:08" */
			timestring[11 + 8] = '\0';

			fformat(stdout, "%8s | %3d | %*s | %6d | %19s | %19s\n",
					timestring + 11,
					notification.nodeId, maxHostNameSize,
					notification.hostName,
					notification.nodePort,
					NodeStateToString(notification.reportedState),
					NodeStateToString(notification.goalState));

			if (notification.goalState == targetState &&
				notification.reportedState == targetState)
			{
				failoverIsDone = true;
				break;
			}

			/* prepare next iteration */
			PQfreemem(notify);
			PQconsumeInput(connection);
		}
	}

	/* disconnect from monitor */
	pgsql_finish(&monitor->pgsql);

	return failoverIsDone;
}


/*
 * monitor_get_extension_version gets the current extension version from the
 * Monitor's Postgres catalog pg_available_extensions.
 */
bool
monitor_get_extension_version(Monitor *monitor, MonitorExtensionVersion *version)
{
	MonitorExtensionVersionParseContext context = { { 0 }, version, false };
	PGSQL *pgsql = &monitor->pgsql;
	const char *sql =
		"SELECT default_version, installed_version"
		"  FROM pg_available_extensions WHERE name = $1";
	int paramCount = 1;
	Oid paramTypes[1] = { TEXTOID };
	const char *paramValues[1];

	paramValues[0] = PG_AUTOCTL_MONITOR_EXTENSION_NAME;

	if (!pgsql_execute_with_params(pgsql, sql,
								   paramCount, paramTypes, paramValues,
								   &context, &parseExtensionVersion))
	{
		log_error("Failed to get the current version for extension \"%s\", "
				  "see previous lines for details.",
				  PG_AUTOCTL_MONITOR_EXTENSION_NAME);
		return false;
	}

	if (!context.parsedOK)
	{
		/* errors have already been logged */
		return false;
	}

	return true;
}


/*
 * parseExtensionVersion parses the resultset of a query on the Postgres
 * pg_available_extension_versions catalogs.
 */
static void
parseExtensionVersion(void *ctx, PGresult *result)
{
	MonitorExtensionVersionParseContext *context =
		(MonitorExtensionVersionParseContext *) ctx;

	char *value = NULL;
	int length = -1;

	/* we have rows: we accept only one */
	if (PQntuples(result) != 1)
	{
		log_error("Query returned %d rows, expected 1", PQntuples(result));
		context->parsedOK = false;
		return;
	}

	if (PQnfields(result) != 2)
	{
		log_error("Query returned %d columns, expected 2", PQnfields(result));
		context->parsedOK = false;
		return;
	}

	if (PQgetisnull(result, 0, 0) || PQgetisnull(result, 0, 1))
	{
		log_error("default_version or installed_version for extension \"%s\" "
				  "is NULL ", PG_AUTOCTL_MONITOR_EXTENSION_NAME);
		context->parsedOK = false;
		return;
	}

	value = PQgetvalue(result, 0, 0);
	length = strlcpy(context->version->defaultVersion, value, BUFSIZE);
	if (length >= BUFSIZE)
	{
		log_error("default_version \"%s\" returned by monitor is %d characters, "
				  "the maximum supported by pg_autoctl is %d",
				  value, length, BUFSIZE - 1);
		context->parsedOK = false;
		return;
	}

	value = PQgetvalue(result, 0, 1);
	length = strlcpy(context->version->installedVersion, value, BUFSIZE);
	if (length >= BUFSIZE)
	{
		log_error("installed_version \"%s\" returned by monitor is %d characters, "
				  "the maximum supported by pg_autoctl is %d",
				  value, length, BUFSIZE - 1);
		context->parsedOK = false;
		return;
	}

	context->parsedOK = true;
}


/*
 * monitor_extension_update executes ALTER EXTENSION ... UPDATE TO ...
 */
bool
monitor_extension_update(Monitor *monitor, const char *targetVersion)
{
	PGSQL *pgsql = &monitor->pgsql;

	return pgsql_alter_extension_update_to(pgsql,
										   PG_AUTOCTL_MONITOR_EXTENSION_NAME,
										   targetVersion);
}


/*
 * monitor_ensure_extension_version checks that we are running a extension
 * version on the monitor that we are compatible with in pg_autoctl. If that's
 * not the case, we blindly try to update the extension version on the monitor
 * to the target version we have in our default.h.
 *
 * NOTE: we don't check here if the update is an upgrade or a downgrade, we
 * rely on the extension's update path to be free of downgrade paths (such as
 * pgautofailover--1.2--1.1.sql).
 */
bool
monitor_ensure_extension_version(Monitor *monitor,
								 MonitorExtensionVersion *version)
{
	const char *extensionVersion = PG_AUTOCTL_EXTENSION_VERSION;
	char envExtensionVersion[MAXPGPATH];

	/* in test environment, we can export any target version we want */
	if (env_exists(PG_AUTOCTL_DEBUG) &&
		env_exists(PG_AUTOCTL_EXTENSION_VERSION_VAR))
	{
		if (!get_env_copy(PG_AUTOCTL_EXTENSION_VERSION_VAR, envExtensionVersion,
						  MAXPGPATH))
		{
			/* errors have already been logged */
			return false;
		}
		extensionVersion = envExtensionVersion;
		log_debug("monitor_ensure_extension_version targets extension "
				  "version \"%s\" - as per environment.",
				  extensionVersion);
	}

	if (!monitor_get_extension_version(monitor, version))
	{
		log_fatal("Failed to check version compatibility with the monitor "
				  "extension \"%s\", see above for details",
				  PG_AUTOCTL_MONITOR_EXTENSION_NAME);
		return false;
	}

	if (strcmp(version->installedVersion, extensionVersion) != 0)
	{
		Monitor dbOwnerMonitor = { 0 };

		log_warn("This version of pg_autoctl requires the extension \"%s\" "
				 "version \"%s\" to be installed on the monitor, current "
				 "version is \"%s\".",
				 PG_AUTOCTL_MONITOR_EXTENSION_NAME,
				 extensionVersion,
				 version->installedVersion);

		/*
		 * Ok, let's try to update the extension then.
		 *
		 * For that we need to connect as the owner of the database, which was
		 * the current $USER at the time of the `pg_autoctl create monitor`
		 * command.
		 */
		if (!prepare_connection_to_current_system_user(monitor,
													   &dbOwnerMonitor))
		{
			log_error("Failed to update extension \"%s\" to version \"%s\": "
					  "failed prepare a connection string to the "
					  "monitor as the database owner",
					  PG_AUTOCTL_MONITOR_EXTENSION_NAME,
					  extensionVersion);
			return false;
		}

		if (!monitor_extension_update(&dbOwnerMonitor,
									  extensionVersion))
		{
			log_fatal("Failed to update extension \"%s\" to version \"%s\" "
					  "on the monitor, see above for details",
					  PG_AUTOCTL_MONITOR_EXTENSION_NAME,
					  extensionVersion);
			return false;
		}

		if (!monitor_get_extension_version(monitor, version))
		{
			log_fatal("Failed to check version compatibility with the monitor "
					  "extension \"%s\", see above for details",
					  PG_AUTOCTL_MONITOR_EXTENSION_NAME);
			return false;
		}

		log_info("Updated extension \"%s\" to version \"%s\"",
				 PG_AUTOCTL_MONITOR_EXTENSION_NAME,
				 version->installedVersion);

		return true;
	}

	/* just mention we checked, and it's ok */
	log_info("The version of extension \"%s\" is \"%s\" on the monitor",
			 PG_AUTOCTL_MONITOR_EXTENSION_NAME, version->installedVersion);

	return true;
}


/*
 * prepare_connection_to_current_system_user changes a given pguri to remove
 * its "user" connection parameter, filling in the pre-allocated keywords and
 * values string arrays.
 *
 * Postgres docs at the following address show 30 connection parameters, so the
 * arrays should allocate 31 entries at least. The last one is going to be
 * NULL.
 *
 *  https://www.postgresql.org/docs/current/libpq-connect.html
 */
static bool
prepare_connection_to_current_system_user(Monitor *source, Monitor *target)
{
	const char *keywords[41] = { 0 };
	const char *values[41] = { 0 };

	char *errmsg;
	PQconninfoOption *conninfo, *option;
	int argCount = 0;

	conninfo = PQconninfoParse(source->pgsql.connectionString, &errmsg);
	if (conninfo == NULL)
	{
		log_error("Failed to parse pguri \"%s\": %s",
				  source->pgsql.connectionString, errmsg);
		PQfreemem(errmsg);
		return false;
	}

	for (option = conninfo; option->keyword != NULL; option++)
	{
		if (strcmp(option->keyword, "user") == 0)
		{
			/* skip the user, $USER is what we want to use here */
			continue;
		}
		else if (option->val)
		{
			if (argCount == 40)
			{
				log_error("Failed to parse Postgres URI options: "
						  "pg_autoctl supports up to 40 options "
						  "and we are parsing more than that.");
				return false;
			}
			keywords[argCount] = option->keyword;
			values[argCount] = option->val;
			++argCount;
		}
	}
	keywords[argCount] = NULL;
	values[argCount] = NULL;

	/* open the connection now, and check that everything is ok */
	target->pgsql.connection = PQconnectdbParams(keywords, values, 0);

	/* Check to see that the backend connection was successfully made */
	if (PQstatus(target->pgsql.connection) != CONNECTION_OK)
	{
		log_error("Connection to database failed: %s",
				  PQerrorMessage(target->pgsql.connection));
		pgsql_finish(&(target->pgsql));
		PQconninfoFree(conninfo);
		return false;
	}

	PQconninfoFree(conninfo);

	return true;
}
