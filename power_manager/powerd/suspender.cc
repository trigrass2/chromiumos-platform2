// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "power_manager/powerd/suspender.h"

#include <inttypes.h>

#include <algorithm>

#include "base/file_util.h"
#include "base/logging.h"
#include "base/string_number_conversions.h"
#include "base/string_util.h"
#include "base/stringprintf.h"
#include "chromeos/dbus/dbus.h"
#include "chromeos/dbus/service_constants.h"
#include "power_manager/common/dbus_sender.h"
#include "power_manager/common/power_constants.h"
#include "power_manager/common/prefs.h"
#include "power_manager/common/util.h"
#include "power_manager/common/util_dbus.h"
#include "power_manager/powerd/powerd.h"
#include "power_manager/powerd/suspend_delay_controller.h"
#include "power_manager/powerd/system/input.h"
#include "power_manager/suspend.pb.h"

namespace power_manager {

namespace {

const char kWakeupCountPath[] = "/sys/power/wakeup_count";

}  // namespace

// Real implementation of the Delegate interface.
class Suspender::RealDelegate : public Suspender::Delegate {
 public:
  RealDelegate(Daemon* daemon, system::Input* input)
      : daemon_(daemon),
        input_(input) {
  }

  virtual ~RealDelegate() {}

  // Delegate implementation:
  virtual bool IsLidClosed() OVERRIDE {
    LidState state = LID_OPEN;
    return input_->QueryLidState(&state) ? (state == LID_CLOSED) : false;
  }

  virtual bool GetWakeupCount(uint64* wakeup_count) OVERRIDE {
    DCHECK(wakeup_count);
    base::FilePath path(kWakeupCountPath);
    std::string buf;
    if (file_util::ReadFileToString(path, &buf)) {
      TrimWhitespaceASCII(buf, TRIM_TRAILING, &buf);
      if (base::StringToUint64(buf, wakeup_count))
        return true;

      LOG(ERROR) << "Could not parse wakeup count from \"" << buf << "\"";
    } else {
      LOG(ERROR) << "Could not read " << kWakeupCountPath;
    }
    return false;
  }

  virtual void PrepareForSuspendAnnouncement() OVERRIDE {
    daemon_->PrepareForSuspendAnnouncement();
  }

  virtual void HandleCanceledSuspendAnnouncement() OVERRIDE {
    daemon_->HandleCanceledSuspendAnnouncement();
    SendPowerStateChangedSignal("on");
  }

  virtual void PrepareForSuspend() {
    daemon_->PrepareForSuspend();
    SendPowerStateChangedSignal("mem");
  }

  virtual bool Suspend(uint64 wakeup_count,
                       bool wakeup_count_valid,
                       base::TimeDelta duration) OVERRIDE {
    std::string args;
    if (wakeup_count_valid) {
      args += StringPrintf(" --suspend_wakeup_count_valid"
                           " --suspend_wakeup_count %" PRIu64, wakeup_count);
    }
    if (duration != base::TimeDelta()) {
      args += StringPrintf(" --suspend_duration %" PRId64,
                           duration.InSeconds());
    }
    return util::RunSetuidHelper("suspend", args, true) == 0;
  }

  virtual void HandleResume(bool suspend_was_successful,
                            int num_suspend_retries,
                            int max_suspend_retries) OVERRIDE {
    SendPowerStateChangedSignal("on");
    daemon_->HandleResume(suspend_was_successful,
                          num_suspend_retries,
                          max_suspend_retries);
  }

  virtual void ShutdownForFailedSuspend() OVERRIDE {
    daemon_->ShutdownForFailedSuspend();
  }

  virtual void ShutdownForDarkResume() OVERRIDE {
    daemon_->OnRequestShutdown();
  }

 private:
  void SendPowerStateChangedSignal(const std::string& power_state) {
    chromeos::dbus::Proxy proxy(chromeos::dbus::GetSystemBusConnection(),
                                kPowerManagerServicePath,
                                kPowerManagerInterface);
    const char* state = power_state.c_str();
    DBusMessage* signal = dbus_message_new_signal(kPowerManagerServicePath,
                                                  kPowerManagerInterface,
                                                  kPowerStateChanged);
    CHECK(signal);
    dbus_message_append_args(signal,
                             DBUS_TYPE_STRING, &state,
                             DBUS_TYPE_INVALID);
    dbus_g_proxy_send(proxy.gproxy(), signal, NULL);
    dbus_message_unref(signal);
  }

  Daemon* daemon_;  // not owned
  system::Input* input_;  // not owned

  DISALLOW_COPY_AND_ASSIGN(RealDelegate);
};

Suspender::TestApi::TestApi(Suspender* suspender) : suspender_(suspender) {}

void Suspender::TestApi::SetCurrentWallTime(base::Time wall_time) {
  suspender_->current_wall_time_for_testing_ = wall_time;
}

bool Suspender::TestApi::TriggerRetryTimeout() {
  if (suspender_->retry_suspend_timeout_id_ == 0)
    return false;

  guint old_id = suspender_->retry_suspend_timeout_id_;
  if (!suspender_->RetrySuspend())
    util::RemoveTimeout(&old_id);
  return true;
}

// static
Suspender::Delegate* Suspender::CreateDefaultDelegate(Daemon* daemon,
                                                      system::Input* input) {
  return new RealDelegate(daemon, input);
}

Suspender::Suspender(Delegate* delegate,
                     DBusSenderInterface* dbus_sender,
                     policy::DarkResumePolicy* dark_resume_policy)
    : delegate_(delegate),
      dbus_sender_(dbus_sender),
      dark_resume_policy_(dark_resume_policy),
      suspend_delay_controller_(new SuspendDelayController(dbus_sender)),
      waiting_for_readiness_(false),
      suspend_id_(0),
      wakeup_count_(0),
      wakeup_count_valid_(false),
      max_retries_(0),
      num_retries_(0),
      retry_suspend_timeout_id_(0) {
  suspend_delay_controller_->AddObserver(this);
}

Suspender::~Suspender() {
  suspend_delay_controller_->RemoveObserver(this);
  util::RemoveTimeout(&retry_suspend_timeout_id_);
}

void Suspender::Init(PrefsInterface* prefs) {
  int64 retry_delay_ms = 0;
  CHECK(prefs->GetInt64(kRetrySuspendMsPref, &retry_delay_ms));
  retry_delay_ = base::TimeDelta::FromMilliseconds(retry_delay_ms);

  CHECK(prefs->GetInt64(kRetrySuspendAttemptsPref, &max_retries_));
}

void Suspender::RequestSuspend() {
  if (waiting_for_readiness_)
    return;

  waiting_for_readiness_ = true;
  util::RemoveTimeout(&retry_suspend_timeout_id_);
  wakeup_count_valid_ = delegate_->GetWakeupCount(&wakeup_count_);
  suspend_id_++;
  delegate_->PrepareForSuspendAnnouncement();
  suspend_delay_controller_->PrepareForSuspend(suspend_id_);
}

DBusMessage* Suspender::RegisterSuspendDelay(DBusMessage* message) {
  RegisterSuspendDelayRequest request;
  if (!util::ParseProtocolBufferFromDBusMessage(message, &request)) {
    LOG(ERROR) << "Unable to parse RegisterSuspendDelay request";
    return util::CreateDBusInvalidArgsErrorReply(message);
  }
  RegisterSuspendDelayReply reply_proto;
  suspend_delay_controller_->RegisterSuspendDelay(
      request, util::GetDBusSender(message), &reply_proto);
  return util::CreateDBusProtocolBufferReply(message, reply_proto);
}

DBusMessage* Suspender::UnregisterSuspendDelay(DBusMessage* message) {
  UnregisterSuspendDelayRequest request;
  if (!util::ParseProtocolBufferFromDBusMessage(message, &request)) {
    LOG(ERROR) << "Unable to parse UnregisterSuspendDelay request";
    return util::CreateDBusInvalidArgsErrorReply(message);
  }
  suspend_delay_controller_->UnregisterSuspendDelay(
      request, util::GetDBusSender(message));
  return NULL;
}

DBusMessage* Suspender::HandleSuspendReadiness(DBusMessage* message) {
  SuspendReadinessInfo info;
  if (!util::ParseProtocolBufferFromDBusMessage(message, &info)) {
    LOG(ERROR) << "Unable to parse HandleSuspendReadiness request";
    return util::CreateDBusInvalidArgsErrorReply(message);
  }
  suspend_delay_controller_->HandleSuspendReadiness(
      info, util::GetDBusSender(message));
  return NULL;
}

void Suspender::HandleLidOpened() {
  CancelSuspend();
}

void Suspender::HandleUserActivity() {
  // Avoid canceling suspend for errant touchpad, power button, etc. events
  // that can be generated by closing the lid.
  if (!delegate_->IsLidClosed())
    CancelSuspend();
}

void Suspender::HandleShutdown() {
  CancelSuspend();
}

void Suspender::HandleDBusNameOwnerChanged(const std::string& name,
                                           const std::string& old_owner,
                                           const std::string& new_owner) {
  if (new_owner.empty())
    suspend_delay_controller_->HandleDBusClientDisconnected(name);
}

void Suspender::OnReadyForSuspend(int suspend_id) {
  if (waiting_for_readiness_ && suspend_id == suspend_id_) {
    LOG(INFO) << "Ready to suspend";
    waiting_for_readiness_ = false;
    Suspend();
  }
}

base::Time Suspender::GetCurrentWallTime() const {
  return !current_wall_time_for_testing_.is_null() ?
      current_wall_time_for_testing_ : base::Time::Now();
}

void Suspender::Suspend() {
  bool dark_resume = false;
  bool success = false;
  // Note: If this log message is changed, the power_AudioDetector test
  // must be updated.
  LOG(INFO) << "Starting suspend";
  SendSuspendStateChangedSignal(
      SuspendState_Type_SUSPEND_TO_MEMORY, GetCurrentWallTime());
  delegate_->PrepareForSuspend();

  do {
    base::TimeDelta suspend_duration;
    policy::DarkResumePolicy::Action action = dark_resume_policy_->GetAction();
    switch (action) {
      case policy::DarkResumePolicy::SHUT_DOWN:
        LOG(INFO) << "Shutting down from dark resume";
        delegate_->ShutdownForDarkResume();
        return;
      case policy::DarkResumePolicy::SUSPEND_FOR_DURATION:
        suspend_duration = dark_resume_policy_->GetSuspendDuration();
        LOG(INFO) << "Suspending for " << suspend_duration.InSeconds()
                  << " seconds";
        break;
      case policy::DarkResumePolicy::SUSPEND_INDEFINITELY:
        suspend_duration = base::TimeDelta();
        break;
      default:
        NOTREACHED() << "Unhandled dark resume action " << action;
    }

    // We don't want to use the wakeup_count in the case of a dark resume. The
    // kernel may not have initialized some of the devices to make the dark
    // resume as inconspicuous as possible, so allowing the user to use the
    // system in this state would be bad.
    success = delegate_->Suspend(wakeup_count_,
                                 wakeup_count_valid_ && !dark_resume,
                                 suspend_duration);
    dark_resume = dark_resume_policy_->IsDarkResume();
    // Failure handling for dark resume. We don't want to process events during
    // a dark resume, even if we fail to suspend. To solve this, instead of
    // scheduling a retry later, delay here and retry without returning from
    // this function. num_retries_ is not reset until there is a successful user
    // requested resume.
    if (!success && dark_resume) {
      if (num_retries_ >= max_retries_) {
        LOG(ERROR) << "Retried suspend from dark resume" << num_retries_
                   << " times; shutting down";
        delegate_->ShutdownForFailedSuspend();
      }
      num_retries_++;
      LOG(WARNING) << "Retry #" << num_retries_
                   << " for suspend from dark resume";
      sleep(retry_delay_.InSeconds());
    }
  } while (dark_resume);

  if (success) {
    LOG(INFO) << "Resumed successfully from suspend attempt " << suspend_id_;
    num_retries_ = 0;
    SendSuspendStateChangedSignal(
        SuspendState_Type_RESUME, GetCurrentWallTime());
  } else {
    LOG(INFO) << "Suspend attempt " << suspend_id_ << " failed; "
              << "will retry in " << retry_delay_.InMilliseconds() << " ms";
    DCHECK(!retry_suspend_timeout_id_);
    retry_suspend_timeout_id_ =
        g_timeout_add(retry_delay_.InMilliseconds(), RetrySuspendThunk, this);
  }
  dark_resume_policy_->HandleResume();
  delegate_->HandleResume(success, num_retries_, max_retries_);
}

gboolean Suspender::RetrySuspend() {
  retry_suspend_timeout_id_ = 0;

  if (num_retries_ >= max_retries_) {
    LOG(ERROR) << "Retried suspend " << num_retries_ << " times; shutting down";
    delegate_->ShutdownForFailedSuspend();
  } else {
    num_retries_++;
    LOG(WARNING) << "Retry #" << num_retries_;
    RequestSuspend();
  }
  return FALSE;
}

void Suspender::CancelSuspend() {
  if (waiting_for_readiness_) {
    LOG(INFO) << "Canceling suspend before running powerd_suspend";
    waiting_for_readiness_ = false;
    DCHECK(!retry_suspend_timeout_id_);
    delegate_->HandleCanceledSuspendAnnouncement();
  } else if (retry_suspend_timeout_id_) {
    LOG(INFO) << "Canceling suspend between retries";
    util::RemoveTimeout(&retry_suspend_timeout_id_);
  }
}

void Suspender::SendSuspendStateChangedSignal(SuspendState_Type type,
                                              const base::Time& wall_time) {
  SuspendState proto;
  proto.set_type(type);
  proto.set_wall_time(wall_time.ToInternalValue());
  dbus_sender_->EmitSignalWithProtocolBuffer(kSuspendStateChangedSignal, proto);
}

}  // namespace power_manager
