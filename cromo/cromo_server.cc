// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cromo_server.h"

#include <chromeos/dbus/service_constants.h>

#include <base/logging.h>
#include <dbus-c++/glib-integration.h>
#include <dbus/dbus.h>
#include <mm/mm-modem.h>

#include "carrier.h"
#include "modem_handler.h"
#include "plugin_manager.h"
#include "syslog_helper.h"

using std::vector;

const char* CromoServer::kServiceName = "org.chromium.ModemManager";
const char* CromoServer::kServicePath = "/org/chromium/ModemManager";

static const char* kDBusInterface = "org.freedesktop.DBus";
static const char* kDBusPath = "/org/freedesktop/DBus";
static const char* kDBusListNames = "ListNames";
static const char* kDBusInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

// Returns the current time, in milliseconds, from an unspecified epoch.
// TODO(ellyjones): duplicated in some plugins. Figure out how to refactor that.
static unsigned long long time_ms(void) {
  struct timespec ts;
  unsigned long long rv;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  rv = ts.tv_sec;
  rv *= 1000;
  rv += ts.tv_nsec / 1000000;
  return rv;
}

CromoServer::CromoServer(DBus::Connection& connection)
    : DBus::ObjectAdaptor(connection, kServicePath),
      conn_(connection),
      powerd_up_(false),
      max_suspend_delay_(0),
      suspend_nonce_(0),
      suspend_completion_timeout_(0),
      metrics_lib_(new MetricsLibrary()) {
  metrics_lib_->Init();
}

CromoServer::~CromoServer() {
  CancelSuspendCompletionTimeout();

  for (ModemHandlers::iterator it = modem_handlers_.begin();
       it != modem_handlers_.end(); ++it) {
    delete *it;
  }
  modem_handlers_.clear();

  for (CarrierMap::iterator it = carriers_.begin();
       it != carriers_.end(); ++it) {
    delete it->second;
  }
  carriers_.clear();
}

vector<DBus::Path> CromoServer::EnumerateDevices(DBus::Error& error) {
  vector<DBus::Path> allpaths;

  for (vector<ModemHandler*>::iterator it = modem_handlers_.begin();
       it != modem_handlers_.end(); it++) {
    vector<DBus::Path> paths = (*it)->EnumerateDevices(error);
    allpaths.insert(allpaths.end(), paths.begin(), paths.end());
  }
  return allpaths;
}

void CromoServer::SetLogging(const std::string& level, DBus::Error& error) {
  if (SysLogHelperSetLevel(level)) {
    std::string msg(std::string("Invalid Logging Level: ") + level);
    LOG(ERROR) << msg;
    error.set(kDBusInvalidArgs, msg.c_str());
  }

  return;
}

void CromoServer::AddModemHandler(ModemHandler* handler) {
  LOG(INFO) << "AddModemHandler(" << handler->vendor_tag() << ")";
  modem_handlers_.push_back(handler);
}

void CromoServer::PowerDaemonUp() {
  LOG(INFO) << "Power daemon: up";
  if (!powerd_up_) {
    powerd_up_ = true;
    RegisterSuspendDelay();
  }
}

void CromoServer::PowerDaemonDown() {
  LOG(INFO) << "Power daemon: down";
  if (powerd_up_) {
    powerd_up_ = false;
  }
}

void CromoServer::CheckForPowerDaemon() {
  DBus::CallMessage msg;
  DBus::MessageIter iter;

  LOG(INFO) << "Checking for power daemon...";
  msg.destination(kDBusInterface);
  msg.interface(kDBusInterface);
  msg.member(kDBusListNames);
  msg.path(kDBusPath);
  // In RegisterSuspendDelayCallback, we catch any exceptions send_blocking()
  // throws, since that is how it announces that the target of your message is
  // gone. Here the target is the dbus bus itself - if this fails, we're
  // completely hosed and should just abort, which the exception will do for us.
  DBus::Message ret = conn_.send_blocking(msg, -1);
  iter = ret.reader();
  iter = iter.recurse();
  while (!iter.at_end()) {
    if (!powerd_up_ &&
        strcmp(iter.get_string(), power_manager::kPowerManagerInterface) == 0) {
      PowerDaemonUp();
    }
    iter++;
  }
}

gboolean AssumeSuspendCancelled(void *arg) {
  LOG(INFO) << "Assume suspend cancelled";
  CromoServer *srv = static_cast<CromoServer*>(arg);
  srv->PowerStateChanged("on");
  return FALSE;
}

void CromoServer::SuspendReady() {
  unsigned long duration = time_ms() - suspend_start_time_;
  metrics_lib_->SendToUMA("Network.3G.SuspendTime", duration, 0, 10000, 20);
  DBus::SignalMessage msg("/", power_manager::kPowerManagerInterface,
                          power_manager::kSuspendReady);
  LOG(INFO) << "SuspendReady: " << suspend_nonce_;
  msg.destination(power_manager::kPowerManagerInterface);
  msg.writer().append_uint32(suspend_nonce_);
  conn_.send(msg, NULL);

  // HACK: The suspend request may be cancelled but power manager does not
  // notify such event (crosbug.com/33852). As a workaround, if cromo does not
  // receive a PowerStateChanged("mem") signal within 5 seconds, it assumes the
  // suspend request is cancelled.
  CancelSuspendCompletionTimeout();
  LOG(INFO) << "Schedule a suspend completion timeout";
  suspend_completion_timeout_ =
      g_timeout_add_seconds(5, AssumeSuspendCancelled, this);
}

bool CromoServer::CheckSuspendReady() {
  bool okay_to_suspend = suspend_ok_hooks_.Run();
  if (okay_to_suspend) {
    SuspendReady();
  }
  // We return whether we need to run again. We only run again if the suspend
  // wasn't ready.
  return okay_to_suspend;
}

gboolean test_for_suspend(void *arg) {
  CromoServer *srv = static_cast<CromoServer*>(arg);
  return !srv->CheckSuspendReady();
}

void CromoServer::PowerStateChanged(const std::string& new_power_state) {
  LOG(INFO) << "PowerStateChanged: " << new_power_state;
  if (new_power_state == "mem") {
    CancelSuspendCompletionTimeout();
    on_suspended_hooks_.Run();
  } else if (new_power_state == "on") {
    on_resumed_hooks_.Run();
  }
}

void CromoServer::SuspendDelay(unsigned int nonce) {
  LOG(INFO) << "SuspendDelay: " << nonce;
  suspend_nonce_ = nonce;
  suspend_start_time_ = time_ms();
  start_suspend_hooks_.Run();
  if (CheckSuspendReady()) {
    return;
  }
  g_timeout_add_seconds(1, test_for_suspend, static_cast<void*>(this));
}

void CromoServer::RegisterStartSuspend(const std::string& name,
                                       bool (*func)(void *), void *arg,
                                       unsigned int maxdelay) {
  suspend_delays_[name] = maxdelay;
  if (max_suspend_delay_ < maxdelay)
    max_suspend_delay_ = maxdelay;
  start_suspend_hooks_.Add(name, func, arg);
  if (powerd_up_)
    RegisterSuspendDelay();
}

gboolean CromoServer::RegisterSuspendDelayCallback(gpointer arg) {
  CromoServer* srv = static_cast<CromoServer*>(arg);
  DBus::CallMessage* call = new DBus::CallMessage();
  call->destination(power_manager::kPowerManagerInterface);
  call->interface(power_manager::kPowerManagerInterface);
  call->path("/");
  call->member(power_manager::kRegisterSuspendDelay);
  call->append(DBUS_TYPE_UINT32, &srv->max_suspend_delay_, DBUS_TYPE_INVALID);
  call->terminate();
  // dbus-c++ rudely throws an exception here if the target of the call is gone.
  // It doesn't cause problems for us that powerd is gone, but we have to
  // violate the style guide a bit and catch the exception or it'll abort() the
  // program.
  try {
    DBus::Message reply = srv->conn_.send_blocking(*call, -1);
    if (reply.is_error())
      LOG(WARNING) << "Can't register for suspend delay: "
                   << srv->max_suspend_delay_;
    else
      LOG(INFO) << "Registered for suspend delay: " << srv->max_suspend_delay_;
    return FALSE;
  } catch (DBus::Error e) {
    LOG(ERROR) << "dbus error " << e;
    return FALSE;
  }
}

void CromoServer::RegisterSuspendDelay() {
  // shill now handles disconnect on suspend, so skip the suspend delay
  // registration in cromo. See crosbug.com/30587 for details.

  // g_idle_add(RegisterSuspendDelayCallback, this);
}

void CromoServer::CancelSuspendCompletionTimeout() {
  if (suspend_completion_timeout_) {
    LOG(INFO) << "Cancel suspend completion timeout";
    g_source_remove(suspend_completion_timeout_);
    suspend_completion_timeout_ = 0;
  }
}

void CromoServer::UnregisterStartSuspend(const std::string& name) {
  suspend_delays_.erase(name);
  start_suspend_hooks_.Del(name);
  max_suspend_delay_ = MaxSuspendDelay();
  RegisterSuspendDelay();
}

unsigned int CromoServer::MaxSuspendDelay() {
  SuspendDelayMap::iterator it;
  unsigned int max = 0;
  for (it = suspend_delays_.begin(); it != suspend_delays_.end(); it++) {
    if (it->second > max)
      max = it->second;
  }
  return max;
}

void CromoServer::AddCarrier(Carrier *carrier) {
  delete carriers_[carrier->name()];
  carriers_[carrier->name()] = carrier;
}

Carrier* CromoServer::FindCarrierByName(const std::string &name) {
  return carriers_[name];
}

Carrier* CromoServer::FindCarrierByCarrierId(unsigned long id) {
  for (CarrierMap::iterator i = carriers_.begin();
       i != carriers_.end();
       ++i) {
    if (i->second &&
        i->second->carrier_id() == id) {
      return i->second;
    }
  }
  return NULL;
}

Carrier* CromoServer::FindCarrierNoOp() {
  if (carrier_no_op_.get() == NULL) {
    carrier_no_op_.reset(new Carrier(
        "no_op_name", "invalid", -1, MM_MODEM_TYPE_GSM, Carrier::kNone, NULL));
  }
  return carrier_no_op_.get();
}
