#pragma once

#include "defs.h"

#include <ydb/public/api/protos/ydb_table.pb.h>

#include <limits>

namespace NKikimr::NFulltext {

enum class EIndexMode {
    Invalid = 0,
    Fulltext = 1,
    JsonIndexOverJson = 2,
    JsonIndexOverJsonDocument = 3,
};

void BuildNgrams(const TString& token, size_t lengthMin, size_t lengthMax, bool edge, TVector<TString>& ngrams);
Ydb::Table::FulltextIndexSettings::Analyzers GetAnalyzersForQuery(Ydb::Table::FulltextIndexSettings::Analyzers analyzers);

TVector<TString> Analyze(const TStringBuf text, const Ydb::Table::FulltextIndexSettings::Analyzers& settings, const std::unordered_set<wchar32>& ignoredDelimiters = {});

// Bounds input bytes, tokens generated at every stage, and bytes retained in the token vectors.
// A budget stop returns no tokens. Callers must not index a partial token list.
struct TAnalyzeBudget {
    ui64 MaxInputBytes = std::numeric_limits<ui64>::max();
    ui64 MaxGeneratedTokens = std::numeric_limits<ui64>::max();
    ui64 MaxRetainedBytes = std::numeric_limits<ui64>::max();
};

enum class EAnalyzeBudgetStatus {
    Ok = 0,
    InputExceeded,
    GeneratedTokensExceeded,
    RetainedBytesExceeded,
};

struct TBoundedAnalyzeResult {
    TVector<TString> Tokens;
    EAnalyzeBudgetStatus Status = EAnalyzeBudgetStatus::Ok;

    bool Ok() const {
        return Status == EAnalyzeBudgetStatus::Ok;
    }
};

TBoundedAnalyzeResult AnalyzeBounded(const TStringBuf text, const Ydb::Table::FulltextIndexSettings::Analyzers& settings,
    const TAnalyzeBudget& budget, const std::unordered_set<wchar32>& ignoredDelimiters = {});
TVector<TString> BuildSearchTerms(const TString& query, const Ydb::Table::FulltextIndexSettings::Analyzers& settings);

bool ValidateColumnsMatches(const NProtoBuf::RepeatedPtrField<TString>& columns, const Ydb::Table::FulltextIndexSettings& settings, TString& error);
bool ValidateColumnsMatches(const TVector<TString>& columns, const Ydb::Table::FulltextIndexSettings& settings, TString& error);

bool ValidateSettings(const Ydb::Table::FulltextIndexSettings& settings, TString& error);
bool HasSuperLemmer(const Ydb::Table::FulltextIndexSettings& settings);
bool FillSetting(Ydb::Table::FulltextIndexSettings& settings, const TString& nameLower, const TString& value, TString& error);

// Fills unset analyzer flags with their defaults so serialized settings bytes are stable.
Ydb::Table::FulltextIndexSettings::Analyzers NormalizeAnalyzers(const Ydb::Table::FulltextIndexSettings::Analyzers& settings);
void NormalizeFulltextSettings(Ydb::Table::FulltextIndexSettings& settings);
TString FulltextAnalyzerIdentity(const Ydb::Table::FulltextIndexSettings::Analyzers& normalizedAnalyzers);
ui64 FulltextAnalyzerRevision(const Ydb::Table::FulltextIndexSettings::Analyzers& normalizedAnalyzers);

// Version-1 analyzed state stored in __ydb_tokens.
// Plain mode is a strictly increasing token set. Relevance mode also stores a frequency per token.
// A zero-token document, including a tombstone, encodes as a header with a zero count.
struct TDocumentStateToken {
    TString Token;
    ui32 Freq = 1;
};

bool EncodeDocumentState(TConstArrayRef<TDocumentStateToken> tokens, bool withFreq, TString& out, TString& error);
bool DecodeDocumentState(TStringBuf bytes, bool withFreq, TVector<TDocumentStateToken>& tokens, TString& error);

// Compact fulltext compaction reads input segments and writes merged segments.
// Counters are cumulative bytes so amplification is output/input.
void AccountFulltextCompaction(ui64 inputBytes, ui64 outputBytes);

inline constexpr TStringBuf LocalFulltextClassName = "FULLTEXT";
inline constexpr TStringBuf LocalFulltextIndexDisabled =
    "Local fulltext index is disabled with EnableLocalFulltextIndex feature flag";
inline constexpr TStringBuf LocalFulltextIndexColumnTableOnly =
    "Local fulltext index is supported only for column tables";
inline constexpr TStringBuf LocalFulltextIndexOneColumn =
    "Local fulltext index requires exactly one String or Utf8 column";
inline constexpr TStringBuf LocalFulltextIndexNoDataColumns =
    "Local fulltext index does not support data columns";
inline constexpr TStringBuf LocalFulltextIndexAlterRejected =
    "Analyzer settings of a local fulltext index cannot be changed; drop the index and create it again";
inline constexpr TStringBuf LocalFulltextScoreRejected =
    "FulltextScore is not supported for local fulltext indexes";

inline constexpr ui64 LocalFulltextConstructionMemoryBudget = 64ull << 20;
inline constexpr ui64 LocalFulltextAnalyzerMaxInputBytes = 1ull << 20;
inline constexpr ui64 LocalFulltextAnalyzerMaxGeneratedTokens = 100000;
inline constexpr ui64 LocalFulltextAnalyzerMaxRetainedBytes = 4ull << 20;

class TDeltaWriter {
    TVector<ui8> Buf;
    ui64 MaxId = 0;
    ui64 Count = 0;
    bool WithFreq = false;
    bool Sign = false;
public:
    void Reset(bool withFreq, bool sign);
    void Add(ui64 DocId, ui32 Freq);
    ui64 GetMaxId() const;
    ui64 GetCount() const;
    TConstArrayRef<ui8> GetBuf() const;
};

class IDeltaReader {
public:
    virtual ~IDeltaReader() = default;
    virtual bool Read(ui64& docId, ui32& freq) = 0;
};

class TDeltaReader: public IDeltaReader {
    TConstArrayRef<ui8> Buf;
    size_t Pos = 0;
    ui64 LastId = 0;
    bool WithFreq = false;
    bool Sign = false;
    ui64 MaxId = UINT64_MAX;
    size_t SavedPos = 0;
    ui64 SavedLastId = 0;
public:
    TDeltaReader(TConstArrayRef<ui8> buf, bool withFreq, bool sign);
    bool Read(ui64& docId, ui32& freq) override;
    void Save();
    void Restore();
    void SetMaxId(ui64 maxId);
};

class TMultiDeltaReader: public IDeltaReader {
    struct TReaderRef {
        TDeltaReader *Reader;
        bool Added = false;
        bool Owned = false;
    };
    struct TItem {
        ui64 DocId = 0;
        // A segment frequency is ui32. Keep the signed merge accumulator wider so a valid
        // frequency above INT32_MAX is not reinterpreted as a deletion.
        i64 Freq = 0;
        ui32 RdrId = 0;
    };
    TVector<std::unique_ptr<TDeltaReader>> OwnedReaders;
    TVector<TReaderRef> Readers;
    TVector<TItem> Items;
    TItem NextItem = { 0, 0, 0 };
    bool Started = false;
    bool WithFreq = false;
    bool Sign = false;
    bool OneLeft = false;
    static bool CompareItems(const TItem& a, const TItem& b) {
        return a.DocId > b.DocId; // min-heap
    }
    static bool CompareSigned(const TItem& a, const TItem& b) {
        return (i64)a.DocId > (i64)b.DocId; // min-heap
    }
    void Consume(ui32 rdrId, TReaderRef& rdr);
    void SelectNext();
public:
    void Reset(bool withFreq, bool sign);
    void Add(bool added, TDeltaReader* rdr);
    void Add(bool added, TConstArrayRef<ui8> buf);
    void Pop();
    void SetMaxId(ui64 maxId);
    void Start();
    void Stop();
    bool Read(ui64& docId, ui32& freq);
};

}
