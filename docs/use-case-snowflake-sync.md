---
title: Sync Postgres tables to Snowflake
parent: Use cases
nav_order: 1
---

# Sync Postgres tables to Snowflake with Iceberg
{: .no_toc }

Applications write to regular PostgreSQL tables, which are fast for transactions but not for
analytics over months of history. With pg_lake, you can keep an Iceberg copy of those tables in
object storage, updated automatically from within PostgreSQL, and query it from Snowflake
without an ETL pipeline or data movement tooling.

1. TOC
{:toc}

## How it works

```text
application --> sensor_readings                 heap table in PostgreSQL
                      |
                      |  pg_incremental, every minute, exactly once
                      v
                sensor_readings_iceberg         Iceberg table in object storage
                      |
                      |  Iceberg metadata, read through a catalog integration
                      v
                iot_sensors_from_postgres       Iceberg table in Snowflake
```

1. The application keeps writing to a heap table.
2. [pg_incremental](https://github.com/CrunchyData/pg_incremental), which runs on
   [pg_cron](https://github.com/citusdata/pg_cron), periodically copies new rows into an
   Iceberg table. It first backfills existing rows, then processes each new time range or
   sequence range exactly once.
3. Snowflake reads the Iceberg table in place through a catalog integration.

The Snowflake developer guide
[Sync Data from Snowflake Postgres to Snowflake with Iceberg and pg_lake](https://www.snowflake.com/en/developers/guides/sync-data-from-postgres-to-snowflake-with-iceberg-and-pg-lake/)
walks through the same pattern on Snowflake Postgres. This page also covers self-managed
pg_lake.

## Prerequisites

- pg_lake, with a `pg_lake_iceberg.default_location_prefix` that Snowflake can reach. On
  Snowflake Postgres, storage is managed for you.
- pg_cron and pg_incremental. pg_cron must be in `shared_preload_libraries`:

  ```ini
  shared_preload_libraries = 'pg_extension_base, pg_cron'
  cron.database_name = 'postgres'
  ```

  ```sql
  CREATE EXTENSION pg_cron;
  CREATE EXTENSION pg_incremental CASCADE;
  ```

## Create the operational table

This table stands in for your application's data: IoT sensor readings, written continuously.

```sql
CREATE TABLE sensor_readings (
  reading_id bigint GENERATED ALWAYS AS IDENTITY,
  sensor_id int,
  device_type text,
  reading_time timestamptz,
  temperature numeric(5,2),
  humidity numeric(5,2),
  battery_pct numeric(4,1)
);

-- 90 days of sample data
INSERT INTO sensor_readings (sensor_id, device_type, reading_time, temperature, humidity, battery_pct)
SELECT (random() * 50)::int,
       (ARRAY['thermostat', 'weather_station', 'greenhouse', 'cold_storage', 'hvac'])[1 + (random() * 4)::int],
       now() - random() * interval '90 days',
       (random() * 40 + 5)::numeric(5,2),
       (random() * 60 + 20)::numeric(5,2),
       (random() * 80 + 20)::numeric(4,1)
FROM generate_series(1, 5000);
```

## Create the Iceberg table

Create an Iceberg table with the same columns. `compatibility_mode = 'snowflake'` stores data in
a way Snowflake can read in every case, such as `uuid` values nested in arrays:

```sql
CREATE TABLE sensor_readings_iceberg (LIKE sensor_readings)
USING iceberg WITH (compatibility_mode = 'snowflake');
```

For large tables, add a [partition spec](iceberg-partitioning.md) such as
`partition_by = 'month(reading_time)'`, so that both pg_lake and Snowflake can skip old data.

## Sync new rows automatically

A time interval pipeline runs a command for each completed time range. `$1` and `$2` are the
start and end of the range. Setting `start_time` to the oldest row makes the pipeline backfill
all existing data when it is created; after that it runs every minute:

```sql
SELECT incremental.create_time_interval_pipeline(
  pipeline_name := 'sync-sensor-readings',
  time_interval := '1 minute',
  source_table_name := 'sensor_readings',
  start_time := (SELECT min(reading_time) FROM sensor_readings),
  command := $$
    INSERT INTO sensor_readings_iceberg
    SELECT * FROM sensor_readings
    WHERE reading_time >= $1 AND reading_time < $2
  $$);

NOTICE:  pipeline sync-sensor-readings: processing time range from 2026-07-02 10:24:17+00 to 2026-09-30 10:03:00+00
NOTICE:  pipeline sync-sensor-readings: scheduled cron job with ID 1 and schedule * * * * *
```

pg_incremental records which ranges it has processed in the same transaction as the insert, so
each range is copied exactly once, even if a run fails and is retried. With
`source_table_name`, it also waits for in-progress writes to `sensor_readings` before processing
a range, so rows that commit late are not skipped.

New rows appear in the Iceberg table within about two minutes:

```sql
INSERT INTO sensor_readings (sensor_id, device_type, reading_time, temperature, humidity, battery_pct)
SELECT (random() * 50)::int, 'hvac', now(), 20, 50, 90 FROM generate_series(1, 100);

-- two minutes later
SELECT (SELECT count(*) FROM sensor_readings) AS heap_rows,
       (SELECT count(*) FROM sensor_readings_iceberg) AS iceberg_rows;

 heap_rows | iceberg_rows
-----------+--------------
      5100 |         5100
```

Each pipeline run writes new Parquet files. [Autovacuum](iceberg-maintenance.md#autovacuum)
compacts them in the background. Choose a longer `time_interval`, such as `'1 hour'`, if you do
not need minute-level freshness; fewer, larger files are cheaper for both engines to read.

### Syncing by sequence instead of time

If your table has an identity or serial column, a sequence pipeline copies rows by ID range,
which does not depend on timestamps being accurate:

```sql
SELECT incremental.create_sequence_pipeline(
  pipeline_name := 'sync-orders',
  source_table_name := 'orders',
  command := $$
    INSERT INTO orders_iceberg
    SELECT * FROM orders
    WHERE order_id BETWEEN $1 AND $2
  $$);
```

### Updates and deletes

Both pipelines copy new rows, which suits append-mostly data such as events, readings, orders
and logs. If rows in the source table change after they are copied, you can:

- Periodically replace a recent window in one transaction, for example
  `DELETE FROM sensor_readings_iceberg WHERE reading_time >= now() - interval '1 day'` followed
  by the matching `INSERT ... SELECT`. With a time-based partition spec, the delete only touches
  recent files.
- Record changes in an append-only history table with a trigger, and sync that table instead.

To monitor or stop a pipeline, see `cron.job_run_details` and `incremental.drop_pipeline`.

## Read the table from Snowflake

How Snowflake finds the table depends on where pg_lake runs.

### Snowflake Postgres

On [Snowflake Postgres](https://docs.snowflake.com/en/user-guide/snowflake-postgres/postgres-pg_lake),
Snowflake reads the Iceberg tables of an instance through a catalog integration with
`CATALOG_SOURCE = SNOWFLAKE_POSTGRES`, with no storage configuration. In Snowflake:

```sql
CREATE OR REPLACE CATALOG INTEGRATION postgres_iceberg_integration
  CATALOG_SOURCE = SNOWFLAKE_POSTGRES
  TABLE_FORMAT = ICEBERG
  CATALOG_NAMESPACE = 'public'
  REST_CONFIG = (
    POSTGRES_INSTANCE = '<instance name>'
    CATALOG_NAME = 'postgres'
    ACCESS_DELEGATION_MODE = VENDED_CREDENTIALS
  )
  ENABLED = TRUE;

CREATE OR REPLACE ICEBERG TABLE iot_sensors_from_postgres
  CATALOG = 'postgres_iceberg_integration'
  CATALOG_TABLE_NAME = 'sensor_readings_iceberg';
```

`CATALOG_NAME` is the PostgreSQL database and `CATALOG_NAMESPACE` the schema. The table follows
the instance's catalog, so a refresh picks up the latest commit. Enable automatic refresh, and
set how often Snowflake polls on the integration:

```sql
ALTER ICEBERG TABLE iot_sensors_from_postgres SET AUTO_REFRESH = TRUE;
ALTER CATALOG INTEGRATION postgres_iceberg_integration SET REFRESH_INTERVAL_SECONDS = 60;
```

### Self-managed pg_lake

For pg_lake on your own infrastructure, Snowflake reads the table from its files in your
bucket. You need an
[external volume](https://docs.snowflake.com/en/user-guide/tables-iceberg-configure-external-volume)
for the bucket that holds `pg_lake_iceberg.default_location_prefix`, and a
[catalog integration for Iceberg files in object storage](https://docs.snowflake.com/en/user-guide/tables-iceberg-configure-catalog-integration-object-storage).
In Snowflake:

```sql
CREATE EXTERNAL VOLUME pg_lake_volume
  STORAGE_LOCATIONS = ((
    NAME = 'pg-lake-bucket'
    STORAGE_PROVIDER = 'S3'
    STORAGE_BASE_URL = 's3://mybucket/iceberg/'
    STORAGE_AWS_ROLE_ARN = 'arn:aws:iam::123456789012:role/snowflake-pg-lake'
  ));

CREATE CATALOG INTEGRATION pg_lake_files
  CATALOG_SOURCE = OBJECT_STORE
  TABLE_FORMAT = ICEBERG
  ENABLED = TRUE;
```

Then look up the table's current metadata file in PostgreSQL, relative to the external
volume's base URL:

```sql
SELECT replace(metadata_location, 's3://mybucket/iceberg/', '') AS metadata_file_path
FROM iceberg_tables
WHERE table_name = 'sensor_readings_iceberg';

                                          metadata_file_path
------------------------------------------------------------------------------------------------------
 postgres/public/sensor_readings_iceberg/20142/metadata/00002-3b8fceea-cd23-46a4-8b83-81b9178e5225.metadata.json
```

and create the table in Snowflake from it:

```sql
CREATE ICEBERG TABLE iot_sensors_from_postgres
  EXTERNAL_VOLUME = 'pg_lake_volume'
  CATALOG = 'pg_lake_files'
  METADATA_FILE_PATH = 'postgres/public/sensor_readings_iceberg/20142/metadata/00002-3b8fceea-cd23-46a4-8b83-81b9178e5225.metadata.json';
```

The Snowflake table is pinned to that metadata file. To pick up new data, pass the latest
metadata file to a refresh, for example from the job that runs your reports:

```sql
ALTER ICEBERG TABLE iot_sensors_from_postgres
  REFRESH 'postgres/public/sensor_readings_iceberg/20142/metadata/00003-a41d2e07-6f0b-4b8e-9d3c-5f2a7c1e8b64.metadata.json';
```

If you would rather have Snowflake follow new commits on its own, register the table in an
Iceberg REST catalog that both systems use, such as
[Apache Polaris](https://polaris.apache.org/); see
[REST catalogs](iceberg-catalogs.md#rest-catalogs).

## Query from Snowflake

The table behaves like any other Iceberg table in Snowflake:

```sql
SELECT reading_time::date AS reading_date,
       device_type,
       count(*) AS readings,
       avg(temperature) AS avg_temp
FROM iot_sensors_from_postgres
GROUP BY 1, 2
ORDER BY 1, 2;
```

Snowflake reads the table; it cannot write to it. The data stays in one copy in object storage,
PostgreSQL remains the system of record, and both engines see the same, transactionally
consistent snapshots.

## Going further

- **Keep only recent data in PostgreSQL.** Once rows are in Iceberg, you can delete old rows from
  the heap table, as in [archiving old data](use-case-archiving.md).
- **Sync many tables.** Create one pipeline per table. Each pipeline is a pg_cron job.
- **Transform on the way.** The pipeline command can be any SQL, so you can filter, join or
  aggregate rows while copying them, for example to maintain an hourly rollup in Iceberg.
