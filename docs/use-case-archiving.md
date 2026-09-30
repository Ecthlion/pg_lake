---
title: Archive old data to Iceberg
parent: Use cases
nav_order: 3
---

# Archive old data to Iceberg
{: .no_toc }

Many tables grow forever, but only recent rows are updated or looked up by key: orders, events,
audit logs, measurements. Keeping years of history in heap tables makes backups, vacuum and
storage expensive. With pg_lake, you can keep recent data in a heap table and move older rows
into an Iceberg table, where they take far less space and analytical queries over history run
on DuckDB.

1. TOC
{:toc}

## How it works

The operational table stays a regular heap table, partitioned by month with PostgreSQL's
[declarative partitioning](https://www.postgresql.org/docs/current/ddl-partitioning.html). Next
to it, a single Iceberg table holds the history, partitioned by month with
[Iceberg partitioning](iceberg-partitioning.md):

```text
app_events                  heap, partitioned by month of event_time
  +-- app_events_2026_09    heap, with indexes
  +-- app_events_2026_10    heap, with indexes

app_events_archive          Iceberg, partition_by = 'month(event_time)'
                            July and August 2026, in object storage
```

Once a month has passed, a job copies its heap partition into the archive and drops the
partition, in one transaction. Dropping a partition is instant and leaves no dead rows to
vacuum, unlike deleting the rows.

## Create the tables

```sql
CREATE TABLE app_events (
  event_id bigint GENERATED ALWAYS AS IDENTITY,
  event_time timestamptz NOT NULL,
  user_id bigint NOT NULL,
  event_type text,
  payload jsonb
) PARTITION BY RANGE (event_time);

CREATE TABLE app_events_2026_07 PARTITION OF app_events FOR VALUES FROM ('2026-07-01') TO ('2026-08-01');
CREATE TABLE app_events_2026_08 PARTITION OF app_events FOR VALUES FROM ('2026-08-01') TO ('2026-09-01');
CREATE TABLE app_events_2026_09 PARTITION OF app_events FOR VALUES FROM ('2026-09-01') TO ('2026-10-01');
CREATE TABLE app_events_2026_10 PARTITION OF app_events FOR VALUES FROM ('2026-10-01') TO ('2026-11-01');

CREATE INDEX ON app_events (user_id, event_time);

CREATE TABLE app_events_archive (
  event_id bigint,
  event_time timestamptz,
  user_id bigint,
  event_type text,
  payload jsonb
) USING iceberg WITH (partition_by = 'month(event_time)');
```

The archive has the same columns as `app_events`, in the same order, so rows can be copied with
`SELECT *`. It does not need the identity or `NOT NULL` constraints: the rows already satisfied
them in the heap table.

## Move a month to Iceberg

To archive July, copy its partition into the archive and drop it:

```sql
BEGIN;
INSERT INTO app_events_archive SELECT * FROM app_events_2026_07;
ALTER TABLE app_events DETACH PARTITION app_events_2026_07;
DROP TABLE app_events_2026_07;
COMMIT;
```

Because the Iceberg catalog lives in PostgreSQL, the move is atomic: every query sees July's
rows either in `app_events` or in `app_events_archive`, never in both or neither.

A row that arrives late for an archived month no longer has a partition in `app_events`, so
PostgreSQL rejects it:

```text
ERROR:  no partition of relation "app_events" found for row
DETAIL:  Partition key of the failing row contains (event_time) = (2026-07-15 00:00:00+00).
```

If your application can produce such rows, insert them into `app_events_archive` directly, or
archive with enough delay that late rows are no longer expected.

## Query the archive

Queries on recent data use `app_events` and its indexes as before. Analytical queries over
history use `app_events_archive`, and pg_lake pushes the whole query down to DuckDB, which only
reads the months and columns the query needs:

```sql
EXPLAIN (COSTS OFF)
SELECT event_type, count(*) FROM app_events_archive WHERE event_time < '2026-08-01' GROUP BY 1;

 Custom Scan (Query Pushdown)
   Engine: DuckDB
   ->  HASH_GROUP_BY
         Groups: #0
         Aggregates: count_star()
         ->  PROJECTION
               Projections: event_type
               ->  READ_PARQUET
                     Filters: event_time<'2026-08-01 00:00:00+00'::TIMESTAMP WITH TIME ZONE
                     Projections: event_type
```

You can combine both tables with `UNION ALL`, but then PostgreSQL computes the aggregate and
scans the heap side itself, and only the scan of the archive runs on DuckDB. If most
analytical queries need recent rows too, keep a full copy in Iceberg instead (see
[alternatives](#alternatives)).

`UPDATE` and `DELETE` work on archived rows, for example to correct data or to erase a user's
events. `DELETE` statements that remove whole months only change the Iceberg metadata, so
expiring the oldest data is cheap:

```sql
DELETE FROM app_events_archive WHERE event_time < now() - interval '3 years';
```

## Automate it

Wrap the move in a procedure:

```sql
CREATE PROCEDURE archive_app_events(month date)
LANGUAGE plpgsql AS $$
DECLARE
  partition_name text := format('app_events_%s', to_char(month, 'YYYY_MM'));
BEGIN
  EXECUTE format('INSERT INTO app_events_archive SELECT * FROM %I', partition_name);
  EXECUTE format('ALTER TABLE app_events DETACH PARTITION %I', partition_name);
  EXECUTE format('DROP TABLE %I', partition_name);
END;
$$;

CALL archive_app_events('2026-08-01');
```

and schedule it with [pg_cron](https://github.com/citusdata/pg_cron), for example to archive the
month before last on the first of every month:

```sql
SELECT cron.schedule('archive-app-events', '0 3 1 * *',
  $$CALL archive_app_events((date_trunc('month', now()) - interval '2 months')::date)$$);
```

You also need to create future heap partitions ahead of time, with a similar job or with
[pg_partman](https://github.com/pgpartman/pg_partman).

## Alternatives

- **No heap partitions.** If `app_events` is not partitioned, move rows with an `INSERT ...
  SELECT` into the archive followed by a `DELETE` of the same rows, in one transaction. This is
  simpler to set up, but the `DELETE` leaves dead rows behind for vacuum.
- **Keep a full copy in Iceberg.** If analytics should see all data, including recent rows,
  sync new rows into Iceberg continuously, as in
  [syncing tables to Iceberg](use-case-iceberg-sync.md), and delete old rows from the heap table
  once they are in Iceberg. Analytical queries then only read the Iceberg table.
