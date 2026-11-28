"""Pack UTF-8 source files into byte-bounded blocks, preserving their original text."""

import re


MAX_BLOCK_BYTES = 1_000_000
BLOCK_BYTES = 8 * 1024
# search.sql can look up one more character than the overlap at a split.
PREFIX_CHARS = 127
TOKEN_STEP = 2048
_LONG_TOKEN = re.compile(r"[^ \t\r\n]{%d,}" % (TOKEN_STEP + 1))
_LINE_END = re.compile(r"\r\n|[\r\n]")
_BYTE_LINE_END = re.compile(rb"\r\n|[\r\n]")


def source_lines(text):
    """Yield physical line bodies, recognizing CR, LF and CRLF."""
    start = 0
    for end in _LINE_END.finditer(text):
        yield text[start:end.start()]
        start = end.end()
    if start < len(text):
        yield text[start:]


def _read_parts(stream, size):
    """Read bounded pieces, keeping CRLF together across reads."""
    pending_cr = b""
    while data := stream.read(size):
        if b"\0" in data:
            raise ValueError("source contains a NUL character")
        data = pending_cr + data
        # A trailing CR might be the first byte of CRLF in the next read.
        pending_cr = b"\r" if data.endswith(b"\r") else b""
        stop = len(data) - len(pending_cr)
        start = 0
        for match in _BYTE_LINE_END.finditer(data, 0, stop):
            yield data[start:match.end()], True
            start = match.end()
        if start < stop:
            yield data[start:stop], False
    if pending_cr:
        yield pending_cr, True


def _line_fragments(stream, max_bytes):
    """Yield UTF-8 fragments, whether their line was split, and whether it ended."""
    buffered = bytearray()
    split = False
    for data, ends_line in _read_parts(stream, max_bytes):
        buffered.extend(data)
        while len(buffered) > max_bytes:
            end = max_bytes
            while end and buffered[end] & 0xC0 == 0x80:
                end -= 1
            if not end:
                raise ValueError("byte limit is smaller than a UTF-8 code point")
            yield buffered[:end], True, False
            del buffered[:end]
            split = True
        if ends_line:
            yield buffered, split, True
            buffered = bytearray()
            split = False
    if buffered:
        yield buffered, split, True


def _source_blocks(stream, max_bytes, target_bytes):
    first_line = line_no = 1
    normal = bytearray()
    for data, split, ends_line in _line_fragments(stream, max_bytes):
        if normal and (split or len(normal) + len(data) > target_bytes):
            yield first_line, line_no - 1, normal.decode("utf-8"), False
            normal.clear()
        if split:
            yield line_no, line_no, data.decode("utf-8"), False
        else:
            if not normal:
                first_line = line_no
            normal.extend(data)
        if ends_line:
            line_no += 1
    if normal or line_no == 1:
        whole_file = first_line == 1 and len(normal) <= target_bytes
        yield first_line, line_no - 1, normal.decode("utf-8"), whole_file


def _token_parts(source):
    # The YDB trigram analyzer scans to the end of the token from every position.
    start = 0
    for token in _LONG_TOKEN.finditer(source):
        for cut in range(token.start() + TOKEN_STEP, token.end(), TOKEN_STEP):
            yield source[start:cut]
            start = cut
    yield source[start:]


def iter_blocks(stream, max_bytes=MAX_BLOCK_BYTES, target_bytes=BLOCK_BYTES):
    """Yield blocks in file order with their first and last source line numbers.

    Keep small files whole. Pack complete lines up to the target and split oversized
    lines at max_bytes. Long tokens in larger files receive additional cuts. Within
    each group, every part after the first repeats the previous 127 code points.
    """
    if not 1 <= target_bytes <= max_bytes:
        raise ValueError("block target must be positive and within the byte limit")
    previous_line = None
    prefix = ""
    for first_line, last_line, source, whole_file in _source_blocks(stream, max_bytes, target_bytes):
        pieces = (source,) if whole_file else _token_parts(source)
        for piece in pieces:
            if first_line != previous_line:
                prefix = ""
            elif len(prefix) < PREFIX_CHARS:
                raise ValueError("hard byte limit is too small for the search overlap")
            text = prefix + piece
            yield dict(first_line=first_line, text=text, last_line=last_line)
            previous_line = first_line
            prefix = text[-PREFIX_CHARS:]
