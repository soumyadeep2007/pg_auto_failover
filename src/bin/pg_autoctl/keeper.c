/*
 * src/bin/pg_autoctl/keeper.c
 *     Keeper state functions
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "parson.h"

#include "env_utils.h"
#include "file_utils.h"
#include "keeper.h"
#include "keeper_config.h"
#include "pghba.h"
#include "pgsetup.h"
#include "primary_standby.h"
#include "state.h"


static bool keeper_state_check_postgres(Keeper *keeper,
										PostgresControlData *control);

static void diff_nodesArray(NodeAddressArray *previousNodesArray,
							NodeAddressArray *currentNodesArray,
							NodeAddressArray *diffNodesArray);


/*
 * keeper_init initializes the keeper logic according to the given keeper
 * configuration. It also reads the state file from disk. The state file
 * must be generated before calling keeper_init.
 */
bool
keeper_init(Keeper *keeper, KeeperConfig *config)
{
	PostgresSetup *pgSetup = &(config->pgSetup);

	keeper->config = *config;

	local_postgres_init(&keeper->postgres, pgSetup);

	if (!config->monitorDisabled)
	{
		if (!monitor_init(&keeper->monitor, config->monitor_pguri))
		{
			return false;
		}
	}

	if (!keeper_load_state(keeper))
	{
		/* errors logged in keeper_state_read */
		return false;
	}

	return true;
}


/*
 * keeper_load_state loads the current state of the keeper from the
 * configured state file.
 */
bool
keeper_load_state(Keeper *keeper)
{
	KeeperStateData *keeperState = &(keeper->state);
	KeeperConfig *config = &(keeper->config);

	return keeper_state_read(keeperState, config->pathnames.state);
}


/*
 * keeper_store_state stores the current state of the keeper in the configured
 * state file.
 */
bool
keeper_store_state(Keeper *keeper)
{
	KeeperStateData *keeperState = &(keeper->state);
	KeeperConfig *config = &(keeper->config);

	return keeper_state_write(keeperState, config->pathnames.state);
}


/*
 * keeper_update_state updates the keeper state and immediately writes
 * it to disk.
 */
bool
keeper_update_state(Keeper *keeper, int node_id, int group_id,
					NodeState state, bool update_last_monitor_contact)
{
	KeeperStateData *keeperState = &(keeper->state);
	uint64_t now = time(NULL);

	if (update_last_monitor_contact)
	{
		keeperState->last_monitor_contact = now;
	}
	keeperState->current_node_id = node_id;
	keeperState->current_group = group_id;
	keeperState->assigned_role = state;

	if (!keeper_store_state(keeper))
	{
		/* keeper_state_write logs errors */
		return false;
	}

	log_keeper_state(keeperState);

	return true;
}


/*
 * keeper_should_ensure_current_state returns true when pg_autoctl should
 * ensure that Postgres is running, or not running, depending on the current
 * FSM state, before calling the transition function to the next state.
 *
 * At the moment, the only cases when we DON'T want to ensure the current state
 * are when either the current state or the goal state are one of the following:
 *
 *  - DRAINING
 *  - DEMOTED
 *  - DEMOTE TIMEOUT
 *
 * That's because we would then stop Postgres first when going from DEMOTED to
 * SINGLE, or ensure Postgres is running when going from PRIMARY to DEMOTED.
 * This last example is a split-brain hazard, too.
 */
bool
keeper_should_ensure_current_state_before_transition(Keeper *keeper)
{
	KeeperStateData *keeperState = &(keeper->state);

	if (keeperState->assigned_role == keeperState->current_role)
	{
		/* this function should not be called in that case */
		log_debug("BUG: keeper_should_ensure_current_state_before_transition "
				  "called with assigned role == current role == %s",
				  NodeStateToString(keeperState->assigned_role));
		return false;
	}

	if (keeperState->assigned_role == DRAINING_STATE ||
		keeperState->assigned_role == DEMOTE_TIMEOUT_STATE ||
		keeperState->assigned_role == DEMOTED_STATE)
	{
		/* don't ensure Postgres is running before shutting it down */
		return false;
	}

	if (keeperState->current_role == DRAINING_STATE ||
		keeperState->current_role == DEMOTE_TIMEOUT_STATE ||
		keeperState->current_role == DEMOTED_STATE)
	{
		/* don't ensure Postgres is down before starting it again */
		return false;
	}

	/* in all other cases, yes please ensure the current state */
	return true;
}


/*
 * keeper_ensure_current_state ensures that the current keeper's state is met
 * with the current PostgreSQL status, at minimum that PostgreSQL is running
 * when it's expected to be, etc.
 */
bool
keeper_ensure_current_state(Keeper *keeper)
{
	KeeperStateData *keeperState = &(keeper->state);
	LocalPostgresServer *postgres = &(keeper->postgres);

	log_debug("Ensuring current state: %s",
			  NodeStateToString(keeperState->current_role));

	switch (keeperState->current_role)
	{
		/*
		 * When in primary state, publishing that PostgreSQL is down might
		 * trigger a failover. This is the best solution only when we tried
		 * everything else. So first, retry starting PostgreSQL a couple more
		 * times.
		 *
		 * See configuration parameters:
		 *
		 *   timeout.postgresql_fails_to_start_timeout (default 20s)
		 *   timeout.postgresql_fails_to_start_retries (default 3 times)
		 */
		case PRIMARY_STATE:
		{
			if (!keeper_ensure_postgres_is_running(keeper, true))
			{
				/* errors have already been logged */
				return false;
			}

			/* when a standby has been removed, remove its replication slot */
			return keeper_drop_replication_slots_for_removed_nodes(keeper);
		}

		case SINGLE_STATE:
		{
			/* a single node does not need to maintain retries attempts */
			if (!keeper_ensure_postgres_is_running(keeper, false))
			{
				/* errors have already been logged */
				return false;
			}

			/* when a standby has been removed, remove its replication slot */
			return keeper_drop_replication_slots_for_removed_nodes(keeper);
		}

		/*
		 * In the following states, we don't want to maintain local replication
		 * slots, either because we're a primary and the replication protocol
		 * is taking care of that, or because we're in the middle of changing
		 * the replication upstream node.
		 */
		case WAIT_PRIMARY_STATE:
		case PREP_PROMOTION_STATE:
		case STOP_REPLICATION_STATE:
		{
			return keeper_ensure_postgres_is_running(keeper, false);
		}

		case SECONDARY_STATE:
		{
			bool updateRetries = false;

			if (!keeper_ensure_postgres_is_running(keeper, updateRetries))
			{
				/* errors have already been logged */
				return false;
			}

			/* now ensure progress is made on the replication slots */
			return keeper_maintain_replication_slots(keeper);
		}

		/*
		 * We don't maintain replication slots in CATCHINGUP state. We might
		 * not be in a position to pg_replication_slot_advance() the slot to
		 * the position required by the other standby nodes. Typically we would
		 * get a Postgres error such as the following:
		 *
		 *   cannot advance replication slot to 0/5000060, minimum is 0/6000028
		 */
		case CATCHINGUP_STATE:
		{
			bool updateRetries = false;

			return keeper_ensure_postgres_is_running(keeper, updateRetries);
		}

		case DEMOTED_STATE:
		case DEMOTE_TIMEOUT_STATE:
		case DRAINING_STATE:
		{
			if (postgres->pgIsRunning)
			{
				log_warn("PostgreSQL is running while in state \"%s\", "
						 "stopping PostgreSQL.",
						 NodeStateToString(keeperState->current_role));

				return ensure_postgres_service_is_stopped(postgres);
			}
			return true;
		}

		case MAINTENANCE_STATE:
		default:

			/* nothing to be done here */
			return true;
	}

	/* should never happen */
	return false;
}


/*
 * reportPgIsRunning returns the boolean that we should use to report
 * pgIsRunning to the monitor. When the local PostgreSQL isn't running, we
 * continue reporting that it is for some time, depending on the following
 * configuration parameters:
 *
 *   timeout.postgresql_restart_failure_timeout (default 20s)
 *   timeout.postgresql_restart_failure_max_retries (default 3 times)
 */
bool
ReportPgIsRunning(Keeper *keeper)
{
	KeeperStateData *keeperState = &(keeper->state);
	KeeperConfig *config = &(keeper->config);
	LocalPostgresServer *postgres = &(keeper->postgres);

	int retries = config->postgresql_restart_failure_max_retries;
	int timeout = config->postgresql_restart_failure_timeout;
	uint64_t now = time(NULL);

	if (keeperState->current_role != PRIMARY_STATE)
	{
		/*
		 * Only when in the PRIMARY_STATE is the monitor going to consider a
		 * failover to another node. That's when we should be careful about
		 * having attempted all we could before resigning.
		 *
		 * When we're not in PRIMARY_STATE, then it's ok to immediately report
		 * that PostgreSQL is not running, for immediate decision making on the
		 * monitor's side.
		 */
		return postgres->pgIsRunning;
	}

	/*
	 * Now we know the current state is PRIMARY_STATE. If PostgreSQL is
	 * running, then we simply report that, easy.
	 */
	if (postgres->pgIsRunning)
	{
		return postgres->pgIsRunning;
	}
	else if (postgres->pgFirstStartFailureTs == 0)
	{
		/*
		 * Oh, that's quite strange. It means we just fell in a code path where
		 * pgIsRunning is set to false, and didn't call
		 * ensure_local_postgres_is_running() to restart it.
		 */
		log_debug("ReportPgIsRunning: PostgreSQL is not running, "
				  "and has not been restarted.");

		return postgres->pgIsRunning;
	}
	else if ((now - postgres->pgFirstStartFailureTs) > timeout ||
			 postgres->pgStartRetries >= retries)
	{
		/*
		 * If we fail to restart PostgreSQL 3 times in a row within the last 20
		 * seconds (default values), then report the failure to the monitor for
		 * immediate action (failover, depending on the secondary health &
		 * reporting).
		 */
		log_error("Failed to restart PostgreSQL %d times in the "
				  "last %" PRIu64 "s, reporting PostgreSQL not running to "
								  "the pg_auto_failover monitor.",
				  postgres->pgStartRetries,
				  now - postgres->pgFirstStartFailureTs);

		return false;
	}
	else
	{
		/*
		 * Don't tell the monitor yet, pretend PostgreSQL is running: we might
		 * be able to get the service back running, it's too early for a
		 * failover to be our best option yet.
		 */
		log_warn("PostgreSQL failed to start %d/%d times before "
				 "reporting to the monitor, trying again",
				 postgres->pgStartRetries, retries);

		return true;
	}

	/* we never reach this point. */
}


/*
 * keeper_update_pg_state updates our internal reflection of the PostgreSQL
 * state.
 *
 * It returns true when we could successfully update the PostgreSQL state and
 * everything makes sense, and false when either we failed to update the state,
 * or when there's a serious problem with PostgreSQL and our expections are not
 * met. Examples of returning false include:
 *  - Postgres is running on a different port than configured
 *  - Postgres system identifier has changed from our keeper state
 *  - We failed to obtain the replication state from pg_stat_replication
 */
bool
keeper_update_pg_state(Keeper *keeper)
{
	KeeperStateData *keeperState = &(keeper->state);
	KeeperConfig *config = &(keeper->config);
	PostgresSetup *pgSetup = &(keeper->postgres.postgresSetup);
	LocalPostgresServer *postgres = &(keeper->postgres);
	PGSQL *pgsql = &(postgres->sqlClient);

	bool pgIsNotRunningIsOk = true;

	log_debug("Update local PostgreSQL state");

	*pgSetup = config->pgSetup;

	/* reinitialize the replication state values each time we update */
	postgres->pgIsRunning = false;
	memset(postgres->pgsrSyncState, 0, PGSR_SYNC_STATE_MAXLENGTH);
	strlcpy(postgres->currentLSN, "0/0", sizeof(postgres->currentLSN));

	/*
	 * When PostgreSQL is running, do some extra checks that are going to be
	 * helpful to drive the keeper's FSM decision making.
	 */
	if (pg_setup_is_ready(pgSetup, pgIsNotRunningIsOk))
	{
		char connInfo[MAXCONNINFO];

		if (pgSetup->pidFile.port != config->pgSetup.pgport)
		{
			log_fatal("PostgreSQL is expected to run on port %d, "
					  "found to be running on port %d",
					  config->pgSetup.pgport, pgSetup->pidFile.port);
			return false;
		}

		/* we know now that Postgres is running (and ready) */
		postgres->pgIsRunning = true;

		/*
		 * Reinitialize connection string in case host changed or was first
		 * discovered.
		 */
		pg_setup_get_local_connection_string(pgSetup, connInfo);
		pgsql_init(pgsql, connInfo, PGSQL_CONN_LOCAL);

		/*
		 * Update our Postgres metadata now.
		 *
		 * First, update our cache of file path locations for Postgres
		 * configuration files (including HBA), in case it's been moved to
		 * somewhere else. This could happen when using the debian/ubuntu
		 * pg_createcluster command on an already existing cluster, for
		 * instance.
		 *
		 * Also update our view of pg_is_in_recovery, the replication sync
		 * state when we are a primary with a standby currently using our
		 * replication slot, our current LSN position, and the control data
		 * values (pg_control_version, catalog_version_no, and
		 * system_identifier).
		 */
		if (!pgsql_get_postgres_metadata(pgsql,
										 &pgSetup->is_in_recovery,
										 postgres->pgsrSyncState,
										 postgres->currentLSN,
										 &(pgSetup->control)))
		{
			log_error("Failed to update the local Postgres metadata");
			return false;
		}

		if (!keeper_state_check_postgres(keeper, &(pgSetup->control)))
		{
			log_error("Failed to update the local Postgres metadata, "
					  "see above for details");
			return false;
		}

		/* update the state from the metadata we just obtained */
		keeperState->pg_control_version = pgSetup->control.pg_control_version;
		keeperState->catalog_version_no = pgSetup->control.catalog_version_no;
		keeperState->system_identifier = pgSetup->control.system_identifier;
	}
	else
	{
		/* Postgres is not running. */
		postgres->pgIsRunning = false;

		/*
		 * Cache invalidation: keep the current values we have for the Postgres
		 * characteristics, when we already have them, or fetch them anew using
		 * pg_controldata.
		 */
		if (keeperState->pg_control_version != 0)
		{
			pgSetup->control.pg_control_version = keeperState->pg_control_version;
			pgSetup->control.catalog_version_no = keeperState->catalog_version_no;
			pgSetup->control.system_identifier = keeperState->system_identifier;
		}
		else
		{
			/* Postgres is not running and we have yet to call pg_controldata */
			const bool missingPgdataIsOk = false;

			if (!pg_controldata(pgSetup, missingPgdataIsOk))
			{
				/* errors have already been logged */
				return false;
			}
		}
	}

	/*
	 * In some states, PostgreSQL isn't expected to be running, or not expected
	 * to have a streaming replication to monitor at all.
	 */
	switch (keeperState->current_role)
	{
		case WAIT_PRIMARY_STATE:
		{
			/* we don't expect to have a streaming replica */
			return postgres->pgIsRunning;
		}

		case PRIMARY_STATE:
		{
			/*
			 * We expect to be able to read the current LSN, as always when
			 * Postgres is running, and we also expect replication to be in
			 * place when in PRIMARY state.
			 *
			 * On the primary, we use pg_stat_replication.sync_state to have an
			 * idea of how the replication is going. The query we use in
			 * pgsql_get_postgres_metadata should always return a non-empty
			 * string when we are a PRIMARY and our standby is connected.
			 */

			if (IS_EMPTY_STRING_BUFFER(postgres->pgsrSyncState))
			{
				log_error("Failed to fetch current replication properties "
						  "from standby node: no standby connected in "
						  "pg_stat_replication.");
				log_warn("HINT: check pg_autoctl and Postgres logs on "
						 "standby nodes");
			}

			return postgres->pgIsRunning &&
				   !IS_EMPTY_STRING_BUFFER(postgres->currentLSN) &&
				   !IS_EMPTY_STRING_BUFFER(postgres->pgsrSyncState);
		}

		case SECONDARY_STATE:
		case CATCHINGUP_STATE:
		{
			/* pg_stat_replication.sync_state is only available upstream */
			bool success = postgres->pgIsRunning;

			if (!success)
			{
				log_warn("Postgres is %s and we are in state %s",
						 postgres->pgIsRunning ? "running" : "not running",
						 NodeStateToString(keeperState->current_role));
			}
			return success;
		}

		default:
		{
			/* we don't need to check replication state in those states */
			break;
		}
	}

	return true;
}


/*
 * keeper_state_check_postgres checks that the Postgres control data main
 * properties are still as we expect them to be. At the moment we don't support
 * Postgres minor and major upgrades, and we can't support the system
 * identifier ever changing.
 */
static bool
keeper_state_check_postgres(Keeper *keeper, PostgresControlData *control)
{
	KeeperStateData *keeperState = &(keeper->state);

	/*
	 * We got new control data from either running pg_controldata or connecting
	 * to the local Postgres instance and running our
	 * pgsql_get_postgres_metadata() SQL query. In either case we now need to
	 * update our Keeper State with the control data values.
	 */
	if (keeperState->system_identifier != control->system_identifier &&
		keeperState->system_identifier != 0)
	{
		/*
		 * This is a physical replication deal breaker, so it's mighty
		 * confusing to get that here. In the least, the keeper should get
		 * initialized from scratch again, but basically, we don't know what we
		 * are doing anymore.
		 */
		log_error("Unknown PostgreSQL system identifier: %" PRIu64 ", "
																   "expected %" PRIu64,
				  keeperState->system_identifier,
				  control->system_identifier);
		return false;
	}

	if (keeperState->pg_control_version != control->pg_control_version &&
		keeperState->pg_control_version != 0)
	{
		/* Postgres minor upgrade happened */
		log_warn("PostgreSQL version changed from %u to %u",
				 keeperState->pg_control_version,
				 control->pg_control_version);
	}

	if (keeperState->catalog_version_no != control->catalog_version_no &&
		keeperState->catalog_version_no != 0)
	{
		/* Postgres major upgrade happened */
		log_warn("PostgreSQL catalog version changed from %u to %u",
				 keeperState->catalog_version_no,
				 control->catalog_version_no);
	}

	return true;
}


/*
 * keeper_restart_postgres asks the Postgres controller process to stop and
 * then to restart Postgres.
 *
 * TODO: At the moment we just ensure postgres is stopped, and when that's the
 * case, ensure it's running again. It would arguably be more efficient to send
 * the explicit order to restart Postgres on the Postgres controller process
 * though.
 */
bool
keeper_restart_postgres(Keeper *keeper)
{
	LocalPostgresServer *postgres = &(keeper->postgres);

	log_info("Restarting Postgres at \"%s\"", postgres->postgresSetup.pgdata);

	if (ensure_postgres_service_is_stopped(postgres))
	{
		bool updateRetries = false;

		return keeper_ensure_postgres_is_running(keeper, updateRetries);
	}

	return false;
}


/*
 * keeper_ensure_postgres_is_running ensures that Postgres is running.
 */
bool
keeper_ensure_postgres_is_running(Keeper *keeper, bool updateRetries)
{
	PostgresSetup *pgSetup = &(keeper->postgres.postgresSetup);
	LocalPostgresServer *postgres = &(keeper->postgres);

	if (postgres->pgIsRunning)
	{
		if (updateRetries)
		{
			/* reset PostgreSQL restart failures tracking */
			postgres->pgFirstStartFailureTs = 0;
			postgres->pgStartRetries = 0;
		}
		return true;
	}
	else if (ensure_postgres_service_is_running(postgres))
	{
		log_warn("PostgreSQL was not running, restarted with pid %d",
				 pgSetup->pidFile.pid);
		return true;
	}
	else
	{
		log_error("Failed to restart PostgreSQL, "
				  "see PostgreSQL logs for instance at \"%s\".",
				  pgSetup->pgdata);
		return false;
	}
}


/*
 * keeper_create_self_signed_cert creates SSL self-signed certificates if
 * needed within the current configuration, and then makes sure we update our
 * keeper configuration both in-memory and on-disk with the new normalized
 * filenames of the certificate files created.
 */
bool
keeper_create_self_signed_cert(Keeper *keeper)
{
	KeeperConfig *config = &(keeper->config);
	LocalPostgresServer *postgres = &(keeper->postgres);
	PostgresSetup *pgSetup = &(postgres->postgresSetup);

	if (pgSetup->ssl.createSelfSignedCert &&
		!(file_exists(pgSetup->ssl.serverKey) &&
		  file_exists(pgSetup->ssl.serverCert)))
	{
		if (!pg_create_self_signed_cert(pgSetup, config->hostname))
		{
			log_error("Failed to create SSL self-signed certificate, "
					  "see above for details");
			return false;
		}
	}

	/* ensure the SSL setup is synced with the keeper config */
	config->pgSetup.ssl = pgSetup->ssl;

	/* update our configuration with ssl server.{key,cert} */
	if (!keeper_config_write_file(config))
	{
		/* errors have already been logged */
		return false;
	}
	return true;
}


/*
 * keeper_ensure_configuration updates the Postgres settings to match the
 * pg_autoctl configuration file, if necessary.
 *
 * This includes making sure that the SSL server.{key,cert} files are used in
 * the Postgres configuration, and on a secondary server, that means updating
 * the primary_conninfo connection string to make sure we use the proper
 * sslmode that is setup.
 *
 * This could change anytime with `pg_autoctl enable|disable ssl`. We cache the
 * primary node information in the LocalPostgresServer with the other
 * replicationSource parameters, and the monitor has the responsiblity to
 * instruct us when this cache needs to be invalidated (new primary, etc).
 */
bool
keeper_ensure_configuration(Keeper *keeper, bool postgresNotRunningIsOk)
{
	Monitor *monitor = &(keeper->monitor);
	KeeperConfig *config = &(keeper->config);
	KeeperStateData *state = &(keeper->state);
	LocalPostgresServer *postgres = &(keeper->postgres);
	PostgresSetup *pgSetup = &(postgres->postgresSetup);

	/*
	 * We just reloaded our configuration file from disk. Use the pgSetup from
	 * the new configuration to re-init our local postgres instance
	 * information, including a maybe different SSL setup.
	 */
	postgres->postgresSetup = config->pgSetup;

	if (!keeper_config_update(config,
							  state->current_node_id,
							  state->current_group))
	{
		log_error("Failed to update configuration");
		return false;
	}

	if (!local_postgres_update(postgres, postgresNotRunningIsOk))
	{
		log_error("Failed to reload configuration, see above for details");
		return false;
	}

	/*
	 * We might have to deploy a new Postgres configuration, from new SSL
	 * options being found in our pg_autoctl configuration file or for other
	 * reasons.
	 */
	if (!postgres_add_default_settings(postgres))
	{
		log_warn("Failed to edit Postgres configuration after "
				 "reloading pg_autoctl configuration, "
				 "see above for details");
		return false;
	}

	/*
	 * In pg_auto_failover before version 1.3 we would use pg_basebackup with
	 * the --write-recovery-conf option. Starting with Postgres 12, this option
	 * would cause pg_basebackup to edit postgresql.auto.conf rather than
	 * recovery.conf... meaning that our own setup would not have any effect.
	 *
	 * Now is a good time to clean-up, at reload, and either on a primary or a
	 * secondary, because those parameters should not remain set on a primary
	 * either.
	 *
	 * At start-up, we call reload_configuration() before having contacted the
	 * monitor, so Postgres is not running yet. When Postgres is not running we
	 * can't ALTER SYSTEM to clean-up the primary_conninfo and
	 * primary_slot_name, so we skip that step.
	 *
	 * At start-up we don't need to reload the configuration by calling the SQL
	 * function pg_reload_conf() because Postgres is not running yet, it will
	 * start with the new setup already.
	 */
	if (pg_setup_is_running(pgSetup))
	{
		if (state->pg_control_version >= 1200)
		{
			/* errors are logged already, and non-fatal to this function */
			(void) pgsql_reset_primary_conninfo(&(postgres->sqlClient));
		}

		if (!pgsql_reload_conf(&(postgres->sqlClient)))
		{
			log_warn("Failed to reload Postgres configuration after "
					 "reloading pg_autoctl configuration, "
					 "see above for details");
			return false;
		}
	}

	if (!monitor_init(&(keeper->monitor), config->monitor_pguri))
	{
		/* we tested already in keeper_config_accept_new, but... */
		log_warn("Failed to contact the monitor because its "
				 "URL is invalid, see above for details");
		return false;
	}

	/*
	 * On a standby server we might have to produce a new recovery settings
	 * file (either recovery.conf or postgresql-auto-failover-standby.conf) and
	 * then restart Postgres.
	 */
	if (state->current_role == CATCHINGUP_STATE ||
		state->current_role == SECONDARY_STATE ||
		state->current_role == MAINTENANCE_STATE)
	{
		ReplicationSource *upstream = &(postgres->replicationSource);

		/* either recovery.conf or AUTOCTL_STANDBY_CONF_FILENAME */
		char *relativeConfPathName =
			state->pg_control_version < 1200
			? "recovery.conf"
			: AUTOCTL_STANDBY_CONF_FILENAME;

		char upstreamConfPath[MAXPGPATH] = { 0 };

		char *currentConfContents = NULL;
		long currentConfSize = 0L;

		char *newConfContents = NULL;
		long newConfSize = 0L;

		/* do we have the primaryNode already? */
		if (IS_EMPTY_STRING_BUFFER(upstream->primaryNode.host))
		{
			log_debug("keeper_update_primary_conninfo: monitor_get_primary()");

			if (!monitor_get_primary(monitor,
									 config->formation,
									 state->current_group,
									 &(upstream->primaryNode)))
			{
				log_error("Failed to update primary_conninfo because getting "
						  "the primary node from the monitor failed, "
						  "see above for details");
				return false;
			}
		}

		/*
		 * Read the contents of the standby configuration file now, so that we
		 * only restart Postgres when it has been changed with the next step.
		 */
		join_path_components(upstreamConfPath,
							 pgSetup->pgdata,
							 relativeConfPathName);

		if (file_exists(upstreamConfPath))
		{
			if (!read_file(upstreamConfPath,
						   &currentConfContents,
						   &currentConfSize))
			{
				/* errors have already been logged */
				return false;
			}
		}

		/* prepare a replicationSource from the primary and our SSL setup */
		if (!standby_init_replication_source(postgres,
											 NULL, /* primaryNode is done */
											 PG_AUTOCTL_REPLICA_USERNAME,
											 config->replication_password,
											 config->replication_slot_name,
											 config->maximum_backup_rate,
											 config->backupDirectory,
											 NULL, /* no targetLSN */
											 config->pgSetup.ssl,
											 state->current_node_id))
		{
			/* can't happen at the moment */
			return false;
		}

		/* now setup the replication configuration (primary_conninfo etc) */
		if (!pg_setup_standby_mode(state->pg_control_version,
								   pgSetup->pgdata,
								   pgSetup->pg_ctl,
								   upstream))
		{
			log_error("Failed to setup Postgres as a standby after primary "
					  "connection settings change");
			return false;
		}

		/* restart Postgres only when the configuration file has changed */
		if (!read_file(upstreamConfPath, &newConfContents, &newConfSize))
		{
			/* errors have already been logged */
			return false;
		}

		if (currentConfContents == NULL ||
			strcmp(newConfContents, currentConfContents) != 0)
		{
			log_info("Replication settings at \"%s\" have changed, "
					 "restarting Postgres", upstreamConfPath);

			if (!pgsql_checkpoint(&(postgres->sqlClient)))
			{
				log_warn("Failed to CHECKPOINT before restart, "
						 "see above for details");
			}

			if (!keeper_restart_postgres(keeper))
			{
				log_error("Failed to restart Postgres to enable new "
						  "replication settings, see above for details");
				return false;
			}
		}
	}

	return true;
}


/*
 * keeper_drop_replication_slots_for_removed_nodes drops replication slots that
 * we have on the local Postgres instance when the node is not registered on
 * the monitor anymore (after a pgautofailover.remove_node() has been issued,
 * maybe with the command `pg_autoctl drop node [ --destroy ]`).
 */
bool
keeper_drop_replication_slots_for_removed_nodes(Keeper *keeper)
{
	LocalPostgresServer *postgres = &(keeper->postgres);
	NodeAddressArray *otherNodesArray = &(keeper->otherNodes);

	log_trace("keeper_drop_replication_slots_for_removed_nodes");

	if (!postgres_replication_slot_drop_removed(postgres, otherNodesArray))
	{
		log_error("Failed to maintain replication slots on the local Postgres "
				  "instance, see above for details");
		return false;
	}

	return true;
}


/*
 * keeper_advance_replication_slots loops over the other standby nodes and
 * advance their replication slots up to the current LSN value known by the
 * monitor.
 */
bool
keeper_maintain_replication_slots(Keeper *keeper)
{
	Monitor *monitor = &(keeper->monitor);
	PostgresSetup *pgSetup = &(keeper->postgres.postgresSetup);
	LocalPostgresServer *postgres = &(keeper->postgres);

	/* do we bypass the whole operation? */
	bool bypass = false;

	/*
	 * We would like to maintain replication slots on the standby nodes in a
	 * group by using the function pg_replication_slot_advance(). This ensures
	 * that every node keep a local copy of the WAL files that each other node
	 * might need.
	 *
	 * This WAL files might be necessary in the following two cases:
	 *
	 * - when a primary has been demoted and now rejoins as a secondary, then
	 *   it uses pg_rewind and needs to find the WAL it missed on the new
	 *   primary ; in that case we need the replication slot to have been
	 *   maintained before the failover.
	 *
	 * - when a failover happens with more than one standby, all the standby
	 *   nodes that are not promoted need to follow a new primary node, and for
	 *   that it's best that the new-primary already had a replication slot for
	 *   its new set of standby nodes.
	 *
	 * The pg_replication_slot_advance() function is new in Postgres 11, so we
	 * can't install replication slots on our standby nodes when using Postgres
	 * 10.
	 *
	 * In Postgres 11 and 12, the pg_replication_slot_advance() function has
	 * been buggy for quite some time and prevented WAL recycling on standby
	 * servers, see https://github.com/citusdata/pg_auto_failover/issues/283
	 * for the problem and
	 * https://git.postgresql.org/gitweb/?p=postgresql.git;a=commit;h=b48df81
	 * for the solution.
	 *
	 * The bug fix appears in the minor releases 12.4 and 11.9. Before that, we
	 * disable the slot maintenance feature of pg_auto_failover.
	 */
	if (pgSetup->control.pg_control_version < 1100)
	{
		/* Postgres 10 does not have pg_replication_slot_advance() */
		bypass = true;
	}
	else
	{
		/*
		 * When running our test suite, we still use replication slots in all
		 * versions of Postgres 11 and 12, for testing purposes.
		 *
		 * We estimate that we are in the test suite when both of
		 * PG_AUTOCTL_DEBUG and PG_REGRESS_SOCK_DIR are set.
		 */
		if (env_exists(PG_AUTOCTL_DEBUG) && env_exists("PG_REGRESS_SOCK_DIR"))
		{
			bypass = false;
		}
		else
		{
			bool maintainSlots =
				pg_setup_standby_slot_supported(pgSetup, LOG_TRACE);

			bypass = !maintainSlots;
		}
	}

	/*
	 * Do we actually want to maintain replication slots on this standby node?
	 */
	if (bypass)
	{
		log_trace("Skipping replication slots on a secondary running %d",
				  pgSetup->control.pg_control_version);
		return true;
	}

	if (!monitor_get_other_nodes(monitor, keeper->state.current_node_id,
								 ANY_STATE, &(keeper->otherNodes)))
	{
		/* errors have already been logged */
		return false;
	}

	if (!postgres_replication_slot_maintain(postgres, &(keeper->otherNodes)))
	{
		log_error("Failed to maintain replication slots on the local Postgres "
				  "instance, see above for details");
		return false;
	}

	return true;
}


/*
 * keeper_check_monitor_extension_version checks that the monitor we connect to
 * has an extension version compatible with our expectations.
 */
bool
keeper_check_monitor_extension_version(Keeper *keeper)
{
	Monitor *monitor = &(keeper->monitor);
	MonitorExtensionVersion version = { 0 };

	if (!monitor_get_extension_version(monitor, &version))
	{
		/*
		 * Only output a FATAL error message when we could connect and then
		 * failed to get the monitor extension version that we expect.
		 * Connection failures are retried the usual way.
		 */
		if (monitor->pgsql.status == PG_CONNECTION_OK)
		{
			log_fatal("Failed to check version compatibility with the monitor "
					  "extension \"%s\", see above for details",
					  PG_AUTOCTL_MONITOR_EXTENSION_NAME);
		}
		return false;
	}

	/* from a member of the cluster, we don't try to upgrade the extension */
	if (strcmp(version.installedVersion, PG_AUTOCTL_EXTENSION_VERSION) != 0)
	{
		log_fatal("The monitor at \"%s\" has extension \"%s\" version \"%s\", "
				  "this pg_autoctl version requires version \"%s\".",
				  keeper->config.monitor_pguri,
				  PG_AUTOCTL_MONITOR_EXTENSION_NAME,
				  PG_AUTOCTL_EXTENSION_VERSION,
				  version.installedVersion);
		log_info("Please connect to the monitor node and restart pg_autoctl.");
		return false;
	}
	else
	{
		log_trace("The version of extension \"%s\" is \"%s\" on the monitor",
				  PG_AUTOCTL_MONITOR_EXTENSION_NAME, version.installedVersion);
	}

	return true;
}


/*
 * keeper_init_fsm initializes the keeper's local FSM and does nothing more.
 *
 * It's only intended to be used when we are not using a monitor, which means
 * we're going to expose our FSM driving as an HTTP API, and sit there waiting
 * for orders from another software.
 *
 * The function is modeled to look like keeper_register_and_init with the
 * difference that we don't have a monitor to talk to.
 */
bool
keeper_init_fsm(Keeper *keeper)
{
	KeeperConfig *config = &(keeper->config);
	PostgresSetup *pgSetup = &(config->pgSetup);

	/* fake the initial state provided at monitor registration time */
	MonitorAssignedState assignedState = {
		.nodeId = -1,
		.groupId = -1,
		.state = INIT_STATE
	};

	/*
	 * First try to create our state file. The keeper_state_create_file function
	 * may fail if we have no permission to write to the state file directory
	 * or the disk is full. In that case, we stop before having registered the
	 * local PostgreSQL node to the monitor.
	 */
	if (!keeper_state_create_file(config->pathnames.state))
	{
		log_fatal("Failed to create a state file prior to registering the "
				  "node with the monitor, see above for details");
		return false;
	}

	/* now that we have a state on-disk, finish init of the keeper instance */
	if (!keeper_init(keeper, config))
	{
		return false;
	}

	/* initialize FSM state */
	if (!keeper_update_state(keeper,
							 assignedState.nodeId,
							 assignedState.groupId,
							 assignedState.state,
							 false))
	{
		log_error("Failed to update keepers's state");

		/*
		 * Make sure we don't have a corrupted state file around, that could
		 * prevent trying to init again and cause strange errors.
		 */
		unlink_file(config->pathnames.state);

		return false;
	}

	/*
	 * Leave a track record that we're ok to initialize in PGDATA, so that in
	 * case of `pg_autoctl create` being interrupted, we may resume operations
	 * and accept to work on already running PostgreSQL primary instances.
	 */
	if (!keeper_init_state_create(&(keeper->initState),
								  pgSetup,
								  config->pathnames.init))
	{
		/* errors have already been logged */
		return false;
	}

	return true;
}


/*
 * keeper_register_and_init registers the local node to the pg_auto_failover
 * Monitor in the given initialState, and then create the state on-disk with
 * the assigned goal from the Monitor.
 */
bool
keeper_register_and_init(Keeper *keeper, NodeState initialState)
{
	KeeperConfig *config = &(keeper->config);
	PostgresSetup *pgSetup = &(config->pgSetup);
	KeeperStateInit *initState = &(keeper->initState);

	Monitor *monitor = &(keeper->monitor);

	MonitorAssignedState assignedState = { 0 };
	char expectedSlotName[BUFSIZE] = { 0 };

	/*
	 * First try to create our state file. The keeper_state_create_file function
	 * may fail if we have no permission to write to the state file directory
	 * or the disk is full. In that case, we stop before having registered the
	 * local PostgreSQL node to the monitor.
	 */
	if (!keeper_state_create_file(config->pathnames.state))
	{
		log_fatal("Failed to create a state file prior to registering the "
				  "node with the monitor, see above for details");
		return false;
	}

	/* now that we have a state on-disk, finish init of the keeper instance */
	if (!keeper_init(keeper, config))
	{
		return false;
	}

	/* use a special connection retry policy for initialisation */
	(void) pgsql_set_init_retry_policy(&(keeper->monitor.pgsql));

	/*
	 * When registering to the monitor, we get assigned a nodeId, that we keep
	 * preciously in our state file. We need to have a local version of the
	 * nodeId that is the same one as on the monitor.
	 *
	 * In particular, if we fail to update our local state file, we should
	 * cancel our registration, because there's no way we can re-discover our
	 * nodeId later.
	 *
	 * We register to the monitor in a SQL transaction that we only COMMIT
	 * after we have updated our local state file. If we fail to do so, we
	 * ROLLBACK the transaction, and thus we are not registered to the monitor
	 * and may try again. If we are disconnected halfway through the
	 * registration (process killed, crash, etc), then the server issues a
	 * ROLLBACK for us upon disconnection.
	 */
	if (!pgsql_execute(&(monitor->pgsql), "BEGIN"))
	{
		log_error("Failed to open a SQL transaction to register this node");

		unlink_file(config->pathnames.state);
		return false;
	}

	if (!monitor_register_node(monitor,
							   config->formation,
							   config->name,
							   config->hostname,
							   config->pgSetup.pgport,
							   config->pgSetup.control.system_identifier,
							   config->pgSetup.dbname,
							   config->groupId,
							   initialState,
							   config->pgSetup.pgKind,
							   config->pgSetup.settings.candidatePriority,
							   config->pgSetup.settings.replicationQuorum,
							   &assignedState))
	{
		/* errors have already been logged, remove state file */
		goto rollback;
	}

	/* initialize FSM state from monitor's answer */
	log_info("Writing keeper state file at \"%s\"", config->pathnames.state);

	if (!keeper_update_state(keeper,
							 assignedState.nodeId,
							 assignedState.groupId,
							 assignedState.state,
							 true))
	{
		log_error("Failed to update keepers's state");

		goto rollback;
	}

	/*
	 * Also update the groupId and replication slot name in the
	 * configuration file.
	 */
	(void) postgres_sprintf_replicationSlotName(assignedState.nodeId,
												expectedSlotName,
												sizeof(expectedSlotName));

	/* also update the groupId in the configuration file. */
	if (!keeper_config_update(config,
							  assignedState.nodeId,
							  assignedState.groupId))
	{
		log_error("Failed to update the configuration file with the groupId: %d",
				  assignedState.groupId);
		goto rollback;
	}

	/*
	 * Leave a track record that we're ok to initialize in PGDATA, so that in
	 * case of `pg_autoctl create` being interrupted, we may resume operations
	 * and accept to work on already running PostgreSQL primary instances.
	 */
	if (!keeper_init_state_create(initState,
								  pgSetup,
								  keeper->config.pathnames.init))
	{
		/* errors have already been logged */
		goto rollback;
	}

	if (!pgsql_execute(&(monitor->pgsql), "COMMIT"))
	{
		log_error("Failed to COMMIT register_node transaction on the "
				  "monitor, see above for details");

		/* we can't send a ROLLBACK when a COMMIT failed */
		unlink_file(config->pathnames.state);

		pgsql_finish(&(monitor->pgsql));
		return false;
	}

	pgsql_finish(&(monitor->pgsql));
	return true;

rollback:

	/*
	 * Make sure we don't have a corrupted state file around, that could
	 * prevent trying to init again and cause strange errors.
	 */
	unlink_file(config->pathnames.state);

	if (!pgsql_execute(&(monitor->pgsql), "ROLLBACK"))
	{
		log_error("Failed to ROLLBACK failed register_node transaction "
				  " on the monitor, see above for details.");
	}
	pgsql_finish(&(monitor->pgsql));

	return false;
}


/*
 * keeper_remove removes the local node from the monitor and then removes the
 * local state file.
 */
bool
keeper_remove(Keeper *keeper, KeeperConfig *config, bool ignore_monitor_errors)
{
	int errors = 0;

	/*
	 * We don't require keeper_init() to have been done before calling
	 * keeper_remove, because then we would fail to finish a remove that was
	 * half-done only: keeper_init loads the state from the state file, which
	 * might not exists anymore.
	 *
	 * That said, we're going to require keeper->config to have been set the
	 * usual way, so do that at least.
	 */
	keeper->config = *config;

	if (!config->monitorDisabled)
	{
		if (!monitor_init(&(keeper->monitor), config->monitor_pguri))
		{
			return false;
		}

		log_info("Removing local node from the pg_auto_failover monitor.");

		/*
		 * If the node was already removed from the monitor, then the
		 * monitor_remove function is going to return true here. It means that
		 * we can call `pg_autoctl drop node` again when we removed the node
		 * from the monitor already, but failed to remove the state file.
		 */
		if (!monitor_remove(&(keeper->monitor),
							config->hostname,
							config->pgSetup.pgport))
		{
			/* we already logged about errors */
			errors++;

			if (!ignore_monitor_errors)
			{
				return false;
			}
		}
	}

	log_info("Removing local node state file: \"%s\"", config->pathnames.state);

	if (!unlink_file(config->pathnames.state))
	{
		/* we already logged about errors */
		errors++;
	}

	log_info("Removing local node init state file: \"%s\"",
			 config->pathnames.init);

	if (!unlink_file(config->pathnames.init))
	{
		/* we already logged about errors */
		errors++;
	}

	return errors == 0;
}


/*
 * keeper_state_as_json prepares the current keeper state as a JSON object and
 * copy the string to the given pre-allocated memory area, of given size.
 */
bool
keeper_state_as_json(Keeper *keeper, char *json, int size)
{
	JSON_Value *js = json_value_init_object();
	JSON_Value *jsPostgres = json_value_init_object();
	JSON_Value *jsKeeperState = json_value_init_object();

	JSON_Object *jsRoot = json_value_get_object(js);

	char *serialized_string = NULL;
	int len;

	pg_setup_as_json(&(keeper->postgres.postgresSetup), jsPostgres);
	keeperStateAsJSON(&(keeper->state), jsKeeperState);

	json_object_set_value(jsRoot, "postgres", jsPostgres);
	json_object_set_value(jsRoot, "state", jsKeeperState);

	serialized_string = json_serialize_to_string_pretty(js);

	len = strlcpy(json, serialized_string, size);

	json_free_serialized_string(serialized_string);
	json_value_free(js);

	/* strlcpy returns how many bytes where necessary */
	return len < size;
}


/*
 * keeper_update_group_hba updates updates the HBA file to ensure we have two
 * entries per other node in the group, allowing for both replication
 * connections and connections to the --dbname.
 */
bool
keeper_update_group_hba(Keeper *keeper, NodeAddressArray *diffNodesArray)
{
	LocalPostgresServer *postgres = &(keeper->postgres);
	PostgresSetup *postgresSetup = &(postgres->postgresSetup);
	PGSQL *pgsql = &(postgres->sqlClient);

	char hbaFilePath[MAXPGPATH] = { 0 };
	char *authMethod = pg_setup_get_auth_method(postgresSetup);

	/* early exit when we're alone in the group */
	if (diffNodesArray->count == 0)
	{
		return true;
	}

	sformat(hbaFilePath, MAXPGPATH, "%s/pg_hba.conf", postgresSetup->pgdata);

	if (!pghba_ensure_host_rules_exist(hbaFilePath,
									   diffNodesArray,
									   postgresSetup->ssl.active,
									   postgresSetup->dbname,
									   PG_AUTOCTL_REPLICA_USERNAME,
									   authMethod))
	{
		log_error("Failed to edit HBA file \"%s\" to update rules to current "
				  "list of nodes registered on the monitor",
				  hbaFilePath);
		return false;
	}

	/*
	 * Only reload if Postgres is known to be running. If it's not running, we
	 * edited the HBA and it's going to take effect at next restart of
	 * Postgres, so we're good here.
	 */
	if (pg_setup_is_running(postgresSetup))
	{
		if (!pgsql_reload_conf(pgsql))
		{
			log_error("Failed to reload the postgres configuration after adding "
					  "the standby user to pg_hba");
			return false;
		}
	}

	return true;
}


/*
 * keeper_refresh_other_nodes call pgautofailover.get_other_nodes on the
 * monitor and refreshes the keeper otherNodes array with fresh information,
 * including each node's LSN position.
 *
 * When forceCacheInvalidation is true, instead of trusting our previous value
 * for the keeper otherNodes array, keeper_refresh_other_nodes() instead runs
 * through the whole monitor.get_other_nodes() result and updates HBA rules for
 * all entries there. That's necessary after a pg_basebackup for instance.
 * which will copy over the origin's pg_hba.conf.
 */
bool
keeper_refresh_other_nodes(Keeper *keeper, bool forceCacheInvalidation)
{
	Monitor *monitor = &(keeper->monitor);

	NodeAddressArray *otherNodesArray = &(keeper->otherNodes);
	NodeAddressArray newNodesArray = { 0 };
	NodeAddressArray diffNodesArray = { 0 };

	int nodeId = keeper->state.current_node_id;

	log_trace("keeper_refresh_other_nodes");

	if (!monitor_get_other_nodes(monitor, nodeId, ANY_STATE, &newNodesArray))
	{
		log_error("Failed to get_other_nodes() on the monitor");
		return false;
	}

	/* compute nodes that need an HBA change (new ones, new hostnames) */
	if (forceCacheInvalidation)
	{
		diffNodesArray = newNodesArray;
	}
	else
	{
		(void) diff_nodesArray(otherNodesArray, &newNodesArray, &diffNodesArray);
	}

	/*
	 * When we're alone in the group, and also when there's no change, then we
	 * are done here already.
	 */
	if (newNodesArray.count == 0 || diffNodesArray.count == 0)
	{
		/* refresh the keeper's cache with the current other nodes array */
		keeper->otherNodes = newNodesArray;
		return true;
	}

	log_info("Fetched current list of %d other nodes from the monitor "
			 "to update HBA rules, including %d changes.",
			 newNodesArray.count, diffNodesArray.count);

	/*
	 * We have a new list of other nodes, update the HBA file. We only update
	 * the nodes that we didn't know before, or that have a new host property.
	 */
	if (!keeper_update_group_hba(keeper, &diffNodesArray))
	{
		log_error("Failed to update the HBA entries for the new "
				  "elements in the our formation \"%s\" and group %d",
				  keeper->config.formation,
				  keeper->state.current_group);

		return false;
	}

	/*
	 * In case of success, copy the current nodes array to the keeper's cache.
	 */
	keeper->otherNodes = newNodesArray;

	return true;
}


/*
 * diff_nodesArray computes the array of nodes entries that should be added in
 * the HBA file in the given pre-allocated diffNodesArray parameter. The diff
 * is computed from the keeper's otherNodesArray on the previous round, and the
 * one we just got from the monitor.
 */
static void
diff_nodesArray(NodeAddressArray *previousNodesArray,
				NodeAddressArray *currentNodesArray,
				NodeAddressArray *diffNodesArray)
{
	int prevIndex = 0;
	int currIndex = 0;
	int diffIndex = 0;

	if (previousNodesArray->count == 0)
	{
		/* all the entries are new and we want them in diffNodesArray */
		*diffNodesArray = *currentNodesArray;
		return;
	}

	/* we only care about the nodes in the current nodes array */
	for (currIndex = 0; currIndex < currentNodesArray->count; currIndex++)
	{
		NodeAddress *currNode = &(currentNodesArray->nodes[currIndex]);
		NodeAddress *prevNode = &(previousNodesArray->nodes[prevIndex]);

		/* remember, the input arrays are sorted on nodeId */
		if (currNode->nodeId < prevNode->nodeId)
		{
			diffNodesArray->count++;
			diffNodesArray->nodes[diffIndex++] = *currNode;
		}
		else if (currNode->nodeId == prevNode->nodeId)
		{
			/*
			 * We still have to update our HBA file when the host of a node
			 * that we already have has changed on the monitor.
			 */
			if (!streq(currNode->host, prevNode->host))
			{
				log_debug("Node %d has a new hostname \"%s\"",
						  currNode->nodeId, currNode->host);

				diffNodesArray->count++;
				diffNodesArray->nodes[diffIndex++] = *currNode;
			}

			/*
			 * In any case, if we have more elements in previousNodesArray,
			 * advance our position there.
			 */
			if (prevIndex < previousNodesArray->count)
			{
				prevIndex++;
			}
		}
		else if (currNode->nodeId > prevNode->nodeId)
		{
			/*
			 * All the remaining entries of currentNodesArray are new.
			 *
			 * We might have entries in previousNodesArray that are not found
			 * in currentNodesArray anymore, but we don't know how to clean-up
			 * the HBA file entries at the moment anyway, so we just skip them.
			 */
			diffNodesArray->count++;
			diffNodesArray->nodes[diffIndex++] = *currNode;

			break;
		}
		else
		{
			log_error("BUG in diff_nodesArray!");
			return;
		}
	}
}


/*
 * keeper_set_node_metadata sets a new nodename for the current pg_autoctl node
 * on the monitor. This node might be in an environment where you might get a
 * new IP at reboot, such as in Kubernetes.
 */
bool
keeper_set_node_metadata(Keeper *keeper, KeeperConfig *oldConfig)
{
	KeeperConfig *config = &(keeper->config);
	KeeperStateData keeperState = { 0 };
	int nodeId = -1;

	if (!keeper_state_read(&keeperState, keeper->config.pathnames.state))
	{
		/* errors have already been logged */
		return false;
	}

	nodeId = keeperState.current_node_id;

	if (streq(oldConfig->name, config->name) &&
		streq(oldConfig->hostname, config->hostname) &&
		oldConfig->pgSetup.pgport == config->pgSetup.pgport)
	{
		log_trace("keeper_set_node_metadata: no changes");
		return true;
	}

	if (!monitor_update_node_metadata(&(keeper->monitor),
									  nodeId,
									  keeper->config.name,
									  keeper->config.hostname,
									  keeper->config.pgSetup.pgport))
	{
		/* errors have already been logged */
		return false;
	}

	if (!keeper_config_write_file(&(keeper->config)))
	{
		log_warn("This node nodename has been updated with nodename \"%s\", "
				 "hostname \"%s\" and pgport %d on the monitor "
				 "but could not be update in the local configuration file!",
				 keeper->config.name,
				 keeper->config.hostname,
				 keeper->config.pgSetup.pgport);
		return false;
	}

	if (strneq(oldConfig->name, config->name))
	{
		log_info("Node name is now \"%s\", used to be \"%s\"",
				 config->name, oldConfig->name);
	}

	if (strneq(oldConfig->hostname, config->hostname))
	{
		log_info("Node hostname is now \"%s\", used to be \"%s\"",
				 config->hostname, oldConfig->hostname);
	}

	if (oldConfig->pgSetup.pgport != config->pgSetup.pgport)
	{
		log_info("Node pgport is now %d, used to be %d",
				 config->pgSetup.pgport, oldConfig->pgSetup.pgport);
	}

	return true;
}


/*
 * keeper_config_accept_new returns true when we can accept to RELOAD our
 * current config into the new one that's been editing.
 */
bool
keeper_config_accept_new(Keeper *keeper, KeeperConfig *newConfig)
{
	/* make a copy of the current values before changing them */
	KeeperConfig oldConfig = keeper->config;
	KeeperConfig *config = &(keeper->config);
	bool monitorUpdateNeeded = false;

	/* some elements are not supposed to change on a reload */
	if (strneq(newConfig->pgSetup.pgdata, config->pgSetup.pgdata))
	{
		log_error("Attempt to change postgresql.pgdata from \"%s\" to \"%s\"",
				  config->pgSetup.pgdata, newConfig->pgSetup.pgdata);
		return false;
	}

	/*
	 * Changing the monitor URI. Well it might just be about using a new IP
	 * address, e.g. switching to IPv6, or maybe the monitor has moved to
	 * another hostname.
	 *
	 * We don't check if we are still registered on the new monitor, only that
	 * we can connect. The node_active calls are going to fail it we then
	 * aren't registered anymore.
	 */
	if (strneq(newConfig->monitor_pguri, config->monitor_pguri))
	{
		Monitor monitor = { 0 };

		if (!monitor_init(&monitor, newConfig->monitor_pguri))
		{
			log_fatal("Failed to contact the monitor because its URL is invalid, "
					  "see above for details");
			return false;
		}

		log_info("Reloading configuration: monitor uri is now \"%s\"; "
				 "used to be \"%s\"",
				 newConfig->monitor_pguri, config->monitor_pguri);

		strlcpy(config->monitor_pguri, newConfig->monitor_pguri, MAXCONNINFO);
	}

	/*
	 * We don't support changing formation, group, or hostname mid-flight: we
	 * might have to register again to the monitor to make that work, and in
	 * that case an admin should certainly be doing some offline steps, maybe
	 * even having to `pg_autoctl create` all over again.
	 */
	if (strneq(newConfig->formation, config->formation))
	{
		log_warn("pg_autoctl doesn't know how to change formation at run-time, "
				 "continuing with formation \"%s\".",
				 config->formation);
	}

	/*
	 * Changing the node name is okay, we need to sync the update to the
	 * monitor though.
	 */
	if (strneq(newConfig->name, config->name))
	{
		monitorUpdateNeeded = true;

		log_info("Reloading configuration: node name is now \"%s\"; "
				 "used to be \"%s\"",
				 newConfig->name, config->name);
		strlcpy(config->name, newConfig->name, _POSIX_HOST_NAME_MAX);
	}

	/*
	 * Changing the hostname seems ok, our registration is checked against
	 * formation/groupId/nodeId anyway. The hostname is used so that other
	 * nodes in the network may contact us. Again, it might be a change of
	 * public IP address, e.g. switching to IPv6.
	 *
	 * Changing the hostname in the local configuration file requires also an
	 * update of the metadata on the monitor.
	 */
	if (strneq(newConfig->hostname, config->hostname))
	{
		monitorUpdateNeeded = true;

		log_info("Reloading configuration: hostname is now \"%s\"; "
				 "used to be \"%s\"",
				 newConfig->hostname, config->hostname);
		strlcpy(config->hostname, newConfig->hostname, _POSIX_HOST_NAME_MAX);
	}

	if (monitorUpdateNeeded)
	{
		log_info("Node name or hostname have changed, updating the "
				 "metadata on the monitor");

		if (!keeper_set_node_metadata(keeper, &oldConfig))
		{
			log_error("Failed to update name and hostname on the monitor, "
					  "see above for details");
			return false;
		}
	}

	/*
	 * Changing the replication password? Sure.
	 */
	if (strneq(newConfig->replication_password, config->replication_password))
	{
		log_info("Reloading configuration: replication password has changed");

		/* note: strneq checks args are not NULL, it's safe to proceed */
		strlcpy(config->replication_password,
				newConfig->replication_password,
				MAXCONNINFO);
	}

	/*
	 * Changing replication.maximum_backup_rate.
	 */
	if (strneq(newConfig->maximum_backup_rate, config->maximum_backup_rate))
	{
		log_info("Reloading configuration: "
				 "replication.maximum_backup_rate is now \"%s\"; "
				 "used to be \"%s\"",
				 newConfig->maximum_backup_rate, config->maximum_backup_rate);

		/* note: strneq checks args are not NULL, it's safe to proceed */
		free(config->maximum_backup_rate);
		config->maximum_backup_rate = strdup(newConfig->maximum_backup_rate);
	}

	/*
	 * The backupDirectory can be changed online too.
	 */
	if (strneq(newConfig->backupDirectory, config->backupDirectory))
	{
		log_info("Reloading configuration: "
				 "replication.backup_directory is now \"%s\"; "
				 "used to be \"%s\"",
				 newConfig->backupDirectory, config->backupDirectory);

		strlcpy(config->backupDirectory, newConfig->backupDirectory, MAXPGPATH);
	}

	/*
	 * And now the timeouts. Of course we support changing them at run-time.
	 */
	if (newConfig->network_partition_timeout != config->network_partition_timeout)
	{
		log_info("Reloading configuration: timeout.network_partition_timeout "
				 "is now %d; used to be %d",
				 newConfig->network_partition_timeout,
				 config->network_partition_timeout);

		config->network_partition_timeout =
			newConfig->network_partition_timeout;
	}

	if (newConfig->prepare_promotion_catchup != config->prepare_promotion_catchup)
	{
		log_info("Reloading configuration: timeout.prepare_promotion_catchup "
				 "is now %d; used to be %d",
				 newConfig->prepare_promotion_catchup,
				 config->prepare_promotion_catchup);

		config->prepare_promotion_catchup =
			newConfig->prepare_promotion_catchup;
	}

	if (newConfig->prepare_promotion_walreceiver != config->prepare_promotion_walreceiver)
	{
		log_info(
			"Reloading configuration: timeout.prepare_promotion_walreceiver "
			"is now %d; used to be %d",
			newConfig->prepare_promotion_walreceiver,
			config->prepare_promotion_walreceiver);

		config->prepare_promotion_walreceiver =
			newConfig->prepare_promotion_walreceiver;
	}

	if (newConfig->postgresql_restart_failure_timeout !=
		config->postgresql_restart_failure_timeout)
	{
		log_info(
			"Reloading configuration: timeout.postgresql_restart_failure_timeout "
			"is now %d; used to be %d",
			newConfig->postgresql_restart_failure_timeout,
			config->postgresql_restart_failure_timeout);

		config->postgresql_restart_failure_timeout =
			newConfig->postgresql_restart_failure_timeout;
	}

	if (newConfig->postgresql_restart_failure_max_retries !=
		config->postgresql_restart_failure_max_retries)
	{
		log_info(
			"Reloading configuration: retries.postgresql_restart_failure_max_retries "
			"is now %d; used to be %d",
			newConfig->postgresql_restart_failure_max_retries,
			config->postgresql_restart_failure_max_retries);

		config->postgresql_restart_failure_max_retries =
			newConfig->postgresql_restart_failure_max_retries;
	}

	/* we can change any SSL related setup options at runtime */
	return config_accept_new_ssloptions(&(config->pgSetup),
										&(newConfig->pgSetup));
}
