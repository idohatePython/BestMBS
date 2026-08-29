#pragma once

#include "mbs/application/Ports.hpp"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace mbs::optimization {

struct Prediction final {
    domain::DesignParameters parameters;
    double mean_objective{};
    double standard_deviation{};
};

struct OptimizationProposal final {
    domain::DesignParameters parameters;
    std::string acquisition;
    std::size_t evaluated_count{};
    int initial_points_remaining{};
    bool used_surrogate{};
    double predicted_objective{};
    double predicted_standard_deviation{};
    double acquisition_value{};

    [[nodiscard]] std::string to_json() const;
};

struct SlicePoint final {
    double coordinate{};
    double mean_objective{};
    double standard_deviation{};
};

struct SurrogateSlices final {
    domain::DesignParameters anchor;
    std::array<std::vector<SlicePoint>, 4> axes;
};

class GaussianProcessOptimizer final : public application::IOptimizationEngine {
  public:
    [[nodiscard]] domain::DesignParameters
    propose(const domain::BayesianOptimizationConfig& config,
            std::span<const domain::OptimizationObservation> observations) override;

    [[nodiscard]] OptimizationProposal
    propose_detailed(const domain::BayesianOptimizationConfig& config,
                     std::span<const domain::OptimizationObservation> observations) const;

    [[nodiscard]] std::vector<Prediction>
    predict(std::span<const domain::OptimizationObservation> observations,
            std::span<const domain::DesignParameters> points) const;

    [[nodiscard]] SurrogateSlices
    slices(std::span<const domain::OptimizationObservation> observations,
           std::size_t points_per_axis = 80) const;
};

} // namespace mbs::optimization
