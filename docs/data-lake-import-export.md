---
title: Import and export
parent: User guide
nav_order: 3
---

# Import and export
{: .no_toc }

pg_lake extends `COPY` and `CREATE TABLE` so that you can load data from files in object
storage or on the web into any table, and write any query result out as Parquet, CSV or JSON.

1. TOC
{:toc}

## Import data

### Create a table from a file

`load_from` creates a table with the columns of a file and loads its data, in one step:

```sql
-- a regular PostgreSQL table
CREATE TABLE trips () WITH (load_from = 's3://mybucket/trips/2026-09.parquet');

-- an Iceberg table
CREATE TABLE trips_iceberg () USING iceberg
WITH (load_from = 's3://mybucket/trips/2026-09.parquet');
```

`definition_from` only creates the columns, so you can load the data later, or add
indexes first:

```sql
CREATE TABLE trips () WITH (definition_from = 's3://mybucket/trips/2026-09.parquet');
```

With both options, leave the column list empty to infer the columns, or specify columns to use
your own types. Columns are matched by position, as with `COPY`.

### Load into an existing table

`COPY ... FROM` accepts a URL:

```sql
COPY trips FROM 's3://mybucket/trips/2026-10.parquet';

-- CSV options work as usual
COPY trips FROM 's3://mybucket/trips/2026-10.csv.gz' WITH (header true, delimiter ';');
```

Like PostgreSQL's own `COPY`, the file's columns are matched to the table's columns by
position; use `COPY table (col1, col2, ...)` to load into specific columns.

To load many files at once, or only some rows or columns, create a
[foreign table](query-data-lake-files.md) on the files and use `INSERT ... SELECT`:

```sql
CREATE FOREIGN TABLE trips_files () SERVER pg_lake
OPTIONS (path 's3://mybucket/trips/*.parquet');

INSERT INTO trips SELECT * FROM trips_files WHERE pickup_time >= '2026-10-01';
```

### Formats and compression

The format is detected from the file extension; specify `format` for files without one:

```sql
COPY trips FROM 's3://mybucket/trips/latest' WITH (format 'parquet');
```

| Format | Extensions | Compression |
|:--|:--|:--|
| Parquet | `.parquet` | Detected from the file metadata. |
| CSV | `.csv`, `.csv.gz`, `.csv.zst` | `gzip`, `zstd` |
| JSON (newline-delimited) | `.json`, `.json.gz`, `.json.zst` | `gzip`, `zstd` |
| GDAL (geospatial) | See [geospatial](spatial.md#gdal-formats-shapefile-geopackage-and-more) | `zip`, `gzip` |

Parquet files record their compression internally. For CSV and JSON files whose name does not
show the compression, specify it:

```sql
-- without compression 'gzip', the file would be read as uncompressed CSV and fail
CREATE FOREIGN TABLE compressed () SERVER pg_lake
OPTIONS (path 's3://mybucket/data/export_file', format 'csv', compression 'gzip');
```

For CSV, pg_lake supports PostgreSQL's options such as `header`, `delimiter`, `quote`,
`escape` and `null`. See the [file formats reference](file-formats-reference.md) for all
options.

## Export data

`COPY ... TO` a URL writes a table or query result to object storage. The format and
compression follow from the file extension:

```sql
-- Parquet, with snappy compression by default
COPY trips TO 's3://mybucket/exports/trips.parquet';

-- the result of a query
COPY (SELECT * FROM trips JOIN zones USING (zone_id) WHERE pickup_time >= '2026-10-01')
TO 's3://mybucket/exports/october.parquet';

-- CSV, uncompressed and gzip-compressed
COPY trips TO 's3://mybucket/exports/trips.csv' WITH (header true);
COPY trips TO 's3://mybucket/exports/trips.csv.gz' WITH (header true);

-- newline-delimited JSON, compressed with zstd
COPY trips TO 's3://mybucket/exports/trips.json.zst';

-- Parquet with zstd compression
COPY trips TO 's3://mybucket/exports/trips.parquet' WITH (compression 'zstd');
```

The export runs on DuckDB when the query can be [pushed down](performance.md#query-pushdown),
which is typically the case for queries on Iceberg tables and files. Exports are written as
one file per `COPY`; to write a data set in many files, run several `COPY` statements, for
example one per day.

## Client-side import and export

pg_lake's formats also work with psql's `\copy`, which reads and writes files on the client
machine. Always specify the format and compression, since the server cannot see the local
file name:

```sql
-- import a compressed JSON file from local disk
\copy trips FROM '/tmp/trips.json.gz' WITH (format 'json', compression 'gzip')

-- export a Parquet file to local disk
\copy trips TO '/tmp/trips.parquet' WITH (format 'parquet')
```

## Converting CSV and JSON to Parquet

Parquet is columnar, compressed, and carries statistics that let queries skip data, so it is
much faster to query than CSV or JSON. To convert a set of text files, query them through a
foreign table and export the result:

```sql
CREATE FOREIGN TABLE thermostat_csv () SERVER pg_lake
OPTIONS (path 's3://mybucket/thermostat/*.csv');

COPY (SELECT * FROM thermostat_csv) TO 's3://mybucket/thermostat.parquet';

CREATE FOREIGN TABLE thermostat_parquet () SERVER pg_lake
OPTIONS (path 's3://mybucket/thermostat.parquet');
```

Even on a small file, the difference shows:

```sql
EXPLAIN ANALYZE SELECT * FROM thermostat_csv;
 Foreign Scan on thermostat_csv  (actual time=38.885..59.216 rows=7205 loops=1)
 Execution Time: 60.624 ms

EXPLAIN ANALYZE SELECT * FROM thermostat_parquet;
 Foreign Scan on thermostat_parquet  (actual time=5.427..21.359 rows=7205 loops=1)
 Execution Time: 26.496 ms
```

The gap grows with the size of the data, and with queries that only need some columns or
rows. If the data keeps growing or changing, load it into an
[Iceberg table](iceberg-tables.md) instead.
