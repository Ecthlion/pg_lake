/*
 * Copyright 2025 Snowflake Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "postgres.h"
#include "access/hash.h"
#include "access/xact.h"
#include "pg_lake/rest_catalog/rest_catalog.h"

typedef struct TableMetadataOperationTracker
{
	Oid			relationId;

	bool		relationCreated;
	bool		relationAltered;
	bool		relationPartitionByChanged;
	bool		relationDataFileChanged;
	bool		relationManifestMergeRequested;
	bool		relationSnapshotExpirationRequested;
	bool		relationDataFilesRemoveAllSeen;

	/*
	 * Set when a single data file was removed from the relation, by a DELETE,
	 * an UPDATE or a rewrite. The commit-time diff needs the last pushed
	 * metadata only to find such files, so a transaction that never set this
	 * can build its operations from the catalog alone.
	 */
	bool		relationDataFileRemoveSeen;

	/*
	 * Ids of the files this transaction added, tagged with the id of the
	 * subtransaction each was added in. TrackAddedFileIds appends here from
	 * the same place that inserts into lake_table.files. Once a
	 * subtransaction aborts, AddedFileIdsSubXactCallback drops the entries it
	 * added, the same way the row it was tracking drops out of
	 * lake_table.files; once a subtransaction commits (releases its
	 * savepoint), its entries are reassigned to the parent subtransaction id
	 * so a later sibling subtransaction's rollback cannot mistake them for
	 * its own.
	 *
	 * NIL both before any file is added and once addedFileIdsOverflowed is
	 * set, so emptiness alone does not mean "nothing added" -- check the
	 * overflow flag first.
	 */
	List	   *addedFileIds;

	/*
	 * Set once addedFileIds would grow past MAX_TRACKED_ADDED_FILES. The list
	 * is discarded at that point rather than left partial, since a partial
	 * list would silently under-report what a bulk load added. Once set, the
	 * append-only commit path is unavailable for this relation for the rest
	 * of the transaction and the diff runs instead.
	 */
	bool		addedFileIdsOverflowed;

	/*
	 * Number of single-file data-file operations recorded for this relation
	 * in the current transaction (DATA_FILE_ADD + DATA_FILE_REMOVE). Each
	 * such op rewrites the pg_lake catalogs that the commit-time diff joins,
	 * so the count tracks the work the diff will do regardless of direction.
	 * Used by pg_lake_table.commit_time_analyze_threshold.
	 */
	int64		dataFileChangeCount;

	/*
	 * Set when DATA_FILE_REMOVE_ALL was recorded for this relation. That
	 * single op typically maps to thousands of catalog deletes, so we force
	 * commit-time ANALYZE regardless of dataFileChangeCount.
	 */
	bool		forceCommitTimeAnalyze;
}			TableMetadataOperationTracker;

extern PGDLLEXPORT int CommitTimeCatalogAnalyzeThreshold;
extern PGDLLEXPORT bool EnableAppendOnlyCommitFastPath;

extern PGDLLEXPORT void ConsumeTrackedIcebergMetadataChanges(bool isVerbose);
extern PGDLLEXPORT void PostAllRestCatalogRequests(void);
extern PGDLLEXPORT void TrackIcebergMetadataChangesInTx(Oid relationId, List *metadataOperationTypes);
extern PGDLLEXPORT void TrackAddedFileIds(Oid relationId, const int64 *fileIds, int fileIdCount);
extern PGDLLEXPORT void RecordRestCatalogRequestInTx(Oid relationId, RestCatalogOperationType operationType,
													 const char *body);
extern PGDLLEXPORT void ResetTrackedIcebergMetadataOperation(void);
extern PGDLLEXPORT void ResetRestCatalogRequests(void);
extern PGDLLEXPORT HTAB *GetTrackedIcebergMetadataOperations(void);
extern PGDLLEXPORT bool HasAnyTrackedIcebergMetadataChanges(void);
extern PGDLLEXPORT bool IsIcebergTableCreatedInCurrentTransaction(Oid relation);
extern PGDLLEXPORT void BindRelationToXactRestCatalog(Oid relationId);
extern PGDLLEXPORT void RegisterRestCatalogXactCaptureCallback(void);
extern PGDLLEXPORT void AddedFileIdsSubXactCallback(SubXactEvent event, SubTransactionId mySubid,
													SubTransactionId parentSubid, void *arg);
