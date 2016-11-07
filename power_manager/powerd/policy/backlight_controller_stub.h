// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWERD_POLICY_BACKLIGHT_CONTROLLER_STUB_H_
#define POWER_MANAGER_POWERD_POLICY_BACKLIGHT_CONTROLLER_STUB_H_

#include <vector>

#include <base/macros.h>
#include <base/observer_list.h>

#include "power_manager/powerd/policy/backlight_controller.h"
#include "power_manager/powerd/policy/backlight_controller_observer.h"
#include "power_manager/proto_bindings/policy.pb.h"

namespace power_manager {
namespace policy {

// policy::BacklightController implementation that returns dummy values.
class BacklightControllerStub : public policy::BacklightController {
 public:
  BacklightControllerStub();
  virtual ~BacklightControllerStub();

  const std::vector<PowerSource>& power_source_changes() const {
    return power_source_changes_;
  }
  const std::vector<DisplayMode>& display_mode_changes() const {
    return display_mode_changes_;
  }
  const std::vector<SessionState>& session_state_changes() const {
    return session_state_changes_;
  }
  int power_button_presses() const { return power_button_presses_; }
  const std::vector<UserActivityType>& user_activity_reports() const {
    return user_activity_reports_;
  }
  const std::vector<bool>& video_activity_reports() const {
    return video_activity_reports_;
  }
  const std::vector<bool>& hover_state_changes() const {
    return hover_state_changes_;
  }
  const std::vector<TabletMode>& tablet_mode_changes() const {
    return tablet_mode_changes_;
  }
  const std::vector<PowerManagementPolicy>& policy_changes() const {
    return policy_changes_;
  }
  int chrome_starts() const { return chrome_starts_; }
  bool dimmed() const { return dimmed_; }
  bool off() const { return off_; }
  bool suspended() const { return suspended_; }
  bool shutting_down() const { return shutting_down_; }
  bool docked() const { return docked_; }
  bool forced_off() const { return forced_off_; }
  double user_brightness_percent() const { return user_brightness_percent_; }
  int num_user_brightness_increases() const {
    return num_user_brightness_increases_;
  }
  int num_user_brightness_decreases() const {
    return num_user_brightness_decreases_;
  }

  void set_percent(double percent) { percent_ = percent; }
  void set_num_als_adjustments(int num) { num_als_adjustments_ = num; }
  void set_num_user_adjustments(int num) { num_user_adjustments_ = num; }

  void ResetStats();

  // Notify |observers_| that the brightness has changed to |percent| due
  // to |cause|. Also updates |percent_|.
  void NotifyObservers(double percent, BrightnessChangeCause cause);

  // policy::BacklightController implementation:
  void AddObserver(policy::BacklightControllerObserver* observer) override;
  void RemoveObserver(policy::BacklightControllerObserver* observer) override;
  void HandlePowerSourceChange(PowerSource source) override;
  void HandleDisplayModeChange(DisplayMode mode) override;
  void HandleSessionStateChange(SessionState state) override;
  void HandlePowerButtonPress() override;
  void HandleUserActivity(UserActivityType type) override;
  void HandleVideoActivity(bool is_fullscreen) override;
  void HandleHoverStateChange(bool hovering) override;
  void HandleTabletModeChange(TabletMode mode) override;
  void HandlePolicyChange(const PowerManagementPolicy& policy) override;
  void HandleChromeStart() override;
  void SetDimmedForInactivity(bool dimmed) override;
  void SetOffForInactivity(bool off) override;
  void SetSuspended(bool suspended) override;
  void SetShuttingDown(bool shutting_down) override;
  void SetDocked(bool docked) override;
  void SetForcedOff(bool forced_off) override;
  bool GetForcedOff() override;
  bool GetBrightnessPercent(double* percent) override;
  bool SetUserBrightnessPercent(double percent, Transition transition) override;
  bool IncreaseUserBrightness() override;
  bool DecreaseUserBrightness(bool allow_off) override;
  int GetNumAmbientLightSensorAdjustments() const override {
    return num_als_adjustments_;
  }
  int GetNumUserAdjustments() const override { return num_user_adjustments_; }

 private:
  base::ObserverList<BacklightControllerObserver> observers_;

  // Percent to be returned by GetBrightnessPercent().
  double percent_ = 100.0;

  std::vector<PowerSource> power_source_changes_;
  std::vector<DisplayMode> display_mode_changes_;
  std::vector<SessionState> session_state_changes_;
  int power_button_presses_ = 0;
  std::vector<UserActivityType> user_activity_reports_;
  std::vector<bool> video_activity_reports_;
  std::vector<bool> hover_state_changes_;
  std::vector<TabletMode> tablet_mode_changes_;
  std::vector<PowerManagementPolicy> policy_changes_;
  int chrome_starts_ = 0;

  bool dimmed_ = false;
  bool off_ = false;
  bool suspended_ = false;
  bool shutting_down_ = false;
  bool docked_ = false;
  bool forced_off_ = false;

  double user_brightness_percent_ = 0.0;
  int num_user_brightness_increases_ = 0;
  int num_user_brightness_decreases_ = 0;

  // Counts to be returned by GetNum*Adjustments().
  int num_als_adjustments_ = 0;
  int num_user_adjustments_ = 0;

  DISALLOW_COPY_AND_ASSIGN(BacklightControllerStub);
};

}  // namespace policy
}  // namespace power_manager

#endif  // POWER_MANAGER_POWERD_POLICY_BACKLIGHT_CONTROLLER_STUB_H_
