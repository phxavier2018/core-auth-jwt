/* Copyright (c) 2011-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "restrict-access.h"
#include "mail-storage-service.h"
#include "mail-storage-settings.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "master-connection.h"

static struct mail_storage_service_ctx *storage_service;

static void client_connected(struct master_service_connection *conn)
{
	master_service_client_connection_accept(conn);

	if (!master_connection_create(conn, storage_service)) {
		i_error("indexer-worker must be configured with client_limit=1");
		i_close_fd(&conn->fd);
		master_service_client_connection_destroyed(master_service);
	}
}

static void drop_privileges(void)
{
	struct restrict_access_settings set;
	const char *error;

	/* by default we don't drop any privileges, but keep running as root. */
	restrict_access_get_env(&set);
	if (set.uid != 0) {
		/* open config connection before dropping privileges */
		struct master_service_settings_input input;
		struct master_service_settings_output output;

		i_zero(&input);
		input.service = "indexer-worker";
		(void)master_service_settings_read(master_service,
						   &input, &output, &error);
	}
	restrict_access_by_env(RESTRICT_ACCESS_FLAG_ALLOW_ROOT, NULL);
}

int main(int argc, char *argv[])
{
	enum master_service_flags service_flags = 0;
	enum mail_storage_service_flags storage_service_flags =
		MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP |
		MAIL_STORAGE_SERVICE_FLAG_TEMP_PRIV_DROP |
		MAIL_STORAGE_SERVICE_FLAG_NO_IDLE_TIMEOUT;
	int c;

	master_service = master_service_init("indexer-worker", service_flags,
					     &argc, &argv, "D");
	while ((c = master_getopt(master_service)) > 0) {
		switch (c) {
		case 'D':
			storage_service_flags |=
				MAIL_STORAGE_SERVICE_FLAG_ENABLE_CORE_DUMPS;
			break;
		default:
			return FATAL_DEFAULT;
		}
	}

	drop_privileges();
	master_service_init_log_with_pid(master_service);

	storage_service = mail_storage_service_init(master_service, NULL,
						    storage_service_flags);
	restrict_access_allow_coredumps(TRUE);
	master_service_init_finish(master_service);

	master_service_run(master_service, client_connected);

	master_connections_destroy();
	mail_storage_service_deinit(&storage_service);
	master_service_deinit(&master_service);
        return 0;
}
