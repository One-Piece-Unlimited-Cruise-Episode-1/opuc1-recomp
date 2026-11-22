#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        "One Piece: Unlimited Recompilation Episode 1",
    };
    return descriptor;
}

} // namespace RuntimeProduct
