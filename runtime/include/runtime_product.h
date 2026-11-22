#pragma once

#include <string_view>

namespace RuntimeProduct {

struct Descriptor {
    std::string_view displayName;
};

// The executable links one small provider definition (src/product/base_product.cpp).
const Descriptor& Active() noexcept;

} // namespace RuntimeProduct
