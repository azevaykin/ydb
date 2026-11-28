"""Index UTF-8 source files in YDB in blocks of complete source lines."""

import argparse
from contextlib import contextmanager, suppress
from functools import cache
from itertools import count
import os
from pathlib import Path
import sys

from blocks import BLOCK_BYTES, MAX_BLOCK_BYTES, iter_blocks
from local_ydb import LocalYdb


ROOT = (Path.home() / "browser").resolve()
SOURCE_SUFFIXES = set("""
    .c .cc .cpp .cxx .h .hh .hpp .hxx .inc .inl .ipp .m .mm .s .asm
    .py .pyi .js .jsx .mjs .ts .tsx .java .kt .kts .rs .go .swift
    .gn .gni .cmake .proto .mojom .idl .webidl .sh .bash .rb .pl
    .html .css .scss .xml .sql
""".split())
SOURCE_NAMES = {"CMakeLists.txt", "Makefile", "ya.make"}

# Each file owns a range of consecutive block IDs in path order.
FILE_RANGE = 1 << 32
RANK_STEP = 4096

CREATE_BLOCKS = """
CREATE TABLE IF NOT EXISTS browser_source_blocks (
    block_id Uint64 NOT NULL,
    path Utf8 NOT NULL,
    first_line Uint64 NOT NULL,
    text Utf8 NOT NULL,
    PRIMARY KEY (block_id),
    INDEX by_location GLOBAL SYNC ON (path, first_line),
    INDEX block_trigrams GLOBAL SYNC USING fulltext_plain ON (text)
    WITH (tokenizer=whitespace, use_filter_ngram=true,
          filter_ngram_min_length=3, filter_ngram_max_length=3),
    FAMILY `default` (COMPRESSION = 'lz4')
);
"""

PREVIOUS_FILE = """
DECLARE $path AS Utf8;
SELECT path, block_id FROM browser_source_blocks VIEW by_location WHERE path <= $path
ORDER BY path DESC LIMIT 1;
"""

NEXT_FILE = """
DECLARE $path AS Utf8;
SELECT block_id FROM browser_source_blocks VIEW by_location WHERE path > $path
ORDER BY path LIMIT 1;
"""

READ_KEYS = """
DECLARE $first AS Uint64;
DECLARE $last AS Uint64;
SELECT block_id, LENGTH(CAST(text AS String)) AS bytes
FROM browser_source_blocks WHERE block_id BETWEEN $first AND $last;
"""

READ_SLOTS = """
DECLARE $keys AS List<Uint64>;
SELECT block_id FROM browser_source_blocks WHERE block_id IN $keys;
"""

# Flush each bounded write to the shards inside the same serializable transaction.
WRITE_BLOCKS = """
PRAGMA kikimr.KqpForceImmediateEffectsExecution = "true";
DECLARE $path AS Utf8;
DECLARE $rows AS List<Struct<block_id:Uint64, first_line:Uint64, text:Utf8>>;
UPSERT INTO browser_source_blocks (block_id, path, first_line, text)
SELECT block_id, $path AS path, first_line, text FROM AS_TABLE($rows);
"""

DELETE_BLOCKS = """
PRAGMA kikimr.KqpForceImmediateEffectsExecution = "true";
DECLARE $keys AS List<Uint64>;
DELETE FROM browser_source_blocks WHERE block_id IN $keys;
"""

MOVE_BLOCKS = """
PRAGMA kikimr.KqpForceImmediateEffectsExecution = "true";
DECLARE $keys AS List<Uint64>;
DECLARE $old AS Uint64;
DECLARE $new AS Uint64;
INSERT INTO browser_source_blocks (block_id, path, first_line, text)
SELECT (block_id - $old) + $new, path, first_line, text
FROM browser_source_blocks WHERE block_id IN $keys;
"""


@cache
def parameter_types():
    import ydb

    p = ydb.PrimitiveType
    rows = (ydb.StructType().add_member("block_id", p.Uint64).add_member("first_line", p.Uint64)
            .add_member("text", p.Utf8))
    return p.Utf8, p.Uint64, ydb.ListType(p.Uint64), ydb.ListType(rows)


@contextmanager
def transaction(session):
    with session.transaction().begin() as tx:
        try:
            yield tx
            tx.commit()
        except Exception:
            # An aborted YDB transaction can reject rollback as well. Keep the
            # original error so the session pool can retry a write conflict.
            with suppress(Exception):
                tx.rollback()
            raise


def read_rows(tx, sql, params):
    with tx.execute(sql, params) as results:
        return [row for result in results for row in result.rows]


def file_keys(tx, rank):
    _, uint, _, _ = parameter_types()
    first = rank * FILE_RANGE
    params = {"$first": (first, uint), "$last": (first + FILE_RANGE - 1, uint)}
    return {row["block_id"]: row["bytes"] for row in read_rows(tx, READ_KEYS, params)}


class RankGapFull(Exception):
    """The two adjacent file ranks have no unused integer between them."""


def file_rank(tx, relative):
    path_type, _, _, _ = parameter_types()
    params = {"$path": (relative, path_type)}
    prev = read_rows(tx, PREVIOUS_FILE, params)
    left = prev[0]["block_id"] // FILE_RANGE if prev else -1
    if prev and prev[0]["path"] == relative:
        return left

    following = read_rows(tx, NEXT_FILE, params)
    right = following[0]["block_id"] // FILE_RANGE if following else FILE_RANGE
    if right == left + 1:
        raise RankGapFull(left, right)
    if not following:
        return min(max(left, 0) + RANK_STEP, FILE_RANGE - 1)
    return (left + right) // 2


def occupied_ranks(tx, ranks):
    # Even an empty file has a block at ordinal zero.
    _, _, keys_type, _ = parameter_types()
    keys = [rank * FILE_RANGE for rank in ranks]
    return {r["block_id"] // FILE_RANGE for r in read_rows(tx, READ_SLOTS, {"$keys": (keys, keys_type)})}


def make_rank_gap(session, left, right):
    # Find the closest free slot on either side of the insertion point.
    with transaction(session) as tx:
        for distance in count(1):
            possible = [r for r in (right + distance, left - distance) if 0 <= r < FILE_RANGE]
            if not possible:
                raise RuntimeError("all 32-bit file ranks are in use")
            occupied = occupied_ranks(tx, possible)
            free = next((r for r in possible if r not in occupied), None)
            if free is not None:
                break

    # Move one file at a time. Every commit preserves path order, including if a
    # writer races this one or indexing stops before the new file is inserted.
    _, uint, keys_type, _ = parameter_types()
    step = 1 if free > left else -1
    end = right if step == 1 else left
    for destination in range(free, end, -step):
        source = destination - step
        with transaction(session) as tx:
            if occupied_ranks(tx, (source, destination)) != {source}:
                return
            keys = file_keys(tx, source)
            for batch in block_batches(keys, keys.__getitem__):
                params = {"$keys": (batch, keys_type)}
                list(tx.execute(MOVE_BLOCKS, {**params, "$old": (source * FILE_RANGE, uint),
                                               "$new": (destination * FILE_RANGE, uint)}))
                list(tx.execute(DELETE_BLOCKS, params))


def file_identity(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def block_batches(rows, byte_size):
    batch = []
    size = 0
    for row in rows:
        width = byte_size(row)
        if batch and (size + width > 2_000_000 or len(batch) == 512):
            yield batch
            batch = []
            size = 0
        batch.append(row)
        size += width
    if batch:
        yield batch


def write_blocks(tx, relative, stream, block_bytes):
    path_type, _, key_type, row_type = parameter_types()
    rank = file_rank(tx, relative)
    stale = file_keys(tx, rank)

    def numbered_blocks():
        for ordinal, row in enumerate(iter_blocks(stream, target_bytes=block_bytes)):
            if ordinal == FILE_RANGE:
                raise RuntimeError("file exceeds the 32-bit block ordinal")
            row["block_id"] = rank * FILE_RANGE + ordinal
            yield row

    # Updating the synchronous trigram index touches both the old and new text.
    def write_size(row):
        return len(row["text"].encode("utf-8")) + stale.get(row["block_id"], 0)

    count = lines = 0
    for batch in block_batches(numbered_blocks(), write_size):
        for row in batch:
            lines = row.pop("last_line")
            stale.pop(row["block_id"], None)
        list(tx.execute(WRITE_BLOCKS, {"$path": (relative, path_type), "$rows": (batch, row_type)}))
        count += len(batch)

    for batch in block_batches(stale, stale.__getitem__):
        list(tx.execute(DELETE_BLOCKS, {"$keys": (batch, key_type)}))
    return count, lines


def replace_file(session, path, block_bytes):
    while True:
        try:
            with path.open("rb") as stream, transaction(session) as tx:
                original = file_identity(os.fstat(stream.fileno()))
                result = write_blocks(tx, path.relative_to(ROOT).as_posix(), stream, block_bytes)
                if original != file_identity(path.stat()) or original != file_identity(os.fstat(stream.fileno())):
                    raise RuntimeError("source changed while indexing")
                return result
        except RankGapFull as error:
            make_rank_gap(session, *error.args)


def index_file(pool, path, block_bytes=BLOCK_BYTES):
    return pool.retry_operation_sync(lambda session: replace_file(session, path, block_bytes))


def walk_files(path):
    # A directory entry sorts as "name/" when comparing complete file paths.
    with os.scandir(path) as entries:
        ordered = sorted(entries, key=lambda e: e.name + ("/" if e.is_dir(follow_symlinks=False) else ""))
    for entry in ordered:
        if entry.is_dir(follow_symlinks=False):
            if not entry.name.startswith("."):
                yield from walk_files(entry.path)
        elif entry.is_file(follow_symlinks=False):
            candidate = Path(entry.path)
            if candidate.suffix.lower() in SOURCE_SUFFIXES or candidate.name in SOURCE_NAMES:
                yield candidate


def source_files(paths):
    for name in paths:
        path = ROOT / name
        if path.is_symlink():
            raise ValueError(f"expected a regular file or directory: {name}")
        path = path.resolve(strict=True)
        if not path.is_relative_to(ROOT):
            raise ValueError(f"expected a source path inside {ROOT}: {name}")
        if path.is_file():
            yield path
            continue
        if not path.is_dir():
            raise ValueError(f"expected a regular file or directory: {name}")
        yield from walk_files(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", default=["."],
                        help="file or directory, relative to ~/browser or absolute (default: entire tree)")
    parser.add_argument("--max-files", type=int, help="stop after this many UTF-8 source files")
    parser.add_argument("--block-bytes", type=int, default=BLOCK_BYTES,
                        help="target bytes per block of complete lines (default: %(default)s)")
    args = parser.parse_args()
    if args.max_files is not None and args.max_files < 1:
        parser.error("--max-files must be positive")
    if not 1 <= args.block_bytes <= MAX_BLOCK_BYTES:
        parser.error(f"--block-bytes must be between 1 and {MAX_BLOCK_BYTES}")

    db = LocalYdb()
    with db.session_pool() as pool:
        pool.execute_with_retries(CREATE_BLOCKS)
        index_paths(pool, args)


def index_paths(pool, args):
    lines = blocks = 0
    indexed = set()
    for path in source_files(args.paths):
        if path in indexed:
            continue
        relative = path.relative_to(ROOT).as_posix()
        try:
            block_count, line_count = index_file(pool, path, args.block_bytes)
        except ValueError as error:
            print(f"Skipping {relative}: {error}", file=sys.stderr)
            continue
        except RuntimeError as error:
            sys.exit(f"{relative}: {error}")
        indexed.add(path)
        lines += line_count
        blocks += block_count
        unit = "block" if block_count == 1 else "blocks"
        print(f"{relative} ({line_count} lines, {block_count} {unit})", flush=True)
        if len(indexed) == args.max_files:
            break
    print(f"Indexed {len(indexed)} files, {lines} lines in {blocks} blocks.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        sys.exit(str(error))
