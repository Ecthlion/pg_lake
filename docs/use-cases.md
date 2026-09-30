---
title: Use cases
nav_order: 5
has_children: true
has_toc: false
---

# Use cases

End-to-end examples of pg_lake applied to real workloads. Each one can be run as written on a
pg_lake installation with object storage.

<div class="pglake-cards">
  <a class="pglake-card" href="{{ '/use-case-snowflake-sync.html' | relative_url }}">
    <span class="pglake-card-title">Sync Postgres tables to Snowflake</span>
    <span class="pglake-card-text">Keep an Iceberg copy of operational tables up to date with pg_incremental, and query it from Snowflake without ETL.</span>
  </a>
  <a class="pglake-card" href="{{ '/use-case-log-management.html' | relative_url }}">
    <span class="pglake-card-title">Log management</span>
    <span class="pglake-card-text">Turn log files in object storage into a compact Iceberg table, processing each new file exactly once.</span>
  </a>
  <a class="pglake-card" href="{{ '/use-case-archiving.html' | relative_url }}">
    <span class="pglake-card-title">Archive old data to Iceberg</span>
    <span class="pglake-card-text">Keep recent partitions in heap tables and move old ones to Iceberg, in the same partitioned table.</span>
  </a>
  <a class="pglake-card" href="{{ '/use-case-geospatial.html' | relative_url }}">
    <span class="pglake-card-title">Geospatial analytics</span>
    <span class="pglake-card-text">Extract a city from Overture Maps into Iceberg, join it with PostGIS polygons and export GeoParquet.</span>
  </a>
  <a class="pglake-card" href="{{ '/use-case-migrate.html' | relative_url }}">
    <span class="pglake-card-title">Migrate tables to Iceberg</span>
    <span class="pglake-card-text">Copy tables from any PostgreSQL server into Iceberg with pg_dump and psql.</span>
  </a>
</div>
