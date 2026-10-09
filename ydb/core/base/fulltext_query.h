#pragma once

#include "table_index.h"

#include <ydb/public/api/protos/ydb_table.pb.h>

#include <util/generic/hash_set.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/stream/output.h>

#include <optional>

namespace NKikimr::NFulltext {

// A single search term together with its query-syntax modifier.
// Required == true marks a `+term` (Lucene MUST): every match must contain it.
struct TSearchTerm {
    TString Token;
    bool Required = false;

    bool operator==(const TSearchTerm& other) const = default;
};

inline IOutputStream& operator<<(IOutputStream& out, const TSearchTerm& term) {
    return out << (term.Required ? "+" : "") << term.Token;
}

// Like BuildSearchTerms, but parses the `+term` required-term syntax: a `+` that
// begins a whitespace-delimited term marks every token analyzed from that term as
// required. Queries without that syntax tokenize identically to BuildSearchTerms,
// with every term optional.
//
// Kept in its own header (not fulltext.h) so that adding query-syntax support does
// not rebuild every consumer of the widely-included fulltext.h.
TVector<TSearchTerm> BuildSearchTermsStructured(const TString& query, const Ydb::Table::FulltextIndexSettings::Analyzers& settings);

// FulltextMatch mode. Keywords is the default. Wildcard adds a case-insensitive
// LIKE residual over the original text; token membership alone is not the result.
enum class EFulltextQueryMode {
    Keywords,
    Wildcard,
};

// Column checks enforce the column-table query contract, including unknown-mode
// rejection. RowTable is the existing DataShard source: it keeps queries that
// source already accepts, and does not reject modes it does not interpret.
enum class EFulltextQueryChecks {
    Column,
    RowTable,
};

struct TFulltextQueryOptions {
    TString DefaultOperator;
    TString MinimumShouldMatch;
    TString Mode;
    EFulltextQueryChecks Checks = EFulltextQueryChecks::Column;
    // Column contract: Mode must be a bound literal. The row-table source does not reject this.
    bool ModeIsParameter = false;
};

// One normalized query token. Required is true when any occurrence was marked with `+`.
struct TCompiledFulltextTerm {
    TString Token;
    bool Required = false;

    bool operator==(const TCompiledFulltextTerm& other) const = default;
};

inline IOutputStream& operator<<(IOutputStream& out, const TCompiledFulltextTerm& term) {
    return out << (term.Required ? "+" : "") << term.Token;
}

// Validated boolean query shared by postings and text evaluation.
struct TCompiledFulltextQuery {
    // Unique normalized tokens in first-seen order.
    TVector<TCompiledFulltextTerm> Terms;
    NTableIndex::NFulltext::EDefaultOperator Operator = NTableIndex::NFulltext::EDefaultOperator::And;
    // Threshold among distinct optional terms. Empty when the operator is And,
    // or when every term is required (all required terms must match).
    std::optional<ui32> OptionalThreshold;
    // Value the row-table posting merge stores before it clamps to the term count:
    // required-term count plus the parsed optional threshold.
    ui32 MinimumShouldMatch = 0;
    Ydb::Table::FulltextIndexSettings::Analyzers Analyzers;
    // Deterministic analyzer settings bytes and a content hash of those bytes.
    TString AnalyzerIdentity;
    ui64 AnalyzerRevision = 0;
    EFulltextQueryMode Mode = EFulltextQueryMode::Keywords;
    // Original query text when Mode is Wildcard. Empty for Keywords.
    TString WildcardPattern;
};

struct TFulltextQueryValidation {
    std::optional<TCompiledFulltextQuery> Compiled;
    TString Error;

    explicit operator bool() const {
        return Compiled.has_value();
    }
};

// Compiles one bound query. Empty query, no extracted terms, and invalid operator
// or threshold are validation errors. Unknown mode is an error only for Column checks.
TFulltextQueryValidation CompileFulltextQuery(
    const TString& query,
    const Ydb::Table::FulltextIndexSettings::Analyzers& analyzers,
    const TFulltextQueryOptions& options = {});

// Token condition only. Does not apply the wildcard residual.
bool EvaluateFulltextMembership(
    const TCompiledFulltextQuery& query,
    const THashSet<TString>& documentTokens);

// Case-insensitive anchored LIKE (`Re2.PatternFromLike` + `Re2.Match`) of the original
// wildcard pattern. False when the compiled query is not Wildcard.
bool MatchFulltextWildcardResidual(const TCompiledFulltextQuery& query, TStringBuf text);

// Document tokens from the same analyzer the query was compiled with.
// KEYWORD emits one empty token for empty text; other tokenizers emit none.
TVector<TString> TokenizeFulltextDocument(const TCompiledFulltextQuery& query, TStringBuf text);

// Null text never matches. Otherwise: analyze, test membership, and, for Wildcard,
// also require the original-text residual.
bool EvaluateFulltextText(const TCompiledFulltextQuery& query, std::optional<TStringBuf> text);

}
