#pragma once

#include "common.h"

#include <yql/essentials/core/histogram/eq_height_histogram.h>

#include <span>
#include <string_view>

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
    return state.Serialize().SerializeAsString();
  }

  static TState DeserializeState(const char *data, size_t size) {
    TEqHeightHistogramIntermediateState state;
    Y_ENSURE(state.ParseFromArray(data, size));
    return TState(state);
  }

  // Empty means "no histogram"; TMultiColumnEqHeightHistogramEval turns that
  // into a skipped row.
  static TString FinalizeState(const TState &state) {
    auto result = state.Finalize();
    return result.Defined() ? result->SerializeAsString() : TString();
  }
};

// Keep the full-ANALYZE UDAF unchanged. The sampled variant rejects wide keys
// before the builder can retain them and carries omissions through its wire
// state, so one shard's omission suppresses the final histogram everywhere.
class TEQHSampledAggFunc {
public:
  static constexpr ui32 MaxEncodedKeyBytes = 256;
  static constexpr ui32 NumBuckets = 32;
  static constexpr ui32 EmissionRate = NumBuckets * 8;
  static constexpr ui64 MaxStateBytes = 32u << 10;
  static constexpr size_t ParamsCount = 0;
  static constexpr size_t HeaderBytes = sizeof(ui64);

  static constexpr std::string_view GetName() { return "EQHSampled"; }

  struct TState {
    using TBuilder = NKikimr::TEqHeightHistogramBuilder;

    TBuilder Histogram;
    ui64 OmittedKeys = 0;

    TState()
        : Histogram(TBuilder::TParams{
            .NumBuckets = NumBuckets,
            .EmissionRate = EmissionRate,
            .MaxStateBytes = MaxStateBytes - HeaderBytes,
        })
    {}

    TState(const TEqHeightHistogramIntermediateState &state, ui64 omittedKeys)
        : Histogram(state)
        , OmittedKeys(omittedKeys)
    {}

    void Add(TStringBuf key) {
      if (key.size() > MaxEncodedKeyBytes) {
        Y_ENSURE(OmittedKeys < Max<ui64>(), "sampled EQH omission count overflow");
        ++OmittedKeys;
      } else {
        Histogram.Add(key);
      }
    }

    void Merge(const TState &other) {
      Y_ENSURE(OmittedKeys <= Max<ui64>() - other.OmittedKeys,
               "sampled EQH omission count overflow");
      OmittedKeys += other.OmittedKeys;
      Histogram.Merge(other.Histogram);
    }

    TMaybe<TEqHeightHistogramResult> Finalize() const {
      if (OmittedKeys) {
        return Nothing();
      }
      auto result = Histogram.Finalize();
      if (result && result->ByteSizeLong() > MaxStateBytes) {
        return Nothing();
      }
      return result;
    }
  };

  static TState
  CreateState(TTypeId, const std::span<const TValue, ParamsCount> &) {
    return TState();
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
    // The little-endian counter is part of the reserved 32 KiB state budget.
    TString bytes(HeaderBytes, '\0');
    for (size_t i = 0; i < HeaderBytes; ++i) {
      bytes[i] = static_cast<char>((state.OmittedKeys >> (8 * i)) & 0xff);
    }
    bytes += state.Histogram.Serialize().SerializeAsString();
    Y_ENSURE(bytes.size() <= MaxStateBytes,
             "sampled EQH serialized state exceeds its byte limit");
    return bytes;
  }

  static TState DeserializeState(const char *data, size_t size) {
    Y_ENSURE(size >= HeaderBytes, "truncated sampled EQH state");
    ui64 omittedKeys = 0;
    for (size_t i = 0; i < HeaderBytes; ++i) {
      omittedKeys |= static_cast<ui64>(static_cast<unsigned char>(data[i])) << (8 * i);
    }
    TEqHeightHistogramIntermediateState state;
    Y_ENSURE(state.ParseFromArray(data + HeaderBytes, size - HeaderBytes));
    Y_ENSURE(state.GetParams().GetMaxStateBytes() == MaxStateBytes - HeaderBytes);
    return TState(state, omittedKeys);
  }

  static TString FinalizeState(const TState &state) {
    auto result = state.Finalize();
    return result ? result->SerializeAsString() : TString();
  }
};

} // namespace NKikimr::NStat::NAggFuncs
