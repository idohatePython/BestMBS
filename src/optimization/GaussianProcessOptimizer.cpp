#include "mbs/optimization/GaussianProcessOptimizer.hpp"

#include "mbs/domain/Validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numbers>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace mbs::optimization {
namespace {

using Point = std::array<double, 4>;

constexpr double square_root_five = 2.2360679774997896964;
constexpr std::array<unsigned, 4> halton_primes{2, 3, 5, 7};

struct TrainingData final {
    std::vector<Point> x;
    std::vector<double> y;
    std::vector<domain::DesignParameters> parameters;
};

Point to_internal(const domain::DesignParameters& value) {
    const auto denominator = std::max(1.0 - value.lambda, 1.0e-15);
    const auto mu_ratio = value.lambda >= 1.0 ? 0.0 : value.mu / denominator;
    return {value.lambda, std::clamp(mu_ratio, 0.0, 1.0), value.kappa,
            (value.beta + 1.0) / 2.0};
}

domain::DesignParameters to_actual(const Point& value) {
    const auto lambda = std::clamp(value[0], 0.0, 1.0);
    return {.lambda = lambda,
            .mu = (1.0 - lambda) * std::clamp(value[1], 0.0, 1.0),
            .kappa = std::clamp(value[2], 0.0, 1.0),
            .beta = 2.0 * std::clamp(value[3], 0.0, 1.0) - 1.0};
}

double round_four(const double value) { return std::round(value * 10000.0) / 10000.0; }

domain::DesignParameters rounded(domain::DesignParameters value) {
    value.lambda = round_four(value.lambda);
    value.mu = round_four(value.mu);
    value.kappa = round_four(value.kappa);
    value.beta = round_four(value.beta);
    if (value.lambda + value.mu > 1.0) {
        value.mu = round_four(std::max(0.0, 1.0 - value.lambda));
    }
    return value;
}

std::array<long long, 4> key(const domain::DesignParameters& value) {
    return {std::llround(value.lambda * 10000.0), std::llround(value.mu * 10000.0),
            std::llround(value.kappa * 10000.0), std::llround(value.beta * 10000.0)};
}

TrainingData training_data(
    const std::span<const domain::OptimizationObservation> observations) {
    TrainingData result;
    std::set<std::array<long long, 4>> seen;
    for (const auto& observation : observations) {
        if (!std::isfinite(observation.objective) || !observation.parameters.is_valid()) {
            continue;
        }
        const auto parameter_key = key(observation.parameters);
        if (!seen.insert(parameter_key).second) {
            continue;
        }
        result.x.push_back(to_internal(observation.parameters));
        result.y.push_back(observation.objective);
        result.parameters.push_back(observation.parameters);
    }
    return result;
}

double radical_inverse(std::uint64_t index, const unsigned base) {
    double result = 0.0;
    double factor = 1.0 / static_cast<double>(base);
    while (index != 0) {
        result += factor * static_cast<double>(index % base);
        index /= base;
        factor /= static_cast<double>(base);
    }
    return result;
}

Point halton_point(const std::uint64_t index, const std::uint64_t seed) {
    Point point{};
    for (std::size_t axis = 0; axis < point.size(); ++axis) {
        const auto offset = seed * (4099U + static_cast<std::uint64_t>(axis) * 131U);
        point[axis] = radical_inverse(index + offset + 1U, halton_primes[axis]);
    }
    return point;
}

std::string_view acquisition_name(const domain::AcquisitionFunction acquisition) {
    using enum domain::AcquisitionFunction;
    switch (acquisition) {
    case expected_improvement:
        return "EI";
    case lower_confidence_bound:
        return "LCB";
    case probability_improvement:
        return "PI";
    }
    return "EI";
}

double normal_pdf(const double value) {
    return std::exp(-0.5 * value * value) / std::sqrt(2.0 * std::numbers::pi);
}

double normal_cdf(const double value) {
    return 0.5 * std::erfc(-value / std::sqrt(2.0));
}

struct Hyperparameters final {
    double signal{1.0};
    std::array<double, 4> length{0.35, 0.35, 0.35, 0.35};
    double noise{0.02};
};

double matern52(const Point& left, const Point& right, const Hyperparameters& hyper) {
    double squared_distance = 0.0;
    for (std::size_t axis = 0; axis < left.size(); ++axis) {
        const auto delta = (left[axis] - right[axis]) / hyper.length[axis];
        squared_distance += delta * delta;
    }
    const auto distance = std::sqrt(squared_distance);
    return hyper.signal * hyper.signal *
           (1.0 + square_root_five * distance + 5.0 * squared_distance / 3.0) *
           std::exp(-square_root_five * distance);
}

bool cholesky(std::vector<double>& matrix, const std::size_t size) {
    for (std::size_t row = 0; row < size; ++row) {
        for (std::size_t column = 0; column <= row; ++column) {
            double value = matrix[row * size + column];
            for (std::size_t inner = 0; inner < column; ++inner) {
                value -= matrix[row * size + inner] * matrix[column * size + inner];
            }
            if (row == column) {
                if (!(value > 0.0) || !std::isfinite(value)) {
                    return false;
                }
                matrix[row * size + column] = std::sqrt(value);
            } else {
                matrix[row * size + column] = value / matrix[column * size + column];
            }
        }
        for (std::size_t column = row + 1; column < size; ++column) {
            matrix[row * size + column] = 0.0;
        }
    }
    return true;
}

std::vector<double> forward_solve(const std::vector<double>& lower,
                                  const std::vector<double>& right,
                                  const std::size_t size) {
    std::vector<double> result(size);
    for (std::size_t row = 0; row < size; ++row) {
        double value = right[row];
        for (std::size_t column = 0; column < row; ++column) {
            value -= lower[row * size + column] * result[column];
        }
        result[row] = value / lower[row * size + row];
    }
    return result;
}

std::vector<double> backward_solve(const std::vector<double>& lower,
                                   const std::vector<double>& right,
                                   const std::size_t size) {
    std::vector<double> result(size);
    for (std::size_t reverse = 0; reverse < size; ++reverse) {
        const auto row = size - reverse - 1;
        double value = right[row];
        for (std::size_t column = row + 1; column < size; ++column) {
            value -= lower[column * size + row] * result[column];
        }
        result[row] = value / lower[row * size + row];
    }
    return result;
}

struct GaussianProcess final {
    TrainingData training;
    Hyperparameters hyper;
    double y_mean{};
    double y_scale{1.0};
    std::vector<double> normalized_y;
    std::vector<double> lower;
    std::vector<double> alpha;

    [[nodiscard]] std::pair<double, double> predict(const Point& point) const {
        const auto size = training.x.size();
        std::vector<double> covariance(size);
        for (std::size_t index = 0; index < size; ++index) {
            covariance[index] = matern52(point, training.x[index], hyper);
        }
        double normalized_mean = 0.0;
        for (std::size_t index = 0; index < size; ++index) {
            normalized_mean += covariance[index] * alpha[index];
        }
        const auto projected = forward_solve(lower, covariance, size);
        double explained = 0.0;
        for (const auto value : projected) {
            explained += value * value;
        }
        const auto normalized_variance =
            std::max(hyper.signal * hyper.signal - explained, 1.0e-14);
        return {y_mean + y_scale * normalized_mean,
                y_scale * std::sqrt(normalized_variance)};
    }
};

struct Factorization final {
    std::vector<double> lower;
    std::vector<double> alpha;
    double log_likelihood{-std::numeric_limits<double>::infinity()};
};

Factorization factorize(const TrainingData& training, const std::vector<double>& normalized_y,
                        const Hyperparameters& hyper) {
    const auto size = training.x.size();
    std::vector<double> covariance(size * size);
    for (std::size_t row = 0; row < size; ++row) {
        for (std::size_t column = 0; column <= row; ++column) {
            const auto value = matern52(training.x[row], training.x[column], hyper);
            covariance[row * size + column] = value;
            covariance[column * size + row] = value;
        }
        covariance[row * size + row] += hyper.noise * hyper.noise + 1.0e-10;
    }
    if (!cholesky(covariance, size)) {
        return {};
    }
    const auto intermediate = forward_solve(covariance, normalized_y, size);
    const auto alpha = backward_solve(covariance, intermediate, size);
    double quadratic = 0.0;
    double log_determinant = 0.0;
    for (std::size_t index = 0; index < size; ++index) {
        quadratic += normalized_y[index] * alpha[index];
        log_determinant += std::log(covariance[index * size + index]);
    }
    return {.lower = std::move(covariance),
            .alpha = alpha,
            .log_likelihood = -0.5 * quadratic - log_determinant -
                              0.5 * static_cast<double>(size) *
                                  std::log(2.0 * std::numbers::pi)};
}

GaussianProcess fit_gp(TrainingData training) {
    if (training.x.empty()) {
        throw std::invalid_argument{"at least one valid optimization observation is required"};
    }
    GaussianProcess result;
    result.training = std::move(training);
    result.y_mean = 0.0;
    for (const auto value : result.training.y) {
        result.y_mean += value;
    }
    result.y_mean /= static_cast<double>(result.training.y.size());
    double variance = 0.0;
    for (const auto value : result.training.y) {
        const auto delta = value - result.y_mean;
        variance += delta * delta;
    }
    result.y_scale = std::sqrt(variance / static_cast<double>(result.training.y.size()));
    if (result.y_scale < 1.0e-12) {
        result.y_scale = 1.0;
    }
    result.normalized_y.reserve(result.training.y.size());
    for (const auto value : result.training.y) {
        result.normalized_y.push_back((value - result.y_mean) / result.y_scale);
    }

    Hyperparameters best;
    auto best_factor = factorize(result.training, result.normalized_y, best);
    std::array<double, 6> logarithms{std::log(best.signal), std::log(best.length[0]),
                                     std::log(best.length[1]), std::log(best.length[2]),
                                     std::log(best.length[3]), std::log(best.noise)};
    auto from_logs = [](const std::array<double, 6>& values) {
        return Hyperparameters{.signal = std::clamp(std::exp(values[0]), 0.1, 10.0),
                               .length = {std::clamp(std::exp(values[1]), 0.03, 3.0),
                                          std::clamp(std::exp(values[2]), 0.03, 3.0),
                                          std::clamp(std::exp(values[3]), 0.03, 3.0),
                                          std::clamp(std::exp(values[4]), 0.03, 3.0)},
                               .noise = std::clamp(std::exp(values[5]), 1.0e-6, 0.5)};
    };
    double step = 0.8;
    for (int sweep = 0; sweep < 6; ++sweep) {
        for (std::size_t parameter = 0; parameter < logarithms.size(); ++parameter) {
            for (const double direction : {-1.0, 1.0}) {
                auto candidate_logs = logarithms;
                candidate_logs[parameter] += direction * step;
                const auto candidate_hyper = from_logs(candidate_logs);
                auto candidate_factor =
                    factorize(result.training, result.normalized_y, candidate_hyper);
                if (candidate_factor.log_likelihood > best_factor.log_likelihood) {
                    logarithms = candidate_logs;
                    best = candidate_hyper;
                    best_factor = std::move(candidate_factor);
                }
            }
        }
        step *= 0.55;
    }
    if (!std::isfinite(best_factor.log_likelihood)) {
        throw std::runtime_error{"Gaussian-process covariance is not positive definite"};
    }
    result.hyper = best;
    result.lower = std::move(best_factor.lower);
    result.alpha = std::move(best_factor.alpha);
    return result;
}

double acquisition_value(const domain::BayesianOptimizationConfig& config, const double best,
                         const double mean, const double deviation) {
    using enum domain::AcquisitionFunction;
    if (config.acquisition == lower_confidence_bound) {
        return -(mean - config.kappa * deviation);
    }
    if (deviation <= 1.0e-14) {
        return 0.0;
    }
    const auto improvement = best - mean - config.xi;
    const auto z = improvement / deviation;
    if (config.acquisition == probability_improvement) {
        return normal_cdf(z);
    }
    return improvement * normal_cdf(z) + deviation * normal_pdf(z);
}

domain::DesignParameters random_unique(const std::set<std::array<long long, 4>>& existing,
                                       const std::uint64_t seed) {
    std::mt19937_64 generator{seed};
    std::uniform_real_distribution<double> distribution{0.0, 1.0};
    for (int attempt = 0; attempt < 10000; ++attempt) {
        const Point point{distribution(generator), distribution(generator), distribution(generator),
                          distribution(generator)};
        auto candidate = rounded(to_actual(point));
        if (!existing.contains(key(candidate))) {
            return candidate;
        }
    }
    throw std::runtime_error{"Bayesian optimizer could not produce a unique initial point"};
}

} // namespace

domain::DesignParameters GaussianProcessOptimizer::propose(
    const domain::BayesianOptimizationConfig& config,
    const std::span<const domain::OptimizationObservation> observations) {
    return propose_detailed(config, observations).parameters;
}

OptimizationProposal GaussianProcessOptimizer::propose_detailed(
    const domain::BayesianOptimizationConfig& config,
    const std::span<const domain::OptimizationObservation> observations) const {
    domain::require_valid(config.validation_errors());
    const auto training = training_data(observations);
    if (training.x.empty()) {
        throw std::invalid_argument{
            "no valid evaluated observations satisfy the Bayesian design constraints"};
    }
    std::set<std::array<long long, 4>> existing;
    for (const auto& observation : observations) {
        if (observation.parameters.is_valid()) {
            existing.insert(key(observation.parameters));
        }
    }
    const auto remaining =
        std::max(0, config.initial_points - static_cast<int>(training.x.size()));
    if (remaining > 0) {
        const auto candidate = random_unique(
            existing, config.random_seed + static_cast<std::uint64_t>(observations.size()) + 1000U);
        return {.parameters = candidate,
                .acquisition = std::string{acquisition_name(config.acquisition)},
                .evaluated_count = training.x.size(),
                .initial_points_remaining = remaining,
                .used_surrogate = false};
    }

    const auto process = fit_gp(training);
    const auto best = *std::min_element(process.training.y.begin(), process.training.y.end());
    const auto draws = std::clamp<std::size_t>(
        static_cast<std::size_t>(config.candidate_pool) * 256U, 2048U, 65536U);
    bool found = false;
    OptimizationProposal proposal;
    proposal.acquisition = std::string{acquisition_name(config.acquisition)};
    proposal.evaluated_count = process.training.x.size();
    proposal.used_surrogate = true;
    proposal.acquisition_value = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < draws; ++index) {
        const auto internal = halton_point(index, config.random_seed);
        const auto candidate = rounded(to_actual(internal));
        if (existing.contains(key(candidate))) {
            continue;
        }
        const auto [mean, deviation] = process.predict(to_internal(candidate));
        const auto score = acquisition_value(config, best, mean, deviation);
        if (!found || score > proposal.acquisition_value) {
            found = true;
            proposal.parameters = candidate;
            proposal.predicted_objective = mean;
            proposal.predicted_standard_deviation = deviation;
            proposal.acquisition_value = score;
        }
    }
    if (!found) {
        proposal.parameters = random_unique(existing, config.random_seed + 2000U);
        const auto [mean, deviation] = process.predict(to_internal(proposal.parameters));
        proposal.predicted_objective = mean;
        proposal.predicted_standard_deviation = deviation;
        proposal.acquisition_value = acquisition_value(config, best, mean, deviation);
    }
    return proposal;
}

std::vector<Prediction> GaussianProcessOptimizer::predict(
    const std::span<const domain::OptimizationObservation> observations,
    const std::span<const domain::DesignParameters> points) const {
    const auto process = fit_gp(training_data(observations));
    std::vector<Prediction> result;
    result.reserve(points.size());
    for (const auto& point : points) {
        domain::require_valid(point.validation_errors());
        const auto [mean, deviation] = process.predict(to_internal(point));
        result.push_back(
            {.parameters = point, .mean_objective = mean, .standard_deviation = deviation});
    }
    return result;
}

SurrogateSlices GaussianProcessOptimizer::slices(
    const std::span<const domain::OptimizationObservation> observations,
    const std::size_t points_per_axis) const {
    if (points_per_axis < 2) {
        throw std::invalid_argument{"surrogate slices require at least two points per axis"};
    }
    const auto training = training_data(observations);
    if (training.x.empty()) {
        throw std::invalid_argument{"surrogate slices require valid observations"};
    }
    const auto best_index = static_cast<std::size_t>(std::distance(
        training.y.begin(), std::min_element(training.y.begin(), training.y.end())));
    SurrogateSlices result{.anchor = training.parameters[best_index], .axes = {}};
    std::vector<domain::DesignParameters> queries;
    queries.reserve(points_per_axis * 4U);
    for (std::size_t axis = 0; axis < 4; ++axis) {
        for (std::size_t index = 0; index < points_per_axis; ++index) {
            const auto ratio = static_cast<double>(index) /
                               static_cast<double>(points_per_axis - 1U);
            auto point = result.anchor;
            if (axis == 0) {
                point.lambda = ratio;
                point.mu = std::min(point.mu, 1.0 - point.lambda);
            } else if (axis == 1) {
                point.mu = ratio * (1.0 - point.lambda);
            } else if (axis == 2) {
                point.kappa = ratio;
            } else {
                point.beta = 2.0 * ratio - 1.0;
            }
            queries.push_back(point);
        }
    }
    const auto predictions = predict(observations, queries);
    for (std::size_t axis = 0; axis < 4; ++axis) {
        result.axes[axis].reserve(points_per_axis);
        for (std::size_t index = 0; index < points_per_axis; ++index) {
            const auto& prediction = predictions[axis * points_per_axis + index];
            const auto coordinate = axis == 0   ? prediction.parameters.lambda
                                    : axis == 1 ? prediction.parameters.mu
                                    : axis == 2 ? prediction.parameters.kappa
                                                : prediction.parameters.beta;
            result.axes[axis].push_back({.coordinate = coordinate,
                                         .mean_objective = prediction.mean_objective,
                                         .standard_deviation = prediction.standard_deviation});
        }
    }
    return result;
}

std::string OptimizationProposal::to_json() const {
    std::ostringstream stream;
    stream << std::setprecision(15)
           << "{\"schema_version\":2,\"engine\":\"mbs-cpp-gp-matern52\","
              "\"objective_convention\":\"minimize_negative_proof_stress\",\"params\":["
           << parameters.lambda << ',' << parameters.mu << ',' << parameters.kappa << ','
           << parameters.beta << "],\"acq_func\":\"" << acquisition
           << "\",\"evaluated_count\":" << evaluated_count
           << ",\"initial_points_remaining\":" << initial_points_remaining
           << ",\"used_surrogate\":" << (used_surrogate ? "true" : "false")
           << ",\"predicted_objective\":" << predicted_objective
           << ",\"predicted_proof_stress\":" << -predicted_objective
           << ",\"predicted_stddev\":" << predicted_standard_deviation
           << ",\"acquisition_value\":" << acquisition_value << '}';
    return stream.str();
}

} // namespace mbs::optimization
