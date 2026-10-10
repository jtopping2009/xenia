/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sendspin/sendspin_time_filter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace xe {
namespace apu {
namespace sendspin {

namespace {
// Residual threshold as a multiple of max_error for triggering adaptive
// forgetting: when |residual| > CUTOFF * max_error, the filter widens its
// covariance to recover quickly from outliers (network blips, clock steps).
constexpr double kAdaptiveForgettingCutoff = 3.0;
// max_error (half round-trip delay) overestimates true measurement noise;
// this scales it down before use as the measurement standard deviation.
constexpr double kMaxErrorScale = 0.5;
// Drift is only applied in time conversions when it's statistically
// significant relative to its own estimated covariance.
constexpr double kDriftSignificanceThresholdSquared = 2.0 * 2.0;
}  // namespace

SendspinTimeFilter::SendspinTimeFilter(double process_std_dev,
                                       double forget_factor,
                                       double drift_process_std_dev)
    : process_variance_(process_std_dev * process_std_dev),
      drift_process_variance_(drift_process_std_dev * drift_process_std_dev),
      forget_variance_factor_(forget_factor * forget_factor),
      offset_covariance_(std::numeric_limits<double>::infinity()) {}

void SendspinTimeFilter::Update(int64_t measurement, int64_t max_error,
                                int64_t time_added) {
  if (time_added <= last_update_) {
    // Skip non-monotonic timestamps to guard against backwards dt below.
    return;
  }

  double dt = static_cast<double>(time_added - last_update_);
  last_update_ = time_added;

  double update_std_dev = static_cast<double>(max_error) * kMaxErrorScale;
  double measurement_variance = update_std_dev * update_std_dev;

  // First measurement establishes the offset baseline.
  if (count_ <= 0) {
    ++count_;

    offset_ = static_cast<double>(measurement);
    offset_covariance_ = measurement_variance;
    drift_ = 0.0;

    current_time_element_ =
        TimeElement{last_update_, offset_, drift_, false};
    return;
  }

  // Second measurement: initial drift estimate from finite differences.
  if (count_ == 1) {
    ++count_;

    drift_ = (static_cast<double>(measurement) - offset_) / dt;
    offset_ = static_cast<double>(measurement);

    drift_covariance_ =
        (offset_covariance_ + measurement_variance) / (dt * dt);
    offset_covariance_ = measurement_variance;

    current_time_element_ =
        TimeElement{last_update_, offset_, drift_, false};
    return;
  }

  // Kalman prediction step. State transition matrix F = [1, dt; 0, 1].
  double predicted_offset = offset_ + drift_ * dt;
  double dt_squared = dt * dt;

  double drift_process_variance = dt * drift_process_variance_;
  double new_drift_covariance = drift_covariance_ + drift_process_variance;

  double new_offset_drift_covariance =
      offset_drift_covariance_ + drift_covariance_ * dt;

  double offset_process_variance = dt * process_variance_;
  double new_offset_covariance = offset_covariance_ +
                                 2 * offset_drift_covariance_ * dt +
                                 drift_covariance_ * dt_squared +
                                 offset_process_variance;

  // Innovation and adaptive forgetting.
  double residual = static_cast<double>(measurement) - predicted_offset;
  double max_residual_cutoff =
      static_cast<double>(max_error) * kAdaptiveForgettingCutoff;

  if (count_ < 100) {
    ++count_;
  } else if (std::fabs(residual) > max_residual_cutoff) {
    new_drift_covariance *= forget_variance_factor_;
    new_offset_drift_covariance *= forget_variance_factor_;
    new_offset_covariance *= forget_variance_factor_;
  }

  // Kalman update step. H = [1, 0].
  double uncertainty =
      1.0 / std::max(new_offset_covariance + measurement_variance, 1e-9);

  double offset_gain = new_offset_covariance * uncertainty;
  double drift_gain = new_offset_drift_covariance * uncertainty;

  offset_ = predicted_offset + offset_gain * residual;
  drift_ += drift_gain * residual;

  drift_covariance_ = new_drift_covariance - drift_gain * new_offset_drift_covariance;
  offset_drift_covariance_ =
      new_offset_drift_covariance - drift_gain * new_offset_covariance;
  offset_covariance_ = new_offset_covariance - offset_gain * new_offset_covariance;

  bool use_drift = drift_ * drift_ >
                   kDriftSignificanceThresholdSquared * drift_covariance_;

  current_time_element_ = TimeElement{last_update_, offset_, drift_, use_drift};
}

int64_t SendspinTimeFilter::ComputeServerTime(int64_t client_time) const {
  const TimeElement& element = current_time_element_;
  double effective_drift = element.use_drift ? element.drift : 0.0;
  double dt = static_cast<double>(client_time - element.last_update);
  int64_t offset =
      static_cast<int64_t>(std::llround(element.offset + effective_drift * dt));
  return client_time + offset;
}

int64_t SendspinTimeFilter::ComputeClientTime(int64_t server_time) const {
  const TimeElement& element = current_time_element_;
  double effective_drift = element.use_drift ? element.drift : 0.0;
  double numerator = static_cast<double>(server_time) - element.offset +
                     effective_drift * static_cast<double>(element.last_update);
  return static_cast<int64_t>(std::llround(numerator / (1.0 + effective_drift)));
}

void SendspinTimeFilter::Reset() {
  count_ = 0;
  last_update_ = 0;
  offset_ = 0.0;
  drift_ = 0.0;
  offset_covariance_ = std::numeric_limits<double>::infinity();
  offset_drift_covariance_ = 0.0;
  drift_covariance_ = 0.0;
  current_time_element_ = TimeElement{};
}

bool SendspinTimeFilter::is_synchronized() const {
  return count_ >= 2 && !std::isinf(offset_covariance_);
}

int64_t SendspinTimeFilter::error() const {
  return static_cast<int64_t>(std::llround(std::sqrt(offset_covariance_)));
}

}  // namespace sendspin
}  // namespace apu
}  // namespace xe
