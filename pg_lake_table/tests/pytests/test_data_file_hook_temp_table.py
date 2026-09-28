"""
Regression tests for ALLOW_TEMP_OBJECTS_BEGIN / ALLOW_TEMP_OBJECTS_END.

pg_lake_table's BeginForeignModify creates a per-statement temp tracking
table for both UPDATE and DELETE via CreateUpdateTrackingTable. Before
ALLOW_TEMP_OBJECTS_BEGIN/END existed, that create ran under
SECURITY_RESTRICTED_OPERATION, which PostgreSQL forbids: the temp namespace
cannot be safely established within a restricted context, so the DML failed
with "cannot create temporary table within security-restricted operation".

This loads pg_lake_table_test_data_file_hook (a sample extension exposing
run_iceberg_dml_under_extension_owner) and runs an UPDATE/DELETE on an
Iceberg table from inside SPI_START_EXTENSION_OWNER. If the temp-table site
regresses to the restricted helper, the DML fails with the above error.
"""

import psycopg2
import pytest
from utils_pytest import *


@pytest.fixture(scope="module")
def with_data_file_hook(superuser_conn):
    """Load the extension that exposes run_iceberg_dml_under_extension_owner."""
    run_command(
        "CREATE EXTENSION IF NOT EXISTS pg_lake_table_test_data_file_hook CASCADE",
        superuser_conn,
    )
    superuser_conn.commit()

    yield

    run_command(
        "DROP EXTENSION IF EXISTS pg_lake_table_test_data_file_hook",
        superuser_conn,
    )
    superuser_conn.commit()


@pytest.mark.parametrize("operation", ["delete", "update"])
def test_iceberg_dml_under_extension_owner(
    s3,
    superuser_conn,
    pg_conn,
    extension,
    with_default_location,
    with_data_file_hook,
    operation,
):
    """UPDATE/DELETE on an Iceberg foreign table issued from inside
    SPI_START_EXTENSION_OWNER must succeed.

    pg_lake_table's BeginForeignModify creates a per-statement temp tracking
    table for both UPDATE and DELETE via CreateUpdateTrackingTable, which
    calls DefineRelation + DefineIndex directly.  Under the lockdown added
    in the parent commit those would fail with "cannot create temporary
    table within security-restricted operation" without the narrow
    ALLOW_TEMP_OBJECTS_BEGIN/END scope around the create.

    Downstream extensions that wrap their own SPI calls in
    SPI_START_EXTENSION_OWNER and then issue Iceberg DML hit this -- the
    motivating example was an extension's expiry path running DELETE on a
    change-log Iceberg table.
    """
    # pg_conn is module-scoped, so each parametrization needs a distinct
    # table to avoid "relation already exists" on the second run.
    table = f"test_iceberg_dml_under_owner_{operation}"

    if operation == "delete":
        dml = f"DELETE FROM public.{table} " "WHERE a OPERATOR(pg_catalog.>=) 2"
    else:
        dml = (
            f"UPDATE public.{table} "
            "SET b = b OPERATOR(pg_catalog.||) '!' "
            "WHERE a OPERATOR(pg_catalog.>=) 2"
        )

    run_command(
        f"CREATE TABLE {table}(a int, b text) USING iceberg",
        pg_conn,
    )
    run_command(
        f"INSERT INTO {table} VALUES (1, 'one'), (2, 'two'), (3, 'three')",
        pg_conn,
    )
    pg_conn.commit()

    # Run the DML under the extension-owner lockdown.
    run_command(
        f"SELECT run_iceberg_dml_under_extension_owner({_quote_literal(dml)})",
        superuser_conn,
    )
    superuser_conn.commit()

    if operation == "delete":
        res = run_query(f"SELECT count(*) FROM {table}", pg_conn)
        assert res == [[1]]
    else:
        res = run_query(f"SELECT a, b FROM {table} ORDER BY a", pg_conn)
        assert res == [[1, "one"], [2, "two!"], [3, "three!"]]


def _quote_literal(s: str) -> str:
    """Quote a string for embedding in a SQL statement as a literal."""
    return "'" + s.replace("'", "''") + "'"
