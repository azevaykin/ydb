#include "column_fulltext_seed.h"

namespace NKikimr::NKqp {
namespace {
TColumnFulltextSeedFactory SeedFactory = nullptr;
}

void RegisterColumnFulltextSeedFactory(TColumnFulltextSeedFactory factory) {
    SeedFactory = factory;
}

TColumnFulltextSeedFactory GetColumnFulltextSeedFactory() {
    return SeedFactory;
}

NActors::IActor* CreateColumnFulltextSeedActor(TColumnFulltextSeedSettings&& settings) {
    if (!SeedFactory) {
        return nullptr;
    }
    return SeedFactory(std::move(settings));
}

}
