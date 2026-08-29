#include "mbs/domain/Validation.hpp"

#include <sstream>

namespace mbs::domain {
namespace {

std::string format_errors(const ValidationErrors& errors) {
    std::ostringstream message;
    message << "domain validation failed";
    for (const auto& error : errors) {
        message << "; " << error;
    }
    return message.str();
}

} // namespace

ValidationError::ValidationError(ValidationErrors errors)
    : std::invalid_argument(format_errors(errors)), errors_(std::move(errors)) {}

const ValidationErrors& ValidationError::errors() const noexcept { return errors_; }

void require_valid(ValidationErrors errors) {
    if (!errors.empty()) {
        throw ValidationError{std::move(errors)};
    }
}

} // namespace mbs::domain
