#include "kqp_column_fulltext_write.h"

#include <ydb/core/base/table_index.h>

#include <algorithm>
#include <limits>
#include <numeric>

namespace NKikimr::NKqp {
namespace {

using namespace NTableIndex::NFulltext;

TOwnedCellVec OwnCell(const TCell& cell) {
    return TOwnedCellVec::Make(TConstArrayRef<TCell>(&cell, 1));
}

TOwnedCellVec OwnCells(TConstArrayRef<TCell> cells) {
    return TOwnedCellVec::Make(cells);
}

bool SameCell(const TCell& left, const TCell& right, NScheme::TTypeInfo type) {
    return CompareTypedCells(left, right, type) == 0;
}

ui64 DocIdBits(const TCell& cell, NScheme::TTypeInfo type) {
    switch (type.GetTypeId()) {
        case NScheme::NTypeIds::Int32:
            return static_cast<ui64>(cell.AsValue<i32>());
        case NScheme::NTypeIds::Uint32:
            return cell.AsValue<ui32>();
        case NScheme::NTypeIds::Int64:
            return static_cast<ui64>(cell.AsValue<i64>());
        case NScheme::NTypeIds::Uint64:
            return cell.AsValue<ui64>();
        default:
            Y_ENSURE(false, "fulltext document id type is not an integer");
    }
}

struct TDocIdStorage {
    i32 I32 = 0;
    ui32 U32 = 0;
    i64 I64 = 0;
    ui64 U64 = 0;

    TCell Cell(ui64 bits, NScheme::TTypeInfo type) {
        switch (type.GetTypeId()) {
            case NScheme::NTypeIds::Int32:
                I32 = static_cast<i32>(bits);
                return TCell::Make(I32);
            case NScheme::NTypeIds::Uint32:
                U32 = static_cast<ui32>(bits);
                return TCell::Make(U32);
            case NScheme::NTypeIds::Int64:
                I64 = static_cast<i64>(bits);
                return TCell::Make(I64);
            case NScheme::NTypeIds::Uint64:
                U64 = bits;
                return TCell::Make(U64);
            default:
                Y_ENSURE(false, "fulltext document id type is not an integer");
        }
    }
};

bool MakeEmptyCell(NScheme::TTypeInfo type, TString& storage, TCell& out) {
    storage.clear();
    auto fixed = [&](size_t size) {
        storage.assign(size, '\0');
        out = TCell(storage.data(), storage.size());
        return true;
    };
    switch (type.GetTypeId()) {
        case NScheme::NTypeIds::Bool:
        case NScheme::NTypeIds::Int8:
        case NScheme::NTypeIds::Byte:
            return fixed(1);
        case NScheme::NTypeIds::Int16:
        case NScheme::NTypeIds::Uint16:
        case NScheme::NTypeIds::Date:
            return fixed(2);
        case NScheme::NTypeIds::Int32:
        case NScheme::NTypeIds::Uint32:
        case NScheme::NTypeIds::Float:
        case NScheme::NTypeIds::Datetime:
        case NScheme::NTypeIds::Date32:
            return fixed(4);
        case NScheme::NTypeIds::Int64:
        case NScheme::NTypeIds::Uint64:
        case NScheme::NTypeIds::Double:
        case NScheme::NTypeIds::Timestamp:
        case NScheme::NTypeIds::Interval:
        case NScheme::NTypeIds::Datetime64:
        case NScheme::NTypeIds::Timestamp64:
        case NScheme::NTypeIds::Interval64:
            return fixed(8);
        case NScheme::NTypeIds::Uuid:
        case NScheme::NTypeIds::Decimal:
            return fixed(16);
        case NScheme::NTypeIds::String:
        case NScheme::NTypeIds::Utf8:
        case NScheme::NTypeIds::Json:
        case NScheme::NTypeIds::JsonDocument:
        case NScheme::NTypeIds::Yson:
        case NScheme::NTypeIds::DyNumber:
            out = TCell("", 0);
            return true;
        default:
            return false;
    }
}

TString AnalyzeDocument(
        const TCell& text,
        NScheme::TTypeInfo textType,
        const Ydb::Table::FulltextIndexSettings::Analyzers& analyzers,
        bool withFreq,
        TVector<NFulltext::TDocumentStateToken>& tokens,
        ui32& length)
{
    tokens.clear();
    length = 0;
    if (text.IsNull()) {
        TString encoded;
        TString error;
        if (!NFulltext::EncodeDocumentState(tokens, withFreq, encoded, error)) {
            return error;
        }
        return {};
    }
    if (textType.GetTypeId() != NScheme::NTypeIds::String && textType.GetTypeId() != NScheme::NTypeIds::Utf8) {
        return "Fulltext index text column must be String or Utf8";
    }
    const auto analyzed = NFulltext::Analyze(text.AsBuf(), analyzers);
    THashMap<TString, ui32> freq;
    for (const auto& token : analyzed) {
        auto& count = freq[token];
        if (count == std::numeric_limits<ui32>::max()) {
            return "Fulltext document has too many occurrences of one token";
        }
        ++count;
        if (length == std::numeric_limits<ui32>::max()) {
            return "Fulltext document is too long";
        }
        ++length;
    }
    tokens.reserve(freq.size());
    for (auto& [token, count] : freq) {
        tokens.push_back(NFulltext::TDocumentStateToken{token, withFreq ? count : 1});
    }
    std::sort(tokens.begin(), tokens.end(), [](const auto& left, const auto& right) {
        return left.Token < right.Token;
    });
    TString encoded;
    TString error;
    if (!NFulltext::EncodeDocumentState(tokens, withFreq, encoded, error)) {
        return error;
    }
    return {};
}

bool SameTokens(
        const TVector<NFulltext::TDocumentStateToken>& left,
        const TVector<NFulltext::TDocumentStateToken>& right,
        bool withFreq)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].Token != right[i].Token) {
            return false;
        }
        if (withFreq && left[i].Freq != right[i].Freq) {
            return false;
        }
    }
    return true;
}

TVector<TFulltextAnalyzedToken> TokenViews(const TVector<NFulltext::TDocumentStateToken>& tokens) {
    TVector<TFulltextAnalyzedToken> views;
    views.reserve(tokens.size());
    for (const auto& token : tokens) {
        views.push_back(TFulltextAnalyzedToken{token.Token, token.Freq == 0 ? 1 : token.Freq});
    }
    return views;
}

TVector<NScheme::TTypeInfo> SelectDocsDataTypes(
        const TColumnFulltextMaintainer::TIndex& index,
        const std::vector<TString>& keyColumns)
{
    TVector<NScheme::TTypeInfo> types;
    if (!index.Relevance) {
        return types;
    }
    for (const auto& covered : index.Covered) {
        if (!index.Synthetic && !keyColumns.empty() && covered.Name == keyColumns.front()) {
            continue;
        }
        types.push_back(covered.Type);
    }
    return types;
}

IDataBatchProjectionPtr MakeProjection(
        const TColumnFulltextMaintainer::TIndex& index,
        bool added,
        TConstArrayRef<NScheme::TTypeInfo> dataTypes,
        const std::shared_ptr<NMiniKQL::TScopedAlloc>& alloc)
{
    TVector<NScheme::TTypeInfo> types;
    types.reserve(index.Prefix.size() + 2 + dataTypes.size());
    for (const auto& prefix : index.Prefix) {
        types.push_back(prefix.Type);
    }
    types.push_back(index.Text.Type);
    types.push_back(index.DocIdType);
    const ui32 dataCount = added ? dataTypes.size() : 0;
    if (added) {
        for (const auto& type : dataTypes) {
            types.push_back(type);
        }
    }
    TVector<ui32> indexes(types.size());
    std::iota(indexes.begin(), indexes.end(), 0);
    return CreateFulltextTokenizeProjection(
        types,
        dataCount,
        index.Relevance,
        added,
        index.Settings,
        indexes,
        alloc);
}

void AppendBatch(
        std::vector<TColumnFulltextMaintainer::TWriteBatch>& out,
        const TPathId& pathId,
        bool isDelete,
        IDataBatchPtr batch,
        TColumnFulltextMaintainer::TStats& stats,
        bool posting,
        bool state)
{
    if (!batch || batch->IsEmpty()) {
        return;
    }
    if (posting) {
        stats.PostingBytes += batch->GetSerializedMemory();
    }
    if (state) {
        stats.StateBytes += batch->GetSerializedMemory();
    }
    stats.BatchMemory += batch->GetMemory();
    out.push_back(TColumnFulltextMaintainer::TWriteBatch{
        .PathId = pathId,
        .Delete = isDelete,
        .Batch = std::move(batch),
    });
}

} // namespace

using namespace NTableIndex::NFulltext;

void TColumnFulltextMaintainer::SetLayout(
        std::vector<TString> inputColumns,
        std::vector<TString> lookupColumns,
        std::vector<TString> keyColumns)
{
    InputColumns = std::move(inputColumns);
    LookupColumns = std::move(lookupColumns);
    KeyColumns = std::move(keyColumns);
    InputPos.clear();
    LookupPos.clear();
    KeyPos.clear();
    for (size_t i = 0; i < InputColumns.size(); ++i) {
        InputPos[InputColumns[i]] = i;
    }
    for (size_t i = 0; i < LookupColumns.size(); ++i) {
        LookupPos[LookupColumns[i]] = i;
    }
    for (size_t i = 0; i < KeyColumns.size(); ++i) {
        KeyPos[KeyColumns[i]] = i;
    }
}

void TColumnFulltextMaintainer::AddIndex(TIndex index) {
    TIndexPlan plan;
    plan.Index = std::move(index);
    Indexes.push_back(std::move(plan));
}

void TColumnFulltextMaintainer::SetRows(
        NKikimrKqp::TKqpTableSinkSettings::EType operation,
        const std::vector<TInputRow>& rows)
{
    Operation = operation;
    Rows.clear();
    THashMap<TString, size_t> position;
    ui64 order = 0;
    for (const auto& row : rows) {
        Y_ENSURE(row.Cells.size() >= KeyColumns.size());
        const auto key = TSerializedCellVec::Serialize(row.Cells.first(KeyColumns.size()));
        TRawRow raw;
        raw.Cells = OwnCells(row.Cells);
        raw.BaseExists = row.BaseExists;
        raw.BaseExistenceKnown = row.BaseExistenceKnown;
        raw.Order = order++;
        if (const auto it = position.find(key); it != position.end()) {
            Rows[it->second] = std::move(raw);
        } else {
            position.emplace(key, Rows.size());
            Rows.push_back(std::move(raw));
        }
    }
}

void TColumnFulltextMaintainer::ClearState() {
    for (auto& index : Indexes) {
        index.StateRows.clear();
    }
}

void TColumnFulltextMaintainer::SetStateRow(const TPathId& stateId, TConstArrayRef<TCell> row) {
    Y_ENSURE(row.size() >= KeyColumns.size());
    const auto key = TSerializedCellVec::Serialize(row.first(KeyColumns.size()));
    for (auto& index : Indexes) {
        if (index.Index.StateId == stateId) {
            index.StateRows[key] = OwnCells(row);
        }
    }
}

TColumnFulltextMaintainer::TResolved TColumnFulltextMaintainer::FromInput(
        const TPlanned& row, const TString& name) const
{
    const auto it = InputPos.find(name);
    if (it == InputPos.end() || it->second >= row.Row.size()) {
        return {};
    }
    return {true, row.Row[it->second]};
}

TColumnFulltextMaintainer::TResolved TColumnFulltextMaintainer::FromLookup(
        const TPlanned& row, const TString& name) const
{
    const auto it = LookupPos.find(name);
    if (it == LookupPos.end()) {
        return {};
    }
    const size_t pos = InputColumns.size() + it->second;
    if (pos >= row.Row.size()) {
        return {};
    }
    return {true, row.Row[pos]};
}

TColumnFulltextMaintainer::TResolved TColumnFulltextMaintainer::FromState(
        const TIndexPlan& index, const TPlanned& row, const TString& name) const
{
    const auto stateIt = index.StateRows.find(row.Key);
    if (stateIt == index.StateRows.end()) {
        return {};
    }
    const auto& state = stateIt->second;
    if (const auto key = KeyPos.find(name); key != KeyPos.end()) {
        if (key->second < state.size()) {
            return {true, state[key->second]};
        }
        return {};
    }
    for (size_t i = 0; i < index.Index.StateValueNames.size(); ++i) {
        if (index.Index.StateValueNames[i] == name) {
            const size_t pos = KeyColumns.size() + i;
            if (pos < state.size()) {
                return {true, state[pos]};
            }
            return {};
        }
    }
    return {};
}

TColumnFulltextMaintainer::TResolved TColumnFulltextMaintainer::FinalCell(
        const TIndexPlan& index, const TPlanned& row, const TColumn& column, bool stateLive) const
{
    if (const auto key = KeyPos.find(column.Name); key != KeyPos.end() && key->second < row.Row.size()) {
        return {true, row.Row[key->second]};
    }
    if (const auto input = FromInput(row, column.Name); input.Found) {
        return input;
    }
    if (stateLive) {
        if (const auto state = FromState(index, row, column.Name); state.Found) {
            return state;
        }
    }
    if (const auto lookup = FromLookup(row, column.Name); lookup.Found) {
        return lookup;
    }
    return {};
}

TString TColumnFulltextMaintainer::Prepare() {
    if (Indexes.empty()) {
        return {};
    }
    if (KeyColumns.empty()) {
        return "Fulltext index maintenance requires a primary key";
    }
    for (auto& index : Indexes) {
        index.Rows.clear();
        index.DocIdCount = 0;
        index.GenerationCount = 0;
        index.DocIds.clear();
        index.Generations.clear();
        if (index.Index.Settings.columns_size() != 1) {
            return "Fulltext index is missing analyzer settings";
        }
        const auto& analyzers = index.Index.Settings.columns(0).analyzers();
        const bool withFreq = index.Index.Relevance;
        i64 tokenBytes = 0;
        bool anyRemove = false;
        bool anyAdd = false;

        for (const auto& raw : Rows) {
            TPlanned planned;
            planned.Row = raw.Cells;
            planned.Key = TSerializedCellVec::Serialize(raw.Cells.first(KeyColumns.size()));
            planned.Order = raw.Order;
            planned.BaseExists = raw.BaseExists;
            planned.BaseExistenceKnown = raw.BaseExistenceKnown;

            const auto stateIt = index.StateRows.find(planned.Key);
            const bool statePresent = stateIt != index.StateRows.end();
            if (index.Index.SeedOnly && statePresent) {
                continue;
            }
            bool stateExists = false;
            bool docIdPresent = false;
            ui64 stateDocId = 0;
            ui32 stateLength = 0;
            if (statePresent) {
                const auto exists = FromState(index, planned, ExistsColumn);
                if (!exists.Found || exists.Cell.IsNull()) {
                    return "Fulltext forward state is missing __ydb_exists";
                }
                stateExists = exists.Cell.AsValue<bool>();
                const auto format = FromState(index, planned, StateFormatColumn);
                if (format.Found && !format.Cell.IsNull()
                        && format.Cell.AsValue<ui32>() != DocumentStateFormatVersion)
                {
                    return "Unsupported fulltext forward-state format";
                }
                const auto tokens = FromState(index, planned, TokensColumn);
                if (!tokens.Found || tokens.Cell.IsNull()) {
                    return "Fulltext forward state is missing analyzed tokens";
                }
                TString error;
                if (!NFulltext::DecodeDocumentState(tokens.Cell.AsBuf(), withFreq, planned.OldTokens, error)) {
                    return TStringBuilder() << "Fulltext forward state cannot be read: " << error;
                }
                if (const auto docId = FromState(index, planned, DocIdColumn); docId.Found && !docId.Cell.IsNull()) {
                    docIdPresent = true;
                    stateDocId = DocIdBits(docId.Cell, index.Index.DocIdType);
                }
                if (withFreq) {
                    if (const auto length = FromState(index, planned, DocLengthColumn); length.Found && !length.Cell.IsNull()) {
                        stateLength = length.Cell.AsValue<ui32>();
                    }
                }
            }
            const bool stateLive = statePresent && stateExists;
            const bool isDelete = Operation == NKikimrKqp::TKqpTableSinkSettings::MODE_DELETE;

            if (isDelete) {
                if (!stateLive) {
                    if (!index.Index.Ready) {
                        planned.WriteState = true;
                        planned.Tombstone = true;
                    }
                } else {
                    planned.WriteState = true;
                    planned.Tombstone = true;
                    planned.RemovePostings = true;
                    planned.OldPrefix = OwnCells([&] {
                        TVector<TCell> cells;
                        for (const auto& prefix : index.Index.Prefix) {
                            const auto resolved = FromState(index, planned, prefix.Name);
                            cells.push_back(resolved.Found ? resolved.Cell : TCell());
                        }
                        return cells;
                    }());
                    if (!index.Index.Synthetic) {
                        if (KeyColumns.size() != 1) {
                            return "Native fulltext document id requires a single-column primary key";
                        }
                        planned.DocIdNull = false;
                        planned.DocId = DocIdBits(planned.Row[0], index.Index.DocIdType);
                    } else if (!docIdPresent) {
                        return "Fulltext forward state is missing the document id of a live row";
                    } else {
                        planned.DocIdNull = false;
                        planned.DocId = stateDocId;
                        planned.RetireDocId = true;
                        planned.RetiredDocId = stateDocId;
                    }
                    anyRemove = true;
                }
            } else {
                const bool textInInput = InputPos.contains(index.Index.Text.Name) || KeyPos.contains(index.Index.Text.Name);
                bool tokensSame = false;
                if (stateLive && !textInInput) {
                    planned.NewTokens = planned.OldTokens;
                    ui32 length = 0;
                    for (const auto& token : planned.NewTokens) {
                        length += token.Freq;
                    }
                    planned.NewLength = withFreq ? length : stateLength;
                    tokensSame = true;
                } else {
                    const auto text = FinalCell(index, planned, index.Index.Text, stateLive);
                    if (!text.Found) {
                        if (index.Index.Text.NotNull) {
                            return "Fulltext index maintenance is missing the text column";
                        }
                    }
                    const auto analyzeError = AnalyzeDocument(
                        text.Found ? text.Cell : TCell(),
                        index.Index.Text.Type,
                        analyzers,
                        withFreq,
                        planned.NewTokens,
                        planned.NewLength);
                    if (analyzeError) {
                        return TStringBuilder() << "Fulltext document was rejected: " << analyzeError;
                    }
                    TString error;
                    if (!NFulltext::EncodeDocumentState(planned.NewTokens, withFreq, planned.TokensEncoded, error)) {
                        return TStringBuilder() << "Fulltext document was rejected: " << error;
                    }
                    for (const auto& token : planned.NewTokens) {
                        tokenBytes += static_cast<i64>(token.Token.size()) + 24;
                    }
                    if (tokenBytes > ColumnFulltextTokenMemoryLimit) {
                        return "Fulltext index maintenance batch exceeds the memory limit";
                    }
                    tokensSame = stateLive && SameTokens(planned.OldTokens, planned.NewTokens, withFreq);
                }
                if (planned.TokensEncoded.empty()) {
                    TString error;
                    if (!NFulltext::EncodeDocumentState(planned.NewTokens, withFreq, planned.TokensEncoded, error)) {
                        return TStringBuilder() << "Fulltext document was rejected: " << error;
                    }
                }

                bool prefixSame = true;
                TVector<TCell> newPrefix;
                TVector<TCell> oldPrefix;
                for (const auto& prefix : index.Index.Prefix) {
                    const auto resolved = FinalCell(index, planned, prefix, stateLive);
                    if (!resolved.Found && prefix.NotNull) {
                        return TString("Fulltext index maintenance is missing prefix column ") + prefix.Name;
                    }
                    newPrefix.push_back(resolved.Found ? resolved.Cell : TCell());
                    if (stateLive) {
                        const auto old = FromState(index, planned, prefix.Name);
                        const TCell oldCell = old.Found ? old.Cell : TCell();
                        oldPrefix.push_back(oldCell);
                        if (!SameCell(newPrefix.back(), oldCell, prefix.Type)) {
                            prefixSame = false;
                        }
                    }
                }
                planned.NewPrefix = OwnCells(newPrefix);
                if (stateLive) {
                    planned.OldPrefix = OwnCells(oldPrefix);
                }

                bool coveredSame = true;
                TVector<TCell> covered;
                for (const auto& column : index.Index.Covered) {
                    const auto resolved = FinalCell(index, planned, column, stateLive);
                    if (!resolved.Found && column.NotNull) {
                        return TString("Fulltext index maintenance is missing covered column ") + column.Name;
                    }
                    covered.push_back(resolved.Found ? resolved.Cell : TCell());
                    if (stateLive) {
                        const auto old = FromState(index, planned, column.Name);
                        const TCell oldCell = old.Found ? old.Cell : TCell();
                        if (!SameCell(covered.back(), oldCell, column.Type)) {
                            coveredSame = false;
                        }
                    }
                }
                planned.NewCovered = OwnCells(covered);

                bool ttlSame = true;
                if (index.Index.Ttl) {
                    const auto resolved = FinalCell(index, planned, *index.Index.Ttl, stateLive);
                    if (stateLive) {
                        const auto old = FromState(index, planned, index.Index.Ttl->Name);
                        const TCell oldCell = old.Found ? old.Cell : TCell();
                        const TCell newCell = resolved.Found ? resolved.Cell : TCell();
                        ttlSame = SameCell(newCell, oldCell, index.Index.Ttl->Type);
                    } else {
                        ttlSame = false;
                    }
                }

                if (!stateLive) {
                    // Ready absence means the row was not indexed. WriteOnly absence is uninitialized.
                    // Either way the final image is what gets posted; old tokens were not in the index.
                    planned.WriteState = true;
                    planned.AddPostings = true;
                    planned.UpdateDocs = index.Index.Relevance;
                    anyAdd = true;
                    if (index.Index.Synthetic) {
                        planned.AllocateDocId = true;
                        planned.DocIdSlot = static_cast<int>(index.DocIdCount++);
                    } else {
                        if (KeyColumns.size() != 1) {
                            return "Native fulltext document id requires a single-column primary key";
                        }
                        planned.DocIdNull = false;
                        planned.DocId = DocIdBits(planned.Row[0], index.Index.DocIdType);
                    }
                } else if (!tokensSame || !prefixSame) {
                    planned.WriteState = true;
                    planned.RemovePostings = true;
                    planned.AddPostings = true;
                    planned.UpdateDocs = index.Index.Relevance;
                    anyRemove = true;
                    anyAdd = true;
                    planned.DocIdNull = false;
                    if (index.Index.Synthetic) {
                        if (!docIdPresent) {
                            return "Fulltext forward state is missing the document id of a live row";
                        }
                        planned.DocId = stateDocId;
                    } else {
                        planned.DocId = DocIdBits(planned.Row[0], index.Index.DocIdType);
                    }
                } else if (!coveredSame || !ttlSame) {
                    planned.WriteState = true;
                    planned.UpdateDocs = index.Index.Relevance && !coveredSame;
                    planned.DocIdNull = false;
                    planned.NewLength = stateLength;
                    if (planned.TokensEncoded.empty()) {
                        TString error;
                        if (!NFulltext::EncodeDocumentState(planned.NewTokens, withFreq, planned.TokensEncoded, error)) {
                            return TStringBuilder() << "Fulltext document was rejected: " << error;
                        }
                    }
                    if (index.Index.Synthetic) {
                        if (!docIdPresent) {
                            return "Fulltext forward state is missing the document id of a live row";
                        }
                        planned.DocId = stateDocId;
                    } else {
                        planned.DocId = DocIdBits(planned.Row[0], index.Index.DocIdType);
                    }
                }
            }

            if (!planned.WriteState && !planned.RemovePostings && !planned.AddPostings) {
                continue;
            }

            auto putField = [&](const TCell& cell) {
                TPlanned::TStateField field;
                if (cell.IsNull()) {
                    field.Null = true;
                } else {
                    field.Value = OwnCell(cell);
                }
                planned.Fields.push_back(std::move(field));
            };

            for (const auto& column : index.Index.StateColumns) {
                TPlanned::TStateField field;
                if (column.Name == DocIdColumn) {
                    field.DocId = true;
                    planned.Fields.push_back(std::move(field));
                    continue;
                }
                if (column.Name == TokensColumn) {
                    field.Tokens = true;
                    planned.Fields.push_back(std::move(field));
                    continue;
                }
                if (column.Name == ExistsColumn) {
                    const bool exists = !planned.Tombstone;
                    const auto cell = TCell::Make(exists);
                    putField(cell);
                    continue;
                }
                if (column.Name == BuildGenerationColumn) {
                    const auto cell = TCell::Make(index.Index.BuildGeneration);
                    putField(cell);
                    continue;
                }
                if (column.Name == StateFormatColumn) {
                    const ui32 format = DocumentStateFormatVersion;
                    const auto cell = TCell::Make(format);
                    putField(cell);
                    continue;
                }
                if (column.Name == DocLengthColumn) {
                    const ui32 length = planned.Tombstone ? 0 : planned.NewLength;
                    const auto cell = TCell::Make(length);
                    putField(cell);
                    continue;
                }
                TResolved resolved;
                if (planned.Tombstone && statePresent) {
                    resolved = FromState(index, planned, column.Name);
                }
                if (!resolved.Found) {
                    resolved = FinalCell(index, planned, column, stateLive && !planned.Tombstone);
                }
                if (!resolved.Found || resolved.Cell.IsNull()) {
                    if (column.NotNull) {
                        TString storage;
                        TCell empty;
                        if (!MakeEmptyCell(column.Type, storage, empty)) {
                            return TString("Fulltext forward state cannot store column ") + column.Name;
                        }
                        putField(empty);
                    } else {
                        field.Null = true;
                        planned.Fields.push_back(std::move(field));
                    }
                } else {
                    putField(resolved.Cell);
                }
            }
            if (planned.Tombstone && planned.TokensEncoded.empty()) {
                TString error;
                TVector<NFulltext::TDocumentStateToken> empty;
                if (!NFulltext::EncodeDocumentState(empty, withFreq, planned.TokensEncoded, error)) {
                    return TStringBuilder() << "Fulltext document was rejected: " << error;
                }
                planned.NewLength = 0;
            }
            index.Rows.push_back(std::move(planned));
        }

        std::sort(index.Rows.begin(), index.Rows.end(), [](const TPlanned& left, const TPlanned& right) {
            return left.Order < right.Order;
        });
        if (anyRemove) {
            ++index.GenerationCount;
        }
        if (anyAdd) {
            ++index.GenerationCount;
        }
    }
    return {};
}

std::vector<TColumnFulltextMaintainer::TSequenceNeed> TColumnFulltextMaintainer::Sequences() const {
    std::vector<TSequenceNeed> needs;
    for (const auto& index : Indexes) {
        if (index.GenerationCount > 0) {
            needs.push_back(TSequenceNeed{index.Index.PostingId, index.GenerationCount});
        }
        if (index.DocIdCount > 0) {
            needs.push_back(TSequenceNeed{index.Index.MapId, index.DocIdCount});
        }
    }
    return needs;
}

void TColumnFulltextMaintainer::AssignSequences(const TPathId& pathId, const std::vector<ui64>& values) {
    for (auto& index : Indexes) {
        if (index.Index.PostingId == pathId && index.GenerationCount > 0) {
            Y_ENSURE(values.size() == index.GenerationCount);
            index.Generations = values;
        }
        if (index.Index.MapId == pathId && index.DocIdCount > 0) {
            Y_ENSURE(values.size() == index.DocIdCount);
            index.DocIds = values;
        }
    }
}

TString TColumnFulltextMaintainer::PrepareSeed(const std::vector<TInputRow>& rows) {
    for (const auto& index : Indexes) {
        if (!index.Index.SeedOnly) {
            return "Fulltext seed requires SeedOnly indexes";
        }
    }
    std::vector<TInputRow> seeded;
    seeded.reserve(rows.size());
    for (const auto& row : rows) {
        TInputRow copy = row;
        copy.BaseExists = true;
        copy.BaseExistenceKnown = true;
        seeded.push_back(copy);
    }
    SetRows(NKikimrKqp::TKqpTableSinkSettings::MODE_UPSERT, seeded);
    return Prepare();
}

TString TColumnFulltextMaintainer::BuildSeedBatches(
        std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> alloc,
        std::vector<TWriteBatch>& out,
        TStats& stats)
{
    return Build(std::move(alloc), out, stats);
}

TString TColumnFulltextMaintainer::Build(
        std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> alloc,
        std::vector<TWriteBatch>& out,
        TStats& stats)
{
    const bool deleteStatement = Operation == NKikimrKqp::TKqpTableSinkSettings::MODE_DELETE;
    for (auto& index : Indexes) {
        size_t genCursor = 0;
        TString generationError;
        auto nextGen = [&]() -> ui64 {
            if (genCursor >= index.Generations.size()) {
                generationError = "Fulltext index maintenance did not receive online generations";
                return 0;
            }
            const ui64 gen = index.Generations[genCursor++];
            if (gen == std::numeric_limits<ui64>::max()) {
                generationError = "Online fulltext generation must not be the maximum value";
                return 0;
            }
            return gen;
        };

        for (auto& row : index.Rows) {
            if (row.DocIdSlot >= 0) {
                Y_ENSURE(static_cast<size_t>(row.DocIdSlot) < index.DocIds.size());
                const ui64 seq = index.DocIds[row.DocIdSlot];
                if (!IsSyntheticDocIdSeq(seq)) {
                    return "Synthetic fulltext document id sequence is exhausted; ids are never recycled";
                }
                row.DocId = SyntheticDocIdFromSeq(seq);
                row.DocIdNull = false;
            }
        }

        const auto docsDataTypes = SelectDocsDataTypes(index.Index, KeyColumns);
        auto feedPostings = [&](bool added, ui64 gen) -> TString {
            bool any = false;
            for (const auto& row : index.Rows) {
                if (added ? row.AddPostings : row.RemovePostings) {
                    any = true;
                    break;
                }
            }
            if (!any) {
                return {};
            }
            auto projection = MakeProjection(index.Index, added, added ? TConstArrayRef<NScheme::TTypeInfo>(docsDataTypes) : TConstArrayRef<NScheme::TTypeInfo>(), alloc);
            auto* fulltext = static_cast<IFulltextTokenizeProjection*>(projection.Get());
            fulltext->SetGen(static_cast<NTableIndex::NFulltext::TGen>(gen));
            for (const auto& row : index.Rows) {
                if (added ? !row.AddPostings : !row.RemovePostings) {
                    continue;
                }
                const auto& prefix = added ? row.NewPrefix : row.OldPrefix;
                const auto views = TokenViews(added ? row.NewTokens : row.OldTokens);
                TVector<TCell> covered;
                if (added && index.Index.Relevance) {
                    for (size_t i = 0; i < index.Index.Covered.size() && i < row.NewCovered.size(); ++i) {
                        if (!index.Index.Synthetic && index.Index.Covered[i].Name == KeyColumns.front()) {
                            continue;
                        }
                        covered.push_back(row.NewCovered[i]);
                    }
                }
                fulltext->AddAnalyzedDocument(prefix, row.DocId, views, covered);
                if (fulltext->TokenMemoryExceeded()) {
                    return "Fulltext index maintenance batch exceeds the memory limit";
                }
            }
            AppendBatch(out, index.Index.PostingId, false, projection->Flush(), stats, true, false);
            if (index.Index.Relevance) {
                const bool docsDelete = !added;
                const bool useDeleteCookie = docsDelete && !deleteStatement;
                AppendBatch(out, index.Index.DocsId, useDeleteCookie, fulltext->FlushDocs(), stats, false, false);
                AppendBatch(out, index.Index.StatsId, false, fulltext->FlushStats(), stats, false, false);
            }
            return {};
        };

        if (index.GenerationCount > 0 && index.Generations.size() != index.GenerationCount) {
            return "Fulltext index maintenance did not receive online generations";
        }
        if (std::any_of(index.Rows.begin(), index.Rows.end(), [](const TPlanned& row) { return row.RemovePostings; })) {
            const ui64 gen = nextGen();
            if (generationError) {
                return generationError;
            }
            if (const auto error = feedPostings(false, gen)) {
                return error;
            }
        }
        if (std::any_of(index.Rows.begin(), index.Rows.end(), [](const TPlanned& row) { return row.AddPostings; })) {
            const ui64 gen = nextGen();
            if (generationError) {
                return generationError;
            }
            if (const auto error = feedPostings(true, gen)) {
                return error;
            }
        }

        if (index.Index.Relevance) {
            bool docsOnly = false;
            for (const auto& row : index.Rows) {
                if (row.UpdateDocs && !row.AddPostings && !row.RemovePostings) {
                    docsOnly = true;
                    break;
                }
            }
            if (docsOnly) {
                auto batcher = CreateRowsBatcher(2 + docsDataTypes.size(), alloc);
                for (const auto& row : index.Rows) {
                    if (!row.UpdateDocs || row.AddPostings || row.RemovePostings || row.Tombstone) {
                        continue;
                    }
                    TDocIdStorage storage;
                    batcher->AddCell(storage.Cell(row.DocId, index.Index.DocIdType));
                    const auto length = TCell::Make(row.NewLength);
                    batcher->AddCell(length);
                    for (size_t i = 0; i < index.Index.Covered.size() && i < row.NewCovered.size(); ++i) {
                        if (!index.Index.Synthetic && !KeyColumns.empty() && index.Index.Covered[i].Name == KeyColumns.front()) {
                            continue;
                        }
                        batcher->AddCell(row.NewCovered[i]);
                    }
                    batcher->AddRow();
                }
                AppendBatch(out, index.Index.DocsId, false, batcher->Flush(), stats, false, false);
            }
        }

        if (index.Index.Synthetic && index.Index.MapId) {
            auto deletes = CreateRowsBatcher(1, alloc);
            bool anyDelete = false;
            for (const auto& row : index.Rows) {
                if (!row.RetireDocId) {
                    continue;
                }
                const auto cell = TCell::Make(row.RetiredDocId);
                deletes->AddCell(cell);
                deletes->AddRow();
                anyDelete = true;
            }
            if (anyDelete) {
                AppendBatch(out, index.Index.MapId, true, deletes->Flush(), stats, false, false);
            }

            ui32 mapColumns = index.Index.MapColumns.empty() ? 1 + KeyColumns.size() : index.Index.MapColumns.size();
            auto upserts = CreateRowsBatcher(mapColumns, alloc);
            bool anyUpsert = false;
            for (const auto& row : index.Rows) {
                if (row.Tombstone || row.DocIdNull || !row.AddPostings) {
                    continue;
                }
                // A kept document id does not need a new map row. A newly allocated id does.
                if (row.DocIdSlot < 0 && !row.AllocateDocId) {
                    continue;
                }
                TDocIdStorage storage;
                if (index.Index.MapColumns.empty()) {
                    upserts->AddCell(storage.Cell(row.DocId, NScheme::TTypeInfo(NScheme::NTypeIds::Uint64)));
                    for (size_t i = 0; i < KeyColumns.size(); ++i) {
                        upserts->AddCell(row.Row[i]);
                    }
                } else {
                    for (const auto& column : index.Index.MapColumns) {
                        if (column.Name == DocIdColumn) {
                            upserts->AddCell(storage.Cell(row.DocId, column.Type));
                        } else if (const auto key = KeyPos.find(column.Name); key != KeyPos.end()) {
                            upserts->AddCell(row.Row[key->second]);
                        } else {
                            return TString("Fulltext document id map is missing column ") + column.Name;
                        }
                    }
                }
                upserts->AddRow();
                anyUpsert = true;
            }
            if (anyUpsert) {
                AppendBatch(out, index.Index.MapId, false, upserts->Flush(), stats, false, false);
            }
        }

        auto state = CreateRowsBatcher(index.Index.StateColumns.size(), alloc);
        bool anyState = false;
        for (const auto& row : index.Rows) {
            if (!row.WriteState) {
                continue;
            }
            if (row.Fields.size() != index.Index.StateColumns.size()) {
                return "Fulltext forward-state row does not match the state table";
            }
            TDocIdStorage storage;
            const auto tokens = TCell(row.TokensEncoded.data(), row.TokensEncoded.size());
            for (size_t i = 0; i < row.Fields.size(); ++i) {
                const auto& field = row.Fields[i];
                if (field.DocId) {
                    if (row.DocIdNull || row.Tombstone) {
                        state->AddCell(TCell());
                    } else {
                        state->AddCell(storage.Cell(row.DocId, index.Index.DocIdType));
                    }
                } else if (field.Tokens) {
                    state->AddCell(tokens);
                } else if (field.Null || field.Value.empty()) {
                    state->AddCell(TCell());
                } else {
                    state->AddCell(field.Value[0]);
                }
            }
            state->AddRow();
            anyState = true;
        }
        if (anyState) {
            AppendBatch(out, index.Index.StateId, false, state->Flush(), stats, false, true);
        }
    }
    return {};
}

}
