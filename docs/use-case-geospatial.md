---
title: Geospatial analytics
parent: Use cases
nav_order: 4
---

# Geospatial analytics with Overture Maps
{: .no_toc }

[Overture Maps](https://overturemaps.org/) publishes open map data for the whole world, with
tens of millions of places and buildings, as GeoParquet files in a public S3 bucket. This
example uses pg_lake to extract one city from the worldwide data set, store it in Iceberg,
combine it with indexed boundary polygons in PostGIS, and export the results for mapping tools.

The same approach works for any large geospatial data set: query it in place, keep the part
you need in Iceberg, and use PostGIS where it is strongest.

1. TOC
{:toc}

## Set up

You need pg_lake with [pg_lake_spatial](spatial.md#set-up). The Overture bucket is in
`us-west-2` and can be read without credentials; running pg_lake in the same region makes
these queries faster and avoids data transfer charges.

```sql
CREATE EXTENSION pg_lake_spatial CASCADE;
```

## Connect the worldwide data sets

Create foreign tables for Overture's places and administrative areas. Overture keeps only
recent releases in the bucket, so replace `2026-09-23.0` with a
[current release](https://docs.overturemaps.org/release-calendar/):

```sql
CREATE FOREIGN TABLE ov_places () SERVER pg_lake
OPTIONS (path 's3://overturemaps-us-west-2/release/2026-09-23.0/theme=places/type=place/*.parquet');

CREATE FOREIGN TABLE ov_division_areas () SERVER pg_lake
OPTIONS (path 's3://overturemaps-us-west-2/release/2026-09-23.0/theme=divisions/type=division_area/*.parquet');
```

The columns, including the `geometry` column and nested structs such as `names` and `bbox`,
are inferred from the files. Every Overture feature has a bounding box in `bbox`; filtering on
it lets DuckDB skip most of the files using Parquet statistics, so you can query the worldwide
data set without reading all of it:

```sql
-- which kinds of areas cover Dam Square in Amsterdam?
SELECT DISTINCT subtype, (names).primary AS name
FROM ov_division_areas
WHERE country = 'NL'
  AND (bbox).xmin <= 4.8932 AND (bbox).xmax >= 4.8932
  AND (bbox).ymin <= 52.3731 AND (bbox).ymax >= 52.3731
  AND ST_Contains(geometry, ST_Point(4.8932, 52.3731))
ORDER BY 1;

  subtype  |     name
-----------+---------------
 country   | Nederland
 county    | Amsterdam
 locality  | Amsterdam
 macrohood | Centrum
 microhood | Dam
 region    | Noord-Holland
```

## Extract a city into Iceberg

Queries against the worldwide files take seconds, since they read from S3. For repeated
analysis, copy the area you need into your own tables. Places, a large point data set that you
will mostly scan and aggregate, go into an Iceberg table:

```sql
CREATE TABLE amsterdam_places USING iceberg AS
SELECT id, (names).primary AS name, basic_category AS category, geometry AS geom
FROM ov_places
WHERE (bbox).xmin >= 4.73 AND (bbox).xmax <= 5.07
  AND (bbox).ymin >= 52.28 AND (bbox).ymax <= 52.43;

SELECT 52667
```

Neighborhood boundaries, a small set of polygons that you will use for point-in-polygon
lookups, go into a regular table with a GiST index:

```sql
CREATE TABLE amsterdam_neighborhoods AS
SELECT id, (names).primary AS name, geometry AS geom
FROM ov_division_areas
WHERE country = 'NL' AND subtype = 'microhood'
  AND (bbox).xmin >= 4.73 AND (bbox).xmax <= 5.07
  AND (bbox).ymin >= 52.28 AND (bbox).ymax <= 52.43;

CREATE INDEX ON amsterdam_neighborhoods USING gist (geom);
```

Both statements run in a few seconds, reading only the files that overlap the bounding box.

## Analyze

Aggregations over the Iceberg table run on DuckDB:

```sql
SELECT category, count(*)
FROM amsterdam_places
GROUP BY 1 ORDER BY 2 DESC LIMIT 5;

          category          | count
----------------------------+-------
 restaurant                 |  4214
 professional_service       |  2808
 fashion_and_apparel_store  |  2360
 personal_or_beauty_service |  1808
                            |  1451
```

Spatial filters are pushed down as well. This radius search around Dam Square, 0.005 degrees
or roughly 500 meters, runs entirely in DuckDB (see
[spatial query pushdown](spatial.md#spatial-query-pushdown)):

```sql
SELECT category, count(*)
FROM amsterdam_places
WHERE ST_DWithin(geom, ST_Point(4.8932, 52.3731), 0.005)
GROUP BY 1 ORDER BY 2 DESC LIMIT 5;

          category          | count
----------------------------+-------
 restaurant                 |   287
 fashion_and_apparel_store  |   266
 bar                        |    99
 personal_or_beauty_service |    90
 professional_service       |    88
```

Point-in-polygon lookups on the neighborhoods use the GiST index:

```sql
SELECT name FROM amsterdam_neighborhoods
WHERE ST_Contains(geom, ST_Point(4.8932, 52.3731));

 name
------
 Dam
```

And the two combine in a spatial join. PostgreSQL reads the matching places from Iceberg and
joins them with the indexed polygons:

```sql
-- the neighborhoods with the most cafes
SELECT n.name, count(*) AS cafes
FROM amsterdam_places p
JOIN amsterdam_neighborhoods n ON ST_Contains(n.geom, p.geom)
WHERE p.category = 'cafe'
GROUP BY n.name
ORDER BY cafes DESC
LIMIT 5;

         name          | cafes
-----------------------+-------
 Grachtengordel        |    40
 Oud-West              |    39
 Burgwallen-Oude Zijde |    38
 De Pijp               |    36
 Jordaan               |    34
```

For distances in meters, use `geography`, which PostGIS evaluates on the rows DuckDB returns:

```sql
-- the five nearest museums to Dam Square
SELECT name, round(ST_Distance(geom::geography, ST_Point(4.8932, 52.3731)::geography)) AS meters
FROM amsterdam_places
WHERE category = 'museum'
ORDER BY geom <-> ST_Point(4.8932, 52.3731)
LIMIT 5;
```

## Share the results

Export query results as GeoParquet, which QGIS, GeoPandas, DuckDB and Snowflake can read:

```sql
COPY (SELECT name, category, geom FROM amsterdam_places WHERE category = 'cafe')
TO 's3://mybucket/exports/amsterdam_cafes.parquet';
```

The Iceberg table itself can be read by other engines too, through its
[catalog](iceberg-catalogs.md). To look at the data on a map, connect
[QGIS](spatial.md#using-qgis) to PostgreSQL and add the tables as layers.

## Keep it up to date

When Overture publishes a new release, point the foreign tables at it, and refresh your
extract in one transaction:

```sql
ALTER FOREIGN TABLE ov_places
  OPTIONS (SET path 's3://overturemaps-us-west-2/release/<new release>/theme=places/type=place/*.parquet');

BEGIN;
DELETE FROM amsterdam_places;
INSERT INTO amsterdam_places
SELECT id, (names).primary, basic_category, geometry
FROM ov_places
WHERE (bbox).xmin >= 4.73 AND (bbox).xmax <= 5.07
  AND (bbox).ymin >= 52.28 AND (bbox).ymax <= 52.43;
COMMIT;
```

Queries keep seeing the old data until the transaction commits.
