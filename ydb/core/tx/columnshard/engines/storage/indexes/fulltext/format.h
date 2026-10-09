#pragma once

#include <ydb/core/base/fulltext.h>
#include <ydb/library/conclusion/result.h>

#include <vector>

namespace NKikimr::NOlap::NIndexes::NFulltext {

// Version 1 portion chunk. Every integer is little-endian. There is no padding.
// Offsets below are from byte 0 of the chunk unless a field says otherwise.
//
// Prefix
//   [0, 4)    magic 'Y','F','T','I'
//   [4, 8)    u32 format_version = 1
//   [8, 12)   u32 header_length
//             The header occupies [12, 12 + header_length).
//             The dictionary starts at absolute offset 12 + header_length.
//
// Header, relative to byte 12
//   [0, 4)           u32 column_id
//   [4, 8)           u32 index_id
//   [8, 16)          u64 analyzer_revision
//                    CityHash64(SerializeAsString(normalized analyzers))
//   [16, 20)         u32 canonical_settings_size
//   [20, 20 + S)     canonical analyzer settings (S = canonical_settings_size)
//   [20 + S, 24 + S) u32 chunk_row_count
//   [24 + S, 28 + S) u32 token_count
//   header_length = 28 + S
//
// Canonical analyzer settings, relative to the settings bytes. Explicit defaults are stored
// even when the protobuf omits them, in this fixed order:
//   u32 tokenizer
//   u32 language_size, then language_size bytes (0 when unset)
//   u8  use_filter_lowercase
//   u8  use_filter_stopwords
//   u8  use_filter_ngram
//   u8  use_filter_edge_ngram
//   i32 filter_ngram_min_length   (configured value, or 3 when ngram filters are off)
//   i32 filter_ngram_max_length   (configured value, or 4 when ngram filters are off)
//   u8  use_filter_length
//   i32 filter_length_min         (configured value, or 0 when unset)
//   i32 filter_length_max         (configured value, or 0 when unset)
//   u8  use_filter_snowball
//   u8  use_filter_superlemmer
//
// Dictionary: token_count entries, packed, bytewise strictly increasing token bytes.
// Each entry:
//   u32 token_size
//   u8  token[token_size]
//   u32 posting_count          (at least 1)
//   u32 posting_offset         (absolute offset from byte 0)
//   u32 posting_length
//
// Postings follow the dictionary in entry order, contiguous, with no gaps and no trailing bytes.
// posting_offset of entry 0 is the dictionary end. Each next offset is the previous offset plus
// the previous length. The last posting ends at the chunk size.
//
// A posting region is TDeltaWriter::Reset(false, false) then Add(localRow, 1). The frequency
// argument is not stored. Deltas are unsigned varints of (row - previous), and previous starts
// at 0, so row 0 encodes as a single 0x00 byte. Row ids are strictly increasing, unique, and
// each row is < chunk_row_count. The varint stream consumes posting_length bytes exactly.
//
// A row with no tokens still counts in chunk_row_count. An empty token is a normal dictionary key.

inline constexpr char FulltextChunkMagic[4] = {'Y', 'F', 'T', 'I'};
inline constexpr ui32 FulltextChunkFormatVersion = 1;
inline constexpr ui32 FulltextChunkPrefixBytes = 12;
inline constexpr ui32 FulltextChunkHeaderFixedBytes = 28;
inline constexpr i32 FulltextCanonicalNgramMinDefault = 3;
inline constexpr i32 FulltextCanonicalNgramMaxDefault = 4;

inline ui32 FulltextVarintSize(ui64 value) {
    ui32 size = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++size;
    }
    return size;
}

inline ui64 FulltextChunkSerializedBytes(ui32 canonicalSettingsBytes, ui64 dictionaryEntryBytes, ui64 postingBytes) {
    return static_cast<ui64>(FulltextChunkPrefixBytes) + FulltextChunkHeaderFixedBytes + canonicalSettingsBytes + dictionaryEntryBytes +
        postingBytes;
}

struct TFulltextPostingList {
    TString Token;
    std::vector<ui32> Rows;
};

struct TFulltextChunkPlain {
    ui32 ColumnId = 0;
    ui32 IndexId = 0;
    ui64 AnalyzerRevision = 0;
    TString CanonicalSettings;
    Ydb::Table::FulltextIndexSettings::Analyzers Analyzers;
    ui32 ChunkRowCount = 0;
    std::vector<TFulltextPostingList> Postings;
};

TString CanonicalAnalyzerSettings(const Ydb::Table::FulltextIndexSettings::Analyzers& normalized);
TConclusion<Ydb::Table::FulltextIndexSettings::Analyzers> ParseCanonicalAnalyzerSettings(TStringBuf bytes);

// Encoder failure is a caller invariant failure. Decoder failure is malformed stored bytes.
TConclusion<TString> EncodeFulltextChunk(const TFulltextChunkPlain& chunk);
TConclusion<TFulltextChunkPlain> DecodeFulltextChunk(TStringBuf bytes);

} // namespace NKikimr::NOlap::NIndexes::NFulltext
