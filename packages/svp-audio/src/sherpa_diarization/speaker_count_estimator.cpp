#include "speaker_count_estimator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace svp::audio::sherpa_diarization_internal {
namespace {

// Cannot-link coloring is a defensive fallback over sparse chunk conflicts.
// Cap pathological graphs before backtracking can dominate diarization time.
constexpr std::size_t kMaximumCannotLinkAssignmentAttempts = 100000;

struct EigenSystem {
  std::vector<double> values;
  std::vector<double> vectors;
};

EigenSystem symmetric_eigenvalues(std::vector<double> matrix,
                                  std::size_t dimension) {
  if (dimension == 0) return {};
  std::vector<double> vectors(dimension * dimension, 0.0);
  for (std::size_t index = 0; index < dimension; ++index) {
    vectors[index * dimension + index] = 1.0;
  }
  const std::size_t maximum_sweeps = dimension * 32;
  const double convergence_tolerance = 1e-10;
  for (std::size_t sweep = 0; sweep < maximum_sweeps; ++sweep) {
    double maximum_change = 0.0;
    for (std::size_t row = 0; row < dimension; ++row) {
      for (std::size_t column = row + 1; column < dimension; ++column) {
        const double off_diagonal = matrix[row * dimension + column];
        maximum_change = std::max(maximum_change, std::abs(off_diagonal));
        if (std::abs(off_diagonal) <= convergence_tolerance) continue;
        const double row_value = matrix[row * dimension + row];
        const double column_value = matrix[column * dimension + column];
        const double tau =
            (column_value - row_value) / (2.0 * off_diagonal);
        const double tangent =
            (tau >= 0.0 ? 1.0 : -1.0) /
            (std::abs(tau) + std::sqrt(1.0 + tau * tau));
        const double cosine = 1.0 / std::sqrt(1.0 + tangent * tangent);
        const double sine = tangent * cosine;
        for (std::size_t index = 0; index < dimension; ++index) {
          if (index == row || index == column) continue;
          const double row_entry = matrix[row * dimension + index];
          const double column_entry = matrix[column * dimension + index];
          const double rotated_row =
              cosine * row_entry - sine * column_entry;
          const double rotated_column =
              sine * row_entry + cosine * column_entry;
          matrix[row * dimension + index] = rotated_row;
          matrix[index * dimension + row] = rotated_row;
          matrix[column * dimension + index] = rotated_column;
          matrix[index * dimension + column] = rotated_column;
        }
        matrix[row * dimension + row] = row_value - tangent * off_diagonal;
        matrix[column * dimension + column] =
            column_value + tangent * off_diagonal;
        matrix[row * dimension + column] = 0.0;
        matrix[column * dimension + row] = 0.0;
        for (std::size_t index = 0; index < dimension; ++index) {
          const double row_vector = vectors[index * dimension + row];
          const double column_vector = vectors[index * dimension + column];
          vectors[index * dimension + row] =
              cosine * row_vector - sine * column_vector;
          vectors[index * dimension + column] =
              sine * row_vector + cosine * column_vector;
        }
      }
    }
    if (maximum_change <= convergence_tolerance) break;
  }

  EigenSystem result;
  std::vector<std::pair<double, std::size_t>> ordered;
  ordered.reserve(dimension);
  for (std::size_t index = 0; index < dimension; ++index) {
    ordered.push_back({matrix[index * dimension + index], index});
  }
  std::sort(ordered.begin(), ordered.end());
  result.values.resize(dimension);
  result.vectors.resize(dimension * dimension);
  for (std::size_t output = 0; output < dimension; ++output) {
    result.values[output] = ordered[output].first;
    const std::size_t source = ordered[output].second;
    for (std::size_t row = 0; row < dimension; ++row) {
      result.vectors[row * dimension + output] =
          vectors[row * dimension + source];
    }
  }
  return result;
}

std::vector<int32_t> cluster_eigenvectors(
    const std::vector<double>& eigenvectors,
    std::size_t observation_count,
    std::size_t cluster_count) {
  std::vector<double> points(observation_count * cluster_count, 0.0);
  for (std::size_t row = 0; row < observation_count; ++row) {
    double norm = 0.0;
    for (std::size_t column = 0; column < cluster_count; ++column) {
      const double value =
          eigenvectors[row * observation_count + column];
      points[row * cluster_count + column] = value;
      norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm > 1e-12) {
      for (std::size_t column = 0; column < cluster_count; ++column) {
        points[row * cluster_count + column] /= norm;
      }
    }
  }

  std::vector<std::size_t> seeds = {0};
  while (seeds.size() < cluster_count) {
    double farthest_distance = -1.0;
    std::size_t farthest_point = 0;
    for (std::size_t point = 0; point < observation_count; ++point) {
      double nearest_distance = std::numeric_limits<double>::infinity();
      for (std::size_t seed : seeds) {
        double distance = 0.0;
        for (std::size_t dimension = 0;
             dimension < cluster_count; ++dimension) {
          const double delta =
              points[point * cluster_count + dimension] -
              points[seed * cluster_count + dimension];
          distance += delta * delta;
        }
        nearest_distance = std::min(nearest_distance, distance);
      }
      if (nearest_distance > farthest_distance) {
        farthest_distance = nearest_distance;
        farthest_point = point;
      }
    }
    seeds.push_back(farthest_point);
  }

  std::vector<double> centroids(cluster_count * cluster_count, 0.0);
  for (std::size_t cluster = 0; cluster < cluster_count; ++cluster) {
    for (std::size_t dimension = 0;
         dimension < cluster_count; ++dimension) {
      centroids[cluster * cluster_count + dimension] =
          points[seeds[cluster] * cluster_count + dimension];
    }
  }
  std::vector<int32_t> assignments(observation_count, -1);
  for (std::size_t iteration = 0; iteration < 100; ++iteration) {
    bool changed = false;
    for (std::size_t point = 0; point < observation_count; ++point) {
      double best_distance = std::numeric_limits<double>::infinity();
      int32_t best_cluster = 0;
      for (std::size_t cluster = 0; cluster < cluster_count; ++cluster) {
        double distance = 0.0;
        for (std::size_t dimension = 0;
             dimension < cluster_count; ++dimension) {
          const double delta =
              points[point * cluster_count + dimension] -
              centroids[cluster * cluster_count + dimension];
          distance += delta * delta;
        }
        if (distance < best_distance) {
          best_distance = distance;
          best_cluster = static_cast<int32_t>(cluster);
        }
      }
      changed = changed || assignments[point] != best_cluster;
      assignments[point] = best_cluster;
    }
    if (!changed && iteration > 0) break;

    std::fill(centroids.begin(), centroids.end(), 0.0);
    std::vector<std::size_t> members(cluster_count, 0);
    for (std::size_t point = 0; point < observation_count; ++point) {
      const std::size_t cluster =
          static_cast<std::size_t>(assignments[point]);
      ++members[cluster];
      for (std::size_t dimension = 0;
           dimension < cluster_count; ++dimension) {
        centroids[cluster * cluster_count + dimension] +=
            points[point * cluster_count + dimension];
      }
    }
    for (std::size_t cluster = 0; cluster < cluster_count; ++cluster) {
      if (members[cluster] == 0) continue;
      for (std::size_t dimension = 0;
           dimension < cluster_count; ++dimension) {
        centroids[cluster * cluster_count + dimension] /=
            static_cast<double>(members[cluster]);
      }
    }
  }
  return assignments;
}

bool enforce_cannot_links(
    const std::vector<SpeakerObservation>& observations,
    const std::set<std::pair<int32_t, int32_t>>& cannot_links,
    std::size_t cluster_count,
    std::vector<int32_t>& assignments) {
  std::map<int32_t, std::size_t> index_by_observation;
  for (std::size_t index = 0; index < observations.size(); ++index) {
    index_by_observation[observations[index].observation_id] = index;
  }
  std::vector<std::vector<std::size_t>> conflicts(observations.size());
  for (const auto& [left_id, right_id] : cannot_links) {
    const auto left = index_by_observation.find(left_id);
    const auto right = index_by_observation.find(right_id);
    if (left == index_by_observation.end() ||
        right == index_by_observation.end()) {
      continue;
    }
    conflicts[left->second].push_back(right->second);
    conflicts[right->second].push_back(left->second);
  }
  std::vector<std::size_t> order(observations.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](std::size_t left,
                                                    std::size_t right) {
    return conflicts[left].size() > conflicts[right].size();
  });
  const std::vector<int32_t> preferred = assignments;
  std::fill(assignments.begin(), assignments.end(), -1);
  std::size_t assignment_attempts = 0;
  bool assignment_budget_exhausted = false;
  const auto assign = [&](const auto& self, std::size_t position) -> bool {
    if (assignment_budget_exhausted) return false;
    if (position == order.size()) return true;
    const std::size_t observation = order[position];
    for (std::size_t attempt = 0; attempt < cluster_count; ++attempt) {
      if (assignment_attempts >= kMaximumCannotLinkAssignmentAttempts) {
        assignment_budget_exhausted = true;
        return false;
      }
      ++assignment_attempts;
      const int32_t candidate = attempt == 0
          ? preferred[observation]
          : static_cast<int32_t>((preferred[observation] + attempt) %
                                 cluster_count);
      bool allowed = true;
      for (std::size_t conflict : conflicts[observation]) {
        if (assignments[conflict] == candidate) {
          allowed = false;
          break;
        }
      }
      if (!allowed) continue;
      assignments[observation] = candidate;
      if (self(self, position + 1)) return true;
      assignments[observation] = -1;
      if (assignment_budget_exhausted) return false;
    }
    return false;
  };
  if (assign(assign, 0)) return true;
  assignments = preferred;
  return false;
}

}  // namespace

SpeakerCountEstimate estimate_speaker_count(
    const std::vector<SpeakerObservation>& observations,
    const std::set<std::pair<int32_t, int32_t>>& cannot_link_observations) {
  SpeakerCountEstimate result;
  const std::size_t count = observations.size();
  if (count == 0) return result;
  if (count == 1) {
    result.speaker_count = 1;
    result.laplacian_eigenvalues = {0.0};
    return result;
  }

  std::vector<double> distances(count * count, 0.0);
  for (std::size_t left = 0; left < count; ++left) {
    for (std::size_t right = left + 1; right < count; ++right) {
      const double similarity = cosine_similarity(
          observations[left].embedding, observations[right].embedding);
      const double distance = std::max(0.0, 1.0 - similarity);
      distances[left * count + right] = distance;
      distances[right * count + left] = distance;
    }
  }

  const std::size_t neighbor_count = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::ceil(std::sqrt(count))));
  std::vector<double> local_scales(count, 1.0);
  std::vector<std::set<std::size_t>> nearest_neighbors(count);
  for (std::size_t row = 0; row < count; ++row) {
    std::vector<std::pair<double, std::size_t>> neighbors;
    neighbors.reserve(count - 1);
    for (std::size_t column = 0; column < count; ++column) {
      if (row == column) continue;
      neighbors.push_back({distances[row * count + column], column});
    }
    std::sort(neighbors.begin(), neighbors.end());
    const std::size_t retained = std::min(neighbor_count, neighbors.size());
    for (std::size_t index = 0; index < retained; ++index) {
      nearest_neighbors[row].insert(neighbors[index].second);
    }
    if (retained > 0) {
      local_scales[row] = std::max(neighbors[retained - 1].first, 1e-6);
    }
  }

  std::vector<double> affinity(count * count, 0.0);
  std::vector<double> degree(count, 1.0);
  for (std::size_t left = 0; left < count; ++left) {
    affinity[left * count + left] = 1.0;
    for (std::size_t right = left + 1; right < count; ++right) {
      const auto cannot_link = std::minmax(
          observations[left].observation_id,
          observations[right].observation_id);
      if (cannot_link_observations.contains(cannot_link) ||
          (!nearest_neighbors[left].contains(right) &&
           !nearest_neighbors[right].contains(left))) {
        continue;
      }
      const double distance = distances[left * count + right];
      const double scale = local_scales[left] * local_scales[right];
      const double value = std::exp(-(distance * distance) / scale);
      affinity[left * count + right] = value;
      affinity[right * count + left] = value;
      degree[left] += value;
      degree[right] += value;
    }
  }

  std::vector<double> laplacian(count * count, 0.0);
  for (std::size_t row = 0; row < count; ++row) {
    for (std::size_t column = 0; column < count; ++column) {
      const double normalized_affinity =
          affinity[row * count + column] /
          std::sqrt(degree[row] * degree[column]);
      laplacian[row * count + column] =
          (row == column ? 1.0 : 0.0) - normalized_affinity;
    }
  }

  const EigenSystem eigen =
      symmetric_eigenvalues(std::move(laplacian), count);
  result.laplacian_eigenvalues = eigen.values;
  result.speaker_count = static_cast<std::size_t>(std::count_if(
      result.laplacian_eigenvalues.begin(),
      result.laplacian_eigenvalues.end(),
      [](double value) { return std::abs(value) <= 1e-6; }));
  result.speaker_count = std::max<std::size_t>(1, result.speaker_count);
  for (std::size_t index = 0;
       index + 1 < result.laplacian_eigenvalues.size(); ++index) {
    const double gap = result.laplacian_eigenvalues[index + 1] -
                       result.laplacian_eigenvalues[index];
    if (gap > result.selected_eigengap) {
      result.selected_eigengap = gap;
      result.speaker_count = index + 1;
    }
  }
  std::vector<int32_t> assignments = cluster_eigenvectors(
      eigen.vectors, count, result.speaker_count);
  result.assignments_respect_cannot_link = enforce_cannot_links(
      observations, cannot_link_observations, result.speaker_count,
      assignments);
  std::vector<std::pair<float, int32_t>> cluster_starts;
  for (std::size_t cluster = 0; cluster < result.speaker_count; ++cluster) {
    float first_start = std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < count; ++index) {
      if (assignments[index] == static_cast<int32_t>(cluster)) {
        first_start = std::min(first_start,
                               observations[index].first_start_sec);
      }
    }
    cluster_starts.push_back({first_start, static_cast<int32_t>(cluster)});
  }
  std::sort(cluster_starts.begin(), cluster_starts.end());
  std::map<int32_t, int32_t> canonical_cluster;
  for (std::size_t index = 0; index < cluster_starts.size(); ++index) {
    canonical_cluster[cluster_starts[index].second] =
        static_cast<int32_t>(index);
  }
  for (std::size_t index = 0; index < count; ++index) {
    result.observation_to_speaker[observations[index].observation_id] =
        canonical_cluster[assignments[index]];
  }
  return result;
}

}  // namespace svp::audio::sherpa_diarization_internal
