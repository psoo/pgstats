/*
 * pgstats, a PostgreSQL app to gather statistical informations
 * from a PostgreSQL database.
 *
 * Guillaume Lelarge, guillaume@lelarge.info, 2011.
 *
 * contrib/pgstats/pgstats.c
 */

#include "postgres_fe.h"
#include <sys/stat.h>

#include <unistd.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

extern char *optarg;

#include "libpq-fe.h"

/* these are the opts structures for command line params */
struct options
{
	bool		quiet;
	bool		nodb;
	char	   *directory;

	char	   *dbname;
	char	   *hostname;
	char	   *port;
	char	   *username;
};

/* function prototypes */
static void help(const char *progname);
void		get_opts(int, char **, struct options *);
void	   *myalloc(size_t size);
char	   *mystrdup(const char *str);
PGconn	   *sql_conn(struct options *);
int			sql_exec(PGconn *, const char *sql, const char *filename, bool quiet);
void        sql_exec_dump_pgstatactivity(PGconn *, struct options *);
void		sql_exec_dump_pgstatbgwriter(PGconn *, struct options *);
void		sql_exec_dump_pgstatdatabase(PGconn *, struct options *);
void		sql_exec_dump_pgstatalltables(PGconn *, struct options *);
void		sql_exec_dump_pgstatallindexes(PGconn *, struct options *);
void		sql_exec_dump_pgstatioalltables(PGconn *, struct options *);
void		sql_exec_dump_pgstatioallindexes(PGconn *, struct options *);
void		sql_exec_dump_pgstatioallsequences(PGconn *, struct options *);
void		sql_exec_dump_pgstatuserfunctions(PGconn *, struct options *);
void		sql_exec_dump_pgclass_size(PGconn *, struct options *);
void		sql_exec_dump_xlog_stat(PGconn *, struct options *);

/* function to parse command line options and check for some usage errors. */
void
get_opts(int argc, char **argv, struct options * my_opts)
{
	int			c;
	const char *progname;

	progname = get_progname(argv[0]);

	/* set the defaults */
	my_opts->quiet = false;
	my_opts->nodb = false;
	my_opts->directory = NULL;
	my_opts->dbname = NULL;
	my_opts->hostname = NULL;
	my_opts->port = NULL;
	my_opts->username = NULL;

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pgstats (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	/* get opts */
	while ((c = getopt(argc, argv, "H:p:U:d:D:qh")) != -1)
	{
		switch (c)
		{
				/* specify the database */
			case 'd':
				my_opts->dbname = mystrdup(optarg);
				break;

				/* specify the directory */
			case 'D':
				my_opts->directory = mystrdup(optarg);
				break;

				/* don't show headers */
			case 'q':
				my_opts->quiet = true;
				break;

				/* host to connect to */
			case 'H':
				my_opts->hostname = mystrdup(optarg);
				break;

				/* port to connect to on remote host */
			case 'p':
				my_opts->port = mystrdup(optarg);
				break;

				/* username */
			case 'U':
				my_opts->username = mystrdup(optarg);
				break;

			case 'h':
				help(progname);
				exit(0);
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}
}

static void
help(const char *progname)
{
	printf("%s gathers statistics from a PostgreSQL database.\n\n"
		   "Usage:\n"
		   "  %s [OPTIONS]...\n"
		   "\nOptions:\n"
		   "  -d DBNAME    database to connect to\n"
		   "  -D DIRECTORY directory for stats files (defaults to current)\n"
		   "  -q           quiet (don't show headers)\n"
		   "  --help       show this help, then exit\n"
		   "  --version    output version information, then exit\n"
		   "  -H HOSTNAME  database server host or socket directory\n"
		   "  -p PORT      database server port number\n"
		   "  -U USER      connect as specified database user\n"
		   "\nThe default action is to show all database OIDs.\n\n"
		   "Report bugs to <pgsql-bugs@postgresql.org>.\n",
		   progname, progname);
}

void *
myalloc(size_t size)
{
	void	   *ptr = malloc(size);

	if (!ptr)
	{
		fprintf(stderr, "out of memory");
		exit(1);
	}
	return ptr;
}

char *
mystrdup(const char *str)
{
	char	   *result = strdup(str);

	if (!result)
	{
		fprintf(stderr, "out of memory");
		exit(1);
	}
	return result;
}

/* establish connection with database. */
PGconn *
sql_conn(struct options * my_opts)
{
	PGconn	   *conn;
	char	   *password = NULL;
	bool		new_pass;

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
		new_pass = false;
		conn = PQsetdbLogin(my_opts->hostname,
							my_opts->port,
							NULL,		/* options */
							NULL,		/* tty */
							my_opts->dbname,
							my_opts->username,
							password);
		if (!conn)
		{
			fprintf(stderr, "%s: could not connect to database %s\n",
					"pgstats", my_opts->dbname);
			exit(1);
		}

		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			password == NULL)
		{
			PQfinish(conn);
			password = simple_prompt("Password: ", 100, false);
			new_pass = true;
		}
	} while (new_pass);

	if (password)
		free(password);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		fprintf(stderr, "%s: could not connect to database %s: %s",
				"pgstats", my_opts->dbname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	/* return the conn if good */
	return conn;
}

/*
 * Actual code to make call to the database and print the output data.
 */
int
sql_exec(PGconn *conn, const char *todo, const char* filename, bool quiet)
{
	PGresult   *res;
	FILE	   *fdcsv;
	struct stat st;

	int			nfields;
	int			nrows;
	int			i,
				j;
	int			size;

	/* open the csv file */
	fdcsv = fopen(filename, "a");

	/* get size of file */
	stat(filename, &st);
	size = st.st_size;

	/* make the call */
	res = PQexec(conn, todo);

	/* check and deal with errors */
	if (!res || PQresultStatus(res) > 2)
	{
		fprintf(stderr, "pgstats: query failed: %s\n", PQerrorMessage(conn));
		fprintf(stderr, "pgstats: query was: %s\n", todo);

		PQclear(res);
		PQfinish(conn);
		exit(-1);
	}

	/* get the number of fields */
	nrows = PQntuples(res);
	nfields = PQnfields(res);

	/* print a header */
	if (!quiet && size == 0)
	{
		for (j = 0; j < nfields; j++)
		{
			fprintf(fdcsv, "%s;", PQfname(res, j));
		}
		fprintf(fdcsv, "\n");
	}

	/* for each row, dump the information */
	for (i = 0; i < nrows; i++)
	{
		for (j = 0; j < nfields; j++)
			fprintf(fdcsv, "%s;", PQgetvalue(res, i, j));
		fprintf(fdcsv, "\n");
	}

	/* cleanup */
	PQclear(res);

	/* close the csv file */
	fclose(fdcsv);

	return 0;
}

/*
 * Dump all activities.
 */
void
sql_exec_dump_pgstatactivity(PGconn *conn, struct options * opts)
{
	char		todo[1024];
	char		filename[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT date_trunc('seconds', now()), datid, datname, procpid, "
             "usesysid, usename, current_query, waiting, "
             "date_trunc('seconds', xact_start) AS xact_start, date_trunc('seconds', query_start) AS query_start, "
             "date_trunc('seconds', backend_start) AS backend_start, client_addr, client_port "
             "FROM pg_stat_activity "
	     "ORDER BY procpid");
	snprintf(filename, sizeof(filename),
			 "%s/pg_stat_activity.csv", opts->directory);

	sql_exec(conn, todo, filename, opts->quiet);
}

/*
 * Dump all bgwriter stats.
 */
void
sql_exec_dump_pgstatbgwriter(PGconn *conn, struct options * opts)
{
	char		todo[1024];
	char		filename[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT date_trunc('seconds', now()), * FROM pg_stat_bgwriter");
	snprintf(filename, sizeof(filename),
			 "%s/pg_stat_bgwriter.csv", opts->directory);

	sql_exec(conn, todo, filename, opts->quiet);
}

/*
 * Dump all databases stats.
 */
void
sql_exec_dump_pgstatdatabase(PGconn *conn, struct options * opts)
{
	char		todo[1024];
	char		filename[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT date_trunc('seconds', now()), * FROM pg_stat_database ORDER BY datname");
	snprintf(filename, sizeof(filename),
			 "%s/pg_stat_database.csv", opts->directory);

	sql_exec(conn, todo, filename, opts->quiet);
}

/*
 * Dump all tables stats.
 */
void
sql_exec_dump_pgstatalltables(PGconn *conn, struct options * opts)
{
	char		todo[1024];
	char		filename[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT date_trunc('seconds', now()), relid, schemaname, relname, "
             "seq_scan, seq_tup_read, idx_scan, idx_tup_fetch, n_tup_ins, "
             "n_tup_upd, n_tup_del, n_tup_hot_upd, n_live_tup, n_dead_tup, "
             "date_trunc('seconds', last_vacuum) AS last_vacuum, date_trunc('seconds', last_autovacuum) AS last_autovacuum, "
             "date_trunc('seconds',last_analyze) AS last_analyze, date_trunc('seconds',last_autoanalyze) AS last_autoanalyze "
             "FROM pg_stat_all_tables "
	     "WHERE schemaname <> 'information_schema' "
	     "ORDER BY schemaname, relname");
	snprintf(filename, sizeof(filename),
			 "%s/pg_stat_all_tables.csv", opts->directory);

	sql_exec(conn, todo, filename, opts->quiet);
}

/*
 * Dump all indexes stats.
 */
void
sql_exec_dump_pgstatallindexes(PGconn *conn, struct options * opts)
{
	char		todo[1024];
	char		filename[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT date_trunc('seconds', now()), * "
			 "FROM pg_stat_all_indexes "
	     		 "WHERE schemaname <> 'information_schema' "
			 "ORDER BY schemaname, relname");
	snprintf(filename, sizeof(filename),
			 "%s/pg_stat_all_indexes.csv", opts->directory);

	sql_exec(conn, todo, filename, opts->quiet);
}

/*
 * Dump all tables IO stats.
 */
void
sql_exec_dump_pgstatioalltables(PGconn *conn, struct options * opts)
{
	char		todo[1024];
	char		filename[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT date_trunc('seconds', now()), * "
			 "FROM pg_statio_all_tables "
	     		 "WHERE schemaname <> 'information_schema' "
			 "ORDER BY schemaname, relname");
	snprintf(filename, sizeof(filename),
			 "%s/pg_statio_all_tables.csv", opts->directory);

	sql_exec(conn, todo, filename, opts->quiet);
}

/*
 * Dump all indexes IO stats.
 */
void
sql_exec_dump_pgstatioallindexes(PGconn *conn, struct options * opts)
{
	char		todo[1024];
	char		filename[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT date_trunc('seconds', now()), * "
			 "FROM pg_statio_all_indexes "
	     		 "WHERE schemaname <> 'information_schema' "
			 "ORDER BY schemaname, relname");
	snprintf(filename, sizeof(filename),
			 "%s/pg_statio_all_indexes.csv", opts->directory);

	sql_exec(conn, todo, filename, opts->quiet);
}

/*
 * Dump all sequences IO stats.
 */
void
sql_exec_dump_pgstatioallsequences(PGconn *conn, struct options * opts)
{
	char		todo[1024];
	char		filename[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT date_trunc('seconds', now()), * "
			 "FROM pg_statio_all_sequences "
	     		 "WHERE schemaname <> 'information_schema' "
			 "ORDER BY schemaname, relname");
	snprintf(filename, sizeof(filename),
			 "%s/pg_statio_all_sequences.csv", opts->directory);

	sql_exec(conn, todo, filename, opts->quiet);
}

/*
 * Dump all functions stats.
 */
void
sql_exec_dump_pgstatuserfunctions(PGconn *conn, struct options * opts)
{
	char		todo[1024];
	char		filename[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT date_trunc('seconds', now()), * "
			 "FROM pg_stat_user_functions "
	     		 "WHERE schemaname <> 'information_schema' "
			 "ORDER BY schemaname, funcname");
	snprintf(filename, sizeof(filename),
			 "%s/pg_stat_user_functions.csv", opts->directory);

	sql_exec(conn, todo, filename, opts->quiet);
}

/*
 * Dump all size class stats.
 */
void
sql_exec_dump_pgclass_size(PGconn *conn, struct options * opts)
{
	char		todo[1024];
	char		filename[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT date_trunc('seconds', now()), n.nspname, c.relname, c.relkind, c.reltuples, c.relpages, pg_relation_size(c.oid) "
			 "FROM pg_class c, pg_namespace n "
			 "WHERE n.oid=c.relnamespace AND n.nspname <> 'information_schema' "
			 "ORDER BY n.nspname, c.relname");
	snprintf(filename, sizeof(filename),
			 "%s/pg_class_size.csv", opts->directory);

	sql_exec(conn, todo, filename, opts->quiet);
}

/*
 * Dump all xlog stats.
 */
void
sql_exec_dump_xlog_stat(PGconn *conn, struct options * opts)
{
	char		todo[1024];
	char		filename[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT date_trunc('seconds', now()), pg_xlogfile_name(pg_current_xlog_location())=pg_ls_dir AS current, pg_ls_dir AS filename, "
			 "(SELECT modification FROM pg_stat_file('pg_xlog/'||pg_ls_dir)) AS modification_timestamp "
			 "FROM pg_ls_dir('pg_xlog') "
			 "WHERE pg_ls_dir ~ E'^[0-9A-F]{24}' "
			 "ORDER BY pg_ls_dir");
	snprintf(filename, sizeof(filename),
			 "%s/pg_xlog_stat.csv", opts->directory);

	sql_exec(conn, todo, filename, opts->quiet);
}


int
main(int argc, char **argv)
{
	struct options *my_opts;
	PGconn	   *pgconn;

	my_opts = (struct options *) myalloc(sizeof(struct options));

	/* parse the opts */
	get_opts(argc, argv, my_opts);

	if (my_opts->dbname == NULL)
	{
		my_opts->dbname = "postgres";
		my_opts->nodb = true;
	}

	if (my_opts->directory == NULL)
	{
		my_opts->directory = "./";
	}

	/* connect to the database */
	pgconn = sql_conn(my_opts);

	/* grap cluster stats info */
	sql_exec_dump_pgstatactivity(pgconn, my_opts);
	sql_exec_dump_pgstatbgwriter(pgconn, my_opts);
	sql_exec_dump_pgstatdatabase(pgconn, my_opts);

	/* grap database stats info */
	sql_exec_dump_pgstatalltables(pgconn, my_opts);
	sql_exec_dump_pgstatallindexes(pgconn, my_opts);
	sql_exec_dump_pgstatioalltables(pgconn, my_opts);
	sql_exec_dump_pgstatioallindexes(pgconn, my_opts);
	sql_exec_dump_pgstatioallsequences(pgconn, my_opts);
	sql_exec_dump_pgstatuserfunctions(pgconn, my_opts);

	/* grab other informations */
	sql_exec_dump_pgclass_size(pgconn, my_opts);
	sql_exec_dump_xlog_stat(pgconn, my_opts);

	PQfinish(pgconn);
	return 0;
}
