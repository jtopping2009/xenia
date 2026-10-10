/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SENDSPIN_SENDSPIN_TIME_FILTER_H_
#define XENIA_APU_SENDSPIN_SENDSPIN_TIME_FILTER_H_

#include <cstdint>

namespace xe {
namespace apu {
namespace sendspin {

// Two-dimensional Kalman filter (state = [offset, drift]) for NTP-style
// client/server clock synchronization. Direct port of aiosendspin's
// SendspinTimeFilter (client/time_sync.py), itself a 1:1 port of the ESPHome
// implementation - the update equations are intentionally not re-derived so
// behavior (including the adaptive-forgetting outlier recovery) matches the
// reference client exactly.
class SendspinTimeFilter {
 public:
  explicit SendspinTimeFilter(double process_std_dev = 0.0,
                              double forget_factor = 2.0,
                              double drift_process_std_dev = 1e-11);

  // Processes one NTP-style measurement.
  //   measurement: ((T2-T1)+(T3-T4))/2 in microseconds.
  //   max_error: ((T4-T1)-(T3-T2))/2, half the round-trip delay, in
  //     microseconds.
  //   time_added: local (client) timestamp the measurement was taken at, in
  //     microseconds - must be monotonically increasing between calls.
  void Update(int64_t measurement, int64_t max_error, int64_t time_added);

  // Converts a local (client) timestamp to the equivalent server timestamp.
  int64_t ComputeServerTime(int64_t client_time) const;
  // Converts a server timestamp to the equivalent local (client) timestamp.
  int64_t ComputeClientTime(int64_t server_time) const;

  void Reset();

  int count() const { return count_; }
  // True once at least 2 measurements have been processed and the offset
  // covariance is finite - matches aiosendspin's is_synchronized property.
  bool is_synchronized() const;
  // Standard deviation estimate of the offset, in microseconds.
  int64_t error() const;
  double offset() const { return offset_; }

 private:
  struct TimeElement {
    int64_t last_update = 0;
    double offset = 0.0;
    double drift = 0.0;
    bool use_drift = false;
  };

  double process_variance_;
  double drift_process_variance_;
  double forget_variance_factor_;

  int64_t last_update_ = 0;
  int count_ = 0;

  double offset_ = 0.0;
  double drift_ = 0.0;

  double offset_covariance_;
  double offset_drift_covariance_ = 0.0;
  double drift_covariance_ = 0.0;

  TimeElement current_time_element_;
};

}  // namespace sendspin
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SENDSPIN_SENDSPIN_TIME_FILTER_H_
