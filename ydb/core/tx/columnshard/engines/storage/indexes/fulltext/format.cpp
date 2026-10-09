#include "format.h"

#include <util/generic/ylimits.h>

#include <algorithm>

namespace NKikimr::NOlap::NIndexes::NFulltext {

namespace {

void AppendU32(TString& out, ui32 value) {
    char buf[4];
    buf[0] = static_cast<char>(value & 0xffu);
    buf[1] = static_cast<char>((value >> 8) & 0xffu);
    buf[2] = static_cast<char>((value >> 16) & 0xffu);
    buf[3] = static_cast<char>((value >> 24) & 0xffu);
    out.append(buf, 4);
}

void AppendU64(TString& out, ui64 value) {
    char buf[8];
    for (int i = 0; i < 8; ++i) {
        buf[i] = static_cast<char>((value >> (8 * i)) & 0xffu);
    }
    out.append(buf, 8);
}

void AppendI32(TString& out, i32 value) {
    AppendU32(out, static_cast<ui32>(value));
}

bool ReadU32(TStringBuf& in, ui32& value, TString& error) {
    if (in.size() < 4) {
        error = "truncated fulltext chunk integer";
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(in.data());
    value = ui32(bytes[0]) | (ui32(bytes[1]) << 8) | (ui32(bytes[2]) << 16) | (ui32(bytes[3]) << 24);
    in.Skip(4);
    return true;
}

bool ReadU64(TStringBuf& in, ui64& value, TString& error) {
    if (in.size() < 8) {
        error = "truncated fulltext chunk integer";
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(in.data());
    value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= ui64(bytes[i]) << (8 * i);
    }
    in.Skip(8);
    return true;
}

bool ReadI32(TStringBuf& in, i32& value, TString& error) {
    ui32 raw = 0;
    if (!ReadU32(in, raw, error)) {
        return false;
    }
    value = static_cast<i32>(raw);
    return true;
}

bool ReadByte(TStringBuf& in, ui8& value, TString& error) {
    if (in.empty()) {
        error = "truncated fulltext canonical settings";
        return false;
    }
    value = static_cast<ui8>(in[0]);
    in.Skip(1);
    return true;
}

bool ReadBool(TStringBuf& in, bool& value, TString& error) {
    ui8 raw = 0;
    if (!ReadByte(in, raw, error)) {
        return false;
    }
    if (raw > 1) {
        error = "fulltext canonical settings bool is not 0 or 1";
        return false;
    }
    value = raw != 0;
    return true;
}

bool ReadVarint(TStringBuf bytes, size_t& pos, ui64& value, TString& error) {
    ui64 result = 0;
    ui32 shift = 0;
    bool terminated = false;
    while (pos < bytes.size()) {
        const ui64 c = static_cast<unsigned char>(bytes[pos++]);
        const ui64 payload = c & 0x7f;
        if (shift >= 64 || (shift != 0 && payload > (Max<ui64>() >> shift))) {
            error = "fulltext posting varint overflows";
            return false;
        }
        result |= payload << shift;
        if ((c & 0x80) == 0) {
            if (shift != 0 && payload == 0) {
                error = "fulltext posting varint is not canonical";
                return false;
            }
            terminated = true;
            break;
        }
        if (shift > 64 - 7) {
            error = "fulltext posting varint is too long";
            return false;
        }
        shift += 7;
    }
    if (!terminated) {
        error = "fulltext posting varint is truncated";
        return false;
    }
    value = result;
    return true;
}

bool DecodePlainPostings(TStringBuf bytes, ui32 expectedCount, ui32 chunkRowCount, std::vector<ui32>& rows, TString& error) {
    rows.clear();
    if (expectedCount == 0) {
        error = "fulltext dictionary entry has no postings";
        return false;
    }
    size_t pos = 0;
    ui64 last = 0;
    bool any = false;
    rows.reserve(expectedCount);
    for (ui32 i = 0; i < expectedCount; ++i) {
        if (pos >= bytes.size()) {
            error = "fulltext posting list ended before its declared count";
            return false;
        }
        ui64 delta = 0;
        if (!ReadVarint(bytes, pos, delta, error)) {
            return false;
        }
        if (delta > Max<ui64>() - last) {
            error = "fulltext posting row id overflows";
            return false;
        }
        const ui64 row = last + delta;
        if (any && row <= last) {
            error = "fulltext posting row ids are not strictly increasing";
            return false;
        }
        if (row >= chunkRowCount || row > Max<ui32>()) {
            error = "fulltext posting row id is outside the chunk";
            return false;
        }
        rows.push_back(static_cast<ui32>(row));
        last = row;
        any = true;
    }
    if (pos != bytes.size()) {
        error = "fulltext posting list has trailing bytes";
        return false;
    }
    return true;
}

ui64 PostingBytes(const std::vector<ui32>& rows) {
    ui64 bytes = 0;
    ui32 previous = 0;
    bool any = false;
    for (const ui32 row : rows) {
        const ui64 delta = any ? static_cast<ui64>(row) - previous : static_cast<ui64>(row);
        bytes += FulltextVarintSize(delta);
        previous = row;
        any = true;
    }
    return bytes;
}

}

TString CanonicalAnalyzerSettings(const Ydb::Table::FulltextIndexSettings::Analyzers& normalized) {
    TString out;
    out.reserve(31 + normalized.language().size());
    AppendU32(out, static_cast<ui32>(normalized.tokenizer()));
    AppendU32(out, static_cast<ui32>(normalized.language().size()));
    out.append(normalized.language());
    out.push_back(normalized.use_filter_lowercase() ? 1 : 0);
    out.push_back(normalized.use_filter_stopwords() ? 1 : 0);
    out.push_back(normalized.use_filter_ngram() ? 1 : 0);
    out.push_back(normalized.use_filter_edge_ngram() ? 1 : 0);
    const bool ngram = normalized.use_filter_ngram() || normalized.use_filter_edge_ngram();
    if (ngram) {
        AppendI32(out, normalized.has_filter_ngram_min_length() ? normalized.filter_ngram_min_length() : 0);
        AppendI32(out, normalized.has_filter_ngram_max_length() ? normalized.filter_ngram_max_length() : 0);
    } else {
        AppendI32(out, FulltextCanonicalNgramMinDefault);
        AppendI32(out, FulltextCanonicalNgramMaxDefault);
    }
    out.push_back(normalized.use_filter_length() ? 1 : 0);
    AppendI32(out, normalized.use_filter_length() && normalized.has_filter_length_min() ? normalized.filter_length_min() : 0);
    AppendI32(out, normalized.use_filter_length() && normalized.has_filter_length_max() ? normalized.filter_length_max() : 0);
    out.push_back(normalized.use_filter_snowball() ? 1 : 0);
    out.push_back(normalized.use_filter_superlemmer() ? 1 : 0);
    return out;
}

TConclusion<Ydb::Table::FulltextIndexSettings::Analyzers> ParseCanonicalAnalyzerSettings(TStringBuf bytes) {
    TString error;
    ui32 tokenizer = 0;
    ui32 languageSize = 0;
    if (!ReadU32(bytes, tokenizer, error) || !ReadU32(bytes, languageSize, error)) {
        return TConclusionStatus::Fail(error);
    }
    if (languageSize > bytes.size()) {
        return TConclusionStatus::Fail("truncated fulltext canonical language");
    }
    const TString language(bytes.data(), languageSize);
    bytes.Skip(languageSize);
    bool lowercase = false;
    bool stopwords = false;
    bool ngram = false;
    bool edge = false;
    i32 ngramMin = 0;
    i32 ngramMax = 0;
    bool length = false;
    i32 lengthMin = 0;
    i32 lengthMax = 0;
    bool snowball = false;
    bool superlemmer = false;
    if (!ReadBool(bytes, lowercase, error) || !ReadBool(bytes, stopwords, error) || !ReadBool(bytes, ngram, error) || !ReadBool(bytes, edge, error) ||
        !ReadI32(bytes, ngramMin, error) || !ReadI32(bytes, ngramMax, error) || !ReadBool(bytes, length, error) || !ReadI32(bytes, lengthMin, error) ||
        !ReadI32(bytes, lengthMax, error) || !ReadBool(bytes, snowball, error) || !ReadBool(bytes, superlemmer, error))
    {
        return TConclusionStatus::Fail(error);
    }
    if (!bytes.empty()) {
        return TConclusionStatus::Fail("fulltext canonical settings have trailing bytes");
    }

    Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
    analyzers.set_tokenizer(static_cast<Ydb::Table::FulltextIndexSettings::Tokenizer>(tokenizer));
    if (!language.empty()) {
        analyzers.set_language(language);
    }
    analyzers.set_use_filter_lowercase(lowercase);
    analyzers.set_use_filter_stopwords(stopwords);
    analyzers.set_use_filter_ngram(ngram);
    analyzers.set_use_filter_edge_ngram(edge);
    if (ngram || edge) {
        if (ngramMin != 0) {
            analyzers.set_filter_ngram_min_length(ngramMin);
        }
        if (ngramMax != 0) {
            analyzers.set_filter_ngram_max_length(ngramMax);
        }
    }
    analyzers.set_use_filter_length(length);
    if (length) {
        if (lengthMin != 0) {
            analyzers.set_filter_length_min(lengthMin);
        }
        if (lengthMax != 0) {
            analyzers.set_filter_length_max(lengthMax);
        }
    }
    analyzers.set_use_filter_snowball(snowball);
    analyzers.set_use_filter_superlemmer(superlemmer);
    return NKikimr::NFulltext::NormalizeAnalyzers(analyzers);
}

TConclusion<TString> EncodeFulltextChunk(const TFulltextChunkPlain& chunk) {
    const auto normalized = NKikimr::NFulltext::NormalizeAnalyzers(chunk.Analyzers);
    const TString canonical = CanonicalAnalyzerSettings(normalized);
    if (canonical.size() > Max<ui32>()) {
        return TConclusionStatus::Fail("fulltext canonical settings do not fit a u32");
    }
    std::vector<TFulltextPostingList> postings = chunk.Postings;
    std::sort(postings.begin(), postings.end(), [](const TFulltextPostingList& left, const TFulltextPostingList& right) {
        return left.Token < right.Token;
    });
    for (size_t i = 0; i < postings.size(); ++i) {
        if (i > 0 && !(postings[i - 1].Token < postings[i].Token)) {
            return TConclusionStatus::Fail("fulltext dictionary tokens are not strictly increasing");
        }
        if (postings[i].Rows.empty()) {
            return TConclusionStatus::Fail("fulltext dictionary entry has no postings");
        }
        for (size_t rowIndex = 0; rowIndex < postings[i].Rows.size(); ++rowIndex) {
            const ui32 row = postings[i].Rows[rowIndex];
            if (row >= chunk.ChunkRowCount || (rowIndex > 0 && row <= postings[i].Rows[rowIndex - 1])) {
                return TConclusionStatus::Fail("fulltext posting rows are not a strictly increasing subset of the chunk");
            }
        }
    }
    if (postings.size() > Max<ui32>()) {
        return TConclusionStatus::Fail("fulltext token count does not fit a u32");
    }

    ui64 dictionaryBytes = 0;
    ui64 postingBytes = 0;
    for (const auto& posting : postings) {
        if (posting.Token.size() > Max<ui32>()) {
            return TConclusionStatus::Fail("fulltext token does not fit a u32");
        }
        dictionaryBytes += 16 + posting.Token.size();
        postingBytes += PostingBytes(posting.Rows);
    }
    const ui64 serializedSize = FulltextChunkSerializedBytes(static_cast<ui32>(canonical.size()), dictionaryBytes, postingBytes);
    TString out;
    out.reserve(serializedSize);
    out.append(FulltextChunkMagic, 4);
    AppendU32(out, FulltextChunkFormatVersion);
    AppendU32(out, FulltextChunkHeaderFixedBytes + static_cast<ui32>(canonical.size()));
    AppendU32(out, chunk.ColumnId);
    AppendU32(out, chunk.IndexId);
    AppendU64(out, NKikimr::NFulltext::FulltextAnalyzerRevision(normalized));
    AppendU32(out, static_cast<ui32>(canonical.size()));
    out.append(canonical);
    AppendU32(out, chunk.ChunkRowCount);
    AppendU32(out, static_cast<ui32>(postings.size()));

    const ui64 dictionaryEnd = out.size() + dictionaryBytes;
    ui64 cursor = dictionaryEnd;
    for (const auto& posting : postings) {
        const ui64 length = PostingBytes(posting.Rows);
        AppendU32(out, static_cast<ui32>(posting.Token.size()));
        out.append(posting.Token);
        AppendU32(out, static_cast<ui32>(posting.Rows.size()));
        AppendU32(out, static_cast<ui32>(cursor));
        AppendU32(out, static_cast<ui32>(length));
        cursor += length;
    }
    if (out.size() != dictionaryEnd) {
        return TConclusionStatus::Fail("fulltext dictionary size estimate diverged");
    }
    for (const auto& posting : postings) {
        NKikimr::NFulltext::TDeltaWriter writer;
        writer.Reset(false, false);
        for (const ui32 row : posting.Rows) {
            writer.Add(row, 1);
        }
        const auto buf = writer.GetBuf();
        if (buf.size() != PostingBytes(posting.Rows)) {
            return TConclusionStatus::Fail("fulltext posting size estimate diverged from TDeltaWriter");
        }
        out.append(reinterpret_cast<const char*>(buf.data()), buf.size());
    }
    if (out.size() != serializedSize || cursor != out.size()) {
        return TConclusionStatus::Fail("fulltext chunk size estimate diverged");
    }
    return out;
}

TConclusion<TFulltextChunkPlain> DecodeFulltextChunk(TStringBuf bytes) {
    if (bytes.size() < FulltextChunkPrefixBytes) {
        return TConclusionStatus::Fail("truncated fulltext chunk prefix");
    }
    if (bytes.Head(4) != TStringBuf(FulltextChunkMagic, 4)) {
        return TConclusionStatus::Fail("fulltext chunk magic is not YFTI");
    }
    TStringBuf cursor = bytes;
    cursor.Skip(4);
    TString error;
    ui32 version = 0;
    ui32 headerLength = 0;
    if (!ReadU32(cursor, version, error) || !ReadU32(cursor, headerLength, error)) {
        return TConclusionStatus::Fail(error);
    }
    if (version != FulltextChunkFormatVersion) {
        return TConclusionStatus::Fail("unsupported fulltext chunk format version");
    }
    if (static_cast<ui64>(headerLength) > cursor.size()) {
        return TConclusionStatus::Fail("fulltext chunk header length exceeds the blob");
    }
    TStringBuf header = cursor.Head(headerLength);
    cursor.Skip(headerLength);
    ui32 columnId = 0;
    ui32 indexId = 0;
    ui64 revision = 0;
    ui32 settingsSize = 0;
    if (!ReadU32(header, columnId, error) || !ReadU32(header, indexId, error) || !ReadU64(header, revision, error) ||
        !ReadU32(header, settingsSize, error))
    {
        return TConclusionStatus::Fail(error);
    }
    if (settingsSize > header.size()) {
        return TConclusionStatus::Fail("fulltext canonical settings exceed the header");
    }
    const TStringBuf settingsBytes = header.Head(settingsSize);
    header.Skip(settingsSize);
    ui32 chunkRowCount = 0;
    ui32 tokenCount = 0;
    if (!ReadU32(header, chunkRowCount, error) || !ReadU32(header, tokenCount, error)) {
        return TConclusionStatus::Fail(error);
    }
    if (!header.empty()) {
        return TConclusionStatus::Fail("fulltext chunk header has trailing bytes");
    }
    if (headerLength != FulltextChunkHeaderFixedBytes + settingsSize) {
        return TConclusionStatus::Fail("fulltext chunk header length does not match its fields");
    }

    auto analyzersConclusion = ParseCanonicalAnalyzerSettings(settingsBytes);
    if (analyzersConclusion.IsFail()) {
        return TConclusionStatus::Fail(analyzersConclusion.GetErrorMessage());
    }
    const auto analyzers = analyzersConclusion.DetachResult();
    const TString canonical = CanonicalAnalyzerSettings(analyzers);
    if (canonical != settingsBytes) {
        return TConclusionStatus::Fail("fulltext canonical settings are not normalized");
    }
    if (NKikimr::NFulltext::FulltextAnalyzerRevision(analyzers) != revision) {
        return TConclusionStatus::Fail("fulltext analyzer revision does not match canonical settings");
    }

    if (tokenCount > 0 && cursor.size() / 16 < tokenCount) {
        return TConclusionStatus::Fail("fulltext token count does not fit the dictionary");
    }
    TFulltextChunkPlain plain;
    plain.ColumnId = columnId;
    plain.IndexId = indexId;
    plain.AnalyzerRevision = revision;
    plain.CanonicalSettings = canonical;
    plain.Analyzers = analyzers;
    plain.ChunkRowCount = chunkRowCount;
    plain.Postings.reserve(tokenCount);

    struct TDictEntry {
        TString Token;
        ui32 Count = 0;
        ui32 Offset = 0;
        ui32 Length = 0;
    };
    std::vector<TDictEntry> entries;
    entries.reserve(tokenCount);
    for (ui32 i = 0; i < tokenCount; ++i) {
        ui32 tokenSize = 0;
        if (!ReadU32(cursor, tokenSize, error)) {
            return TConclusionStatus::Fail(error);
        }
        if (tokenSize > cursor.size()) {
            return TConclusionStatus::Fail("fulltext dictionary token exceeds the blob");
        }
        TDictEntry entry;
        entry.Token = TString(cursor.data(), tokenSize);
        cursor.Skip(tokenSize);
        if (!ReadU32(cursor, entry.Count, error) || !ReadU32(cursor, entry.Offset, error) || !ReadU32(cursor, entry.Length, error)) {
            return TConclusionStatus::Fail(error);
        }
        if (entry.Count == 0) {
            return TConclusionStatus::Fail("fulltext dictionary entry has no postings");
        }
        if (!entries.empty() && !(entries.back().Token < entry.Token)) {
            return TConclusionStatus::Fail("fulltext dictionary tokens are not strictly increasing");
        }
        entries.push_back(std::move(entry));
    }

    const ui64 dictionaryEnd = bytes.size() - cursor.size();
    ui64 expectedOffset = dictionaryEnd;
    for (const auto& entry : entries) {
        if (static_cast<ui64>(entry.Offset) != expectedOffset) {
            return TConclusionStatus::Fail("fulltext posting offset is not contiguous");
        }
        if (entry.Length > bytes.size() || entry.Offset > bytes.size() - entry.Length) {
            return TConclusionStatus::Fail("fulltext posting range is outside the blob");
        }
        if (static_cast<ui64>(entry.Count) > entry.Length) {
            return TConclusionStatus::Fail("fulltext posting count exceeds the posting bytes");
        }
        expectedOffset += entry.Length;
    }
    if (expectedOffset != bytes.size()) {
        return TConclusionStatus::Fail("fulltext chunk has trailing bytes");
    }

    for (const auto& entry : entries) {
        TFulltextPostingList posting;
        posting.Token = entry.Token;
        const TStringBuf region = bytes.SubStr(entry.Offset, entry.Length);
        if (!DecodePlainPostings(region, entry.Count, chunkRowCount, posting.Rows, error)) {
            return TConclusionStatus::Fail(error);
        }
        plain.Postings.push_back(std::move(posting));
    }
    return plain;
}

} // namespace NKikimr::NOlap::NIndexes::NFulltext
