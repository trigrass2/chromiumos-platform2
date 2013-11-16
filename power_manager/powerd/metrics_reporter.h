// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWERD_METRICS_REPORTER_H_
#define POWER_MANAGER_POWERD_METRICS_REPORTER_H_
#pragma once

#include <gtest/gtest_prod.h>

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/time.h"
#include "base/timer.h"
#include "power_manager/common/clock.h"
#include "power_manager/common/power_constants.h"
#include "power_manager/powerd/system/power_supply.h"

class MetricsLibraryInterface;

namespace power_manager {

class PrefsInterface;

namespace policy {
class BacklightController;
}

// Used by Daemon to report metrics by way of Chrome.
class MetricsReporter {
 public:
  // Returns a copy of |enum_name| with a suffix describing |power_source|
  // appended to it. Public so it can be called by tests.
  static std::string AppendPowerSourceToEnumName(
      const std::string& enum_name,
      PowerSource power_source);

  // Ownership of pointers remains with the caller.
  MetricsReporter(PrefsInterface* prefs,
                  MetricsLibraryInterface* metrics_lib,
                  policy::BacklightController* display_backlight_controller,
                  policy::BacklightController* keyboard_backlight_controller);
  ~MetricsReporter();

  // Initializes the object and starts |generate_backlight_metrics_timer_|.
  void Init(const system::PowerStatus& power_status);

  // Records changes to system state.
  void HandleScreenDimmedChange(bool dimmed,
                                base::TimeTicks last_user_activity_time);
  void HandleScreenOffChange(bool off, base::TimeTicks last_user_activity_time);
  void HandleSessionStateChange(SessionState state);
  void HandlePowerStatusUpdate(const system::PowerStatus& status);
  void PrepareForSuspend();
  void HandleResume();

  // Sends a metric describing a suspend attempt that didn't succeed on its
  // first attempt.  Doesn't send anything if |num_retries| is 0.
  void GenerateRetrySuspendMetric(int num_retries, int max_retries);

  // Generates UMA metrics on when leaving the idle state.
  void GenerateUserActivityMetrics();

  // Generates UMA metrics about the current backlight level.
  void GenerateBacklightLevelMetrics();

  // Handles the power button being pressed or released.
  void HandlePowerButtonEvent(ButtonState state);

 private:
  friend class MetricsReporterTest;
  FRIEND_TEST(MetricsReporterTest, BacklightLevel);
  FRIEND_TEST(MetricsReporterTest, SendEnumMetric);
  FRIEND_TEST(MetricsReporterTest, SendMetric);
  FRIEND_TEST(MetricsReporterTest, SendMetricWithPowerSource);

  // See MetricsLibrary::SendToUMA in metrics/metrics_library.h for a
  // description of the arguments in the below methods.

  // Sends a regular (exponential) histogram sample.
  bool SendMetric(const std::string& name,
                  int sample,
                  int min,
                  int max,
                  int nbuckets);

  // Sends an enumeration (linear) histogram sample.
  bool SendEnumMetric(const std::string& name,
                      int sample,
                      int max);

  // These methods also append the current power source to |name|.
  bool SendMetricWithPowerSource(const std::string& name,
                                 int sample,
                                 int min,
                                 int max,
                                 int nbuckets);
  bool SendEnumMetricWithPowerSource(const std::string& name,
                                     int sample,
                                     int max);

  // Generates a battery discharge rate UMA metric sample. Returns
  // true if a sample was sent to UMA, false otherwise.
  void GenerateBatteryDischargeRateMetric();

  // Sends a histogram sample containing the rate at which the battery
  // discharged while the system was suspended if the system was on battery
  // power both before suspending and after resuming.  Called by
  // GenerateMetricsOnPowerEvent().  Returns true if the sample was sent.
  void GenerateBatteryDischargeRateWhileSuspendedMetric();

  // Increments the number of user sessions that have been active on the
  // current battery charge.
  void IncrementNumOfSessionsPerChargeMetric();

  // Generates number of sessions per charge UMA metric sample if the current
  // stored value is greater then 0.
  void GenerateNumOfSessionsPerChargeMetric();

  PrefsInterface* prefs_;
  MetricsLibraryInterface* metrics_lib_;
  policy::BacklightController* display_backlight_controller_;
  policy::BacklightController* keyboard_backlight_controller_;

  Clock clock_;

  // Last power status passed to HandlePowerStatusUpdate().
  system::PowerStatus last_power_status_;

  // Current session state.
  SessionState session_state_;

  // Time at which the current session (if any) started.
  base::TimeTicks session_start_time_;

  // Runs GenerateBacklightLevelMetric().
  base::RepeatingTimer<MetricsReporter> generate_backlight_metrics_timer_;

  // Timestamp of the last generated battery discharge rate metric.
  base::TimeTicks last_battery_discharge_rate_metric_timestamp_;

  // Timestamp of the last time the power button was down.
  base::TimeTicks last_power_button_down_timestamp_;

  // Timestamp of the last idle event (that is, either
  // |screen_dim_timestamp_| or |screen_off_timestamp_|).
  base::TimeTicks last_idle_event_timestamp_;

  // Idle duration as of the last idle event.
  base::TimeDelta last_idle_timedelta_;

  // Timestamps of the last idle-triggered power state transitions.
  base::TimeTicks screen_dim_timestamp_;
  base::TimeTicks screen_off_timestamp_;

  // Information recorded by PrepareForSuspend() just before the system
  // suspends.  |time_before_suspend_| is intentionally base::Time rather
  // than base::TimeTicks because the latter doesn't increase while the
  // system is suspended.
  double battery_energy_before_suspend_;
  bool on_line_power_before_suspend_;
  base::Time time_before_suspend_;

  // Set by HandleResume() to indicate that
  // GenerateBatteryDischargeRateWhileSuspendedMetric() should send a
  // sample when it is next called.
  bool report_battery_discharge_rate_while_suspended_;

  DISALLOW_COPY_AND_ASSIGN(MetricsReporter);
};

}  // namespace power_manager

#endif  // POWER_MANAGER_POWERD_METRICS_REPORTER_H_
