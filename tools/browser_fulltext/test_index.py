import os
import tempfile
import unittest
from contextlib import nullcontext
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import index
from blocks import PREFIX_CHARS


class IndexFileTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name) / "sample.cc"
        root = patch.object(index, "ROOT", self.path.parent)
        root.start()
        self.addCleanup(root.stop)
        types = patch.object(index, "parameter_types", return_value=(None, None, None, None))
        types.start()
        self.addCleanup(types.stop)
        self.first = index.RANK_STEP * index.FILE_RANGE
        self.old_keys = []
        self.writes = []
        self.deletes = []
        self.after_write = None
        self.tx = Mock()
        self.tx.execute.side_effect = self.execute
        session = Mock()
        session.transaction.return_value.begin.return_value = nullcontext(self.tx)
        self.pool = Mock()
        self.pool.retry_operation_sync.side_effect = lambda fn: fn(session)

    def execute(self, sql, params):
        if "$path" in params:
            self.assertEqual(params["$path"][0], self.path.name)
        if sql == index.PREVIOUS_FILE:
            row = [dict(path=self.path.name, block_id=self.first)] if self.old_keys else []
            return nullcontext([SimpleNamespace(rows=row)])
        if sql == index.NEXT_FILE:
            return nullcontext([SimpleNamespace(rows=[])])
        if sql == index.READ_KEYS:
            return nullcontext([SimpleNamespace(rows=self.old_keys)])
        if sql == index.WRITE_BLOCKS:
            self.writes.extend(params["$rows"][0])
            if self.after_write:
                self.after_write()
        else:
            self.assertEqual(sql, index.DELETE_BLOCKS)
            self.deletes.extend(params["$keys"][0])
        return []

    def test_default_packs_eight_kib_before_starting_next_block(self):
        first = "x\n" * 4096
        self.path.write_bytes((first + "y").encode())
        self.assertEqual(index.index_file(self.pool, self.path), (2, 4097))
        self.assertEqual(self.writes, [dict(block_id=self.first, first_line=1, text=first),
                                      dict(block_id=self.first + 1, first_line=4097, text="y")])

    def test_file_at_one_mb_is_stored_whole(self):
        data = b"first\r\n" + b"x " * 499_996 + b"y"
        self.assertEqual(len(data), 1_000_000)
        self.path.write_bytes(data)
        self.assertEqual(index.index_file(self.pool, self.path, block_bytes=1_000_000), (1, 2))
        self.assertEqual(self.writes, [dict(block_id=self.first, first_line=1, text=data.decode())])
        self.tx.commit.assert_called_once()

    def test_split_line_is_preserved(self):
        needle = "IdentifierAcrossBlockBoundary"
        # Place the identifier across the 1 MB cut in the second physical line.
        line = "a " * 499_997 + needle + "β " * 2_000
        data = "first\r\n" + line + "\r\nlast"
        self.path.write_text(data)
        self.assertEqual(index.index_file(self.pool, self.path), (4, 3))
        self.assertEqual([(r["first_line"], r["block_id"] - self.first) for r in self.writes],
                         [(1, 0), (2, 1), (2, 2), (3, 3)])
        head, first, continuation, tail = (r["text"] for r in self.writes)
        self.assertEqual(head + first + continuation[PREFIX_CHARS:] + tail, data)
        self.assertTrue(any(needle in r["text"] for r in self.writes))

    def test_pack_lines_and_remove_obsolete_keys(self):
        self.path.write_bytes(b"A\nB\r12345\r\nC\n")
        self.old_keys = [dict(block_id=self.first + i, bytes=5) for i in range(4)]
        self.assertEqual(index.index_file(self.pool, self.path, block_bytes=4), (3, 4))
        self.assertEqual([(r["first_line"], r["text"]) for r in self.writes],
                         [(1, "A\nB\r"), (3, "12345\r\n"), (4, "C\n")])
        self.assertEqual(self.deletes, [self.first + 3])
        self.tx.commit.assert_called_once()

    def test_replacement_and_deletion_batches_account_for_old_rows(self):
        self.path.write_bytes(b"a\n" * 3)
        self.old_keys = [dict(block_id=self.first + i, bytes=1_000_000) for i in range(6)]
        index.index_file(self.pool, self.path, block_bytes=2)
        writes = [len(params["$rows"][0]) for (sql, params), _ in self.tx.execute.call_args_list
                  if sql == index.WRITE_BLOCKS]
        deletes = [len(params["$keys"][0]) for (sql, params), _ in self.tx.execute.call_args_list
                   if sql == index.DELETE_BLOCKS]
        self.assertEqual(writes, [1, 1, 1])
        self.assertEqual(deletes, [2, 1])

    def test_changed_or_replaced_source_cannot_commit(self):
        for replacement in (False, True):
            with self.subTest(replacement=replacement):
                self.tx.reset_mock()
                self.path.write_bytes(b"old\n")
                info = self.path.stat()

                def change():
                    target = self.path.with_suffix(".new") if replacement else self.path
                    target.write_bytes(b"new\n")
                    if replacement:
                        os.utime(target, ns=(info.st_atime_ns, info.st_mtime_ns))
                        target.replace(self.path)
                    else:
                        os.utime(target, ns=(info.st_atime_ns, info.st_mtime_ns + 1_000_000_000))

                self.after_write = change
                with self.assertRaisesRegex(RuntimeError, "source changed"):
                    index.index_file(self.pool, self.path)
                self.tx.commit.assert_not_called()
                self.tx.rollback.assert_called_once()

    def test_invalid_utf8_after_first_write_cannot_commit(self):
        self.path.write_bytes(b"a\n" * 501_000 + b"\xff")
        with self.assertRaises(UnicodeDecodeError):
            index.index_file(self.pool, self.path, block_bytes=100)
        self.assertTrue(self.writes)
        self.tx.commit.assert_not_called()
        self.tx.rollback.assert_called_once()


class FileRankTests(unittest.TestCase):
    def test_find_room_at_both_ends_and_between_files(self):
        # Occupied slots are deliberately adjacent. Each file has three parts.
        size = index.FILE_RANGE
        for start, left, right, expected_free in [(0, -1, 0, 0), (100, 101, 102, 102),
                                                  (size - 4, size - 1, size, size - 1)]:
            with self.subTest(start=start):
                files = {(start + i) * size + j: (path, j) for i, path in enumerate("abcd") for j in range(3)}
                original = sorted(files.values())
                tx = Mock()
                session = Mock()
                session.transaction.return_value.begin.side_effect = lambda: nullcontext(tx)

                def execute(sql, params):
                    keys = params["$keys"][0]
                    if sql == index.MOVE_BLOCKS:
                        for old in keys:
                            new = old - params["$old"][0] + params["$new"][0]
                            self.assertNotIn(new, files)
                            files[new] = files[old]
                    else:
                        self.assertEqual(sql, index.DELETE_BLOCKS)
                        for old in keys:
                            del files[old]
                    return []

                tx.execute.side_effect = execute
                with patch.object(index, "parameter_types", return_value=(None,) * 4), \
                     patch.object(index, "occupied_ranks", lambda tx, ranks: {r for r in ranks if r * size in files}), \
                     patch.object(index, "file_keys", lambda tx, rank: {k: 1 for k in files if k // size == rank}):
                    index.make_rank_gap(session, left, right)
                self.assertNotIn(expected_free * size, files)
                self.assertEqual([files[k] for k in sorted(files)], original)

    def test_directory_walk_compares_whole_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            for relative in ("c.cc", "a/b.cc", "a.cc", "a0/b.cc", ".hidden/x.cc"):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            with patch.object(index, "ROOT", root):
                self.assertEqual([p.relative_to(root).as_posix() for p in index.source_files(["."])],
                                 ["a.cc", "a/b.cc", "a0/b.cc", "c.cc"])
