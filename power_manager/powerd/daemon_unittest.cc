// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "power_manager/powerd/daemon.h"

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/files/scoped_temp_dir.h>
#include <base/macros.h>
#include <base/strings/stringprintf.h>
#include <chromeos/dbus/service_constants.h>
#include <dbus/exported_object.h>
#include <dbus/message.h>
#include <gtest/gtest.h>

#include "power_manager/common/fake_prefs.h"
#include "power_manager/common/metrics_sender_stub.h"
#include "power_manager/common/power_constants.h"
#include "power_manager/powerd/daemon_delegate.h"
#include "power_manager/powerd/policy/backlight_controller_stub.h"
#include "power_manager/powerd/system/acpi_wakeup_helper_stub.h"
#include "power_manager/powerd/system/ambient_light_sensor_stub.h"
#include "power_manager/powerd/system/audio_client_stub.h"
#include "power_manager/powerd/system/backlight_stub.h"
#include "power_manager/powerd/system/dark_resume_stub.h"
#include "power_manager/powerd/system/dbus_wrapper_stub.h"
#include "power_manager/powerd/system/display/display_power_setter_stub.h"
#include "power_manager/powerd/system/display/display_watcher_stub.h"
#include "power_manager/powerd/system/ec_wakeup_helper_stub.h"
#include "power_manager/powerd/system/input_watcher_stub.h"
#include "power_manager/powerd/system/peripheral_battery_watcher.h"
#include "power_manager/powerd/system/power_supply.h"
#include "power_manager/powerd/system/power_supply_stub.h"
#include "power_manager/powerd/system/udev_stub.h"
#include "power_manager/proto_bindings/power_supply_properties.pb.h"

namespace power_manager {
namespace {

// Transfers |src_response| to |dest_response|. Passed as a response callback to
// exported methods.
void MoveDBusResponse(std::unique_ptr<dbus::Response>* dest_response,
                      std::unique_ptr<dbus::Response> src_response) {
  *dest_response = std::move(src_response);
}

}  // namespace

class DaemonTest : public ::testing::Test, public DaemonDelegate {
 public:
  // The hardcoded constants here are arbitrary and not used by Daemon.
  DaemonTest()
      : passed_prefs_(new FakePrefs()),
        passed_dbus_wrapper_(new system::DBusWrapperStub()),
        passed_udev_(new system::UdevStub()),
        passed_ambient_light_sensor_(new system::AmbientLightSensorStub(0)),
        passed_display_watcher_(new system::DisplayWatcherStub()),
        passed_display_power_setter_(new system::DisplayPowerSetterStub()),
        passed_internal_backlight_(new system::BacklightStub(100, 100)),
        passed_keyboard_backlight_(new system::BacklightStub(100, 100)),
        passed_external_backlight_controller_(
            new policy::BacklightControllerStub()),
        passed_internal_backlight_controller_(
            new policy::BacklightControllerStub()),
        passed_keyboard_backlight_controller_(
            new policy::BacklightControllerStub()),
        passed_input_watcher_(new system::InputWatcherStub()),
        passed_acpi_wakeup_helper_(new system::AcpiWakeupHelperStub()),
        passed_ec_wakeup_helper_(new system::EcWakeupHelperStub()),
        passed_power_supply_(new system::PowerSupplyStub()),
        passed_dark_resume_(new system::DarkResumeStub()),
        passed_audio_client_(new system::AudioClientStub()),
        passed_metrics_sender_(new MetricsSenderStub()),
        prefs_(passed_prefs_.get()),
        dbus_wrapper_(passed_dbus_wrapper_.get()),
        udev_(passed_udev_.get()),
        ambient_light_sensor_(passed_ambient_light_sensor_.get()),
        display_watcher_(passed_display_watcher_.get()),
        display_power_setter_(passed_display_power_setter_.get()),
        internal_backlight_(passed_internal_backlight_.get()),
        keyboard_backlight_(passed_keyboard_backlight_.get()),
        external_backlight_controller_(
            passed_external_backlight_controller_.get()),
        internal_backlight_controller_(
            passed_internal_backlight_controller_.get()),
        keyboard_backlight_controller_(
            passed_keyboard_backlight_controller_.get()),
        input_watcher_(passed_input_watcher_.get()),
        acpi_wakeup_helper_(passed_acpi_wakeup_helper_.get()),
        ec_wakeup_helper_(passed_ec_wakeup_helper_.get()),
        power_supply_(passed_power_supply_.get()),
        dark_resume_(passed_dark_resume_.get()),
        audio_client_(passed_audio_client_.get()),
        metrics_sender_(passed_metrics_sender_.get()),
        pid_(2) {
    CHECK(run_dir_.CreateUniqueTempDir());
    CHECK(run_dir_.IsValid());

    CHECK(temp_dir_.CreateUniqueTempDir());
    CHECK(temp_dir_.IsValid());
    wakeup_count_path_ = temp_dir_.path().Append("wakeup_count");
    oobe_completed_path_ = temp_dir_.path().Append("oobe_completed");
    suspended_state_path_ = temp_dir_.path().Append("suspended_state");
    flashrom_lock_path_ = temp_dir_.path().Append("flashrom_lock");
    battery_tool_lock_path_ = temp_dir_.path().Append("battery_tool_lock");
    proc_path_ = temp_dir_.path().Append("proc");
  }
  ~DaemonTest() override {}

  void Init() {
    // These prefs are required by policy::Suspender.
    // TODO(derat): Remove these after adding a stub for Suspender.
    prefs_->SetInt64(kRetrySuspendMsPref, 10000);
    prefs_->SetInt64(kRetrySuspendAttemptsPref, 10);

    // These prefs are required by policy::StateController.
    // TODO(derat): Remove these after adding a stub for StateController.
    prefs_->SetInt64(kPluggedSuspendMsPref, 1800000);
    prefs_->SetInt64(kPluggedOffMsPref, 480000);
    prefs_->SetInt64(kPluggedDimMsPref, 420000);
    prefs_->SetInt64(kUnpluggedSuspendMsPref, 600000);
    prefs_->SetInt64(kUnpluggedOffMsPref, 360000);
    prefs_->SetInt64(kUnpluggedDimMsPref, 300000);

    daemon_.reset(new Daemon(this, run_dir_.path()));
    daemon_->set_wakeup_count_path_for_testing(wakeup_count_path_);
    daemon_->set_oobe_completed_path_for_testing(oobe_completed_path_);
    daemon_->set_suspended_state_path_for_testing(suspended_state_path_);
    daemon_->set_flashrom_lock_path_for_testing(flashrom_lock_path_);
    daemon_->set_battery_tool_lock_path_for_testing(battery_tool_lock_path_);
    daemon_->set_proc_path_for_testing(proc_path_);
    daemon_->Init();
  }

  // DaemonDelegate:
  std::unique_ptr<PrefsInterface> CreatePrefs() override {
    return std::move(passed_prefs_);
  }
  std::unique_ptr<system::DBusWrapperInterface> CreateDBusWrapper() override {
    return std::move(passed_dbus_wrapper_);
  }
  std::unique_ptr<system::UdevInterface> CreateUdev() override {
    return std::move(passed_udev_);
  }
  std::unique_ptr<system::AmbientLightSensorInterface>
  CreateAmbientLightSensor() override {
    return std::move(passed_ambient_light_sensor_);
  }
  std::unique_ptr<system::DisplayWatcherInterface> CreateDisplayWatcher(
      system::UdevInterface* udev) override {
    EXPECT_EQ(udev_, udev);
    return std::move(passed_display_watcher_);
  }
  std::unique_ptr<system::DisplayPowerSetterInterface> CreateDisplayPowerSetter(
      system::DBusWrapperInterface* dbus_wrapper) override {
    EXPECT_EQ(dbus_wrapper_, dbus_wrapper);
    return std::move(passed_display_power_setter_);
  }
  std::unique_ptr<policy::BacklightController>
  CreateExternalBacklightController(
      system::DisplayWatcherInterface* display_watcher,
      system::DisplayPowerSetterInterface* display_power_setter) override {
    EXPECT_EQ(display_watcher_, display_watcher);
    EXPECT_EQ(display_power_setter_, display_power_setter);
    return std::move(passed_external_backlight_controller_);
  }
  std::unique_ptr<system::BacklightInterface> CreateInternalBacklight(
      const base::FilePath& base_path,
      const base::FilePath::StringType& pattern) override {
    if (base_path == base::FilePath(kInternalBacklightPath) &&
        pattern == kInternalBacklightPattern) {
      return std::move(passed_internal_backlight_);
    } else if (base_path == base::FilePath(kKeyboardBacklightPath) &&
               pattern == kKeyboardBacklightPattern) {
      return std::move(passed_keyboard_backlight_);
    } else {
      LOG(FATAL) << "Invalid backlight path/pattern combination ("
                 << base_path.value() << ", " << pattern << ")";
      return std::unique_ptr<system::BacklightInterface>();
    }
  }
  std::unique_ptr<policy::BacklightController>
  CreateInternalBacklightController(
      system::BacklightInterface* backlight,
      PrefsInterface* prefs,
      system::AmbientLightSensorInterface* sensor,
      system::DisplayPowerSetterInterface* power_setter) override {
    EXPECT_EQ(internal_backlight_, backlight);
    EXPECT_EQ(prefs_, prefs);
    EXPECT_TRUE(!sensor || sensor == ambient_light_sensor_);
    EXPECT_EQ(display_power_setter_, power_setter);
    return std::move(passed_internal_backlight_controller_);
  }
  std::unique_ptr<policy::BacklightController>
  CreateKeyboardBacklightController(
      system::BacklightInterface* backlight,
      PrefsInterface* prefs,
      system::AmbientLightSensorInterface* sensor,
      policy::BacklightController* display_backlight_controller,
      TabletMode initial_tablet_mode) override {
    EXPECT_EQ(keyboard_backlight_, backlight);
    EXPECT_EQ(prefs_, prefs);
    EXPECT_TRUE(!sensor || sensor == ambient_light_sensor_);
    EXPECT_EQ(internal_backlight_controller_, display_backlight_controller);
    EXPECT_EQ(input_watcher_->GetTabletMode(), initial_tablet_mode);
    return std::move(passed_keyboard_backlight_controller_);
  }
  std::unique_ptr<system::InputWatcherInterface> CreateInputWatcher(
      PrefsInterface* prefs, system::UdevInterface* udev) override {
    EXPECT_EQ(prefs_, prefs);
    EXPECT_EQ(udev_, udev);
    return std::move(passed_input_watcher_);
  }
  std::unique_ptr<system::AcpiWakeupHelperInterface> CreateAcpiWakeupHelper()
      override {
    return std::move(passed_acpi_wakeup_helper_);
  }
  std::unique_ptr<system::EcWakeupHelperInterface> CreateEcWakeupHelper()
      override {
    return std::move(passed_ec_wakeup_helper_);
  }
  std::unique_ptr<system::PeripheralBatteryWatcher>
  CreatePeripheralBatteryWatcher(
      system::DBusWrapperInterface* dbus_wrapper) override {
    EXPECT_EQ(dbus_wrapper_, dbus_wrapper);
    return std::unique_ptr<system::PeripheralBatteryWatcher>();
  }
  std::unique_ptr<system::PowerSupplyInterface> CreatePowerSupply(
      const base::FilePath& power_supply_path,
      PrefsInterface* prefs,
      system::UdevInterface* udev) override {
    EXPECT_EQ(kPowerStatusPath, power_supply_path.value());
    EXPECT_EQ(prefs_, prefs);
    EXPECT_EQ(udev_, udev);
    return std::move(passed_power_supply_);
  }
  std::unique_ptr<system::DarkResumeInterface> CreateDarkResume(
      system::PowerSupplyInterface* power_supply,
      PrefsInterface* prefs) override {
    EXPECT_EQ(power_supply_, power_supply);
    EXPECT_EQ(prefs_, prefs);
    return std::move(passed_dark_resume_);
  }
  std::unique_ptr<system::AudioClientInterface> CreateAudioClient(
      system::DBusWrapperInterface* dbus_wrapper) override {
    EXPECT_EQ(dbus_wrapper_, dbus_wrapper);
    return std::move(passed_audio_client_);
  }
  std::unique_ptr<MetricsSenderInterface> CreateMetricsSender() override {
    return std::move(passed_metrics_sender_);
  }
  pid_t GetPid() override { return pid_; }
  void Launch(const std::string& command) override {
    async_commands_.push_back(command);
  }
  int Run(const std::string& command) override {
    sync_commands_.push_back(command);
    // TODO(derat): Support returning canned exit code to simulate failure.
    return 0;
  }

 protected:
  // Calls a synchronous D-Bus previously exported by |daemon_| and returns its
  // response.
  std::unique_ptr<dbus::Response> CallSyncDBusMethod(
      dbus::MethodCall* method_call) {
    std::unique_ptr<dbus::Response> response;
    dbus_wrapper_->CallExportedMethod(method_call,
                                      base::Bind(&MoveDBusResponse, &response));
    return response;
  }

  // Emits a signal announcing that the owner of a D-Bus service name has
  // changed.
  void EmitNameOwnerChangedSignal(const std::string& name,
                                  const std::string& old_owner,
                                  const std::string& new_owner) {
    dbus::Signal signal(kBusInterface, kBusNameOwnerChangedSignal);
    dbus::MessageWriter writer(&signal);
    writer.AppendString(name);
    writer.AppendString(old_owner);
    writer.AppendString(new_owner);
    dbus_wrapper_->EmitRegisteredSignal(
        dbus_wrapper_->GetObjectProxy(kBusServiceName, kBusServicePath),
        &signal);
  }

  // Checks that the D-Bus signal at index |0| has name |signal| and describes a
  // brightness change to |brightness_percent|.
  void CheckBrightnessChangedSignal(size_t index,
                                    const std::string& signal_name,
                                    double brightness_percent,
                                    bool user_initiated) {
    std::unique_ptr<dbus::Signal> signal;
    ASSERT_TRUE(
        dbus_wrapper_->GetSentSignal(index, signal_name, nullptr, &signal));

    dbus::MessageReader reader(signal.get());
    int32_t sent_brightness = 0;
    ASSERT_TRUE(reader.PopInt32(&sent_brightness));
    EXPECT_EQ(static_cast<int32_t>(round(brightness_percent)), sent_brightness);
    bool sent_user_initiated = false;
    ASSERT_TRUE(reader.PopBool(&sent_user_initiated));
    EXPECT_EQ(user_initiated, sent_user_initiated);
  }

  // Returns the command that Daemon should execute to shut down for a given
  // reason.
  std::string GetShutdownCommand(ShutdownReason reason) {
    return base::StringPrintf("%s --action=shut_down --shutdown_reason=%s",
                              kSetuidHelperPath,
                              ShutdownReasonToString(reason).c_str());
  }

  // Returns the command that Daemon should execute to set wifi transmit power
  // for |mode|.
  std::string GetWifiTransmitPowerCommand(TabletMode mode) {
    return base::StringPrintf(
        "%s --action=set_wifi_transmit_power --%swifi_transmit_power_tablet",
        kSetuidHelperPath,
        mode == TabletMode::ON ? "" : "no");
  }

  // Stub objects to be transferred by Create* methods.
  std::unique_ptr<FakePrefs> passed_prefs_;
  std::unique_ptr<system::DBusWrapperStub> passed_dbus_wrapper_;
  std::unique_ptr<system::UdevStub> passed_udev_;
  std::unique_ptr<system::AmbientLightSensorStub> passed_ambient_light_sensor_;
  std::unique_ptr<system::DisplayWatcherStub> passed_display_watcher_;
  std::unique_ptr<system::DisplayPowerSetterStub> passed_display_power_setter_;
  std::unique_ptr<system::BacklightStub> passed_internal_backlight_;
  std::unique_ptr<system::BacklightStub> passed_keyboard_backlight_;
  std::unique_ptr<policy::BacklightControllerStub>
      passed_external_backlight_controller_;
  std::unique_ptr<policy::BacklightControllerStub>
      passed_internal_backlight_controller_;
  std::unique_ptr<policy::BacklightControllerStub>
      passed_keyboard_backlight_controller_;
  std::unique_ptr<system::InputWatcherStub> passed_input_watcher_;
  std::unique_ptr<system::AcpiWakeupHelperStub> passed_acpi_wakeup_helper_;
  std::unique_ptr<system::EcWakeupHelperStub> passed_ec_wakeup_helper_;
  std::unique_ptr<system::PowerSupplyStub> passed_power_supply_;
  std::unique_ptr<system::DarkResumeStub> passed_dark_resume_;
  std::unique_ptr<system::AudioClientStub> passed_audio_client_;
  std::unique_ptr<MetricsSenderStub> passed_metrics_sender_;

  // Pointers to objects originally stored in |passed_*| members. These allow
  // continued access by tests even after the corresponding Create* method has
  // been called and ownership has been transferred to |daemon_|.
  FakePrefs* prefs_;
  system::DBusWrapperStub* dbus_wrapper_;
  system::UdevStub* udev_;
  system::AmbientLightSensorStub* ambient_light_sensor_;
  system::DisplayWatcherStub* display_watcher_;
  system::DisplayPowerSetterStub* display_power_setter_;
  system::BacklightStub* internal_backlight_;
  system::BacklightStub* keyboard_backlight_;
  policy::BacklightControllerStub* external_backlight_controller_;
  policy::BacklightControllerStub* internal_backlight_controller_;
  policy::BacklightControllerStub* keyboard_backlight_controller_;
  system::InputWatcherStub* input_watcher_;
  system::AcpiWakeupHelperStub* acpi_wakeup_helper_;
  system::EcWakeupHelperStub* ec_wakeup_helper_;
  system::PowerSupplyStub* power_supply_;
  system::DarkResumeStub* dark_resume_;
  system::AudioClientStub* audio_client_;
  MetricsSenderStub* metrics_sender_;

  // Run directory passed to |daemon_|.
  base::ScopedTempDir run_dir_;

  // Temp files passed to |daemon_|.
  base::ScopedTempDir temp_dir_;
  base::FilePath wakeup_count_path_;
  base::FilePath oobe_completed_path_;
  base::FilePath suspended_state_path_;
  base::FilePath flashrom_lock_path_;
  base::FilePath battery_tool_lock_path_;
  base::FilePath proc_path_;

  // Value to return from GetPid().
  pid_t pid_;

  // Command lines executed via Launch() and Run(), respectively.
  std::vector<std::string> async_commands_;
  std::vector<std::string> sync_commands_;

  std::unique_ptr<Daemon> daemon_;

 private:
  DISALLOW_COPY_AND_ASSIGN(DaemonTest);
};

TEST_F(DaemonTest, NotifyMembersAboutEvents) {
  prefs_->SetInt64(kHasKeyboardBacklightPref, 1);
  prefs_->SetInt64(kUseCrasPref, 1);

  Init();
  audio_client_->ResetStats();
  internal_backlight_controller_->ResetStats();
  keyboard_backlight_controller_->ResetStats();

  // TODO(derat): Create stubs for policy::StateController, policy::Suspender,
  // and policy::InputDeviceController and verify that they're notified too
  // (including for lid events).

  // Power button events.
  input_watcher_->NotifyObserversAboutPowerButtonEvent(ButtonState::DOWN);
  EXPECT_EQ(1, internal_backlight_controller_->power_button_presses());
  EXPECT_EQ(1, keyboard_backlight_controller_->power_button_presses());

  // Hover state changes.
  input_watcher_->NotifyObserversAboutHoverState(true);
  input_watcher_->NotifyObserversAboutHoverState(false);
  ASSERT_EQ(2, internal_backlight_controller_->hover_state_changes().size());
  EXPECT_TRUE(internal_backlight_controller_->hover_state_changes()[0]);
  EXPECT_FALSE(internal_backlight_controller_->hover_state_changes()[1]);
  ASSERT_EQ(2, keyboard_backlight_controller_->hover_state_changes().size());
  EXPECT_TRUE(keyboard_backlight_controller_->hover_state_changes()[0]);
  EXPECT_FALSE(keyboard_backlight_controller_->hover_state_changes()[1]);

  // Tablet mode changes.
  input_watcher_->set_tablet_mode(TabletMode::ON);
  input_watcher_->NotifyObserversAboutTabletMode();
  ASSERT_EQ(1, internal_backlight_controller_->tablet_mode_changes().size());
  EXPECT_EQ(TabletMode::ON,
            internal_backlight_controller_->tablet_mode_changes()[0]);
  ASSERT_EQ(1, keyboard_backlight_controller_->tablet_mode_changes().size());
  EXPECT_EQ(TabletMode::ON,
            keyboard_backlight_controller_->tablet_mode_changes()[0]);

  // Power source changes.
  system::PowerStatus status;
  status.line_power_on = true;
  power_supply_->set_status(status);
  power_supply_->NotifyObservers();
  ASSERT_EQ(1, internal_backlight_controller_->power_source_changes().size());
  EXPECT_EQ(PowerSource::AC,
            internal_backlight_controller_->power_source_changes()[0]);
  ASSERT_EQ(1, keyboard_backlight_controller_->power_source_changes().size());
  EXPECT_EQ(PowerSource::AC,
            keyboard_backlight_controller_->power_source_changes()[0]);

  // User activity reports.
  dbus::MethodCall user_call(kPowerManagerInterface, kHandleUserActivityMethod);
  dbus::MessageWriter(&user_call)
      .AppendInt32(USER_ACTIVITY_BRIGHTNESS_UP_KEY_PRESS);
  ASSERT_TRUE(CallSyncDBusMethod(&user_call).get());
  ASSERT_EQ(1, internal_backlight_controller_->user_activity_reports().size());
  EXPECT_EQ(USER_ACTIVITY_BRIGHTNESS_UP_KEY_PRESS,
            internal_backlight_controller_->user_activity_reports()[0]);
  ASSERT_EQ(1, keyboard_backlight_controller_->user_activity_reports().size());
  EXPECT_EQ(USER_ACTIVITY_BRIGHTNESS_UP_KEY_PRESS,
            keyboard_backlight_controller_->user_activity_reports()[0]);

  // Video activity reports.
  dbus::MethodCall video_call(kPowerManagerInterface,
                              kHandleVideoActivityMethod);
  dbus::MessageWriter(&video_call).AppendBool(true /* fullscreen */);
  ASSERT_TRUE(CallSyncDBusMethod(&video_call).get());
  ASSERT_EQ(1, internal_backlight_controller_->video_activity_reports().size());
  EXPECT_EQ(true, internal_backlight_controller_->video_activity_reports()[0]);
  ASSERT_EQ(1, keyboard_backlight_controller_->video_activity_reports().size());
  EXPECT_EQ(true, keyboard_backlight_controller_->video_activity_reports()[0]);

  // Display mode / projecting changes.
  dbus::MethodCall display_call(kPowerManagerInterface, kSetIsProjectingMethod);
  dbus::MessageWriter(&display_call).AppendBool(true /* is_projecting */);
  ASSERT_TRUE(CallSyncDBusMethod(&display_call).get());
  ASSERT_EQ(1, internal_backlight_controller_->display_mode_changes().size());
  EXPECT_EQ(DisplayMode::PRESENTATION,
            internal_backlight_controller_->display_mode_changes()[0]);
  ASSERT_EQ(1, keyboard_backlight_controller_->display_mode_changes().size());
  EXPECT_EQ(DisplayMode::PRESENTATION,
            keyboard_backlight_controller_->display_mode_changes()[0]);

  // Policy updates.
  dbus::MethodCall policy_call(kPowerManagerInterface, kSetPolicyMethod);
  PowerManagementPolicy policy;
  const char kPolicyReason[] = "foo";
  policy.set_reason(kPolicyReason);
  dbus::MessageWriter(&policy_call).AppendProtoAsArrayOfBytes(policy);
  ASSERT_TRUE(CallSyncDBusMethod(&policy_call).get());
  ASSERT_EQ(1, internal_backlight_controller_->policy_changes().size());
  EXPECT_EQ(kPolicyReason,
            internal_backlight_controller_->policy_changes()[0].reason());
  ASSERT_EQ(1, keyboard_backlight_controller_->policy_changes().size());
  EXPECT_EQ(kPolicyReason,
            keyboard_backlight_controller_->policy_changes()[0].reason());

  // Session state changes.
  dbus::Signal session_signal(login_manager::kSessionManagerInterface,
                              login_manager::kSessionStateChangedSignal);
  dbus::MessageWriter(&session_signal).AppendString("started");
  dbus_wrapper_->EmitRegisteredSignal(
      dbus_wrapper_->GetObjectProxy(login_manager::kSessionManagerServiceName,
                                    login_manager::kSessionManagerServicePath),
      &session_signal);
  ASSERT_EQ(1, internal_backlight_controller_->session_state_changes().size());
  EXPECT_EQ(SessionState::STARTED,
            internal_backlight_controller_->session_state_changes()[0]);
  ASSERT_EQ(1, keyboard_backlight_controller_->session_state_changes().size());
  EXPECT_EQ(SessionState::STARTED,
            keyboard_backlight_controller_->session_state_changes()[0]);

  // Chrome restarts.
  EmitNameOwnerChangedSignal(chromeos::kLibCrosServiceName, "old", "new");
  EmitNameOwnerChangedSignal(chromeos::kLibCrosServiceName, "new", "newer");
  EXPECT_EQ(2, internal_backlight_controller_->chrome_starts());
  EXPECT_EQ(2, keyboard_backlight_controller_->chrome_starts());

  // CRAS restarts and signals.
  EmitNameOwnerChangedSignal(cras::kCrasServiceName, "old", "new");
  dbus::ObjectProxy* cras_proxy = dbus_wrapper_->GetObjectProxy(
      cras::kCrasServiceName, cras::kCrasServicePath);
  dbus::Signal cras_nodes_signal(cras::kCrasControlInterface,
                                 cras::kNodesChanged);
  dbus_wrapper_->EmitRegisteredSignal(cras_proxy, &cras_nodes_signal);
  dbus::Signal cras_active_node_signal(cras::kCrasControlInterface,
                                       cras::kActiveOutputNodeChanged);
  dbus_wrapper_->EmitRegisteredSignal(cras_proxy, &cras_active_node_signal);
  dbus::Signal cras_streams_signal(cras::kCrasControlInterface,
                                   cras::kNumberOfActiveStreamsChanged);
  dbus_wrapper_->EmitRegisteredSignal(cras_proxy, &cras_streams_signal);
  EXPECT_EQ(1, audio_client_->initial_loads());
  EXPECT_EQ(2, audio_client_->device_updates());
  EXPECT_EQ(1, audio_client_->stream_updates());
}

TEST_F(DaemonTest, DontReportTabletModeChangeFromInit) {
  prefs_->SetInt64(kHasKeyboardBacklightPref, 1);
  input_watcher_->set_tablet_mode(TabletMode::ON);
  Init();

  // The initial tablet mode is already passed to
  // CreateKeyboardBacklightController(), so Init() shouldn't send an extra
  // notification about it changing.
  EXPECT_EQ(0, internal_backlight_controller_->tablet_mode_changes().size());
  EXPECT_EQ(0, keyboard_backlight_controller_->tablet_mode_changes().size());
}

TEST_F(DaemonTest, GetBacklightBrightness) {
  Init();
  const double kBrightnessPercent = 55.0;
  internal_backlight_controller_->set_percent(kBrightnessPercent);

  dbus::MethodCall method_call(kPowerManagerInterface,
                               kGetScreenBrightnessPercentMethod);
  auto response = CallSyncDBusMethod(&method_call);
  ASSERT_TRUE(response.get());
  dbus::MessageReader reader(response.get());
  double percent = 0;
  ASSERT_TRUE(reader.PopDouble(&percent));
  EXPECT_DOUBLE_EQ(kBrightnessPercent, percent);
}

TEST_F(DaemonTest, ChangeBacklightBrightness) {
  prefs_->SetInt64(kHasKeyboardBacklightPref, 1);
  Init();

  double kSetBrightnessPercent = 62.0;
  dbus::MethodCall set_call(kPowerManagerInterface,
                            kSetScreenBrightnessPercentMethod);
  dbus::MessageWriter set_writer(&set_call);
  set_writer.AppendDouble(kSetBrightnessPercent);
  set_writer.AppendInt32(kBrightnessTransitionGradual);
  ASSERT_TRUE(CallSyncDBusMethod(&set_call).get());
  EXPECT_DOUBLE_EQ(kSetBrightnessPercent,
                   internal_backlight_controller_->user_brightness_percent());

  dbus::MethodCall increase_call(kPowerManagerInterface,
                                 kIncreaseScreenBrightnessMethod);
  ASSERT_TRUE(CallSyncDBusMethod(&increase_call).get());
  ASSERT_TRUE(CallSyncDBusMethod(&increase_call).get());
  EXPECT_EQ(2, internal_backlight_controller_->num_user_brightness_increases());

  dbus::MethodCall decrease_call(kPowerManagerInterface,
                                 kDecreaseScreenBrightnessMethod);
  dbus::MessageWriter(&decrease_call).AppendBool(true);
  ASSERT_TRUE(CallSyncDBusMethod(&decrease_call).get());
  EXPECT_EQ(1, internal_backlight_controller_->num_user_brightness_decreases());

  dbus::MethodCall increase_key_call(kPowerManagerInterface,
                                     kIncreaseKeyboardBrightnessMethod);
  ASSERT_TRUE(CallSyncDBusMethod(&increase_key_call).get());
  ASSERT_TRUE(CallSyncDBusMethod(&increase_key_call).get());
  EXPECT_EQ(2, keyboard_backlight_controller_->num_user_brightness_increases());

  dbus::MethodCall decrease_key_call(kPowerManagerInterface,
                                     kDecreaseKeyboardBrightnessMethod);
  ASSERT_TRUE(CallSyncDBusMethod(&decrease_key_call).get());
  EXPECT_EQ(1, keyboard_backlight_controller_->num_user_brightness_decreases());
}

TEST_F(DaemonTest, ForceBacklightsOff) {
  prefs_->SetInt64(kHasKeyboardBacklightPref, 1);
  Init();

  // Tell Daemon to force the backlights off.
  dbus::MethodCall set_off_call(kPowerManagerInterface,
                                kSetBacklightsForcedOffMethod);
  dbus::MessageWriter(&set_off_call).AppendBool(true);
  ASSERT_TRUE(CallSyncDBusMethod(&set_off_call).get());
  EXPECT_TRUE(internal_backlight_controller_->forced_off());
  EXPECT_TRUE(keyboard_backlight_controller_->forced_off());

  dbus::MethodCall get_call(kPowerManagerInterface,
                            kGetBacklightsForcedOffMethod);
  auto response = CallSyncDBusMethod(&get_call);
  ASSERT_TRUE(response.get());
  bool forced_off = false;
  ASSERT_TRUE(dbus::MessageReader(response.get()).PopBool(&forced_off));
  EXPECT_TRUE(forced_off);

  // Now stop forcing them off.
  dbus::MethodCall set_on_call(kPowerManagerInterface,
                               kSetBacklightsForcedOffMethod);
  dbus::MessageWriter(&set_on_call).AppendBool(false);
  ASSERT_TRUE(CallSyncDBusMethod(&set_on_call).get());
  EXPECT_FALSE(internal_backlight_controller_->forced_off());
  EXPECT_FALSE(keyboard_backlight_controller_->forced_off());

  response = CallSyncDBusMethod(&get_call);
  ASSERT_TRUE(response.get());
  ASSERT_TRUE(dbus::MessageReader(response.get()).PopBool(&forced_off));
  EXPECT_FALSE(forced_off);
}

TEST_F(DaemonTest, EmitDBusSignalForBrightnessChange) {
  prefs_->SetInt64(kHasKeyboardBacklightPref, 1);
  Init();

  dbus_wrapper_->ClearSentSignals();
  internal_backlight_controller_->NotifyObservers(
      50.0, policy::BacklightController::BrightnessChangeCause::AUTOMATED);
  internal_backlight_controller_->NotifyObservers(
      25.0, policy::BacklightController::BrightnessChangeCause::USER_INITIATED);
  EXPECT_EQ(2, dbus_wrapper_->num_sent_signals());
  CheckBrightnessChangedSignal(0, kBrightnessChangedSignal, 50.0, false);
  CheckBrightnessChangedSignal(1, kBrightnessChangedSignal, 25.0, true);

  dbus_wrapper_->ClearSentSignals();
  keyboard_backlight_controller_->NotifyObservers(
      8.0, policy::BacklightController::BrightnessChangeCause::AUTOMATED);
  keyboard_backlight_controller_->NotifyObservers(
      4.0, policy::BacklightController::BrightnessChangeCause::USER_INITIATED);
  EXPECT_EQ(2, dbus_wrapper_->num_sent_signals());
  CheckBrightnessChangedSignal(0, kKeyboardBrightnessChangedSignal, 8.0, false);
  CheckBrightnessChangedSignal(1, kKeyboardBrightnessChangedSignal, 4.0, true);
}

TEST_F(DaemonTest, EmitDBusSignalForPowerStatusUpdate) {
  Init();

  system::PowerStatus status;
  status.external_power = PowerSupplyProperties_ExternalPower_AC;
  power_supply_->set_status(status);

  dbus_wrapper_->ClearSentSignals();
  power_supply_->NotifyObservers();
  EXPECT_EQ(1, dbus_wrapper_->num_sent_signals());
  PowerSupplyProperties proto;
  ASSERT_TRUE(
      dbus_wrapper_->GetSentSignal(0, kPowerSupplyPollSignal, &proto, nullptr));
  // Just check the field that we set; the code for copying PowerStatus structs
  // into PowerSupplyPropreties protos lives inside the PowerSupply class and is
  // tested there.
  EXPECT_EQ(status.external_power, proto.external_power());
}

TEST_F(DaemonTest, RequestShutdown) {
  prefs_->SetInt64(kHasKeyboardBacklightPref, 1);
  Init();

  async_commands_.clear();
  sync_commands_.clear();
  dbus::MethodCall method_call(kPowerManagerInterface, kRequestShutdownMethod);
  ASSERT_TRUE(CallSyncDBusMethod(&method_call).get());

  EXPECT_TRUE(internal_backlight_controller_->shutting_down());
  EXPECT_TRUE(keyboard_backlight_controller_->shutting_down());

  EXPECT_EQ(0, sync_commands_.size());
  ASSERT_EQ(1, async_commands_.size());
  EXPECT_EQ(GetShutdownCommand(ShutdownReason::USER_REQUEST),
            async_commands_[0]);

  // Sending another request shouldn't do anything.
  async_commands_.clear();
  ASSERT_TRUE(CallSyncDBusMethod(&method_call).get());
  EXPECT_EQ(0, async_commands_.size());
}

TEST_F(DaemonTest, RequestRestart) {
  Init();

  async_commands_.clear();
  dbus::MethodCall method_call(kPowerManagerInterface, kRequestRestartMethod);
  dbus::MessageWriter message_writer(&method_call);
  message_writer.AppendInt32(REQUEST_RESTART_FOR_UPDATE);
  ASSERT_TRUE(CallSyncDBusMethod(&method_call).get());

  ASSERT_EQ(1, async_commands_.size());
  EXPECT_EQ(base::StringPrintf("%s --action=reboot", kSetuidHelperPath),
            async_commands_[0]);
}

TEST_F(DaemonTest, ShutDownForLowBattery) {
  prefs_->SetInt64(kHasKeyboardBacklightPref, 1);
  Init();

  // We shouldn't shut down if the battery isn't below the threshold.
  async_commands_.clear();
  system::PowerStatus status;
  status.battery_is_present = true;
  status.battery_below_shutdown_threshold = false;
  power_supply_->set_status(status);
  power_supply_->NotifyObservers();
  EXPECT_EQ(0, async_commands_.size());

  // Now drop below the threshold.
  async_commands_.clear();
  status.battery_below_shutdown_threshold = true;
  power_supply_->set_status(status);
  power_supply_->NotifyObservers();

  // Keep the display backlight on so we can show a low-battery alert.
  EXPECT_FALSE(internal_backlight_controller_->shutting_down());
  EXPECT_TRUE(keyboard_backlight_controller_->shutting_down());

  ASSERT_EQ(1, async_commands_.size());
  EXPECT_EQ(GetShutdownCommand(ShutdownReason::LOW_BATTERY),
            async_commands_[0]);
}

TEST_F(DaemonTest, DeferShutdownWhileFlashromRunning) {
  Init();

  const std::string kFlashromPid = "123";
  ASSERT_EQ(
      kFlashromPid.size(),
      base::WriteFile(
          flashrom_lock_path_, kFlashromPid.c_str(), kFlashromPid.size()));
  const base::FilePath kFlashromPidDir(proc_path_.Append(kFlashromPid));
  ASSERT_TRUE(base::CreateDirectory(kFlashromPidDir));

  // The system should stay up if a lockfile exists for a currently-running
  // process.
  dbus::MethodCall method_call(kPowerManagerInterface, kRequestShutdownMethod);
  ASSERT_TRUE(CallSyncDBusMethod(&method_call).get());
  EXPECT_EQ(0, async_commands_.size());

  // It should still be up after the retry timer fires.
  ASSERT_TRUE(daemon_->TriggerRetryShutdownTimerForTesting());
  EXPECT_EQ(0, async_commands_.size());

  // Now delete the dir in /proc as if the process crashed without removing its
  // lockfile. The next time the timer fires, Daemon should start shutting down.
  ASSERT_TRUE(base::DeleteFile(kFlashromPidDir, true /* recursive */));
  ASSERT_TRUE(daemon_->TriggerRetryShutdownTimerForTesting());
  ASSERT_EQ(1, async_commands_.size());
  EXPECT_EQ(GetShutdownCommand(ShutdownReason::USER_REQUEST),
            async_commands_[0]);

  // The timer should've been stopped.
  EXPECT_FALSE(daemon_->TriggerRetryShutdownTimerForTesting());

  // TODO(derat): Also verify that we check battery_tool.
}

TEST_F(DaemonTest, SetWifiTransmitPower) {
  prefs_->SetInt64(kSetWifiTransmitPowerForTabletModePref, 1);
  input_watcher_->set_tablet_mode(TabletMode::ON);
  Init();
  ASSERT_EQ(1, async_commands_.size());
  EXPECT_EQ(GetWifiTransmitPowerCommand(TabletMode::ON), async_commands_[0]);
  async_commands_.clear();

  input_watcher_->set_tablet_mode(TabletMode::OFF);
  input_watcher_->NotifyObserversAboutTabletMode();
  ASSERT_EQ(1, async_commands_.size());
  EXPECT_EQ(GetWifiTransmitPowerCommand(TabletMode::OFF), async_commands_[0]);
}

// TODO(derat): More tests. Namely:
// - Registering for D-Bus service availability
// - PrepareToSuspend / UndoPrepareToSuspend
// - Creating and deleting suspend_announced file
// - Handling D-Bus RequestSuspend and SetPowerSource method calls
// - Reading wakeup_count
// - Fetching update state from update_engine
// - Fetching TPM counter status from cryptohome
// - Emitting IdleActionImminent and IdleActionDeferred D-Bus signals
// - Generating suspend IDs
// - Notifying policy::Suspender about services exiting
// - StateControllerDelegate implementation
// - Parsing IWL wifi transmit power pref
// - Probably other stuff :-/

}  // namespace power_manager
