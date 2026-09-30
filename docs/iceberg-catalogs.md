---
title: Catalogs and interoperability
parent: Iceberg tables
grand_parent: User guide
nav_order: 3
---

# Catalogs and interoperability
{: .no_toc }

An Iceberg *catalog* keeps track of which metadata file is the current version of each table.
Engines find tables through a catalog, and commit changes by swapping the pointer to a new
metadata file. pg_lake can use PostgreSQL itself as the catalog, or register tables in an
external catalog, and it can read Iceberg tables written by other systems.

1. TOC
{:toc}

## Choosing a catalog

Every Iceberg table records its catalog in the `catalog` option when it is created. The choice
cannot be changed afterwards.

| Catalog | `catalog` value | What it does |
|:--|:--|:--|
| PostgreSQL (default) | `postgres` | pg_lake is the catalog. Commits are part of the PostgreSQL transaction, and other engines read tables through the `iceberg_tables` view. |
| REST | `rest`, or the name of an `iceberg_catalog` server | Tables are registered in an [Iceberg REST catalog](https://iceberg.apache.org/rest-catalog-spec/) such as Apache Polaris, where every engine using that catalog can find them. |
| Object store | `object_store` | pg_lake publishes tables to a catalog file in object storage. |

To change the default for new tables, set `pg_lake_iceberg.default_catalog`:

```sql
SET pg_lake_iceberg.default_catalog TO 'rest';
```

## The PostgreSQL catalog

By default, pg_lake acts as its own Iceberg catalog. When a transaction modifies an Iceberg
table, pg_lake writes the new metadata file and updates its catalog in the same transaction,
so the table and the catalog never disagree.

The catalog is exposed as the `iceberg_tables` view:

```sql
SELECT catalog_name, table_namespace, table_name, metadata_location
FROM iceberg_tables;

 catalog_name | table_namespace |  table_name  |                              metadata_location
--------------+-----------------+--------------+------------------------------------------------------------------------------
 postgres     | public          | measurements | s3://testbucket/iceberg/postgres/public/measurements/metadata/00003-6403833e-0766-4496-ad47-ec9641ee965f.metadata.json
```

For tables created through PostgreSQL, `catalog_name` is the database name, and
`table_namespace` is the schema. If the database is renamed, `catalog_name` changes with it.

The view has the layout that the Iceberg SQL catalog implementations expect: the
[Iceberg JDBC catalog](https://iceberg.apache.org/docs/latest/jdbc/#configurations) (used by
Spark, Flink and others), the
[pyiceberg SQL catalog](https://py.iceberg.apache.org/reference/pyiceberg/catalog/sql/) and
[iceberg-rust](https://rust.iceberg.apache.org/api/iceberg_catalog_sql/struct.SqlCatalog.html).
Those tools can connect to PostgreSQL and always read the latest committed version of each
table. They cannot write to tables created by pg_lake; if they create tables of their own
under a different catalog name, those tables have no corresponding PostgreSQL table.

### Reading tables from Spark

Connect Spark's Iceberg JDBC catalog to PostgreSQL. The catalog name must match the database
name, `postgres` in this example:

```bash
export PGHOST="db host"
export PGDATABASE="postgres"
export PGUSER="user name"
export PGPASSWORD="your password"
export AWS_REGION="us-east-1"
export JDBC_CONN_STR="jdbc:postgresql://${PGHOST}/${PGDATABASE}?user=${PGUSER}&password=${PGPASSWORD}"

spark-sql --packages org.apache.iceberg:iceberg-spark-runtime-3.5_2.12:1.4.1 \
  --conf spark.sql.extensions=org.apache.iceberg.spark.extensions.IcebergSparkSessionExtensions \
  --conf spark.sql.catalog.postgres=org.apache.iceberg.spark.SparkCatalog \
  --conf spark.sql.catalog.postgres.catalog-impl=org.apache.iceberg.jdbc.JdbcCatalog \
  --conf spark.sql.catalog.postgres.uri=$JDBC_CONN_STR \
  --conf spark.sql.catalog.postgres.warehouse=s3:// \
  --conf spark.sql.catalog.postgres.io-impl=org.apache.iceberg.aws.s3.S3FileIO \
  --conf spark.sql.catalog.postgres.s3.endpoint=https://s3.${AWS_REGION}.amazonaws.com
```

A table created in PostgreSQL:

```sql
CREATE TABLE public.pg_lake_iceberg_table
USING iceberg
AS SELECT id FROM generate_series(0, 1000) id;
```

can then be queried from Spark:

```sql
spark-sql (default)> SELECT avg(id) FROM postgres.public.pg_lake_iceberg_table;
500.0
```

### Reading tables from Python

pyiceberg's SQL catalog works the same way:

```python
from pyiceberg.catalog.sql import SqlCatalog

catalog = SqlCatalog(
    "postgres",  # must match the database name
    uri="postgresql+psycopg2://user:password@dbhost:5432/postgres",
    warehouse="s3://mybucket/iceberg",
)

table = catalog.load_table("public.measurements")
df = table.scan(row_filter="measurement > 20").to_pandas()
```

### Reading tables from any Iceberg engine

Any engine that can open an Iceberg table from a metadata file can read a snapshot of a
pg_lake table by its `metadata_location`. The location changes on every commit, so this gives
a point-in-time view; use a catalog to always see the latest version. The
[sync use case](use-case-iceberg-sync.md#duckdb) reads a table this way from DuckDB.

## REST catalogs

With a REST catalog, pg_lake creates and commits tables in an external catalog service instead
of its own catalog, and other engines using that catalog see them immediately. pg_lake speaks
the [Iceberg REST catalog protocol](https://iceberg.apache.org/rest-catalog-spec/) with OAuth2
client credentials, and has been tested with [Apache Polaris](https://polaris.apache.org/).

### Configure the built-in `rest` catalog

The built-in `rest` catalog is configured by a superuser, for example in `postgresql.conf` or
with `ALTER SYSTEM`:

```sql
ALTER SYSTEM SET pg_lake_iceberg.rest_catalog_host TO 'https://polaris.example.com/api/catalog';
ALTER SYSTEM SET pg_lake_iceberg.rest_catalog_client_id TO '<client id>';
ALTER SYSTEM SET pg_lake_iceberg.rest_catalog_client_secret TO '<client secret>';
SELECT pg_reload_conf();
```

| Setting | Description |
|:--|:--|
| `pg_lake_iceberg.rest_catalog_host` | Base URL of the REST catalog API. |
| `pg_lake_iceberg.rest_catalog_client_id` | OAuth2 client ID. |
| `pg_lake_iceberg.rest_catalog_client_secret` | OAuth2 client secret. |
| `pg_lake_iceberg.rest_catalog_oauth_host_path` | Token endpoint URL, if the catalog does not serve it at the default path. |
| `pg_lake_iceberg.rest_catalog_scope` | OAuth2 scope. Default `PRINCIPAL_ROLE:ALL`. |
| `pg_lake_iceberg.rest_catalog_enable_vended_credentials` | Ask the catalog for temporary storage credentials instead of using pgduck_server's own. Default `off`. |

### Define named catalogs

To use more than one REST catalog, or to give each database user their own credentials,
define a catalog server with the `iceberg_catalog` foreign data wrapper. Any member of
`lake_write` can create one. Credentials go in a user mapping, and a named server never falls
back to the global `rest_catalog_client_*` settings:

```sql
CREATE SERVER sales_polaris TYPE 'rest'
  FOREIGN DATA WRAPPER iceberg_catalog
  OPTIONS (rest_endpoint 'https://polaris.example.com/api/catalog',
           catalog_name 'sales',
           location_prefix 's3://sales-bucket/iceberg');

CREATE USER MAPPING FOR analyst SERVER sales_polaris
  OPTIONS (client_id '<client id>', client_secret '<client secret>');
```

| Option | Where | Description |
|:--|:--|:--|
| `rest_endpoint` | server | Base URL of the REST catalog API. |
| `catalog_name` | server | Catalog (warehouse) name in the REST catalog. Defaults to the prefix the catalog advertises, or the database name. |
| `location_prefix` | server | Default storage location for tables created in this catalog. |
| `oauth_endpoint` | server | OAuth2 token endpoint URL. |
| `rest_auth_type` | server | Token request format: `oauth2` (default). |
| `enable_vended_credentials` | server | Ask the catalog for temporary storage credentials. |
| `scope` | server or user mapping | OAuth2 scope. The user mapping wins if both set it. |
| `client_id`, `client_secret` | user mapping | OAuth2 client credentials. |

### Create and attach REST catalog tables

A table's relationship to the catalog is decided when you create it.

**Tables pg_lake creates.** Without `read_only`, pg_lake creates the table in the catalog and
owns it: it writes the data and metadata, and names the table in the catalog after the
database, schema and table you used.

```sql
-- pg_lake creates measurements in the catalog and can write to it
CREATE TABLE measurements (device_id bigint, value float8)
USING iceberg WITH (catalog = 'sales_polaris');
```

**Tables that already exist in the catalog.** A table created by another engine is attached
with `read_only`, naming it as the catalog knows it. The columns come from the catalog:

```sql
-- attach an existing catalog table for querying
CREATE TABLE sales () USING iceberg
WITH (catalog = 'sales_polaris', read_only = true,
      catalog_name = 'analytics', catalog_namespace = 'public',
      catalog_table_name = 'sales');
```

A read-only table always reads the catalog's current version, but cannot be written to. The
catalog options that name it are only accepted together with `read_only`. Writing would mean
taking over the table's metadata, field IDs and file inventory from whatever produced them,
which pg_lake does not do: it writes only to tables it created. To move existing data under
pg_lake, create a new table and copy into it.

Writable REST catalog tables cannot be renamed or moved to another schema, since that would
change their identity in the catalog.

## The object store catalog

With `catalog = 'object_store'`, pg_lake publishes the current metadata location of each table
to a catalog file under `pg_lake_iceberg.object_store_catalog_location_prefix`. The file is
rewritten when tables change, and at least every `pg_lake_iceberg.object_store_catalog_max_age`
seconds. Tables that another system publishes under the same prefix can be attached with
`read_only = true` and `catalog_table_name`, the same way as for REST catalogs. This catalog
is intended for managed integrations that exchange tables through object storage.

## External Iceberg tables from metadata files

You can query any Iceberg table, whoever wrote it, by creating a `pg_lake` foreign table that
points at one of its metadata files:

```sql
CREATE FOREIGN TABLE external_iceberg ()
SERVER pg_lake
OPTIONS (path 's3://mybucket/table/metadata/v14.metadata.json');
```

The table is a fixed snapshot: later changes by the writer are not visible until you point it
at a newer metadata file, which keeps dependent views and grants in place:

```sql
ALTER FOREIGN TABLE external_iceberg
OPTIONS (SET path 's3://mybucket/table/metadata/v15.metadata.json');
```

When the table is registered in a REST catalog, attaching it with `read_only = true` (above)
avoids this manual step.

## Snowflake

Snowflake can query pg_lake's Iceberg tables in place, without copying the data:

- **Snowflake Postgres** instances come with pg_lake, and a Snowflake catalog integration
  with `CATALOG_SOURCE = SNOWFLAKE_POSTGRES` reads their Iceberg tables directly. See the
  [Snowflake documentation](https://docs.snowflake.com/en/user-guide/snowflake-postgres/postgres-pg_lake).
- **Self-managed pg_lake** tables can be read through an object storage catalog integration
  from the table's metadata file, or through a REST catalog that both systems use.

For tables that Snowflake reads, create them with `compatibility_mode = 'snowflake'` (or set
`pg_lake_iceberg.default_compatibility_mode`). This stores `uuid` values nested inside arrays
and composite types as strings, which Snowflake requires, while keeping the column type
`uuid` in PostgreSQL.

The [sync use case](use-case-iceberg-sync.md#snowflake) walks through both setups.
