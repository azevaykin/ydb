#pragma once

#include "common.h"

#include <yql/essentials/core/histogram/eq_height_histogram.h>

namespace NKikimr::NStat::NAggFuncs {

// UDAF computing an equi-height histogram over memcomparable keys produced by
// StatisticsInternal::PresortKey. The item type is always String, so the column
// type id is ignored.
class TEQHAggFunc {
public:
  static constexpr std::string_view GetName() { return "EQH"; }

  using TState = NKikimr::TEqHeightHistogramBuilder;

  static constexpr size_t ParamsCount =
      3; // numBuckets, emissionRate, maxStateBytes

  static TState
  CreateState(TTypeId, const std::span<const TValue, ParamsCount> &params) {
    return TState(TState::TParams{
        .NumBuckets = params[0].Get<ui32>(),
        .EmissionRate = params[1].Get<ui32>(),
        .MaxStateBytes = params[2].Get<ui64>(),
    });
  }

  static auto CreateStateUpdater(TTypeId) {
    return [](TState &state, const TValue &val) {
      const auto ref = val.AsStringRef();
      state.Add(TStringBuf(ref.Data(), ref.Size()));
    };
  }

  static void MergeStates(const TState &left, TState &right) {
    right.Merge(left);
  }

  static TString SerializeState(const TState &state) {
    return state.Serialize();
  }

  static TState DeserializeState(const char *data, size_t size) {
    return TState(data, size);
  }

  // Empty means "no histogram"; TMultiColumnEqHeightHistogramEval turns that
  // into a skipped row.
  static TString FinalizeState(const TState &state) {
    return state.Finalize().value_or(TString());
  }
};

} // namespace NKikimr::NStat::NAggFuncs
