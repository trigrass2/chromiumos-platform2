// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdlib>
#include <memory>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include <base/at_exit.h>
#include <base/command_line.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/memory/ptr_util.h>
#include <base/message_loop/message_loop.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <brillo/flag_helper.h>
#include <metrics/metrics_library.h>

#include "power_manager/common/metrics_sender.h"
#include "power_manager/common/prefs.h"
#include "power_manager/common/util.h"
#include "power_manager/powerd/daemon.h"
#include "power_manager/powerd/daemon_delegate.h"
#include "power_manager/powerd/policy/external_backlight_controller.h"
#include "power_manager/powerd/policy/internal_backlight_controller.h"
#include "power_manager/powerd/policy/keyboard_backlight_controller.h"
#include "power_manager/powerd/system/acpi_wakeup_helper.h"
#include "power_manager/powerd/system/ambient_light_sensor.h"
#include "power_manager/powerd/system/audio_client.h"
#include "power_manager/powerd/system/dark_resume.h"
#include "power_manager/powerd/system/dbus_wrapper.h"
#include "power_manager/powerd/system/display/display_power_setter.h"
#include "power_manager/powerd/system/display/display_watcher.h"
#include "power_manager/powerd/system/ec_wakeup_helper.h"
#include "power_manager/powerd/system/event_device.h"
#include "power_manager/powerd/system/input_watcher.h"
#include "power_manager/powerd/system/internal_backlight.h"
#include "power_manager/powerd/system/peripheral_battery_watcher.h"
#include "power_manager/powerd/system/power_supply.h"
#include "power_manager/powerd/system/udev.h"

#ifndef VCSID
#define VCSID "<not set>"
#endif

namespace {

// Moves |latest_log_symlink| to |previous_log_symlink| and creates a relative
// symlink at |latest_log_symlink| pointing to |log_file|. All files must be in
// the same directory.
void UpdateLogSymlinks(const base::FilePath& latest_log_symlink,
                       const base::FilePath& previous_log_symlink,
                       const base::FilePath& log_file) {
  CHECK_EQ(latest_log_symlink.DirName().value(), log_file.DirName().value());
  base::DeleteFile(previous_log_symlink, false);
  base::Move(latest_log_symlink, previous_log_symlink);
  if (!base::CreateSymbolicLink(log_file.BaseName(), latest_log_symlink)) {
    LOG(ERROR) << "Unable to create symbolic link from "
               << latest_log_symlink.value() << " to " << log_file.value();
  }
}

std::string GetTimeAsString(time_t utime) {
  struct tm tm;
  CHECK_EQ(localtime_r(&utime, &tm), &tm);
  char str[16];
  CHECK_EQ(strftime(str, sizeof(str), "%Y%m%d-%H%M%S", &tm), 15UL);
  return std::string(str);
}

}  // namespace

namespace power_manager {

class DaemonDelegateImpl : public DaemonDelegate {
 public:
  DaemonDelegateImpl(const base::FilePath& read_write_prefs_dir,
                     const base::FilePath& read_only_prefs_dir)
      : read_write_prefs_dir_(read_write_prefs_dir),
        read_only_prefs_dir_(read_only_prefs_dir) {}
  ~DaemonDelegateImpl() override {}

  // DaemonDelegate:
  std::unique_ptr<PrefsInterface> CreatePrefs() override {
    auto prefs = base::WrapUnique(new Prefs());
    CHECK(prefs->Init(
        util::GetPrefPaths(read_write_prefs_dir_, read_only_prefs_dir_)));
    return std::move(prefs);
  }

  std::unique_ptr<system::DBusWrapperInterface> CreateDBusWrapper() override {
    auto wrapper = base::WrapUnique(new system::DBusWrapper());
    CHECK(wrapper->Init());
    return std::move(wrapper);
  }

  std::unique_ptr<system::UdevInterface> CreateUdev() override {
    auto udev = base::WrapUnique(new system::Udev());
    CHECK(udev->Init());
    return std::move(udev);
  }

  std::unique_ptr<system::AmbientLightSensorInterface>
  CreateAmbientLightSensor() override {
    auto sensor = base::WrapUnique(new system::AmbientLightSensor());
    sensor->Init();
    return std::move(sensor);
  }

  std::unique_ptr<system::DisplayWatcherInterface> CreateDisplayWatcher(
      system::UdevInterface* udev) override {
    auto watcher = base::WrapUnique(new system::DisplayWatcher());
    watcher->Init(udev);
    return std::move(watcher);
  }

  std::unique_ptr<system::DisplayPowerSetterInterface> CreateDisplayPowerSetter(
      system::DBusWrapperInterface* dbus_wrapper) override {
    auto setter = base::WrapUnique(new system::DisplayPowerSetter());
    setter->Init(dbus_wrapper);
    return std::move(setter);
  }

  std::unique_ptr<policy::BacklightController>
  CreateExternalBacklightController(
      system::DisplayWatcherInterface* display_watcher,
      system::DisplayPowerSetterInterface* display_power_setter) override {
    auto controller =
        base::WrapUnique(new policy::ExternalBacklightController());
    controller->Init(display_watcher, display_power_setter);
    return std::move(controller);
  }

  std::unique_ptr<system::BacklightInterface> CreateInternalBacklight(
      const base::FilePath& base_path,
      const base::FilePath::StringType& pattern) override {
    auto backlight = base::WrapUnique(new system::InternalBacklight());
    if (!backlight->Init(base_path, pattern)) {
      LOG(ERROR) << "Cannot initialize display backlight";
      return std::unique_ptr<system::BacklightInterface>();
    }
    return std::move(backlight);
  }

  std::unique_ptr<policy::BacklightController>
  CreateInternalBacklightController(
      system::BacklightInterface* backlight,
      PrefsInterface* prefs,
      system::AmbientLightSensorInterface* sensor,
      system::DisplayPowerSetterInterface* power_setter) override {
    auto controller =
        base::WrapUnique(new policy::InternalBacklightController());
    controller->Init(backlight, prefs, sensor, power_setter);
    return std::move(controller);
  }

  std::unique_ptr<policy::BacklightController>
  CreateKeyboardBacklightController(
      system::BacklightInterface* backlight,
      PrefsInterface* prefs,
      system::AmbientLightSensorInterface* sensor,
      policy::BacklightController* display_backlight_controller,
      TabletMode initial_tablet_mode) override {
    auto controller =
        base::WrapUnique(new policy::KeyboardBacklightController());
    controller->Init(backlight,
                     prefs,
                     sensor,
                     display_backlight_controller,
                     initial_tablet_mode);
    return std::move(controller);
  }

  std::unique_ptr<system::InputWatcherInterface> CreateInputWatcher(
      PrefsInterface* prefs, system::UdevInterface* udev) override {
    auto watcher = base::WrapUnique(new system::InputWatcher());
    CHECK(watcher->Init(std::unique_ptr<system::EventDeviceFactoryInterface>(
                            new system::EventDeviceFactory),
                        prefs,
                        udev));
    return std::move(watcher);
  }

  std::unique_ptr<system::AcpiWakeupHelperInterface> CreateAcpiWakeupHelper()
      override {
    return base::WrapUnique(new system::AcpiWakeupHelper());
  }

  std::unique_ptr<system::EcWakeupHelperInterface> CreateEcWakeupHelper()
      override {
    return base::WrapUnique(new system::EcWakeupHelper());
  }

  std::unique_ptr<system::PeripheralBatteryWatcher>
  CreatePeripheralBatteryWatcher(
      system::DBusWrapperInterface* dbus_wrapper) override {
    auto watcher = base::WrapUnique(new system::PeripheralBatteryWatcher());
    watcher->Init(dbus_wrapper);
    return watcher;
  }

  std::unique_ptr<system::PowerSupplyInterface> CreatePowerSupply(
      const base::FilePath& power_supply_path,
      PrefsInterface* prefs,
      system::UdevInterface* udev) override {
    auto supply = base::WrapUnique(new system::PowerSupply());
    supply->Init(
        power_supply_path, prefs, udev, true /* log_shutdown_thresholds */);
    return std::move(supply);
  }

  std::unique_ptr<system::DarkResumeInterface> CreateDarkResume(
      system::PowerSupplyInterface* power_supply,
      PrefsInterface* prefs) override {
    auto dark_resume = base::WrapUnique(new system::DarkResume());
    dark_resume->Init(power_supply, prefs);
    return std::move(dark_resume);
  }

  std::unique_ptr<system::AudioClientInterface> CreateAudioClient(
      system::DBusWrapperInterface* dbus_wrapper) override {
    auto client = base::WrapUnique(new system::AudioClient());
    client->Init(dbus_wrapper);
    return std::move(client);
  }

  std::unique_ptr<MetricsSenderInterface> CreateMetricsSender() override {
    std::unique_ptr<MetricsLibrary> metrics_lib(new MetricsLibrary());
    metrics_lib->Init();
    return base::WrapUnique(new MetricsSender(std::move(metrics_lib)));
  }

  pid_t GetPid() override { return getpid(); }

  void Launch(const std::string& command) override {
    LOG(INFO) << "Launching \"" << command << "\"";
    pid_t pid = fork();
    if (pid == 0) {
      // TODO(derat): Is this setsid() call necessary?
      setsid();
      // fork() again and exit so that init becomes the command's parent and
      // cleans up when it finally finishes.
      exit(fork() == 0 ? ::system(command.c_str()) : 0);
    } else if (pid > 0) {
      // powerd cleans up after the originally-forked process, which exits
      // immediately after forking again.
      if (waitpid(pid, NULL, 0) == -1)
        PLOG(ERROR) << "waitpid() on PID " << pid << " failed";
    } else if (pid == -1) {
      PLOG(ERROR) << "fork() failed";
    }
  }

  int Run(const std::string& command) override {
    LOG(INFO) << "Running \"" << command << "\"";
    int return_value = ::system(command.c_str());
    if (return_value == -1) {
      PLOG(ERROR) << "fork() failed";
    } else if (return_value) {
      return_value = WEXITSTATUS(return_value);
      LOG(ERROR) << "Command failed with exit status " << return_value;
    }
    return return_value;
  }

 private:
  base::FilePath read_write_prefs_dir_;
  base::FilePath read_only_prefs_dir_;

  DISALLOW_COPY_AND_ASSIGN(DaemonDelegateImpl);
};

}  // namespace power_manager

int main(int argc, char* argv[]) {
  DEFINE_string(prefs_dir,
                power_manager::kReadWritePrefsDir,
                "Directory holding read/write preferences.");
  DEFINE_string(default_prefs_dir,
                power_manager::kReadOnlyPrefsDir,
                "Directory holding read-only default settings.");
  DEFINE_string(log_dir, "", "Directory where logs are written.");
  DEFINE_string(run_dir, "", "Directory where stateful data is written.");
  // This flag is handled by libbase/libchrome's logging library instead of
  // directly by powerd, but it is defined here so FlagHelper won't abort after
  // seeing an unexpected flag.
  DEFINE_string(
      vmodule, "", "Per-module verbose logging levels, e.g. \"foo=1,bar=2\"");

  brillo::FlagHelper::Init(
      argc, argv, "powerd, the Chromium OS userspace power manager.");

  CHECK(!FLAGS_prefs_dir.empty()) << "--prefs_dir is required";
  CHECK(!FLAGS_log_dir.empty()) << "--log_dir is required";
  CHECK(!FLAGS_run_dir.empty()) << "--run_dir is required";

  const base::FilePath log_file =
      base::FilePath(FLAGS_log_dir)
          .Append(base::StringPrintf("powerd.%s",
                                     GetTimeAsString(::time(NULL)).c_str()));
  UpdateLogSymlinks(base::FilePath(FLAGS_log_dir).Append("powerd.LATEST"),
                    base::FilePath(FLAGS_log_dir).Append("powerd.PREVIOUS"),
                    log_file);

  logging::LoggingSettings logging_settings;
  logging_settings.logging_dest = logging::LOG_TO_FILE;
  logging_settings.log_file = log_file.value().c_str();
  logging_settings.lock_log = logging::DONT_LOCK_LOG_FILE;
  logging::InitLogging(logging_settings);
  LOG(INFO) << "vcsid " << VCSID;

  base::AtExitManager at_exit_manager;
  base::MessageLoopForIO message_loop;

  // Extra parens to avoid http://en.wikipedia.org/wiki/Most_vexing_parse.
  power_manager::DaemonDelegateImpl delegate(
      (base::FilePath(FLAGS_prefs_dir)),
      (base::FilePath(FLAGS_default_prefs_dir)));
  power_manager::Daemon daemon(&delegate, (base::FilePath(FLAGS_run_dir)));
  daemon.Init();

  message_loop.Run();
  return 0;
}
