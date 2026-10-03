#pragma once

#include <util/system/types.h>

namespace NKikimr {

// Shared by ANALYZE range grouping and KQP's sampled source scheduling.
inline constexpr ui32 SampledShardsPerScan = 12;

} // namespace NKikimr
