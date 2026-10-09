// Kept so ydb/core/kqp continues to PEERDIR column_fulltext_seed.
// Registration is done from CreateKqpProxyService via ForceLinkColumnFulltextSeed().
#include <ydb/core/tx/schemeshard/index/column_fulltext_seed.h>

namespace NKikimr::NKqp {
namespace {
[[maybe_unused]] auto* ForceLinkColumnFulltextSeedSymbol = &ForceLinkColumnFulltextSeed;
}
}
