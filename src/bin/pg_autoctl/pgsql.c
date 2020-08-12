/*
 * src/bin/pg_autoctl/pgsql.c
 *	 API for sending SQL commands to a PostgreSQL server
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "postgres_fe.h"
#include "libpq-fe.h"
#include "pqexpbuffer.h"

#include "cli_root.h"
#include "defaults.h"
#include "log.h"
#include "parsing.h"
#include "pgsql.h"
#include "signals.h"
#include "string_utils.h"


#define ERRCODE_DUPLICATE_OBJECT "42710"
#define ERRCODE_DUPLICATE_DATABASE "42P04"

static char * connectionTypeToString(ConnectionType connectionType);
static void log_connection_error(PGconn *connection, int logLevel);
static void pgAutoCtlDefaultNoticeProcessor(void *arg, const char *message);
static void pgAutoCtlDebugNoticeProcessor(void *arg, const char *message);
static PGconn * pgsql_open_connection(PGSQL *pgsql);
static bool pgsql_retry_open_connection(PGSQL *pgsql);
static bool is_response_ok(PGresult *result);
static bool clear_results(PGconn *connection);
static bool pgsql_alter_system_set(PGSQL *pgsql, GUC setting);
static bool pgsql_get_current_setting(PGSQL *pgsql, char *settingName,
									  char **currentValue);
static void parsePgMetadata(void *ctx, PGresult *result);
static void parsePgReachedTargetLSN(void *ctx, PGresult *result);
static void parseReplicationSlotMaintain(void *ctx, PGresult *result);
static void parsePgReachedTargetLSN(void *ctx, PGresult *result);


/*
 * parseSingleValueResult is a ParsePostgresResultCB callback that reads the
 * first column of the first row of the resultset only, and parses the answer
 * into the expected C value, one of type QueryResultType.
 */
void
parseSingleValueResult(void *ctx, PGresult *result)
{
	SingleValueResultContext *context = (SingleValueResultContext *) ctx;

	if (PQntuples(result) == 1)
	{
		char *value = PQgetvalue(result, 0, 0);

		switch (context->resultType)
		{
			case PGSQL_RESULT_BOOL:
			{
				context->boolVal = strcmp(value, "t") == 0;
				context->parsedOk = true;
				break;
			}

			case PGSQL_RESULT_INT:
			{
				if (!stringToInt(value, &context->intVal))
				{
					context->parsedOk = false;
					log_error("Failed to parse int result \"%s\"", value);
				}
				context->parsedOk = true;
				break;
			}

			case PGSQL_RESULT_BIGINT:
			{
				if (!stringToUInt64(value, &context->bigint))
				{
					context->parsedOk = false;
					log_error("Failed to parse uint64_t result \"%s\"", value);
				}
				context->parsedOk = true;
				break;
			}

			case PGSQL_RESULT_STRING:
			{
				context->strVal = strdup(value);
				context->parsedOk = true;
				break;
			}
		}
	}
}


/*
 * pgsql_init initializes a PGSQL struct to connect to the given database
 * URL or connection string.
 */
bool
pgsql_init(PGSQL *pgsql, char *url, ConnectionType connectionType)
{
	pgsql->connectionType = connectionType;
	pgsql->connection = NULL;

	/* set our default retry policy for interactive commands */
	(void) pgsql_set_interactive_retry_policy(&(pgsql->retryPolicy));

	if (validate_connection_string(url))
	{
		/* size of url has already been validated. */
		strlcpy(pgsql->connectionString, url, MAXCONNINFO);
	}
	else
	{
		return false;
	}
	return true;
}


/*
 * pgsql_set_retry_policy sets the retry policy to the given maxT (maximum
 * total time spent retrying), maxR (maximum number of retries, zero when not
 * retrying at all, -1 for unbounded number of retries), and maxSleepTime to
 * cap our exponential backoff with decorrelated jitter computation.
 */
void
pgsql_set_retry_policy(ConnectionRetryPolicy *retryPolicy,
					   int maxT,
					   int maxR,
					   int maxSleepTime,
					   int baseSleepTime)
{
	retryPolicy->maxT = maxT;
	retryPolicy->maxR = maxR;
	retryPolicy->maxSleepTime = maxSleepTime;
	retryPolicy->baseSleepTime = baseSleepTime;

	/* initialize a seed for our random number generator */
	pg_srand48(time(0));
}


/*
 * pgsql_set_default_retry_policy sets the default retry policy: no retry. We
 * use the other default parameters but with a maxR of zero they don't get
 * used.
 *
 * This is the retry policy that prevails in the main keeper loop.
 */
void
pgsql_set_main_loop_retry_policy(ConnectionRetryPolicy *retryPolicy)
{
	(void) pgsql_set_retry_policy(retryPolicy,
								  POSTGRES_PING_RETRY_TIMEOUT,
								  0, /* do not retry by default */
								  POSTGRES_PING_RETRY_CAP_SLEEP_TIME,
								  POSTGRES_PING_RETRY_BASE_SLEEP_TIME);
}


/*
 * pgsql_set_init_retry_policy sets the retry policy to 15 mins of total
 * retrying time, unbounded number of attempts, and up to 2 seconds of sleep
 * time in between attempts.
 *
 * This is the policy that we use in keeper_register_and_init. When using
 * automated provisioning tools and frameworks, it might be that every node is
 * provisionned concurrently and we might try to connect to the monitor before
 * it's ready. In that case we want to retry for a long time.
 */
void
pgsql_set_init_retry_policy(ConnectionRetryPolicy *retryPolicy)
{
	(void) pgsql_set_retry_policy(retryPolicy,
								  POSTGRES_PING_RETRY_TIMEOUT,
								  -1, /* unbounded number of attempts */
								  POSTGRES_PING_RETRY_CAP_SLEEP_TIME,
								  POSTGRES_PING_RETRY_BASE_SLEEP_TIME);
}


/*
 * pgsql_set_interactive_retry_policy sets the retry policy to 2 seconds of
 * total retrying time (or PGCONNECT_TIMEOUT when that's set), unbounded number
 * of attempts, and up to 2 seconds of sleep time in between attempts.
 */
void
pgsql_set_interactive_retry_policy(ConnectionRetryPolicy *retryPolicy)
{
	(void) pgsql_set_retry_policy(retryPolicy,
								  pgconnect_timeout,
								  -1, /* unbounded number of attempts */
								  POSTGRES_PING_RETRY_CAP_SLEEP_TIME,
								  POSTGRES_PING_RETRY_BASE_SLEEP_TIME);
}


/*
 * pgsql_set_monitor_interactive_retry_policy sets the retry policy to 15 mins
 * of total retrying time, unbounded number of attemps, and up to 5 seconds of
 * sleep time in between attemps, starting at 1 second for the first retry.
 *
 * We use this policy in interactive commands when connecting to the monitor,
 * such as when doing pg_autoctl enable|disable maintenance.
 */
void
pgsql_set_monitor_interactive_retry_policy(ConnectionRetryPolicy *retryPolicy)
{
	int cap = 5 * 1000;         /* sleep up to 5s between attempts */
	int sleepTime = 1 * 1000;   /* first retry happens after 1 second */

	(void) pgsql_set_retry_policy(retryPolicy,
								  POSTGRES_PING_RETRY_TIMEOUT,
								  -1, /* unbounded number of attempts */
								  cap,
								  sleepTime);
}


#define min(a, b) (a < b ? a : b)

/*
 * http://c-faq.com/lib/randrange.html
 */
#define random_between(M, N) \
	((M) + pg_lrand48() / (RAND_MAX / ((N) -(M) +1) + 1))

/*
 * pgsql_compute_connection_retry_sleep_time returns how much time to sleep
 * this time, in milliseconds.
 */
int
pgsql_compute_connection_retry_sleep_time(ConnectionRetryPolicy *retryPolicy)
{
	/*
	 * https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/
	 *
	 * Adding jitter is a small change to the sleep function:
	 *
	 *  sleep = random_between(0, min(cap, base * 2^attempt))
	 *
	 * There are a few ways to implement these timed backoff loops. Let’s call
	 * the algorithm above “Full Jitter”, and consider two alternatives. The
	 * first alternative is “Equal Jitter”, where we always keep some of the
	 * backoff and jitter by a smaller amount:
	 *
	 *  temp = min(cap, base * 2^attempt)
	 *  sleep = temp/2 + random_between(0, temp/2)
	 *
	 * The intuition behind this one is that it prevents very short sleeps,
	 * always keeping some of the slow down from the backoff.
	 *
	 * A second alternative is “Decorrelated Jitter”, which is similar to “Full
	 * Jitter”, but we also increase the maximum jitter based on the last
	 * random value.
	 *
	 *  sleep = min(cap, random_between(base, sleep*3))
	 *
	 * Which approach do you think is best?
	 *
	 * The no-jitter exponential backoff approach is the clear loser. [...]
	 *
	 * Of the jittered approaches, “Equal Jitter” is the loser. It does
	 * slightly more work than “Full Jitter”, and takes much longer. The
	 * decision between “Decorrelated Jitter” and “Full Jitter” is less clear.
	 * The “Full Jitter” approach uses less work, but slightly more time. Both
	 * approaches, though, present a substantial decrease in client work and
	 * server load.
	 *
	 * Here we implement "Decorrelated Jitter", which is better in terms of
	 * time spent, something we care to optimize for even when it means more
	 * work on the monitor side.
	 */
	int previousSleepTime = retryPolicy->sleepTime;
	int sleepTime =
		random_between(retryPolicy->baseSleepTime, previousSleepTime * 3);

	retryPolicy->sleepTime = min(retryPolicy->maxSleepTime, sleepTime);

	++(retryPolicy->attempts);

	return retryPolicy->sleepTime;
}


/*
 * pgsql_retry_policy_expired returns true when we should stop retrying, either
 * per the policy (maxR / maxT) or because we received a signal that we have to
 * obey.
 */
bool
pgsql_retry_policy_expired(ConnectionRetryPolicy *retryPolicy)
{
	uint64_t now = time(NULL);

	/* Any signal is reason enough to break out from this retry loop. */
	if (asked_to_quit || asked_to_stop || asked_to_stop_fast || asked_to_reload)
	{
		return true;
	}

	/*
	 * We stop retrying as soon as we have spent all of our time budget or all
	 * of our attempts count budget, whichever comes first.
	 *
	 * maxR = 0 (zero) means no retry at all, checked before the loop
	 * maxR < 0 (zero) means unlimited number of retries
	 */
	if ((now - retryPolicy->startTime) >= retryPolicy->maxT ||
		(retryPolicy->maxR > 0 &&
		 retryPolicy->attempts >= retryPolicy->maxR))
	{
		return true;
	}

	return false;
}


/*
 * Finish a PGSQL client connection.
 */
void
pgsql_finish(PGSQL *pgsql)
{
	if (pgsql->connection != NULL)
	{
		log_debug("Disconnecting from \"%s\"", pgsql->connectionString);
		PQfinish(pgsql->connection);
		pgsql->connection = NULL;

		/*
		 * When we fail to connect, on the way out we call pgsql_finish to
		 * reset the connection to NULL. We still want the callers to be able
		 * to inquire about our connection status, so refrain to reset the
		 * status.
		 */
	}
}


/*
 * connectionTypeToString transforms a connectionType in a string to be used in
 * a user facing message.
 */
static char *
connectionTypeToString(ConnectionType connectionType)
{
	switch (connectionType)
	{
		case PGSQL_CONN_LOCAL:
		{
			return "local Postgres";
		}

		case PGSQL_CONN_MONITOR:
		{
			return "monitor";
		}

		case PGSQL_CONN_COORDINATOR:
		{
			return "coordinator";
		}

		default:
		{
			return "unknown connection type";
		}
	}
}


/*
 * log_connection_error logs the PQerrorMessage from the given connection.
 */
static void
log_connection_error(PGconn *connection, int logLevel)
{
	char *message = connection != NULL ? PQerrorMessage(connection) : NULL;
	char *errorLines[BUFSIZE] = { 0 };
	int lineCount = splitLines(message, errorLines, BUFSIZE);
	int lineNumber = 0;

	/* PQerrorMessage is then "connection pointer is NULL", not helpful */
	if (connection == NULL)
	{
		return;
	}

	for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
	{
		char *line = errorLines[lineNumber];

		if (lineNumber == 0)
		{
			log_level(logLevel, "Connection to database failed: %s", line);
		}
		else
		{
			log_level(logLevel, "%s", line);
		}
	}
}


/*
 * pgsql_open_connection opens a PostgreSQL connection, given a PGSQL client
 * instance. If a connection is already open in the client (it's not NULL),
 * then pgsql_open_connection reuses it and returns it immediately.
 */
static PGconn *
pgsql_open_connection(PGSQL *pgsql)
{
	uint64_t startTime = time(NULL);

	/* we might be connected already */
	if (pgsql->connection != NULL)
	{
		return pgsql->connection;
	}

	log_debug("Connecting to \"%s\"", pgsql->connectionString);

	/* we implement our own retry strategy */
	setenv("PGCONNECT_TIMEOUT", POSTGRES_CONNECT_TIMEOUT, 1);

	/* Make a connection to the database */
	pgsql->connection = PQconnectdb(pgsql->connectionString);

	/* Check to see that the backend connection was successfully made */
	if (PQstatus(pgsql->connection) != CONNECTION_OK)
	{
		/*
		 * Implement the retry policy:
		 *
		 * - for a local Postgres node, we always expect to be able to connect
		 *   and defer managing this at the Postgres Controller level, so we
		 *   never retry.
		 *
		 * - for MONITOR or COORDINATOR (remote) connections, we may have a
		 *   specific policy to follow. In any case we observe first the maxR
		 *   property: maximum retries allowed. When set to zero, we don't
		 *   retry at all.
		 */
		if (pgsql->connectionType == PGSQL_CONN_LOCAL ||
			pgsql->retryPolicy.maxR == 0)
		{
			(void) log_connection_error(pgsql->connection, LOG_ERROR);

			log_error("Failed to connect to %s database at \"%s\", "
					  "see above for details",
					  connectionTypeToString(pgsql->connectionType),
					  pgsql->connectionString);

			pgsql->status = PG_CONNECTION_BAD;

			pgsql_finish(pgsql);
			return NULL;
		}

		/*
		 * If we reach this part of the code, the connectionType is not LOCAL
		 * and the retryPolicy has a non-zero maximum retry count. Let's retry!
		 */
		pgsql->retryPolicy.startTime = startTime;

		if (!pgsql_retry_open_connection(pgsql))
		{
			/* errors have already been logged */
			return NULL;
		}
	}

	pgsql->status = PG_CONNECTION_OK;

	/* set the libpq notice receiver to integrate notifications as warnings. */
	PQsetNoticeProcessor(pgsql->connection,
						 &pgAutoCtlDefaultNoticeProcessor,
						 NULL);

	return pgsql->connection;
}


/*
 * pgsql_retry_open_connection loops over a PQping call until the remote server
 * is ready to accept connections, and then connects to it and returns true
 * when it could connect, false otherwise.
 */
static bool
pgsql_retry_open_connection(PGSQL *pgsql)
{
	bool connectionOk = false;

	PGPing lastWarningMessage = PQPING_OK;
	uint64_t lastWarningTime = 0;

	log_warn("Failed to connect to \"%s\", retrying until "
			 "the server is ready", pgsql->connectionString);

	/* should not happen */
	if (pgsql->retryPolicy.maxR == 0)
	{
		return false;
	}

	/* reset our internal counter before entering the retry loop */
	pgsql->retryPolicy.attempts = 1;

	while (!connectionOk)
	{
		int sleep = 0;

		if (pgsql_retry_policy_expired(&(pgsql->retryPolicy)))
		{
			uint64_t now = time(NULL);

			(void) log_connection_error(pgsql->connection, LOG_ERROR);
			pgsql->status = PG_CONNECTION_BAD;
			pgsql_finish(pgsql);

			log_error("Failed to connect to \"%s\" "
					  "after %d attempts in %d seconds, "
					  "pg_autoctl stops retrying now",
					  pgsql->connectionString,
					  pgsql->retryPolicy.attempts,
					  (int) (now - pgsql->retryPolicy.startTime));

			return false;
		}

		/*
		 * Now compute how much time to wait for this round, and increment how
		 * many times we tried to connect already.
		 */
		sleep = pgsql_compute_connection_retry_sleep_time(&(pgsql->retryPolicy));

		/* we have milliseconds, pg_usleep() wants microseconds */
		(void) pg_usleep(sleep * 1000);

		log_debug("PQping(%s): slept %d ms on attempt %d",
				  pgsql->connectionString,
				  pgsql->retryPolicy.sleepTime,
				  pgsql->retryPolicy.attempts);

		switch (PQping(pgsql->connectionString))
		{
			/*
			 * https://www.postgresql.org/docs/current/libpq-connect.html
			 *
			 * The server is running and appears to be accepting connections.
			 */
			case PQPING_OK:
			{
				log_debug("PQping OK after %d attempts",
						  pgsql->retryPolicy.attempts);

				/*
				 * Ping is now ok, and connection is still NULL because the
				 * first attempt to connect failed. Now is a good time to
				 * establish the connection.
				 *
				 * PQping does not check authentication, so we might still fail
				 * to connect to the server.
				 */
				pgsql->connection = PQconnectdb(pgsql->connectionString);

				if (PQstatus(pgsql->connection) == CONNECTION_OK)
				{
					uint64_t now = time(NULL);

					connectionOk = true;
					pgsql->status = PG_CONNECTION_OK;

					log_info("Successfully connected to \"%s\" "
							 "after %d attempts in %d seconds.",
							 pgsql->connectionString,
							 pgsql->retryPolicy.attempts,
							 (int) (now - pgsql->retryPolicy.startTime));
				}
				else
				{
					uint64_t now = time(NULL);

					lastWarningMessage = PQPING_OK;
					lastWarningTime = now;

					(void) log_connection_error(pgsql->connection, LOG_WARN);
					pgsql_finish(pgsql);

					log_warn("Failed to connect after successful "
							 "ping, please verify authentication "
							 "and logs on the server at \"%s\"",
							 pgsql->connectionString);

					log_warn("Authentication might have failed on the Postgres "
							 "server due to missing HBA rules.");
				}
				break;
			}

			/*
			 * https://www.postgresql.org/docs/current/libpq-connect.html
			 *
			 * The server is running but is in a state that disallows
			 * connections (startup, shutdown, or crash recovery).
			 */
			case PQPING_REJECT:
			{
				uint64_t now = time(NULL);

				if (lastWarningMessage != PQPING_REJECT ||
					(now - lastWarningTime) > 30)
				{
					lastWarningMessage = PQPING_REJECT;
					lastWarningTime = now;

					log_warn(
						"The server at \"%s\" is running but is in a state "
						"that disallows connections (startup, shutdown, or "
						"crash recovery).",
						pgsql->connectionString);
				}
				break;
			}

			/*
			 * https://www.postgresql.org/docs/current/libpq-connect.html
			 *
			 * The server could not be contacted. This might indicate that the
			 * server is not running, or that there is something wrong with the
			 * given connection parameters (for example, wrong port number), or
			 * that there is a network connectivity problem (for example, a
			 * firewall blocking the connection request).
			 */
			case PQPING_NO_RESPONSE:
			{
				uint64_t now = time(NULL);

				if (lastWarningMessage != PQPING_NO_RESPONSE ||
					(now - lastWarningTime) > 30)
				{
					lastWarningMessage = PQPING_NO_RESPONSE;
					lastWarningTime = now;

					log_warn(
						"The server at \"%s\" could not be contacted "
						"after %d attempts in %d seconds. "
						"This might indicate that the server is not running, "
						"or that there is something wrong with the given "
						"connection parameters (for example, wrong port "
						"number), or that there is a network connectivity "
						"problem (for example, a firewall blocking the "
						"connection request).",
						pgsql->connectionString,
						pgsql->retryPolicy.attempts,
						(int) (now - pgsql->retryPolicy.startTime));
				}
				break;
			}

			/*
			 * https://www.postgresql.org/docs/current/libpq-connect.html
			 *
			 * No attempt was made to contact the server, because the supplied
			 * parameters were obviously incorrect or there was some
			 * client-side problem (for example, out of memory).
			 */
			case PQPING_NO_ATTEMPT:
			{
				lastWarningMessage = PQPING_NO_ATTEMPT;
				log_debug("Failed to ping server \"%s\" because of "
						  "client-side problems (no attempt were made)",
						  pgsql->connectionString);
				break;
			}
		}
	}

	if (!connectionOk && pgsql->connection != NULL)
	{
		(void) log_connection_error(pgsql->connection, LOG_ERROR);
		pgsql->status = PG_CONNECTION_BAD;
		pgsql_finish(pgsql);

		return false;
	}

	return true;
}


/*
 * pgAutoCtlDefaultNoticeProcessor is our default PostgreSQL libpq Notice
 * Processing: NOTICE, WARNING, HINT etc are processed as log_warn messages by
 * default.
 */
static void
pgAutoCtlDefaultNoticeProcessor(void *arg, const char *message)
{
	char *m = strdup(message);
	char *lines[BUFSIZE];
	int lineCount = splitLines(m, lines, BUFSIZE);
	int lineNumber = 0;

	for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
	{
		log_warn("%s", lines[lineNumber]);
	}

	free(m);
}


/*
 * pgAutoCtlDebugNoticeProcessor is our PostgreSQL libpq Notice Processing to
 * use when wanting to send NOTICE, WARNING, HINT as log_debug messages.
 */
static void
pgAutoCtlDebugNoticeProcessor(void *arg, const char *message)
{
	char *m = strdup(message);
	char *lines[BUFSIZE];
	int lineCount = splitLines(m, lines, BUFSIZE);
	int lineNumber = 0;

	for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
	{
		log_debug("%s", lines[lineNumber]);
	}

	free(m);
}


/*
 * pgsql_execute opens a connection, runs a given SQL command, and closes
 * the connection again.
 *
 * We avoid persisting connection across multiple commands to simplify error
 * handling.
 */
bool
pgsql_execute(PGSQL *pgsql, const char *sql)
{
	return pgsql_execute_with_params(pgsql, sql, 0, NULL, NULL, NULL, NULL);
}


/*
 * pgsql_execute_with_params opens a connection, runs a given SQL command,
 * and closes the connection again.
 *
 * We avoid persisting connection across multiple commands to simplify error
 * handling.
 */
bool
pgsql_execute_with_params(PGSQL *pgsql, const char *sql, int paramCount,
						  const Oid *paramTypes, const char **paramValues,
						  void *context, ParsePostgresResultCB *parseFun)
{
	PGconn *connection = NULL;
	PGresult *result = NULL;
	char debugParameters[BUFSIZE] = { 0 };

	connection = pgsql_open_connection(pgsql);
	if (connection == NULL)
	{
		return false;
	}

	log_debug("%s;", sql);

	if (paramCount > 0)
	{
		int paramIndex = 0;
		int remainingBytes = BUFSIZE;
		char *writePointer = (char *) debugParameters;

		for (paramIndex = 0; paramIndex < paramCount; paramIndex++)
		{
			int bytesWritten = 0;
			const char *value = paramValues[paramIndex];

			if (paramIndex > 0)
			{
				bytesWritten = sformat(writePointer, remainingBytes, ", ");
				remainingBytes -= bytesWritten;
				writePointer += bytesWritten;
			}

			if (value == NULL)
			{
				bytesWritten = sformat(writePointer, remainingBytes, "NULL");
			}
			else
			{
				bytesWritten =
					sformat(writePointer, remainingBytes, "'%s'", value);
			}
			remainingBytes -= bytesWritten;
			writePointer += bytesWritten;
		}
		log_debug("%s", debugParameters);
	}

	result = PQexecParams(connection, sql,
						  paramCount, paramTypes, paramValues, NULL, NULL, 0);
	if (!is_response_ok(result))
	{
		char *sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
		char *message = PQerrorMessage(connection);
		char *errorLines[BUFSIZE];
		int lineCount = splitLines(message, errorLines, BUFSIZE);
		int lineNumber = 0;

		char *prefix =
			pgsql->connectionType == PGSQL_CONN_MONITOR ? "Monitor" : "Postgres";

		/*
		 * PostgreSQL Error message might contain several lines. Log each of
		 * them as a separate ERROR line here.
		 */
		for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
		{
			log_error("%s %s", prefix, errorLines[lineNumber]);
		}

		log_error("SQL query: %s", sql);
		log_error("SQL params: %s", debugParameters);

		/* now stash away the SQL STATE if any */
		if (context && sqlstate)
		{
			AbstractResultContext *ctx = (AbstractResultContext *) context;

			strlcpy(ctx->sqlstate, sqlstate, SQLSTATE_LENGTH);
		}

		PQclear(result);
		clear_results(connection);
		pgsql_finish(pgsql);

		return false;
	}

	if (parseFun != NULL)
	{
		(*parseFun)(context, result);
	}

	PQclear(result);
	clear_results(connection);

	return true;
}


/*
 * is_response_ok returns whether the query result is a correct response
 * (not an error or failure).
 */
static bool
is_response_ok(PGresult *result)
{
	ExecStatusType resultStatus = PQresultStatus(result);

	return resultStatus == PGRES_SINGLE_TUPLE || resultStatus == PGRES_TUPLES_OK ||
		   resultStatus == PGRES_COMMAND_OK;
}


/*
 * clear_results consumes results on a connection until NULL is returned.
 * If an error is returned it returns false.
 */
static bool
clear_results(PGconn *connection)
{
	bool success = true;

	while (true)
	{
		PGresult *result = PQgetResult(connection);
		if (result == NULL)
		{
			break;
		}

		if (!is_response_ok(result))
		{
			log_error("Failure from Postgres: %s", PQerrorMessage(connection));
			success = false;
		}

		PQclear(result);
	}

	return success;
}


/*
 * pgsql_is_in_recovery connects to PostgreSQL and sets the is_in_recovery
 * boolean to the result of the SELECT pg_is_in_recovery() query. It returns
 * false when something went wrong doing that.
 */
bool
pgsql_is_in_recovery(PGSQL *pgsql, bool *is_in_recovery)
{
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_BOOL, false };
	char *sql = "SELECT pg_is_in_recovery()";

	if (!pgsql_execute_with_params(pgsql, sql, 0, NULL, NULL,
								   &context, &parseSingleValueResult))
	{
		/* errors have been logged already */
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to get result from pg_is_in_recovery()");
		return false;
	}

	*is_in_recovery = context.boolVal;

	return true;
}


/*
 * check_postgresql_settings connects to our local PostgreSQL instance and
 * verifies that our minimal viable configuration is in place by running a SQL
 * query that looks at the current settings.
 */
bool
pgsql_check_postgresql_settings(PGSQL *pgsql, bool isCitusInstanceKind,
								bool *settings_are_ok)
{
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_BOOL, false };
	const char *sql =
		isCitusInstanceKind ?
		CHECK_CITUS_NODE_SETTINGS_SQL : CHECK_POSTGRESQL_NODE_SETTINGS_SQL;

	if (!pgsql_execute_with_params(pgsql, sql, 0, NULL, NULL,
								   &context, &parseSingleValueResult))
	{
		/* errors have been logged already */
		return false;
	}

	if (!context.parsedOk)
	{
		/* errors have already been logged */
		return false;
	}

	*settings_are_ok = context.boolVal;

	return true;
}


/*
 * pgsql_check_monitor_settings connects to the given pgsql instance to check
 * that pgautofailover is part of shared_preload_libraries.
 */
bool
pgsql_check_monitor_settings(PGSQL *pgsql, bool *settings_are_ok)
{
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_BOOL, false };
	const char *sql =
		"select exists(select 1 from "
		"unnest("
		"string_to_array(current_setting('shared_preload_libraries'), ','))"
		" as t(name) "
		"where trim(name) = 'pgautofailover');";

	if (!pgsql_execute_with_params(pgsql, sql, 0, NULL, NULL,
								   &context, &parseSingleValueResult))
	{
		/* errors have been logged already */
		return false;
	}

	if (!context.parsedOk)
	{
		/* errors have already been logged */
		return false;
	}

	*settings_are_ok = context.boolVal;

	return true;
}


/*
 * postgres_sprintf_replicationSlotName prints the replication Slot Name to use
 * for given nodeId in the given slotName buffer of given size.
 */
bool
postgres_sprintf_replicationSlotName(int nodeId, char *slotName, int size)
{
	int bytesWritten =
		sformat(slotName, size, "%s_%d", REPLICATION_SLOT_NAME_DEFAULT, nodeId);

	return bytesWritten <= size;
}


/*
 * pgsql_set_synchronous_standby_names set synchronous_standby_names on the
 * local Postgres to the value computed on the pg_auto_failover monitor.
 */
bool
pgsql_set_synchronous_standby_names(PGSQL *pgsql,
									char *synchronous_standby_names)
{
	char quoted[BUFSIZE] = { 0 };
	GUC setting = { "synchronous_standby_names", quoted };

	if (sformat(quoted, BUFSIZE, "'%s'", synchronous_standby_names) >= BUFSIZE)
	{
		log_error("Failed to apply the synchronous_standby_names value \"%s\": "
				  "pg_autoctl supports values up to %d bytes and this one "
				  "requires %lu bytes",
				  synchronous_standby_names,
				  BUFSIZE,
				  strlen(synchronous_standby_names));
		return false;
	}

	return pgsql_alter_system_set(pgsql, setting);
}


/*
 * pgsql_replication_slot_maintain advances the current confirmed position of
 * the given replication slot up to the given LSN position, create the
 * replication slot if it does not exists yet, and remove the slots that exist
 * in Postgres but are ommited in the given array of slots.
 */
typedef struct ReplicationSlotMaintainContext
{
	char sqlstate[SQLSTATE_LENGTH];
	char operation[NAMEDATALEN];
	char slotName[BUFSIZE];
	char lsn[PG_LSN_MAXLENGTH];
	bool parsedOK;
} ReplicationSlotMaintainContext;


/*
 * pgsql_create_replication_slot tries to create a replication slot on the
 * database identified by a connection string. It's implemented as CREATE IF
 * NOT EXISTS so that it's idempotent and can be retried easily.
 */
bool
pgsql_create_replication_slot(PGSQL *pgsql, const char *slotName)
{
	ReplicationSlotMaintainContext context = { 0 };
	char *sql =
		"SELECT 'create', slot_name, lsn "
		"  FROM pg_create_physical_replication_slot($1) "
		" WHERE NOT EXISTS "
		" (SELECT 1 FROM pg_replication_slots WHERE slot_name = $1)";
	const Oid paramTypes[1] = { TEXTOID };
	const char *paramValues[1] = { slotName };

	log_trace("pgsql_create_replication_slot");

	/*
	 * parseReplicationSlotMaintain will log_info() the replication slot
	 * creation if it happens. When the slot already exists we return 0 row and
	 * remain silent about it.
	 */
	return pgsql_execute_with_params(pgsql, sql, 1, paramTypes, paramValues,
									 &context, parseReplicationSlotMaintain);
}


/*
 * pgsql_drop_replication_slot drops a given replication slot.
 */
bool
pgsql_drop_replication_slot(PGSQL *pgsql, const char *slotName)
{
	char *sql =
		"SELECT pg_drop_replication_slot(slot_name) "
		"  FROM pg_replication_slots "
		" WHERE slot_name = $1";
	Oid paramTypes[1] = { TEXTOID };
	const char *paramValues[1] = { slotName };

	log_info("Drop replication slot \"%s\"", slotName);

	return pgsql_execute_with_params(pgsql, sql,
									 1, paramTypes, paramValues, NULL, NULL);
}


/*
 * BuildNodesArrayValues build the SQL expression to use in a FROM clause to
 * represent the list of other standby nodes from the given nodeArray.
 *
 * Such a list looks either like:
 *
 *   VALUES($1, $2::pg_lsn), ($3, $4)
 *
 * or for an empty set (e.g. when we're the only standby):
 *
 *   SELECT id, lsn
 *     FROM (values (null::int, null::pg_lsn)) as t(id, lsn)
 *    WHERE false
 *
 * We actually need to provide an empty set (0 rows) with columns of the
 * expected data types so that we can join against the existing replication
 * slots and drop them. If the set is empty, we drop all the slots.
 *
 * We return how many parameters we filled in paramTypes and paramValues from
 * the nodeArray.
 */
static int
BuildNodesArrayValues(NodeAddressArray *nodeArray,
					  Oid *paramTypes, char **paramValues,
					  char *values, int size)
{
	char buffer[BUFSIZE];
	int nodeIndex = 0;
	int paramIndex = 0;
	int valuesIndex = 0;

	/*
	 * Build a SQL VALUES statement for every other node registered in the
	 * system, so that we can maintain their LSN position locally on a standby
	 * server.
	 */
	for (nodeIndex = 0; nodeIndex < nodeArray->count; nodeIndex++)
	{
		NodeAddress *node = &(nodeArray->nodes[nodeIndex]);
		IntString nodeIdString = intToString(node->nodeId);

		int idParamIndex = paramIndex;
		int lsnParamIndex = paramIndex + 1;

		paramTypes[idParamIndex] = INT4OID;
		paramValues[idParamIndex] = strdup(nodeIdString.strValue);

		paramTypes[lsnParamIndex] = LSNOID;
		paramValues[lsnParamIndex] = node->lsn;

		valuesIndex += sformat(buffer + valuesIndex, BUFSIZE - valuesIndex,
							   "%s($%d, $%d%s)",
							   valuesIndex == 0 ? "" : ",",

		                       /* we begin at $1 here: intentional off-by-one */
							   idParamIndex + 1, lsnParamIndex + 1,

		                       /* cast only the first row */
							   valuesIndex == 0 ? "::pg_lsn" : "");

		if (valuesIndex > BUFSIZE)
		{
			/* shouldn't happen because we only support up to 12 nodes */
			log_error("Failed to prepare the SQL query for "
					  "pgsql_replication_slot_maintain");
			return false;
		}

		/* prepare next round */
		paramIndex += 2;
	}

	/* when we didn't find any node to process, return our empty set */
	if (paramIndex == 0)
	{
		/* we know it fits, size is BUFSIZE or more */
		sformat(values, size,
				"SELECT id, lsn "
				"FROM (values (null::int, null::pg_lsn)) as t(id, lsn) "
				"where false");
	}
	else
	{
		int bytes = 0;

		bytes = sformat(values, size, "values %s", buffer);

		if (bytes > size)
		{
			/* shouldn't happen because we only support up to 12 nodes */
			log_error("Failed to prepare the SQL query for "
					  "pgsql_replication_slot_maintain");
			return false;
		}
	}
	return paramIndex;
}


/*
 * pgsql_replication_slot_drop_removed drop replication slots that belong to
 * nodes that have been removed. We call that function on the primary, where
 * the slots are maintained by the replication protocol.
 *
 * On the standby nodes, we advance the slots ourselves and use the other
 * function pgsql_replication_slot_maintain which is complete (create, drop,
 * advance).
 */
bool
pgsql_replication_slot_drop_removed(PGSQL *pgsql, NodeAddressArray *nodeArray)
{
	int bytes;
	char sql[2 * BUFSIZE] = { 0 };
	char values[BUFSIZE] = { 0 };

	/* *INDENT-OFF* */
	char *sqlTemplate =
		/*
		 * We could simplify the writing of this query, but we prefer that it
		 * looks as much as possible like the query used in
		 * pgsql_replication_slot_maintain() so that we can maintain both
		 * easily.
		 */
		"WITH nodes(slot_name, lsn) as ("
		" SELECT '" REPLICATION_SLOT_NAME_DEFAULT "_' || id, lsn"
		"   FROM (%s) as sb(id, lsn) "
		"), \n"
		"dropped as ("
		" SELECT slot_name, pg_drop_replication_slot(slot_name) "
		"   FROM pg_replication_slots pgrs LEFT JOIN nodes USING(slot_name) "
		"  WHERE nodes.slot_name IS NULL "
		"    AND (   slot_name ~ '" REPLICATION_SLOT_NAME_PATTERN "' "
		"         OR slot_name ~ '" REPLICATION_SLOT_NAME_DEFAULT "' )"
		"    AND slot_type = 'physical'"
		"), \n"
		"created as ("
		"SELECT c.slot_name, c.lsn "
		"  FROM nodes LEFT JOIN pg_replication_slots pgrs USING(slot_name), "
		"       LATERAL pg_create_physical_replication_slot(slot_name, true) c"
		" WHERE pgrs.slot_name IS NULL "
		") \n"
		"SELECT 'create', slot_name, lsn FROM created "
		" union all "
		"SELECT 'drop', slot_name, NULL::pg_lsn FROM dropped";
	/* *INDENT-ON* */

	Oid paramTypes[NODE_ARRAY_MAX_COUNT * 2] = { 0 };
	const char *paramValues[NODE_ARRAY_MAX_COUNT * 2] = { 0 };
	ReplicationSlotMaintainContext context = { 0 };

	int paramCount = BuildNodesArrayValues(nodeArray,
										   paramTypes, (char **) paramValues,
										   values, BUFSIZE);

	/* add the computed ($1,$2), ... string to the query "template" */
	bytes = sformat(sql, 2 * BUFSIZE, sqlTemplate, values);

	if (bytes > 2 * BUFSIZE)
	{
		/* shouldn't happen because we only support up to 12 nodes */
		log_error("Failed to prepare the SQL query for "
				  "pgsql_replication_slot_maintain");
		return false;
	}

	return pgsql_execute_with_params(pgsql, sql,
									 paramCount, paramTypes, paramValues,
									 &context,
									 parseReplicationSlotMaintain);
}


/*
 * pgsql_replication_slot_maintain creates, drops, and advance replication
 * slots that belong to other standby nodes. We call that function on the
 * standby nodes, where the slots are maintained manually just in case we need
 * them at failover.
 */
bool
pgsql_replication_slot_maintain(PGSQL *pgsql, NodeAddressArray *nodeArray)
{
	int bytes;
	char sql[2 * BUFSIZE] = { 0 };
	char values[BUFSIZE] = { 0 };

	/* *INDENT-OFF* */
	char *sqlTemplate =
		"WITH nodes(slot_name, lsn) as ("
		" SELECT '" REPLICATION_SLOT_NAME_DEFAULT "_' || id, lsn"
		"   FROM (%s) as sb(id, lsn) "
		"), \n"
		"dropped as ("
		" SELECT slot_name, pg_drop_replication_slot(slot_name) "
		"   FROM pg_replication_slots pgrs LEFT JOIN nodes USING(slot_name) "
		"  WHERE nodes.slot_name IS NULL "
		"    AND slot_name ~ '" REPLICATION_SLOT_NAME_PATTERN "' "
		"    AND slot_type = 'physical'"
		"), \n"
		"advanced as ("
		"SELECT a.slot_name, a.end_lsn"
		"  FROM pg_replication_slots s JOIN nodes USING(slot_name), "
		"       LATERAL pg_replication_slot_advance(slot_name, lsn) a"
		" WHERE nodes.lsn <> '0/0' and nodes.lsn >= s.restart_lsn "
		"), \n"
		"created as ("
		"SELECT c.slot_name, c.lsn "
		"  FROM nodes LEFT JOIN pg_replication_slots pgrs USING(slot_name), "
		"       LATERAL pg_create_physical_replication_slot(slot_name, true) c"
		" WHERE pgrs.slot_name IS NULL "
		") \n"
		"SELECT 'create', slot_name, lsn FROM created "
		" union all "
		"SELECT 'drop', slot_name, NULL::pg_lsn FROM dropped "
		" union all "
		"SELECT 'advance', slot_name, end_lsn FROM advanced ";
	/* *INDENT-ON* */

	Oid paramTypes[NODE_ARRAY_MAX_COUNT * 2] = { 0 };
	const char *paramValues[NODE_ARRAY_MAX_COUNT * 2] = { 0 };
	ReplicationSlotMaintainContext context = { 0 };

	int paramCount = BuildNodesArrayValues(nodeArray,
										   paramTypes, (char **) paramValues,
										   values, BUFSIZE);

	/* add the computed ($1,$2), ... string to the query "template" */
	bytes = sformat(sql, 2 * BUFSIZE, sqlTemplate, values);

	if (bytes > 2 * BUFSIZE)
	{
		/* shouldn't happen because we only support up to 12 nodes */
		log_error("Failed to prepare the SQL query for "
				  "pgsql_replication_slot_maintain");
		return false;
	}

	return pgsql_execute_with_params(pgsql, sql,
									 paramCount, paramTypes, paramValues,
									 &context,
									 parseReplicationSlotMaintain);
}


/*
 * parseReplicationSlotMaintain parses the result from a PostgreSQL query
 * fetching two columns from pg_stat_replication: sync_state and currentLSN.
 */
static void
parseReplicationSlotMaintain(void *ctx, PGresult *result)
{
	int rowNumber = 0;
	ReplicationSlotMaintainContext *context =
		(ReplicationSlotMaintainContext *) ctx;

	if (PQnfields(result) != 3)
	{
		log_error("Query returned %d columns, expected 3", PQnfields(result));
		context->parsedOK = false;
		return;
	}

	for (rowNumber = 0; rowNumber < PQntuples(result); rowNumber++)
	{
		/* operation and slotName can't be NULL given how the SQL is built */
		char *operation = PQgetvalue(result, rowNumber, 0);
		char *slotName = PQgetvalue(result, rowNumber, 1);
		char *lsn = PQgetisnull(result, rowNumber, 2) ? ""
					: PQgetvalue(result, rowNumber, 2);

		/* adding or removing another standby node is worthy of a log line */
		if (strcmp(operation, "create") == 0)
		{
			log_info("Creating replication slot \"%s\"", slotName);
		}
		else if (strcmp(operation, "drop") == 0)
		{
			log_info("Dropping replication slot \"%s\"", slotName);
		}
		else
		{
			log_debug("parseReplicationSlotMaintain: %s %s %s",
					  operation, slotName, lsn);
		}
	}

	context->parsedOK = true;
}


/*
 * pgsql_enable_synchronous_replication enables synchronous replication
 * in Postgres such that all writes block post-commit until they are
 * replicated.
 */
bool
pgsql_enable_synchronous_replication(PGSQL *pgsql)
{
	GUC setting = { "synchronous_standby_names", "'*'" };

	log_info("Enabling synchronous replication");

	return pgsql_alter_system_set(pgsql, setting);
}


/*
 * pgsql_disable_synchronous_replication disables synchronous replication
 * in Postgres such that writes do not block if there is no replica.
 */
bool
pgsql_disable_synchronous_replication(PGSQL *pgsql)
{
	GUC setting = { "synchronous_standby_names", "''" };
	char *cancelBlockedStatementsCommand =
		"SELECT pg_cancel_backend(pid) "
		"  FROM pg_stat_activity "
		" WHERE wait_event = 'SyncRep'";

	log_info("Disabling synchronous replication");

	if (!pgsql_alter_system_set(pgsql, setting))
	{
		return false;
	}

	log_debug("Unblocking commands waiting for synchronous replication");

	if (!pgsql_execute(pgsql, cancelBlockedStatementsCommand))
	{
		return false;
	}

	return true;
}


/*
 * pgsql_set_default_transaction_mode_read_only makes it so that the server
 * won't be a target of a connection string requiring target_session_attrs
 * read-write by issuing ALTER SYSTEM SET transaction_mode_read_only TO on;
 *
 */
bool
pgsql_set_default_transaction_mode_read_only(PGSQL *pgsql)
{
	GUC setting = { "default_transaction_read_only", "'on'" };

	log_info("Setting default_transaction_read_only to on");

	return pgsql_alter_system_set(pgsql, setting);
}


/*
 * pgsql_set_default_transaction_mode_read_write makes it so that the server
 * can be a target of a connection string requiring target_session_attrs
 * read-write by issuing ALTER SYSTEM SET transaction_mode_read_only TO off;
 *
 */
bool
pgsql_set_default_transaction_mode_read_write(PGSQL *pgsql)
{
	GUC setting = { "default_transaction_read_only", "'off'" };

	log_info("Setting default_transaction_read_only to off");

	return pgsql_alter_system_set(pgsql, setting);
}


/*
 * pgsql_checkpoint runs a CHECKPOINT command on postgres to trigger a checkpoint.
 */
bool
pgsql_checkpoint(PGSQL *pgsql)
{
	return pgsql_execute(pgsql, "CHECKPOINT");
}


/*
 * pgsql_alter_system_set runs an ALTER SYSTEM SET ... command on Postgres
 * to globally set a GUC and then runs pg_reload_conf() to make existing
 * sessions reload it.
 */
static bool
pgsql_alter_system_set(PGSQL *pgsql, GUC setting)
{
	char command[BUFSIZE];

	sformat(command, sizeof(command),
			"ALTER SYSTEM SET %s TO %s", setting.name, setting.value);

	if (!pgsql_execute(pgsql, command))
	{
		log_error("Failed to set \"%s\" to \"%s\" with ALTER SYSTEM, "
				  "see above for details",
				  setting.name, setting.value);
		return false;
	}

	if (!pgsql_reload_conf(pgsql))
	{
		log_error("Failed to reload Postgres config after ALTER SYSTEM "
				  "to set \"%s\" to \"%s\".",
				  setting.name, setting.value);
		return false;
	}

	return true;
}


/*
 * pgsql_reset_primary_conninfo issues the following SQL commands:
 *
 *   ALTER SYSTEM RESET primary_conninfo;
 *   ALTER SYSTEM RESET primary_slot_name;
 *
 * That's necessary to clean-up the replication settings that pg_basebackup
 * puts in place in postgresql.auto.conf in Postgres 12. We don't reload the
 * configuration after the RESET in that case, because Postgres 12 requires a
 * restart to apply the new setting value anyway.
 */
bool
pgsql_reset_primary_conninfo(PGSQL *pgsql)
{
	char *reset_primary_conninfo = "ALTER SYSTEM RESET primary_conninfo";
	char *reset_primary_slot_name = "ALTER SYSTEM RESET primary_slot_name";

	/* ALTER SYSTEM cannot run inside a transaction block */
	if (!pgsql_execute(pgsql, reset_primary_conninfo))
	{
		return false;
	}

	if (!pgsql_execute(pgsql, reset_primary_slot_name))
	{
		return false;
	}

	return true;
}


/*
 * pgsql_reload_conf causes open sessions to reload the PostgreSQL configuration
 * files.
 */
bool
pgsql_reload_conf(PGSQL *pgsql)
{
	char *sql = "SELECT pg_reload_conf()";

	return pgsql_execute(pgsql, sql);
}


/*
 * pgsql_get_hba_file_path gets the value of the hba_file setting in
 * Postgres or returns false if a failure occurred. The value is copied to
 * the hbaFilePath pointer.
 */
bool
pgsql_get_hba_file_path(PGSQL *pgsql, char *hbaFilePath, int maxPathLength)
{
	char *configValue = NULL;
	int hbaFilePathLength = 0;

	if (!pgsql_get_current_setting(pgsql, "hba_file", &configValue))
	{
		/* pgsql_get_current_setting logs a relevant error */
		return false;
	}

	hbaFilePathLength = strlcpy(hbaFilePath, configValue, maxPathLength);

	if (hbaFilePathLength >= maxPathLength)
	{
		log_error("The hba_file \"%s\" returned by postgres is %d characters, "
				  "the maximum supported by pg_autoctl is %d characters",
				  configValue, hbaFilePathLength, maxPathLength);
		free(configValue);
		return false;
	}

	free(configValue);

	return true;
}


/*
 * pgsql_get_current_setting gets the value of a GUC in Postgres by running
 * SELECT current_setting($settingName), or returns false if a failure occurred.
 *
 * If getting the value was successful, currentValue will point to a copy of the
 * value which should be freed by the caller.
 */
static bool
pgsql_get_current_setting(PGSQL *pgsql, char *settingName, char **currentValue)
{
	SingleValueResultContext context = { 0 };
	char *sql = "SELECT current_setting($1)";
	int paramCount = 1;
	Oid paramTypes[1] = { TEXTOID };
	const char *paramValues[1] = { settingName };

	context.resultType = PGSQL_RESULT_STRING;

	if (!pgsql_execute_with_params(pgsql, sql, paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		/* errors have already been logged */
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to get result from current_setting('%s')", settingName);
		return false;
	}

	*currentValue = context.strVal;

	return true;
}


/*
 * pgsql_create_database issues a CREATE DATABASE statement.
 */
bool
pgsql_create_database(PGSQL *pgsql, const char *dbname, const char *owner)
{
	char command[BUFSIZE];
	char *escapedDBName, *escapedOwner;
	PGconn *connection = NULL;
	PGresult *result = NULL;

	/* open a connection upfront since it is needed by PQescape functions */
	connection = pgsql_open_connection(pgsql);
	if (connection == NULL)
	{
		/* error message was logged in pgsql_open_connection */
		return false;
	}

	/* escape the dbname */
	escapedDBName = PQescapeIdentifier(connection, dbname, strlen(dbname));
	if (escapedDBName == NULL)
	{
		log_error("Failed to create database \"%s\": %s", dbname,
				  PQerrorMessage(connection));
		pgsql_finish(pgsql);
		return false;
	}

	/* escape the username */
	escapedOwner = PQescapeIdentifier(connection, owner, strlen(owner));
	if (escapedOwner == NULL)
	{
		log_error("Failed to create database \"%s\": %s", dbname,
				  PQerrorMessage(connection));
		PQfreemem(escapedDBName);
		pgsql_finish(pgsql);
		return false;
	}

	/* now build the SQL command */
	sformat(command, BUFSIZE,
			"CREATE DATABASE %s WITH OWNER %s",
			escapedDBName,
			escapedOwner);

	log_debug("Running command on Postgres: %s;", command);

	PQfreemem(escapedDBName);
	PQfreemem(escapedOwner);

	result = PQexec(connection, command);

	if (!is_response_ok(result))
	{
		/*
		 * Check if we have a duplicate_database (42P04) error, in which case
		 * it means the user has already been created, accept that as a
		 * non-error, only inform about the situation.
		 */
		char *sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);

		if (strcmp(sqlstate, ERRCODE_DUPLICATE_DATABASE) == 0)
		{
			log_info("The database \"%s\" already exists, skipping.", dbname);
		}
		else
		{
			log_error("Failed to create database \"%s\"[%s]: %s",
					  dbname, sqlstate, PQerrorMessage(connection));
			PQclear(result);
			clear_results(connection);
			pgsql_finish(pgsql);
			return false;
		}
	}

	PQclear(result);
	clear_results(connection);

	return true;
}


/*
 * pgsql_create_extension issues a CREATE EXTENSION statement.
 */
bool
pgsql_create_extension(PGSQL *pgsql, const char *name)
{
	char command[BUFSIZE];
	char *escapedIdentifier;
	PGconn *connection = NULL;
	PGresult *result = NULL;

	/* open a connection upfront since it is needed by PQescape functions */
	connection = pgsql_open_connection(pgsql);
	if (connection == NULL)
	{
		/* error message was logged in pgsql_open_connection */
		return false;
	}

	/* escape the dbname */
	escapedIdentifier = PQescapeIdentifier(connection, name, strlen(name));
	if (escapedIdentifier == NULL)
	{
		log_error("Failed to create extension \"%s\": %s", name,
				  PQerrorMessage(connection));
		pgsql_finish(pgsql);
		return false;
	}

	/* now build the SQL command */
	sformat(command, BUFSIZE, "CREATE EXTENSION IF NOT EXISTS %s CASCADE",
			escapedIdentifier);
	PQfreemem(escapedIdentifier);
	log_debug("Running command on Postgres: %s;", command);

	result = PQexec(connection, command);

	if (!is_response_ok(result))
	{
		/*
		 * Check if we have a duplicate_object (42710) error, in which case
		 * it means the user has already been created, accept that as a
		 * non-error, only inform about the situation.
		 */
		char *sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);

		log_error("Failed to create extension \"%s\"[%s]: %s",
				  name, sqlstate, PQerrorMessage(connection));
		PQclear(result);
		clear_results(connection);
		pgsql_finish(pgsql);
		return false;
	}

	PQclear(result);
	clear_results(connection);

	return true;
}


/*
 * pgsql_create_user creates a user with the given settings.
 *
 * Unlike most functions this function does opens a connection itself
 * because it has some specific requirements around logging, error handling
 * and escaping.
 */
bool
pgsql_create_user(PGSQL *pgsql, const char *userName, const char *password,
				  bool login, bool superuser, bool replication)
{
	PGconn *connection = NULL;
	PGresult *result = NULL;
	PQExpBuffer query = NULL;
	char *escapedIdentifier = NULL;
	PQnoticeProcessor previousNoticeProcessor = NULL;

	/* open a connection upfront since it is needed by PQescape functions */
	connection = pgsql_open_connection(pgsql);
	if (connection == NULL)
	{
		/* error message was logged in pgsql_open_connection */
		return false;
	}

	/* escape the username */
	query = createPQExpBuffer();
	escapedIdentifier = PQescapeIdentifier(connection, userName, strlen(userName));
	if (escapedIdentifier == NULL)
	{
		log_error("Failed to create user \"%s\": %s", userName,
				  PQerrorMessage(connection));
		pgsql_finish(pgsql);
		return false;
	}

	appendPQExpBuffer(query, "CREATE USER %s", escapedIdentifier);
	PQfreemem(escapedIdentifier);

	if (login || superuser || replication || password)
	{
		appendPQExpBufferStr(query, " WITH");
	}
	if (login)
	{
		appendPQExpBufferStr(query, " LOGIN");
	}
	if (superuser)
	{
		appendPQExpBufferStr(query, " SUPERUSER");
	}
	if (replication)
	{
		appendPQExpBufferStr(query, " REPLICATION");
	}
	if (password)
	{
		/* show the statement before we append the password */
		log_debug("Running command on Postgres: %s PASSWORD '*****';", query->data);

		escapedIdentifier = PQescapeLiteral(connection, password, strlen(password));
		if (escapedIdentifier == NULL)
		{
			log_error("Failed to create user \"%s\": %s", userName,
					  PQerrorMessage(connection));
			PQfreemem(escapedIdentifier);
			pgsql_finish(pgsql);
			destroyPQExpBuffer(query);
			return false;
		}

		appendPQExpBuffer(query, " PASSWORD %s", escapedIdentifier);
		PQfreemem(escapedIdentifier);
	}
	else
	{
		log_debug("Running command on Postgres: %s;", query->data);
	}

	/* memory allocation could have failed while building string */
	if (PQExpBufferBroken(query))
	{
		log_error("Failed to allocate memory");
		destroyPQExpBuffer(query);
		pgsql_finish(pgsql);
		return false;
	}

	/*
	 * Set the libpq notice receiver to integrate notifications as debug
	 * message, because when dealing with the citus extension those messages
	 * are not that interesting to our pg_autoctl users frankly:
	 *
	 * NOTICE:  not propagating CREATE ROLE/USER commands to worker nodes
	 * HINT:  Connect to worker nodes directly...
	 */
	previousNoticeProcessor =
		PQsetNoticeProcessor(connection, &pgAutoCtlDebugNoticeProcessor, NULL);

	result = PQexec(connection, query->data);
	destroyPQExpBuffer(query);

	if (!is_response_ok(result))
	{
		/*
		 * Check if we have a duplicate_object (42710) error, in which case
		 * it means the user has already been created, accept that as a
		 * non-error, only inform about the situation.
		 */
		char *sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);

		if (strcmp(sqlstate, ERRCODE_DUPLICATE_OBJECT) == 0)
		{
			log_info("The user \"%s\" already exists, skipping.", userName);
		}
		else
		{
			log_error("Failed to create user \"%s\"[%s]: %s",
					  userName, sqlstate, PQerrorMessage(connection));
			PQclear(result);
			clear_results(connection);
			pgsql_finish(pgsql);
			return false;
		}
	}

	PQclear(result);
	clear_results(connection);

	/* restore the normal notice message processing, if needed. */
	PQsetNoticeProcessor(connection, previousNoticeProcessor, NULL);

	return true;
}


/*
 * pgsql_has_replica returns whether a replica with the given username is active.
 */
bool
pgsql_has_replica(PGSQL *pgsql, char *userName, bool *hasReplica)
{
	SingleValueResultContext context = { { 0 }, PGSQL_RESULT_BOOL, false };

	/*
	 * Check whether there is an entry in pg_stat_replication, which means
	 * there is either a pg_basebackup or streaming replica active. In either
	 * case, it means there is a replica that recently communicated with the
	 * postgres server, which is all we care about for the purpose of this
	 * function.
	 */
	char *sql =
		"SELECT EXISTS (SELECT 1 FROM pg_stat_replication WHERE usename = $1)";

	const Oid paramTypes[1] = { TEXTOID };
	const char *paramValues[1] = { userName };
	int paramCount = 1;

	if (!pgsql_execute_with_params(pgsql, sql, paramCount, paramTypes, paramValues,
								   &context, &parseSingleValueResult))
	{
		/* errors have already been logged */
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to find pg_stat_replication");
		return false;
	}

	*hasReplica = context.boolVal;

	return true;
}


/*
 * hostname_from_uri parses a PostgreSQL connection string URI and returns
 * whether the URL was successfully parsed.
 */
bool
hostname_from_uri(const char *pguri,
				  char *hostname, int maxHostLength, int *port)
{
	int found = 0;
	char *errmsg;
	PQconninfoOption *conninfo, *option;

	conninfo = PQconninfoParse(pguri, &errmsg);
	if (conninfo == NULL)
	{
		log_error("Failed to parse pguri \"%s\": %s", pguri, errmsg);
		PQfreemem(errmsg);
		return false;
	}

	for (option = conninfo; option->keyword != NULL; option++)
	{
		if (strcmp(option->keyword, "host") == 0 ||
			strcmp(option->keyword, "hostaddr") == 0)
		{
			if (option->val)
			{
				int hostNameLength = strlcpy(hostname, option->val, maxHostLength);

				if (hostNameLength >= maxHostLength)
				{
					log_error(
						"The URL \"%s\" contains a hostname of %d characters, "
						"the maximum supported by pg_autoctl is %d characters",
						option->val, hostNameLength, maxHostLength);
					PQconninfoFree(conninfo);
					return false;
				}

				++found;
			}
		}

		if (strcmp(option->keyword, "port") == 0)
		{
			if (option->val)
			{
				/* we expect a single port number in a monitor's URI */
				if (!stringToInt(option->val, port))
				{
					log_error("Failed to parse port number : %s", option->val);

					PQconninfoFree(conninfo);
					return false;
				}
				++found;
			}
			else
			{
				*port = POSTGRES_PORT;
			}
		}

		if (found == 2)
		{
			break;
		}
	}
	PQconninfoFree(conninfo);

	return true;
}


/*
 * validate_connection_string takes a connection string and parses it with
 * libpq, varifying that it's well formed and usable.
 */
bool
validate_connection_string(const char *connectionString)
{
	PQconninfoOption *connInfo = NULL;
	char *errorMessage = NULL;

	int length = strlen(connectionString);
	if (length >= MAXCONNINFO)
	{
		log_error("Connection string \"%s\" is %d "
				  "characters, the maximum supported by pg_autoctl is %d",
				  connectionString, length, MAXCONNINFO);
		return false;
	}

	connInfo = PQconninfoParse(connectionString, &errorMessage);
	if (connInfo == NULL)
	{
		log_error("Failed to parse connection string \"%s\": %s ",
				  connectionString, errorMessage);
		PQfreemem(errorMessage);
		return false;
	}

	PQconninfoFree(connInfo);

	return true;
}


/*
 * pgsql_get_postgres_metadata returns several bits of information that we need
 * to take decisions in the rest of the code:
 *
 *  - pg_is_in_recovery (primary or standby, as expected?)
 *  - sync_state from pg_stat_replication when a primary
 *  - current_lsn from the server
 *  - pg_control_version
 *  - catalog_version_no
 *  - system_identifier
 *
 * With those metadata we can then check our expectations and take decisions in
 * some cases. We can obtain all the metadata that we need easily enough in a
 * single SQL query, so that's what we do.
 */
typedef struct PgMetadata
{
	char sqlstate[6];
	bool parsedOk;
	bool pg_is_in_recovery;
	char syncState[PGSR_SYNC_STATE_MAXLENGTH];
	char currentLSN[PG_LSN_MAXLENGTH];
	PostgresControlData control;
} PgMetadata;


bool
pgsql_get_postgres_metadata(PGSQL *pgsql,
							bool *pg_is_in_recovery,
							char *pgsrSyncState, char *currentLSN,
							PostgresControlData *control)
{
	PgMetadata context = { 0 };

	/* *INDENT-OFF* */
	char *sql =
		/*
		 * Make it so that we still have the current WAL LSN even in the case
		 * where there's no replication slot in use by any standby.
		 *
		 * When on the primary, we might have multiple standby nodes connected.
		 * We're good when at least one of them is either 'sync' or 'quorum'.
		 * We don't check individual replication slots, we take the "best" one
		 * and report that.
		 */
		"select pg_is_in_recovery(),"
		" coalesce(rep.sync_state, '') as sync_state,"
		" case when pg_is_in_recovery()"
		" then coalesce(pg_last_wal_receive_lsn(), pg_last_wal_replay_lsn())"
		" else pg_current_wal_lsn()"
		" end as current_lsn,"
		" pg_control_version, catalog_version_no, system_identifier"
		" from (values(1)) as dummy"
		" full outer join"
		" (select pg_control_version, catalog_version_no, system_identifier "
		"    from pg_control_system()"
		" )"
		" as control on true"
		" full outer join"
		" ("
		"   select sync_state"
		"     from pg_replication_slots slot"
		"     join pg_stat_replication rep"
		"       on rep.pid = slot.active_pid"
		"   where slot_name ~ '" REPLICATION_SLOT_NAME_PATTERN "' "
		"      or slot_name = '" REPLICATION_SLOT_NAME_DEFAULT "' "
		"order by case sync_state "
		"         when 'quorum' then 4 "
		"         when 'sync' then 3 "
		"         when 'potential' then 2 "
		"         when 'async' then 1 "
		"         else 0 end "
		"    desc limit 1"
		" ) "
		"as rep on true";
	/* *INDENT-ON* */

	if (!pgsql_execute_with_params(pgsql, sql, 0, NULL, NULL,
								   &context, &parsePgMetadata))
	{
		/* errors have been logged already */
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to parse the Postgres metadata");
		return false;
	}

	*pg_is_in_recovery = context.pg_is_in_recovery;

	/* the last two metadata items are opt-in */
	if (pgsrSyncState != NULL)
	{
		strlcpy(pgsrSyncState, context.syncState, PGSR_SYNC_STATE_MAXLENGTH);
	}

	if (currentLSN != NULL)
	{
		strlcpy(currentLSN, context.currentLSN, PG_LSN_MAXLENGTH);
	}

	/* overwrite the Control Data fetched from the query */
	*control = context.control;

	pgsql_finish(pgsql);

	return true;
}


/*
 * parsePgMetadata parses the result from a PostgreSQL query fetching
 * two columns from pg_stat_replication: sync_state and currentLSN.
 */
static void
parsePgMetadata(void *ctx, PGresult *result)
{
	PgMetadata *context = (PgMetadata *) ctx;
	char *value;

	if (PQnfields(result) != 6)
	{
		log_error("Query returned %d columns, expected 3", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	if (PQntuples(result) != 1)
	{
		log_error("Query returned %d rows, expected 1", PQntuples(result));
		context->parsedOk = false;
		return;
	}

	context->pg_is_in_recovery = strcmp(PQgetvalue(result, 0, 0), "t") == 0;

	if (!PQgetisnull(result, 0, 1))
	{
		value = PQgetvalue(result, 0, 1);

		strlcpy(context->syncState, value, PGSR_SYNC_STATE_MAXLENGTH);
	}
	else
	{
		context->syncState[0] = '\0';
	}

	if (!PQgetisnull(result, 0, 2))
	{
		value = PQgetvalue(result, 0, 2);

		strlcpy(context->currentLSN, value, PG_LSN_MAXLENGTH);
	}
	else
	{
		context->currentLSN[0] = '\0';
	}

	value = PQgetvalue(result, 0, 3);
	if (!stringToUInt(value, &(context->control.pg_control_version)))
	{
		log_error("Failed to parse pg_control_version \"%s\"", value);
		context->parsedOk = true;
		return;
	}

	value = PQgetvalue(result, 0, 4);
	if (!stringToUInt(value, &(context->control.catalog_version_no)))
	{
		log_error("Failed to parse catalog_version_no \"%s\"", value);
		context->parsedOk = true;
		return;
	}

	value = PQgetvalue(result, 0, 5);
	if (!stringToUInt64(value, &(context->control.system_identifier)))
	{
		log_error("Failed to parse system_identifier \"%s\"", value);
		context->parsedOk = true;
		return;
	}

	context->parsedOk = true;
}


typedef struct PgReachedTargetLSN
{
	char sqlstate[6];
	bool parsedOk;
	bool hasReachedLSN;
	char currentLSN[PG_LSN_MAXLENGTH];
	bool noRows;
} PgReachedTargetLSN;


/*
 * pgsql_one_slot_has_reached_target_lsn checks that at least one replication
 * slot has reached the given LSN already, using the Postgres system views
 * pg_replication_slots and pg_stat_replication on the primary server.
 */
bool
pgsql_one_slot_has_reached_target_lsn(PGSQL *pgsql,
									  char *targetLSN,
									  char *currentLSN,
									  bool *hasReachedLSN)
{
	PgReachedTargetLSN context = { 0 };

	/*
	 * We pick the most advanced LSN reached by the pgautofailover replication
	 * slots, and only consider those that have made it to "sync" or "quorum"
	 * sync_state already. This function is typically called after sync rep has
	 * been enabled on the primary.
	 */

	/* *INDENT-OFF* */
	char *sql =
		"   select $1::pg_lsn <= flush_lsn, flush_lsn "
		"     from pg_replication_slots slot"
		"     join pg_stat_replication rep"
		"       on rep.pid = slot.active_pid"
		"   where (   slot_name ~ '" REPLICATION_SLOT_NAME_PATTERN "' "
		"          or slot_name = '" REPLICATION_SLOT_NAME_DEFAULT "') "
		"     and sync_state in ('sync', 'quorum') "
		"order by flush_lsn desc limit 1";
	/* *INDENT-ON* */

	const Oid paramTypes[1] = { LSNOID };
	const char *paramValues[1] = { targetLSN };

	if (!pgsql_execute_with_params(pgsql, sql, 1, paramTypes, paramValues,
								   &context, &parsePgReachedTargetLSN))
	{
		/* errors have been logged already */
		return false;
	}

	if (!context.parsedOk)
	{
		if (context.noRows)
		{
			log_warn("No standby nodes are connected at the moment");
		}
		else
		{
			log_error("Failed to fetch current flush_lsn location for "
					  "connected standby nodes, see above for details");
		}
		return false;
	}

	*hasReachedLSN = context.hasReachedLSN;
	strlcpy(currentLSN, context.currentLSN, PG_LSN_MAXLENGTH);

	return true;
}


/*
 * pgsql_has_reached_target_lsn calls pg_last_wal_replay_lsn() and compares the
 * current LSN on the system to the given targetLSN.
 */
bool
pgsql_has_reached_target_lsn(PGSQL *pgsql, char *targetLSN,
							 char *currentLSN, bool *hasReachedLSN)
{
	PgReachedTargetLSN context = { 0 };
	char *sql =
		"SELECT $1::pg_lsn <= pg_last_wal_replay_lsn(), "
		" pg_last_wal_replay_lsn()";

	const Oid paramTypes[1] = { LSNOID };
	const char *paramValues[1] = { targetLSN };

	if (!pgsql_execute_with_params(pgsql, sql, 1, paramTypes, paramValues,
								   &context, &parsePgReachedTargetLSN))
	{
		/* errors have been logged already */
		return false;
	}

	if (!context.parsedOk)
	{
		log_error("Failed to get result from pg_last_wal_replay_lsn()");
		return false;
	}

	*hasReachedLSN = context.hasReachedLSN;
	strlcpy(currentLSN, context.currentLSN, PG_LSN_MAXLENGTH);

	return true;
}


/*
 * parsePgMetadata parses the result from a PostgreSQL query fetching
 * two columns from pg_stat_replication: sync_state and currentLSN.
 */
static void
parsePgReachedTargetLSN(void *ctx, PGresult *result)
{
	PgReachedTargetLSN *context = (PgReachedTargetLSN *) ctx;

	if (PQnfields(result) != 2)
	{
		log_error("Query returned %d columns, expected 2", PQnfields(result));
		context->parsedOk = false;
		return;
	}

	if (PQntuples(result) == 0)
	{
		log_debug("parsePgReachedTargetLSN: query returned no rows");
		context->parsedOk = false;
		context->noRows = true;
		return;
	}
	if (PQntuples(result) != 1)
	{
		log_error("Query returned %d rows, expected 1", PQntuples(result));
		context->parsedOk = false;
		return;
	}

	context->hasReachedLSN = strcmp(PQgetvalue(result, 0, 0), "t") == 0;

	if (!PQgetisnull(result, 0, 1))
	{
		char *value = PQgetvalue(result, 0, 1);

		strlcpy(context->currentLSN, value, PG_LSN_MAXLENGTH);
	}
	else
	{
		context->currentLSN[0] = '\0';
	}

	context->parsedOk = true;
}


/*
 * LISTEN/NOTIFY support.
 *
 * First, send a LISTEN command.
 */
bool
pgsql_listen(PGSQL *pgsql, char *channels[])
{
	PGconn *connection = NULL;
	PGresult *result = NULL;
	char sql[BUFSIZE];

	/* open a connection upfront since it is needed by PQescape functions */
	connection = pgsql_open_connection(pgsql);
	if (connection == NULL)
	{
		/* error message was logged in pgsql_open_connection */
		return false;
	}

	for (int i = 0; channels[i]; i++)
	{
		char *channel =
			PQescapeIdentifier(connection, channels[i], strlen(channels[i]));

		if (channel == NULL)
		{
			log_error("Failed to LISTEN \"%s\": %s",
					  channels[i], PQerrorMessage(connection));
			pgsql_finish(pgsql);
			return false;
		}

		sformat(sql, BUFSIZE, "LISTEN %s", channel);

		PQfreemem(channel);

		result = PQexec(connection, sql);

		if (!is_response_ok(result))
		{
			log_error("Failed to LISTEN \"%s\": %s",
					  channels[i], PQerrorMessage(connection));
			PQclear(result);
			clear_results(connection);

			return false;
		}

		PQclear(result);
		clear_results(connection);
	}

	return true;
}


/*
 * pgsql_alter_extension_update_to executes ALTER EXTENSION ... UPDATE TO ...
 */
bool
pgsql_alter_extension_update_to(PGSQL *pgsql,
								const char *extname, const char *version)
{
	int n = 0;
	char command[BUFSIZE];
	char *escapedIdentifier, *escapedVersion;
	PGconn *connection = NULL;
	PGresult *result = NULL;

	/* open a connection upfront since it is needed by PQescape functions */
	connection = pgsql_open_connection(pgsql);
	if (connection == NULL)
	{
		/* error message was logged in pgsql_open_connection */
		return false;
	}

	/* escape the extname */
	escapedIdentifier = PQescapeIdentifier(connection, extname, strlen(extname));
	if (escapedIdentifier == NULL)
	{
		log_error("Failed to update extension \"%s\": %s", extname,
				  PQerrorMessage(connection));
		pgsql_finish(pgsql);
		return false;
	}

	/* escape the version */
	escapedVersion = PQescapeIdentifier(connection, version, strlen(version));
	if (escapedIdentifier == NULL)
	{
		log_error("Failed to update extension \"%s\" to version \"%s\": %s",
				  extname, version,
				  PQerrorMessage(connection));
		pgsql_finish(pgsql);
		return false;
	}

	/* now build the SQL command */
	n = sformat(command, BUFSIZE, "ALTER EXTENSION %s UPDATE TO %s",
				escapedIdentifier, escapedVersion);

	if (n >= BUFSIZE)
	{
		log_error("BUG: pg_autoctl only supports SQL string up to %d bytes, "
				  "a SQL string of %d bytes is needed to "
				  "update the \"%s\" extension.",
				  BUFSIZE, n, extname);
	}

	PQfreemem(escapedIdentifier);
	PQfreemem(escapedVersion);

	log_debug("Running command on Postgres: %s;", command);

	result = PQexec(connection, command);

	if (!is_response_ok(result))
	{
		char *sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);

		log_error("Error %s while running Postgres query: %s: %s",
				  sqlstate, command, PQerrorMessage(connection));
		PQclear(result);
		clear_results(connection);
		pgsql_finish(pgsql);
		return false;
	}

	PQclear(result);
	clear_results(connection);

	return true;
}
