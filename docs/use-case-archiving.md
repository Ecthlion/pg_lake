---
title: Archive old data to Iceberg
parent: Use cases
nav_order: 3
---

# Archive old data to Iceberg
{: .no_toc }

Many tables grow forever, but only recent rows are updated or looked up by key: orders, events,
audit logs, measurements. Keeping years of history in heap tables makes backups, vacuum and
storage expensive. With pg_lake, you can keep recent data in heap partitions and move older
partitions to Iceberg, in the same partitioned table. Applications keep querying one table,
and analytical queries over history run on DuckDB.

1. TOC
{:toc}

## How it works

PostgreSQL's [declarative partitioning](https://www.postgresql.org/docs/current/ddl-partitioning.html)
allows a partitioned table to have partitions of different kinds. Because Iceberg tables are
foreign tables, they can be partitions, next to regular heap partitions:

```text
app_events                        partitioned by month of event_time
  +-- app_events_2026_07_archive  Iceberg, in object storage
  +-- app_events_2026_08_archive  Iceberg, in object storage
  +-- app_events_2026_09          heap, with indexes
  +-- app_events_2026_10          heap, with indexes
```

Queries on `app_events` only scan the partitions that match their filters, so key lookups on
recent data use the heap indexes as before, and scans over old months are pushed down to
DuckDB. Moving a month to Iceberg is a single transaction.

## Create a partitioned table

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

-- indexes are created on the heap partitions
CREATE INDEX ON app_events (user_id, event_time);
```

Indexes on the parent are fine, since PostgreSQL only creates them on heap partitions. Unique
indexes and primary keys on the parent are not possible once it has foreign partitions; enforce
uniqueness on the heap partitions instead.

## Move a partition to Iceberg

To archive July, create an Iceberg table with the same columns, copy the rows, and swap it in
for the heap partition, all in one transaction:

```sql
BEGIN;

CREATE TABLE app_events_2026_07_archive (
  event_id bigint NOT NULL,
  event_time timestamptz NOT NULL,
  user_id bigint NOT NULL,
  event_type text,
  payload jsonb
) USING iceberg;

INSERT INTO app_events_2026_07_archive SELECT * FROM app_events_2026_07;

ALTER TABLE app_events DETACH PARTITION app_events_2026_07;
ALTER TABLE app_events ATTACH PARTITION app_events_2026_07_archive
  FOR VALUES FROM ('2026-07-01') TO ('2026-08-01');

DROP TABLE app_events_2026_07;

COMMIT;
```

Queries see either the heap partition or the Iceberg partition, never both or neither. Declare
the columns, including `NOT NULL`, explicitly: a partition must have the same `NOT NULL`
constraints as its parent.

Afterwards, the partitioned table works as before. Scans of July go to DuckDB:

```sql
EXPLAIN (COSTS OFF)
SELECT event_type, count(*) FROM app_events WHERE event_time < '2026-08-01' GROUP BY 1;

 HashAggregate
   Group Key: app_events.event_type
   ->  Foreign Scan on app_events_2026_07_archive app_events
         Engine: DuckDB
         ->  READ_PARQUET
               Filters: event_time<'2026-08-01 00:00:00+00'::TIMESTAMP WITH TIME ZONE
               Projections: event_type
```

while lookups on recent data still use the heap indexes and never touch Iceberg:

```sql
EXPLAIN (COSTS OFF)
SELECT * FROM app_events WHERE user_id = 42 AND event_time >= '2026-09-15';

 Append
   ->  Bitmap Heap Scan on app_events_2026_09 app_events_1
         Recheck Cond: ((user_id = 42) AND (event_time >= '2026-09-15 00:00:00+00'::timestamp with time zone))
         ->  Bitmap Index Scan on app_events_2026_09_user_id_event_time_idx
   ->  Index Scan using app_events_2026_10_user_id_event_time_idx on app_events_2026_10 app_events_2
```

Late rows for an archived month are routed to its Iceberg partition, and `UPDATE` and `DELETE`
work on archived rows too. An `UPDATE` that would move a row to a different partition is not
supported.

## Automate it

Wrap the steps in a procedure:

```sql
CREATE PROCEDURE archive_app_events(month date)
LANGUAGE plpgsql AS $$
DECLARE
  heap_partition text := format('app_events_%s', to_char(month, 'YYYY_MM'));
  iceberg_partition text := heap_partition || '_archive';
BEGIN
  -- create an Iceberg table with the same columns
  EXECUTE format($sql$
    CREATE TABLE %I (
      event_id bigint NOT NULL,
      event_time timestamptz NOT NULL,
      user_id bigint NOT NULL,
      event_type text,
      payload jsonb
    ) USING iceberg$sql$, iceberg_partition);

  -- copy the rows, and swap the partitions
  EXECUTE format('INSERT INTO %I SELECT * FROM %I', iceberg_partition, heap_partition);
  EXECUTE format('ALTER TABLE app_events DETACH PARTITION %I', heap_partition);
  EXECUTE format('ALTER TABLE app_events ATTACH PARTITION %I FOR VALUES FROM (%L) TO (%L)',
                 iceberg_partition, month, month + interval '1 month');
  EXECUTE format('DROP TABLE %I', heap_partition);
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

- **One Iceberg table for all history.** Instead of an Iceberg table per month, insert archived
  rows into a single Iceberg table with `partition_by = 'month(event_time)'`, delete them from
  the heap table, and query both through a `UNION ALL` view. This keeps the number of tables
  small, at the cost of managing the view yourself.
- **Keep a full copy in Iceberg.** If analytics should see all data, including recent rows,
  sync new rows into Iceberg continuously, as in
  [syncing to Snowflake](use-case-snowflake-sync.md), and delete old rows from the heap table
  once they are in Iceberg.
- **Retention.** Dropping an archived partition, or deleting whole months from a partitioned
  Iceberg table, only removes files from the metadata, so expiring old data stays cheap.
