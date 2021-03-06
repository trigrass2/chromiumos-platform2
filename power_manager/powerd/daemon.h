// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWERD_DAEMON_H_
#define POWER_MANAGER_POWERD_DAEMON_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include <base/compiler_specific.h>
#include <base/files/file_path.h>
#include <base/macros.h>
#include <base/memory/weak_ptr.h>
#include <base/time/time.h>
#include <base/timer/timer.h>
#include <dbus/exported_object.h>

#include "power_manager/common/prefs_observer.h"
#include "power_manager/powerd/policy/backlight_controller_observer.h"
#include "power_manager/powerd/policy/input_event_handler.h"
#include "power_manager/powerd/policy/suspender.h"
#include "power_manager/powerd/system/audio_observer.h"
#include "power_manager/powerd/system/power_supply_observer.h"

namespace dbus {
class ObjectProxy;
}

namespace power_manager {

class DaemonDelegate;
class MetricsSenderInterface;
class PeriodicActivityLogger;
class PrefsInterface;
class StartStopActivityLogger;

namespace metrics {
class MetricsCollector;
}  // namespace metrics

namespace policy {
class BacklightController;
class InputDeviceController;
class StateController;
class Suspender;
}  // namespace policy

namespace system {
class AcpiWakeupHelperInterface;
class AmbientLightSensorInterface;
class AudioClientInterface;
class BacklightInterface;
class DarkResumeInterface;
class DBusWrapperInterface;
class DisplayPowerSetterInterface;
class DisplayWatcherInterface;
class EcWakeupHelperInterface;
class InputWatcherInterface;
class PeripheralBatteryWatcher;
class PowerSupplyInterface;
class UdevInterface;
}  // namespace system

class Daemon;

// Main class within the powerd daemon that ties all other classes together.
class Daemon : public policy::BacklightControllerObserver,
               public policy::InputEventHandler::Delegate,
               public policy::Suspender::Delegate,
               public system::AudioObserver,
               public system::PowerSupplyObserver {
 public:
  Daemon(DaemonDelegate* delegate, const base::FilePath& run_dir);
  virtual ~Daemon();

  void set_wakeup_count_path_for_testing(const base::FilePath& path) {
    wakeup_count_path_ = path;
  }
  void set_oobe_completed_path_for_testing(const base::FilePath& path) {
    oobe_completed_path_ = path;
  }
  void set_suspended_state_path_for_testing(const base::FilePath& path) {
    suspended_state_path_ = path;
  }
  void set_flashrom_lock_path_for_testing(const base::FilePath& path) {
    flashrom_lock_path_ = path;
  }
  void set_battery_tool_lock_path_for_testing(const base::FilePath& path) {
    battery_tool_lock_path_ = path;
  }
  void set_proc_path_for_testing(const base::FilePath& path) {
    proc_path_ = path;
  }

  void Init();

  // If |retry_shutdown_for_firmware_update_timer_| is running, triggers it
  // and returns true. Otherwise, returns false.
  bool TriggerRetryShutdownTimerForTesting();

  // Overridden from policy::BacklightControllerObserver:
  void OnBrightnessChange(
      double brightness_percent,
      policy::BacklightController::BrightnessChangeCause cause,
      policy::BacklightController* source) override;

  // Overridden from policy::InputEventHandler::Delegate:
  void HandleLidClosed() override;
  void HandleLidOpened() override;
  void HandlePowerButtonEvent(ButtonState state) override;
  void HandleHoverStateChange(bool hovering) override;
  void HandleTabletModeChange(TabletMode mode) override;
  void ShutDownForPowerButtonWithNoDisplay() override;
  void HandleMissingPowerButtonAcknowledgment() override;
  void ReportPowerButtonAcknowledgmentDelay(base::TimeDelta delay) override;

  // Overridden from policy::Suspender::Delegate:
  int GetInitialSuspendId() override;
  int GetInitialDarkSuspendId() override;
  bool IsLidClosedForSuspend() override;
  bool ReadSuspendWakeupCount(uint64_t* wakeup_count) override;
  void SetSuspendAnnounced(bool announced) override;
  bool GetSuspendAnnounced() override;
  void PrepareToSuspend() override;
  SuspendResult DoSuspend(uint64_t wakeup_count,
                          bool wakeup_count_valid,
                          base::TimeDelta duration) override;
  void UndoPrepareToSuspend(bool success,
                            int num_suspend_attempts,
                            bool canceled_while_in_dark_resume) override;
  void GenerateDarkResumeMetrics(
      const std::vector<policy::Suspender::DarkResumeInfo>&
          dark_resume_wake_durations,
      base::TimeDelta suspend_duration) override;
  void ShutDownForFailedSuspend() override;
  void ShutDownForDarkResume() override;

  // Overridden from system::AudioObserver:
  void OnAudioStateChange(bool active) override;

  // Overridden from system::PowerSupplyObserver:
  void OnPowerStatusUpdate() override;

 private:
  class StateControllerDelegate;
  class SuspenderDelegate;

  // Passed to ShutDown() to specify whether the system should power off or
  // reboot.
  enum class ShutdownMode {
    POWER_OFF,
    REBOOT,
  };

  // Convenience method that returns true if |name| exists and is true.
  bool BoolPrefIsTrue(const std::string& name) const;

  // Returns true if |path| exists and contains the PID of an active process.
  bool PidLockFileExists(const base::FilePath& path);

  // Returns true if a process that updates firmware is running. |details_out|
  // is updated to contain information about the process(es).
  bool FirmwareIsBeingUpdated(std::string* details_out);

  // Runs powerd_setuid_helper. |action| is passed via --action.  If
  // |additional_args| is non-empty, it will be appended to the command. If
  // |wait_for_completion| is true, this function will block until the helper
  // finishes and return the helper's exit code; otherwise it will return 0
  // immediately.
  int RunSetuidHelper(const std::string& action,
                      const std::string& additional_args,
                      bool wait_for_completion);

  // Decreases/increases the keyboard brightness; direction should be +1 for
  // increase and -1 for decrease.
  void AdjustKeyboardBrightness(int direction);

  // Emits a D-Bus signal named |signal_name| announcing that backlight
  // brightness was changed to |brightness_percent| due to |cause|.
  void SendBrightnessChangedSignal(
      double brightness_percent,
      policy::BacklightController::BrightnessChangeCause cause,
      const std::string& signal_name);

  // Connects to the D-Bus system bus and exports methods.
  void InitDBus();

  // Handles various D-Bus services becoming available or restarting.
  void HandleChromeAvailableOrRestarted(bool available);
  void HandleSessionManagerAvailableOrRestarted(bool available);
  void HandleCrasAvailableOrRestarted(bool available);

  // Handles other D-Bus services just becoming initially available (i.e.
  // restarts are ignored).
  void HandleUpdateEngineAvailable(bool available);
  void HandleCryptohomedAvailable(bool available);

  // Handles changes to D-Bus name ownership.
  void HandleDBusNameOwnerChanged(dbus::Signal* signal);

  // Callbacks for handling D-Bus signals and method calls.
  void HandleSessionStateChangedSignal(dbus::Signal* signal);
  void HandleUpdateEngineStatusUpdateSignal(dbus::Signal* signal);
  void HandleCrasNodesChangedSignal(dbus::Signal* signal);
  void HandleCrasActiveOutputNodeChangedSignal(dbus::Signal* signal);
  void HandleCrasNumberOfActiveStreamsChanged(dbus::Signal* signal);
  void HandleGetTpmStatusResponse(dbus::Response* response);
  std::unique_ptr<dbus::Response> HandleRequestShutdownMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleRequestRestartMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleRequestSuspendMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleDecreaseScreenBrightnessMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleIncreaseScreenBrightnessMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleGetScreenBrightnessMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleSetScreenBrightnessMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleDecreaseKeyboardBrightnessMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleIncreaseKeyboardBrightnessMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleGetPowerSupplyPropertiesMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleVideoActivityMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleUserActivityMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleSetIsProjectingMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleSetPolicyMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleSetPowerSourceMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleSetBacklightsForcedOffMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandleGetBacklightsForcedOffMethod(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> HandlePowerButtonAcknowledgment(
      dbus::MethodCall* method_call);

  // Handles information from the session manager about the session state.
  void OnSessionStateChange(const std::string& state_str);

  // Handles the "operation" field from an update engine status message.
  void OnUpdateOperation(const std::string& operation);

  // Asynchronously asks |cryptohomed_dbus_proxy| (which must be non-null) to
  // return the TPM status, which is handled by HandleGetTpmStatusResponse().
  void RequestTpmStatus();

  // Shuts the system down immediately.
  void ShutDown(ShutdownMode mode, ShutdownReason reason);

  // Starts the suspend process. If |use_external_wakeup_count| is true,
  // passes |external_wakeup_count| to
  // policy::Suspender::RequestSuspendWithExternalWakeupCount();
  void Suspend(bool use_external_wakeup_count, uint64_t external_wakeup_count);

  // Updates state in |all_backlight_controllers_|.
  void SetBacklightsDimmedForInactivity(bool dimmed);
  void SetBacklightsOffForInactivity(bool off);
  void SetBacklightsSuspended(bool suspended);
  void SetBacklightsDocked(bool docked);

  // Parses kIwlWifiTransmitPowerTablePref if set and updates
  // |iwl_wifi_power_table_|.
  void PopulateIwlWifiTransmitPowerTable();

  // Updates wifi transmit power for |mode|. Should only be called if
  // |set_wifi_transmit_power_for_tablet_mode_| is true.
  void UpdateWifiTransmitPowerForTabletMode(TabletMode mode);

  DaemonDelegate* delegate_;  // weak

  std::unique_ptr<PrefsInterface> prefs_;

  std::unique_ptr<system::DBusWrapperInterface> dbus_wrapper_;

  dbus::ObjectProxy* session_manager_dbus_proxy_;  // owned by |dbus_wrapper_|
  // May be null if |kUseCrasPref| is false.
  dbus::ObjectProxy* update_engine_dbus_proxy_;  // owned by |dbus_wrapper_|
  // May be null if the TPM status is not needed.
  dbus::ObjectProxy* cryptohomed_dbus_proxy_;  // owned by |dbus_wrapper_|

  std::unique_ptr<StateControllerDelegate> state_controller_delegate_;
  std::unique_ptr<MetricsSenderInterface> metrics_sender_;

  // Many of these members may be null depending on the device's hardware
  // configuration.
  std::unique_ptr<system::AmbientLightSensorInterface> light_sensor_;
  std::unique_ptr<system::DisplayWatcherInterface> display_watcher_;
  std::unique_ptr<system::DisplayPowerSetterInterface> display_power_setter_;
  std::unique_ptr<system::BacklightInterface> display_backlight_;
  std::unique_ptr<policy::BacklightController> display_backlight_controller_;
  std::unique_ptr<system::BacklightInterface> keyboard_backlight_;
  std::unique_ptr<policy::BacklightController> keyboard_backlight_controller_;

  std::unique_ptr<system::UdevInterface> udev_;
  std::unique_ptr<system::InputWatcherInterface> input_watcher_;
  std::unique_ptr<policy::StateController> state_controller_;
  std::unique_ptr<policy::InputEventHandler> input_event_handler_;
  std::unique_ptr<system::AcpiWakeupHelperInterface> acpi_wakeup_helper_;
  std::unique_ptr<system::EcWakeupHelperInterface> ec_wakeup_helper_;
  std::unique_ptr<policy::InputDeviceController> input_device_controller_;
  std::unique_ptr<system::AudioClientInterface> audio_client_;  // May be null.
  std::unique_ptr<system::PeripheralBatteryWatcher>
      peripheral_battery_watcher_;  // May be null.
  std::unique_ptr<system::PowerSupplyInterface> power_supply_;
  std::unique_ptr<system::DarkResumeInterface> dark_resume_;
  std::unique_ptr<policy::Suspender> suspender_;

  std::unique_ptr<metrics::MetricsCollector> metrics_collector_;

  // Weak pointers to |display_backlight_controller_| and
  // |keyboard_backlight_controller_|, if non-null.
  std::vector<policy::BacklightController*> all_backlight_controllers_;

  // True once the shutdown process has started. Remains true until the
  // system has powered off.
  bool shutting_down_;

  // Recurring timer that's started if a shutdown request is deferred due to a
  // firmware update. ShutDown() is called repeatedly so the system will
  // eventually be shut down after the firmware-updating process exits.
  base::Timer retry_shutdown_for_firmware_update_timer_;

  // Timer that periodically calls RequestTpmStatus() if
  // |cryptohome_dbus_proxy_| is non-null.
  base::RepeatingTimer tpm_status_timer_;

  // Delay with which |tpm_status_timer_| should fire.
  base::TimeDelta tpm_status_interval_;

  // File containing the number of wakeup events.
  base::FilePath wakeup_count_path_;

  // File that's created once the out-of-box experience has been completed.
  base::FilePath oobe_completed_path_;

  // Files where flashrom or battery_tool store their PIDs while performing a
  // potentially-destructive action that powerd shouldn't interrupt by
  // suspending or shutting down the system.
  base::FilePath flashrom_lock_path_;
  base::FilePath battery_tool_lock_path_;

  // Directory containing subdirectories corresponding to running processes
  // (i.e. /proc in non-test environments).
  base::FilePath proc_path_;

  // Path to file that's touched before the system suspends and unlinked after
  // it resumes. Used by crash-reporter to avoid reporting unclean shutdowns
  // that occur while the system is suspended (i.e. probably due to the battery
  // charge reaching zero).
  base::FilePath suspended_state_path_;

  // Path to a file that's touched when a suspend attempt's commencement is
  // announced to other processes and unlinked when the attempt's completion is
  // announced. Used to detect cases where powerd was restarted
  // mid-suspend-attempt and didn't announce that the attempt finished.
  base::FilePath suspend_announced_path_;

  // Last session state that we have been informed of. Initialized as stopped.
  SessionState session_state_;

  // Set to true if powerd touched a file for crash-reporter before
  // suspending. If true, the file will be unlinked after resuming.
  bool created_suspended_state_file_;

  // True if the "mosys" command should be used to record suspend and resume
  // timestamps in eventlog.
  bool log_suspend_with_mosys_eventlog_;

  // True if the system should suspend to idle.
  bool suspend_to_idle_;

  // Set wifi transmit power for tablet mode.
  bool set_wifi_transmit_power_for_tablet_mode_;

  // Intel iwlwifi driver power table.
  std::string iwl_wifi_power_table_;

  // Used to log video, user, and audio activity and hovering.
  std::unique_ptr<PeriodicActivityLogger> video_activity_logger_;
  std::unique_ptr<PeriodicActivityLogger> user_activity_logger_;
  std::unique_ptr<StartStopActivityLogger> audio_activity_logger_;
  std::unique_ptr<StartStopActivityLogger> hovering_logger_;

  // Must come last so that weak pointers will be invalidated before other
  // members are destroyed.
  base::WeakPtrFactory<Daemon> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(Daemon);
};

}  // namespace power_manager

#endif  // POWER_MANAGER_POWERD_DAEMON_H_
