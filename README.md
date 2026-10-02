# pg_lake: Postgres for Iceberg and data lakes

`pg_lake` turns PostgreSQL into a lakehouse. Add `USING iceberg` to `CREATE TABLE` and you get a
transactional table whose data is stored as Parquet files in object storage such as S3, in the
open [Iceberg](https://iceberg.apache.org/) format that Spark, DuckDB, Snowflake and other engines
can read too. The same extensions let you query and load raw Parquet, CSV and JSON files where
they are, and export query results with `COPY`. Queries run on DuckDB's columnar engine, without
leaving PostgreSQL.

**Documentation: [snowflake-labs.github.io/pg_lake](https://snowflake-labs.github.io/pg_lake/)**, with a
[getting started guide](https://snowflake-labs.github.io/pg_lake/get-started.html), the user guide, use cases and reference pages.

```sql
CREATE EXTENSION pg_lake CASCADE;
SET pg_lake_iceberg.default_location_prefix TO 's3://mybucket/iceberg';

-- create an Iceberg table and load a public file with 3 million taxi trips
CREATE TABLE trips () USING iceberg
  WITH (load_from = 'https://d37ci6vzurychx.cloudfront.net/trip-data/yellow_tripdata_2024-01.parquet');

-- query and modify it like any other table
SELECT extract(hour FROM tpep_pickup_datetime) AS hour, round(avg(tip_amount)::numeric, 2) AS avg_tip
FROM trips GROUP BY 1 ORDER BY avg_tip DESC LIMIT 3;

DELETE FROM trips WHERE total_amount < 0;

-- query files in place, and export query results
CREATE FOREIGN TABLE clicks () SERVER pg_lake OPTIONS (path 's3://mybucket/clicks/*.parquet');

COPY (SELECT page, count(*) FROM clicks GROUP BY page) TO 's3://mybucket/reports/clicks.csv';
```

## Features

- **[Iceberg tables](https://snowflake-labs.github.io/pg_lake/iceberg-tables.html)** with `INSERT`, `UPDATE`, `DELETE`, `MERGE`,
  transactions, hidden partitioning and automatic compaction and snapshot expiry
- **[Interoperability](https://snowflake-labs.github.io/pg_lake/iceberg-catalogs.html)** with other engines, through PostgreSQL as
  the Iceberg catalog, or an external Iceberg REST catalog such as Polaris
- **[Query data lake files](https://snowflake-labs.github.io/pg_lake/query-data-lake-files.html)** in Parquet, CSV, JSON and
  Iceberg format, in object storage or at public URLs, with wildcards and inferred columns
- **[Import and export](https://snowflake-labs.github.io/pg_lake/data-lake-import-export.html)** with `COPY` to and from URLs,
  including incremental loading of new files with
  [pg_incremental](https://github.com/CrunchyData/pg_incremental)
- **Mix heap tables, Iceberg tables and files** in the same queries and transactions, with no
  SQL limitations
- **[Geospatial](https://snowflake-labs.github.io/pg_lake/spatial.html)** formats supported by GDAL, such as GeoParquet, GeoJSON and
  Shapefiles, with PostGIS
- **A [map type](./pg_map/README.md)** for semi-structured and key-value data

See the [use cases](https://snowflake-labs.github.io/pg_lake/use-cases.html) for complete examples, such as syncing PostgreSQL tables
to Iceberg and archiving old data.

## Getting started

The quickest way to try pg_lake is Docker, which runs PostgreSQL with pg_lake, pgduck_server and
S3-compatible storage:

```bash
git clone https://github.com/Snowflake-Labs/pg_lake.git
cd pg_lake/docker
task compose:up
psql -h localhost -p 5432 -U postgres
```

See the [Docker README](./docker/README.md) for details. To add pg_lake to an existing
PostgreSQL 16, 17, 18 or 19 installation, run `./install.sh` and follow
[building from source](https://snowflake-labs.github.io/pg_lake/building-from-source.html). In short:

1. Add `shared_preload_libraries = 'pg_extension_base'` to `postgresql.conf` and restart
   PostgreSQL.
2. Start `pgduck_server`, for example `pgduck_server --cache_dir /var/lib/pgduck/cache`. It uses
   the usual AWS or GCP credential chain to reach object storage; see
   [object storage credentials](https://snowflake-labs.github.io/pg_lake/configuration.html#object-storage-credentials) and the
   [pgduck_server options](https://snowflake-labs.github.io/pg_lake/configuration.html#pgduck_server-options).
3. Run `CREATE EXTENSION pg_lake CASCADE` and set `pg_lake_iceberg.default_location_prefix` to
   the location where Iceberg tables should be stored.

The [getting started guide](https://snowflake-labs.github.io/pg_lake/get-started.html) then walks you through your first Iceberg
table.

## Architecture

A `pg_lake` instance consists of two main components: **PostgreSQL with the pg_lake extensions** and **pgduck_server**.

Users connect to PostgreSQL to run SQL queries, and the `pg_lake` extensions integrate with Postgres's hooks to handle query planning, transaction boundaries, and overall orchestration of execution.

Behind the scenes, _parts_ of query execution are delegated to DuckDB through pgduck_server, a separate multi-threaded process that implements the PostgreSQL wire protocol (locally). This process runs DuckDB together with our **duckdb_pglake** extension, which adds PostgreSQL-compatible functions and behavior.

Users typically don't need to be aware of `pgduck_server`; it operates transparently to improve performance. When appropriate, `pg_lake` delegates scanning of the data and the computation to DuckDB's highly parallel, column-oriented execution engine.

This separation also avoids the threading and memory-safety limitations that would arise from embedding DuckDB directly inside the Postgres process, which is designed around process isolation rather than multi-threaded execution. Moreover, it lets us interact with the query engine directly by connecting to it using standard Postgres clients. See [how pg_lake works](https://snowflake-labs.github.io/pg_lake/concepts.html) for more.

![pg_lake Architecture](pglake-arch.png)

### Components

The team behind pg_lake has a lot of experience building Postgres extensions (e.g. Citus, pg_cron, pg_documentdb). Over time, we've learned that large, monolithic PostgreSQL extensions are harder to evolve and maintain.

`pg_lake` follows a modular design built around a **set of interoperating components**, mostly implemented as PostgreSQL extensions, others as supporting services or libraries. Each part focuses on a well-defined layer, such as table and metadata management, catalog and object store integration, query execution, or data format handling. This approach makes it easier to extend, test, and evolve the system, while keeping it familiar to anyone with a PostgreSQL background.

The current set of components are:

- **pg_lake_iceberg**: a PostgreSQL extension that implements the [Iceberg specification](https://iceberg.apache.org/)
- **pg_lake_table**: a PostgreSQL extension that implements a foreign data wrapper to query files in object storage
- **pg_lake_copy**: a PostgreSQL extension that implements COPY to/from your data lake
- **pg_lake_spatial**: a PostgreSQL extension that adds geospatial formats and PostGIS support
- **pg_lake_engine**: a common module for different pg_lake extensions
- **pg_extension_base**: a foundational building block for other extensions
- **pg_extension_updater**: an extension for updating all extensions on start-up. See [README.md](./pg_extension_updater/README.md).
- **pg_lake_benchmark**: a PostgreSQL extension that performs various benchmarks on lake tables. See [README.md](./pg_lake_benchmark/README.md).
- **pg_map**: a generic map type generator
- **pgduck_server**: a stand-alone server that loads DuckDB into the same server machine and exposes DuckDB via the PostgreSQL protocol
- **duckdb_pglake**: a DuckDB extension that adds missing PostgreSQL functions to DuckDB

## History
`pg_lake` development started in early 2024 at [Crunchy Data](https://www.crunchydata.com/) with the goal of bringing Iceberg to PostgreSQL. The first few months were focused on building a robust integration of an external query engine (DuckDB). To get to market early, we made the query/import/export features available to [Crunchy Bridge](https://docs.crunchybridge.com/) customers as [Crunchy Bridge for Analytics](https://www.crunchydata.com/blog/crunchy-bridge-for-analytics-your-data-lake-in-postgresql).

Next, we started building a comprehensive implementation of the Iceberg (v2) protocol with support for transactions and almost all PostgreSQL features. In November 2024, we relaunched Crunchy Bridge for Analytics as Crunchy Data Warehouse available on Crunchy Bridge and on-premises.

In June 2025, [Crunchy Data was acquired by Snowflake](https://www.crunchydata.com/blog/crunchy-data-joins-snowflake). Following the acquisition, Snowflake decided to open source the project as `pg_lake` in November 2025. The initial version is 3.0 because of the two prior generations. If you’re currently a Crunchy Data Warehouse user there will be an automatic upgrade path, though some names will change.

## Contributing

Bug reports and pull requests are welcome. See [building from source](https://snowflake-labs.github.io/pg_lake/building-from-source.html) to set up a development environment and run the tests. The documentation sources are in the [docs](./docs) directory.

## License
Copyright (c) Snowflake Inc. All rights reserved.
Licensed under the [Apache 2.0](https://www.apache.org/licenses/LICENSE-2.0) license.

#### Note on Dependencies
`pg_lake` is dependent on third-party projects Apache Avro and DuckDB. During build, `pg_lake` applies patches to Avro and certain DuckDB extensions in order to provide the `pg_lake` functionality. The source code associated with the Avro and DuckDB extensions is downloaded from the applicable upstream repos and the source code associated with those projects remains under the original licenses. If you are packaging or redistributing packages that include `pg_lake`, please note that you should review those upstream license terms.
