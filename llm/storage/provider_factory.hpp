#pragma once

#include "istorage_provider.hpp"

#include <memory>

namespace velix::llm::storage {

std::shared_ptr<IStorageProvider> make_storage_provider_from_config();

} // namespace velix::llm::storage
