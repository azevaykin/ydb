#include "fulltext_query.h"

#include "fulltext.h"

#include <re2/re2.h>

#include <util/digest/city.h>
#include <util/generic/hash.h>
#include <util/string/builder.h>
#include <util/string/cast.h>
#include <util/string/escape.h>

namespace NKikimr::NFulltext {

namespace {

TFulltextQueryValidation Fail(TString error) {
    TFulltextQueryValidation result;
    result.Error = std::move(error);
    return result;
}

// Same conversion as Re2.PatternFromLike with no escape character.
TString LikePatternToRe2(TStringBuf input) {
    const std::string escaped = re2::RE2::QuoteMeta(absl::string_view(input.data(), input.size()));
    TStringBuilder result;
    result << "(?s)";
    bool slash = false;
    for (const char c : escaped) {
        switch (c) {
            case '\\':
                if (slash) {
                    result << "\\\\";
                }
                slash = !slash;
                break;
            case '%':
                result << ".*";
                slash = false;
                break;
            case '_':
                result << ".";
                slash = false;
                break;
            default:
                if (slash) {
                    result << '\\';
                }
                result << c;
                slash = false;
                break;
        }
    }
    return TString(result);
}

re2::RE2::Options WildcardRe2Options() {
    re2::RE2::Options options;
    options.set_log_errors(false);
    options.set_case_sensitive(false);
    options.set_encoding(re2::RE2::Options::EncodingUTF8);
    return options;
}

bool CompileWildcardPattern(TStringBuf likePattern, TString& error) {
    const TString pattern = LikePatternToRe2(likePattern);
    const re2::RE2 regexp(absl::string_view(pattern.data(), pattern.size()), WildcardRe2Options());
    if (!regexp.ok()) {
        error = TStringBuilder() << "Invalid wildcard pattern: `" << EscapeC(TString(likePattern)) << "`";
        return false;
    }
    return true;
}

bool MatchWildcardPattern(TStringBuf likePattern, TStringBuf text) {
    const TString pattern = LikePatternToRe2(likePattern);
    const re2::RE2 regexp(absl::string_view(pattern.data(), pattern.size()), WildcardRe2Options());
    if (!regexp.ok()) {
        return false;
    }
    return regexp.Match(
        absl::string_view(text.data(), text.size()),
        0,
        text.size(),
        re2::RE2::ANCHOR_BOTH,
        nullptr,
        0);
}

std::optional<EFulltextQueryMode> ParseMode(const TFulltextQueryOptions& options, TString& error) {
    if (options.ModeIsParameter && options.Checks == EFulltextQueryChecks::Column) {
        error = "Parameterized fulltext mode is not supported";
        return std::nullopt;
    }

    if (options.Mode.empty()) {
        return EFulltextQueryMode::Keywords;
    }

    const TString lower = to_lower(options.Mode);
    if (lower == "keywords") {
        return EFulltextQueryMode::Keywords;
    }
    if (lower == "wildcard") {
        return EFulltextQueryMode::Wildcard;
    }
    if (options.Checks == EFulltextQueryChecks::RowTable) {
        // The DataShard source does not interpret Mode. Keep the query.
        return EFulltextQueryMode::Keywords;
    }

    error = TStringBuilder() << "Unsupported fulltext mode: `" << EscapeC(options.Mode)
        << "`. Should be `keywords` or `wildcard`";
    return std::nullopt;
}

TVector<TCompiledFulltextTerm> DeduplicateTerms(const TVector<TSearchTerm>& parsed) {
    THashMap<TString, size_t> seen;
    TVector<TCompiledFulltextTerm> terms;
    for (const auto& term : parsed) {
        const auto it = seen.find(term.Token);
        if (it == seen.end()) {
            seen.emplace(term.Token, terms.size());
            terms.push_back({term.Token, term.Required});
        } else if (term.Required) {
            terms[it->second].Required = true;
        }
    }
    return terms;
}

void SetAnalyzerFingerprint(TCompiledFulltextQuery& compiled, const Ydb::Table::FulltextIndexSettings::Analyzers& analyzers) {
    compiled.Analyzers.CopyFrom(analyzers);
    const auto serialized = compiled.Analyzers.SerializeAsString();
    compiled.AnalyzerIdentity.assign(serialized.data(), serialized.size());
    compiled.AnalyzerRevision = CityHash64(compiled.AnalyzerIdentity.data(), compiled.AnalyzerIdentity.size());
}

}

TFulltextQueryValidation CompileFulltextQuery(
    const TString& query,
    const Ydb::Table::FulltextIndexSettings::Analyzers& analyzers,
    const TFulltextQueryOptions& options)
{
    TString error;
    const auto mode = ParseMode(options, error);
    if (!mode) {
        return Fail(std::move(error));
    }

    if (query.empty()) {
        return Fail("Empty fulltext query");
    }

    const auto defaultOperator = NTableIndex::NFulltext::DefaultOperatorFromString(options.DefaultOperator, error);
    if (!error.empty()) {
        return Fail(std::move(error));
    }

    const bool ngram = analyzers.use_filter_ngram() || analyzers.use_filter_edge_ngram();
    if (*mode == EFulltextQueryMode::Wildcard && options.Checks == EFulltextQueryChecks::Column && !ngram) {
        return Fail("Wildcard mode requires an analyzer with use_filter_ngram or use_filter_edge_ngram");
    }

    // BuildSearchTermsStructured keeps whole-query KEYWORD tokenization when the
    // query has no leading-plus required-term syntax.
    const auto terms = DeduplicateTerms(BuildSearchTermsStructured(query, analyzers));
    if (terms.empty()) {
        return Fail("No search terms were extracted from the query");
    }

    if (*mode == EFulltextQueryMode::Wildcard && !CompileWildcardPattern(query, error)) {
        return Fail(std::move(error));
    }

    size_t requiredCount = 0;
    for (const auto& term : terms) {
        if (term.Required) {
            ++requiredCount;
        }
    }
    const size_t optionalCount = terms.size() - requiredCount;
    const ui32 optionalThreshold = NTableIndex::NFulltext::MinimumShouldMatchFromString(
        static_cast<i32>(optionalCount), defaultOperator, options.MinimumShouldMatch, error);
    if (!error.empty()) {
        return Fail(std::move(error));
    }

    TCompiledFulltextQuery compiled;
    compiled.Terms = terms;
    compiled.Operator = defaultOperator;
    compiled.MinimumShouldMatch = static_cast<ui32>(requiredCount) + optionalThreshold;
    if (defaultOperator == NTableIndex::NFulltext::EDefaultOperator::Or && optionalCount > 0) {
        compiled.OptionalThreshold = optionalThreshold;
    }
    SetAnalyzerFingerprint(compiled, analyzers);
    compiled.Mode = *mode;
    if (*mode == EFulltextQueryMode::Wildcard) {
        compiled.WildcardPattern = query;
    }
    TFulltextQueryValidation result;
    result.Compiled = std::move(compiled);
    return result;
}

bool EvaluateFulltextMembership(const TCompiledFulltextQuery& query, const THashSet<TString>& documentTokens) {
    size_t required = 0;
    size_t matchedRequired = 0;
    size_t matchedOptional = 0;
    for (const auto& term : query.Terms) {
        const bool present = documentTokens.contains(term.Token);
        if (term.Required) {
            ++required;
            if (present) {
                ++matchedRequired;
            }
        } else if (present) {
            ++matchedOptional;
        }
    }

    if (query.Operator != NTableIndex::NFulltext::EDefaultOperator::Or) {
        return matchedRequired + matchedOptional == query.Terms.size();
    }
    if (matchedRequired != required) {
        return false;
    }
    if (!query.OptionalThreshold.has_value()) {
        return true;
    }
    return matchedOptional >= *query.OptionalThreshold;
}

bool MatchFulltextWildcardResidual(const TCompiledFulltextQuery& query, TStringBuf text) {
    if (query.Mode != EFulltextQueryMode::Wildcard) {
        return false;
    }
    return MatchWildcardPattern(query.WildcardPattern, text);
}

TVector<TString> TokenizeFulltextDocument(const TCompiledFulltextQuery& query, TStringBuf text) {
    return Analyze(text, query.Analyzers);
}

bool EvaluateFulltextText(const TCompiledFulltextQuery& query, std::optional<TStringBuf> text) {
    if (!text.has_value()) {
        return false;
    }

    THashSet<TString> membership;
    for (const auto& token : TokenizeFulltextDocument(query, *text)) {
        membership.insert(token);
    }
    if (!EvaluateFulltextMembership(query, membership)) {
        return false;
    }
    if (query.Mode == EFulltextQueryMode::Wildcard) {
        return MatchFulltextWildcardResidual(query, *text);
    }
    return true;
}

}
