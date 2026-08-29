#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mbs::domain {

using ValidationErrors = std::vector<std::string>;

class ValidationError final : public std::invalid_argument {
  public:
    explicit ValidationError(ValidationErrors errors);

    [[nodiscard]] const ValidationErrors& errors() const noexcept;

  private:
    ValidationErrors errors_;
};

void require_valid(ValidationErrors errors);

} // namespace mbs::domain
