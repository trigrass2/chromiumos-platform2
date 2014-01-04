// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWERD_DAEMON_H_
#define POWER_MANAGER_POWERD_DAEMON_H_
#pragma once

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/file_path.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "base/time.h"
#include "dbus/bus.h"
#include "dbus/exported_object.h"
#include "dbus/message.h"
#include "dbus/object_proxy.h"
#include "power_manager/common/prefs_observer.h"
#include "power_manager/powerd/policy/backlight_controller_observer.h"
#include "power_manager/powerd/policy/dark_resume_policy.h"
#include "power_manager/powerd/policy/input_controller.h"
#include "power_manager/powerd/system/audio_observer.h"
#include "power_manager/powerd/system/peripheral_battery_watcher.h"
#include "power_manager/powerd/system/power_supply.h"
#include "power_manager/powerd/system/power_supply_observer.h"

class MetricsLibrary;

namespace power_manager {

class DBusSender;
class MetricsReporter;
class Prefs;

namespace policy {
class BacklightController;
class KeyboardBacklightController;
class StateController;
class Suspender;
}  // namespace policy

namespace system {
class AmbientLightSensor;
class AudioClient;
class DisplayPowerSetter;
class Input;
class InternalBacklight;
class Udev;
}  // namespace system

class Daemon;

// Pointer to a member function for handling D-Bus method calls.
typedef dbus::Response* (Daemon::*DBusMethodCallMemberFunction)(
    dbus::MethodCall*);

// Main class within the powerd daemon that ties all other classes together.
class Daemon : public policy::BacklightControllerObserver,
               public policy::InputController::Delegate,
               public system::AudioObserver,
               public system::PowerSupplyObserver {
 public:
  Daemon(const base::FilePath& read_write_prefs_dir,
         const base::FilePath& read_only_prefs_dir,
         const base::FilePath& run_dir);
  virtual ~Daemon();

  void Init();

  // Overridden from policy::BacklightControllerObserver:
  virtual void OnBrightnessChanged(
      double brightness_percent,
      policy::BacklightController::BrightnessChangeCause cause,
      policy::BacklightController* source) OVERRIDE;

  // Called by |suspender_| before other processes are informed that the
  // system will be suspending soon.
  void PrepareForSuspendAnnouncement();

  // Called by |suspender_| if a suspend request is aborted before
  // PrepareForSuspend() has been called.
  void HandleCanceledSuspendAnnouncement();

  // Called by |suspender_| just before a suspend attempt begins.
  void PrepareForSuspend();

  // Called by |suspender_| after the completion of a suspend/resume cycle
  // (which did not necessarily succeed).
  void HandleResume(bool suspend_was_successful,
                    int num_suspend_retries,
                    int max_suspend_retries);

  // Overridden from policy::InputController::Delegate:
  virtual void HandleLidClosed() OVERRIDE;
  virtual void HandleLidOpened() OVERRIDE;
  virtual void HandlePowerButtonEvent(ButtonState state) OVERRIDE;
  virtual void DeferInactivityTimeoutForVT2() OVERRIDE;
  virtual void ShutDownForPowerButtonWithNoDisplay() OVERRIDE;
  virtual void HandleMissingPowerButtonAcknowledgment() OVERRIDE;

  // Overridden from system::AudioObserver:
  virtual void OnAudioStateChange(bool active) OVERRIDE;

  // Overridden from system::PowerSupplyObserver:
  virtual void OnPowerStatusUpdate() OVERRIDE;

 private:
  class StateControllerDelegate;
  class SuspenderDelegate;

  // Passed to ShutDown() to specify whether the system should power off or
  // reboot.
  enum ShutdownMode {
    SHUTDOWN_MODE_POWER_OFF,
    SHUTDOWN_MODE_REBOOT,
  };

  // Passed to ShutDown() to describe the reason the system is shutting down.
  enum ShutdownReason {
    // Explicit user request (e.g. holding power button).
    SHUTDOWN_REASON_USER_REQUEST,
    // Request from StateController (e.g. lid was closed or user was inactive).
    SHUTDOWN_REASON_STATE_TRANSITION,
    // Battery level dropped below shutdown threshold.
    SHUTDOWN_REASON_LOW_BATTERY,
    // Multiple suspend attempts failed.
    SHUTDOWN_REASON_SUSPEND_FAILED,
    // Battery level was below threshold during dark resume from suspend.
    SHUTDOWN_REASON_DARK_RESUME,
  };

  // Returns a string describing |reason|. The string is passed as a
  // SHUTDOWN_REASON argument to an initctl command to switch to runlevel 0
  // (i.e. don't change these strings without checking that other upstart jobs
  // aren't depending on them).
  static std::string ShutdownReasonToString(ShutdownReason reason);

  // Convenience method that returns true if |name| exists and is true.
  bool BoolPrefIsTrue(const std::string& name) const;

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

  // Handles changes to D-Bus name ownership.
  void HandleDBusNameOwnerChanged(dbus::Signal* signal);

  // Handles the result of an attempt to connect to a D-Bus signal. Logs an
  // error on failure.
  void HandleDBusSignalConnected(const std::string& interface,
                                 const std::string& signal,
                                 bool success);

  // Exports |method_name| and uses |member| to handle calls.
  void ExportDBusMethod(const std::string& method_name,
                        DBusMethodCallMemberFunction member);

  // Callbacks for handling D-Bus signals and method calls.
  void HandleSessionStateChangedSignal(dbus::Signal* signal);
  void HandleUpdateEngineStatusUpdateSignal(dbus::Signal* signal);
  void HandleCrasNodesChangedSignal(dbus::Signal* signal);
  void HandleCrasActiveOutputNodeChangedSignal(dbus::Signal* signal);
  void HandleCrasNumberOfActiveStreamsChanged(dbus::Signal* signal);
  dbus::Response* HandleRequestShutdownMethod(dbus::MethodCall* method_call);
  dbus::Response* HandleRequestRestartMethod(dbus::MethodCall* method_call);
  dbus::Response* HandleRequestSuspendMethod(dbus::MethodCall* method_call);
  dbus::Response* HandleDecreaseScreenBrightnessMethod(
      dbus::MethodCall* method_call);
  dbus::Response* HandleIncreaseScreenBrightnessMethod(
      dbus::MethodCall* method_call);
  dbus::Response* HandleGetScreenBrightnessMethod(
      dbus::MethodCall* method_call);
  dbus::Response* HandleSetScreenBrightnessMethod(
      dbus::MethodCall* method_call);
  dbus::Response* HandleDecreaseKeyboardBrightnessMethod(
      dbus::MethodCall* method_call);
  dbus::Response* HandleIncreaseKeyboardBrightnessMethod(
      dbus::MethodCall* method_call);
  dbus::Response* HandleGetPowerSupplyPropertiesMethod(
      dbus::MethodCall* method_call);
  dbus::Response* HandleVideoActivityMethod(dbus::MethodCall* method_call);
  dbus::Response* HandleUserActivityMethod(dbus::MethodCall* method_call);
  dbus::Response* HandleSetIsProjectingMethod(dbus::MethodCall* method_call);
  dbus::Response* HandleSetPolicyMethod(dbus::MethodCall* method_call);
  dbus::Response* HandlePowerButtonAcknowledgment(
      dbus::MethodCall* method_call);

  // Gets the current session state from the session manager, returning true on
  // success.
  bool QuerySessionState(std::string* state);

  // Handles information from the session manager about the session state.
  void OnSessionStateChange(const std::string& state_str);

  // Shuts the system down immediately.
  void ShutDown(ShutdownMode mode, ShutdownReason reason);

  // Starts the suspend process. If |use_external_wakeup_count| is true,
  // passes |external_wakeup_count| to
  // policy::Suspender::RequestSuspendWithExternalWakeupCount();
  void Suspend(bool use_external_wakeup_count, uint64 external_wakeup_count);

  // Updates state in |backlight_controller_| and |keyboard_controller_|
  // (if non-NULL).
  void SetBacklightsDimmedForInactivity(bool dimmed);
  void SetBacklightsOffForInactivity(bool off);
  void SetBacklightsSuspended(bool suspended);
  void SetBacklightsDocked(bool docked);

  scoped_ptr<Prefs> prefs_;

  scoped_refptr<dbus::Bus> bus_;
  dbus::ExportedObject* powerd_dbus_object_;  // weak; owned by |bus_|
  dbus::ObjectProxy* chrome_dbus_proxy_;  // weak; owned by |bus_|
  dbus::ObjectProxy* session_manager_dbus_proxy_;  // weak; owned by |bus_|
  dbus::ObjectProxy* cras_dbus_proxy_;  // weak; owned by |bus_|

  scoped_ptr<StateControllerDelegate> state_controller_delegate_;
  scoped_ptr<DBusSender> dbus_sender_;

  scoped_ptr<system::AmbientLightSensor> light_sensor_;
  scoped_ptr<system::DisplayPowerSetter> display_power_setter_;
  scoped_ptr<system::InternalBacklight> display_backlight_;
  scoped_ptr<policy::BacklightController> display_backlight_controller_;
  scoped_ptr<system::InternalBacklight> keyboard_backlight_;
  scoped_ptr<policy::KeyboardBacklightController>
      keyboard_backlight_controller_;

  scoped_ptr<system::Udev> udev_;
  scoped_ptr<system::Input> input_;
  scoped_ptr<policy::StateController> state_controller_;
  scoped_ptr<policy::InputController> input_controller_;
  scoped_ptr<system::AudioClient> audio_client_;
  scoped_ptr<system::PeripheralBatteryWatcher> peripheral_battery_watcher_;
  scoped_ptr<system::PowerSupply> power_supply_;
  scoped_ptr<policy::DarkResumePolicy> dark_resume_policy_;
  scoped_ptr<SuspenderDelegate> suspender_delegate_;
  scoped_ptr<policy::Suspender> suspender_;

  scoped_ptr<MetricsLibrary> metrics_library_;
  scoped_ptr<MetricsReporter> metrics_reporter_;

  // True once the shutdown process has started. Remains true until the
  // system has powered off.
  bool shutting_down_;

  base::FilePath run_dir_;

  // Last session state that we have been informed of. Initialized as stopped.
  SessionState session_state_;

  // Has |state_controller_| been initialized?  Daemon::Init() invokes a
  // bunch of event-handling functions directly, but events shouldn't be
  // passed to |state_controller_| until it's been initialized.
  bool state_controller_initialized_;

  // Set to true if powerd touched a file for crash-reporter before
  // suspending. If true, the file will be unlinked after resuming.
  bool created_suspended_state_file_;

  // True if VT switching should be disabled before the system is suspended.
  bool lock_vt_before_suspend_;

  // True if the "mosys" command should be used to record suspend and resume
  // timestamps in eventlog.
  bool log_suspend_with_mosys_eventlog_;

  DISALLOW_COPY_AND_ASSIGN(Daemon);
};

}  // namespace power_manager

#endif  // POWER_MANAGER_POWERD_DAEMON_H_
