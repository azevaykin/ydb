# ColumnShard fulltext — implementation status

## Summary

Two tracks for `STORE = COLUMN` fulltext:

| Track | Intent | Status |
| --- | --- | --- |
| **A** | `LOCAL USING fulltext` — portion-local postings, boolean `FulltextMatch` | Core path A0–A4 landed. Partial A5 (tests exist; force-fallback switch / full counters TBD). |
| **C** | Compact `GLOBAL USING fulltext_*` on a column parent | C0–C3 landed (fence seed scanner). C4/C5 query path largely present. C6 rejects BulkUpsert (TTL adapter TBD). C7 incomplete. |

No package has been compiled or tested in a final `-j80` pass yet.

## Completed

### Q0 — shared boolean query compiler
- `CompileFulltextQuery` / `EvaluateFulltextText` in `ydb/core/base/fulltext_query.h` / `.cpp`
- Row-table `TFullTextSource` uses the shared representation
- Empty query → controlled `BAD_REQUEST`

### A0–A4 — local portion fulltext
- Flag `EnableLocalFulltextIndex` (default off, DDL only)
- `LOCAL USING fulltext`, OLAP class `FULLTEXT`, `KqpOlapFulltextMatch`
- Text evaluation after duplicate resolution (SIMPLE/TRIVIAL); PLAIN unsupported
- YFTI v1 postings; `Built` / `Skipped`; compaction rebuild; scheme actualization
- Exact posting reads with lazy text fallback; wildcard residual on original text

### C0–C2 — mixed-engine writes
- `MixedEngineHtapProtocol`; per-target engine; ColumnShard PK lookup under locks
- Flag `EnableColumnTableGlobalFulltextIndex`; `indexImplStateTable`; native/synthetic doc ids
- `kqp_column_fulltext_write` transactional maintenance; online generations

### C4/C5 — largely present (verify under load)
- `TDocIdMapReader`, `TColumnShardPkFetch`, `ReadyVersion` gate in `kqp_full_text_source.cpp`
- Column-table queries leave `ItemsLimit` unset; residual filters then ORDER BY/LIMIT in KQP
- Not Ready / snapshot before `ReadyVersion` → `PRECONDITION_FAILED`

### C3 — column fulltext seed scanner
- `InitiateColumnShards` queues shards (not false DONE); `FillColumnTableFulltext` drives seed actors
- Seed actor (`ydb/core/kqp/column_fulltext_seed`): `TEvKqpScan` at fence snapshot + `PrepareSeed`/`BuildSeedBatches` + UploadRows
- SchemeShard checkpoints `LastKeyAck`; reboot resumes; resharding rejected during active compact fulltext build
- UTs: `ColumnGlobalFulltextPreFenceSeed`, `ColumnGlobalFulltextDeleteRaceDuringSeed`, `ColumnGlobalFulltextSeedResumeAfterSchemeShardReboot`

### C6 — partial (reject-first)
- BulkUpsert rejected via `ColumnTableGlobalFulltextBulkUpsertRejected` in upload path
- Rejection string for deletion TTL exists; adapter and full enforcement TBD
- UT: `ColumnGlobalFulltextBulkUpsertRejected` in `kqp_olap_fulltext_ut.cpp`

### A5 — partial
- UT suite `KqpOlapFulltext`: DDL default-off, text-fallback match, score rejected, bulk reject
- Force-text-fallback operational switch and full counter set still TBD

## Remaining (priority)

| Package | Gap |
| --- | --- |
| **A5** | Force text-fallback switch; complete counters/hooks; more acceptance cases |
| **C4/C5** | End-to-end verification; any missing lock/flush edges |
| **C6** | Wire TTL rejection everywhere; implement BulkUpsert + deletion-TTL adapters for release |
| **C7** | Recovery/compat/release acceptance matrix |
| **Final verify** | `./ya make --build relwithdebinfo -j80 -tA …` on touched targets |

## Feature flags

| Flag | Role | Default |
| --- | --- | --- |
| `EnableLocalFulltextIndex` | A DDL | off |
| `EnableColumnTableGlobalFulltextIndex` | C DDL | off |

## Notes

- Plan: Cursor plan `columnshard_fulltext_options`.
- Design proposal: [fulltext-index.md](fulltext-index.md).
- Inventory date: from tree inspection after usage-limit interruption of parallel agents.
