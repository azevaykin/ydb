import io
import unittest

from blocks import BLOCK_BYTES, MAX_BLOCK_BYTES, PREFIX_CHARS, TOKEN_STEP, iter_blocks, source_lines


def pack(text, target=BLOCK_BYTES, hard=MAX_BLOCK_BYTES):
    return list(iter_blocks(io.BytesIO(text.encode()), max_bytes=hard, target_bytes=target))


def original_parts(rows):
    previous_line = None
    for row in rows:
        yield row["text"][PREFIX_CHARS:] if row["first_line"] == previous_line else row["text"]
        previous_line = row["first_line"]


class BlockTests(unittest.TestCase):
    def test_small_files_stored_as_is(self):
        for text in ("", "\r", "\n", "\r\n", "\ufeffA\r\n😺\rB\nlast", "\n\r\n\r", "Z" * BLOCK_BYTES):
            with self.subTest(size=len(text)):
                self.assertEqual(pack(text), [dict(first_line=1, text=text,
                                                   last_line=len(list(source_lines(text))))])

    def test_line_mapping_with_mixed_endings(self):
        blocks = pack("A\r\nBB\rC\n\nD", 6)
        self.assertEqual([(b["first_line"], b["last_line"], b["text"]) for b in blocks],
                         [(1, 2, "A\r\nBB\r"), (3, 5, "C\n\nD")])

    def test_physical_line_bodies(self):
        self.assertEqual(list(source_lines("")), [])
        self.assertEqual(list(source_lines("\n\r\r\n")), ["", "", ""])
        self.assertEqual(list(source_lines("x\u0085y\u2028z\r\n\r😺")), ["x\u0085y\u2028z", "", "😺"])

    def test_at_hard_byte_limit(self):
        line = "x " * ((MAX_BLOCK_BYTES - 2) // 2) + "\r\n"
        self.assertEqual(pack(line)[0]["text"], line)
        blocks = pack(line + "b")
        self.assertEqual([(b["first_line"], len(b["text"])) for b in blocks],
                         [(1, MAX_BLOCK_BYTES), (2, 1)])

    def test_pack_to_target_and_keep_long_lines_whole(self):
        blocks = pack("A\nB\n1234567\nC\nD\nE\n", target=4)
        self.assertEqual([(b["first_line"], b["text"]) for b in blocks],
                         [(1, "A\nB\n"), (3, "1234567\n"), (4, "C\nD\n"), (6, "E\n")])

    def test_crlf_at_buffer_boundary(self):
        # CR and LF land in separate reads and separate oversized-line fragments.
        line = "x " * (MAX_BLOCK_BYTES // 2 - 1) + "x\r\n"
        blocks = pack(line + "Z\rX\r")
        self.assertEqual([(b["first_line"], b["last_line"]) for b in blocks],
                         [(1, 1), (1, 1), (2, 3)])
        self.assertEqual(blocks[1]["text"][PREFIX_CHARS:], "\n")
        self.assertEqual("".join(original_parts(blocks)), line + "Z\rX\r")

    def test_multiline_group_with_split_token(self):
        token = "a" * TOKEN_STEP + "b"
        text = "a\n" + token + "\r\nlast\r"
        rows = pack(text + " " * 6000, target=5000)
        self.assertEqual([(r["first_line"], r["last_line"]) for r in rows],
                         [(1, 3), (1, 3), (4, 4)])
        self.assertEqual(rows[1]["text"], "a" * PREFIX_CHARS + "b\r\nlast\r")
        self.assertEqual("".join(original_parts(rows)), text + " " * 6000)

    def test_split_unicode_line_and_reassemble(self):
        # The hard boundary falls inside a UTF-8 character shortly after the token split.
        line = "".join(chr(0x4e00 + i) for i in range(TOKEN_STEP * 2 + 70)) + "\r\n"
        blocks = pack("ok\r" + line + "last\u0085line", target=6000, hard=TOKEN_STEP * 3 + 8)
        self.assertEqual(blocks[0]["text"], "ok\r")
        self.assertEqual((blocks[-1]["first_line"], blocks[-1]["text"]), (3, "last\u0085line"))
        parts = blocks[1:-1]
        self.assertGreaterEqual(len(parts), 4)
        restored = ""
        for b, piece in zip(parts, original_parts(parts)):
            self.assertEqual((b["first_line"], b["last_line"]), (2, 2))
            self.assertEqual(b["text"], restored[-PREFIX_CHARS:] + piece)
            restored += piece
        self.assertEqual(restored.encode(), line.encode())
        # All code points in the body are distinct, including around both seams.
        body = line[:-2]
        width = PREFIX_CHARS + 1
        for start in range(len(body) - width + 1):
            self.assertTrue(any(body[start:start + width] in b["text"] for b in parts), start)

    def test_reject_invalid_input(self):
        with self.assertRaisesRegex(ValueError, "NUL"):
            pack("A" * 20 + "\0", 4)
        with self.assertRaises(UnicodeDecodeError):
            list(iter_blocks(io.BytesIO(b"\xff")))
        for target in (0, MAX_BLOCK_BYTES + 1):
            with self.subTest(target=target), self.assertRaises(ValueError):
                pack("hello", target=target)


if __name__ == "__main__":
    unittest.main()
