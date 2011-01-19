// Copyright (c) 2009-2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "login_manager/session_manager_service.h"

#include <dbus/dbus-glib-lowlevel.h>
#include <errno.h>
#include <glib.h>
#include <grp.h>
#include <secder.h>
#include <signal.h>
#include <stdio.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/basictypes.h"
#include "base/command_line.h"
#include "base/file_path.h"
#include "base/file_util.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/stl_util-inl.h"
#include "base/string_util.h"
#include "chromeos/dbus/dbus.h"
#include "chromeos/dbus/service_constants.h"
#include "metrics/bootstat.h"

#include "login_manager/child_job.h"
#include "login_manager/interface.h"
#include "login_manager/nss_util.h"
#include "login_manager/owner_key.h"

// Forcibly namespace the dbus-bindings generated server bindings instead of
// modifying the files afterward.
namespace login_manager {  // NOLINT
namespace gobject {  // NOLINT
#include "login_manager/bindings/server.h"
}  // namespace gobject
}  // namespace login_manager

namespace login_manager {

using std::make_pair;
using std::pair;
using std::string;
using std::vector;

// Jacked from chrome base/eintr_wrapper.h
#define HANDLE_EINTR(x) ({ \
  typeof(x) __eintr_result__; \
  do { \
    __eintr_result__ = x; \
  } while (__eintr_result__ == -1 && errno == EINTR); \
  __eintr_result__;\
})

int g_shutdown_pipe_write_fd = -1;
int g_shutdown_pipe_read_fd = -1;

// static
// Common code between SIG{HUP, INT, TERM}Handler.
void SessionManagerService::GracefulShutdownHandler(int signal) {
  // Reinstall the default handler.  We had one shot at graceful shutdown.
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  RAW_CHECK(sigaction(signal, &action, NULL) == 0);

  RAW_CHECK(g_shutdown_pipe_write_fd != -1);
  RAW_CHECK(g_shutdown_pipe_read_fd != -1);
  size_t bytes_written = 0;
  do {
    int rv = HANDLE_EINTR(
        write(g_shutdown_pipe_write_fd,
              reinterpret_cast<const char*>(&signal) + bytes_written,
              sizeof(signal) - bytes_written));
    RAW_CHECK(rv >= 0);
    bytes_written += rv;
  } while (bytes_written < sizeof(signal));

  RAW_LOG(INFO,
          "Successfully wrote to shutdown pipe, resetting signal handler.");
}

// static
void SessionManagerService::SIGHUPHandler(int signal) {
  RAW_CHECK(signal == SIGHUP);
  RAW_LOG(INFO, "Handling SIGHUP.");
  GracefulShutdownHandler(signal);
}
// static
void SessionManagerService::SIGINTHandler(int signal) {
  RAW_CHECK(signal == SIGINT);
  RAW_LOG(INFO, "Handling SIGINT.");
  GracefulShutdownHandler(signal);
}

// static
void SessionManagerService::SIGTERMHandler(int signal) {
  RAW_CHECK(signal == SIGTERM);
  RAW_LOG(INFO, "Handling SIGTERM.");
  GracefulShutdownHandler(signal);
}

// static
const uint32 SessionManagerService::kMaxEmailSize = 200;
// static
const char SessionManagerService::kEmailSeparator = '@';
// static
const char SessionManagerService::kLegalCharacters[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    ".@1234567890-";
// static
const char SessionManagerService::kIncognitoUser[] = "";
// static
const char SessionManagerService::kDeviceOwnerPref[] = "cros.device.owner";

// static
const char SessionManagerService::kChromeTestingPrefix[] =
    "ChromeTestingInterface";

namespace {

// Time we wait for child job to die (in seconds).
const int kKillTimeout = 3;

const int kMaxArgumentsSize = 512;

}  // namespace

SessionManagerService::SessionManagerService(
    std::vector<ChildJobInterface*> child_jobs)
    : child_jobs_(child_jobs.begin(), child_jobs.end()),
      child_pids_(child_jobs.size(), -1),
      exit_on_child_done_(false),
      session_manager_(NULL),
      main_loop_(g_main_loop_new(NULL, FALSE)),
      system_(new SystemUtils),
      nss_(NssUtil::Create()),
      key_(new OwnerKey(nss_->GetOwnerKeyFilePath())),
      store_(new PrefStore(FilePath(PrefStore::kDefaultPath))),
      upstart_signal_emitter_(new UpstartSignalEmitter),
      session_started_(false),
      screen_locked_(false),
      set_uid_(false),
      shutting_down_(false) {
  memset(signals_, 0, sizeof(signals_));
  SetupHandlers();
}

SessionManagerService::~SessionManagerService() {
  if (main_loop_)
    g_main_loop_unref(main_loop_);
  if (session_manager_)
    g_object_unref(session_manager_);

  // Remove this in case it was added by StopSession().
  g_idle_remove_by_data(this);

  // Remove this in case it was added by SetOwnerKey().
  g_idle_remove_by_data(key_.get());

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
  CHECK(sigaction(SIGALRM, &action, NULL) == 0);
  CHECK(sigaction(SIGTERM, &action, NULL) == 0);
  CHECK(sigaction(SIGINT, &action, NULL) == 0);
  CHECK(sigaction(SIGHUP, &action, NULL) == 0);

  STLDeleteElements(&child_jobs_);
}

bool SessionManagerService::Initialize() {
  // Install the type-info for the service with dbus.
  dbus_g_object_type_install_info(
      gobject::session_manager_get_type(),
      &gobject::dbus_glib_session_manager_object_info);

  // Creates D-Bus GLib signal ids.
  signals_[kSignalSessionStateChanged] =
      g_signal_new("session_state_changed",
                   gobject::session_manager_get_type(),
                   G_SIGNAL_RUN_LAST,
                   0,               // class offset
                   NULL, NULL,      // accumulator and data
                   // TODO: This is wrong.  If you need to use it at some point
                   // (probably only if you need to listen to this GLib signal
                   // instead of to the D-Bus signal), you should generate an
                   // appropriate marshaller that takes two string arguments.
                   // See e.g. http://goo.gl/vEGT4.
                   g_cclosure_marshal_VOID__STRING,
                   G_TYPE_NONE,     // return type
                   2,               // num params
                   G_TYPE_STRING,   // state ("started" or "stopped")
                   G_TYPE_STRING);  // current user

  if (!store_->LoadOrCreate())
    LOG(ERROR) << "Could not load existing settings.  Continuing anyway...";
  return Reset();
}

bool SessionManagerService::Register(
    const chromeos::dbus::BusConnection &connection) {
  if (!chromeos::dbus::AbstractDbusService::Register(connection))
    return false;
  const std::string filter =
      StringPrintf("type='method_call', interface='%s'", service_interface());
  DBusConnection* conn =
      ::dbus_g_connection_get_connection(connection.g_connection());
  CHECK(conn);
  DBusError error;
  ::dbus_error_init(&error);
  ::dbus_bus_add_match(conn, filter.c_str(), &error);
  if (::dbus_error_is_set(&error)) {
    LOG(WARNING) << "Failed to add match to bus: " << error.name << ", message="
                 << (error.message ? error.message : "unknown error");
    return false;
  }
  if (!::dbus_connection_add_filter(conn,
                                    &SessionManagerService::FilterMessage,
                                    this,
                                    NULL)) {
    LOG(WARNING) << "Failed to add filter to connection";
    return false;
  }
  return true;
}

bool SessionManagerService::Reset() {
  if (session_manager_)
    g_object_unref(session_manager_);
  session_manager_ =
      reinterpret_cast<gobject::SessionManager*>(
          g_object_new(gobject::session_manager_get_type(), NULL));

  // Allow references to this instance.
  session_manager_->service = this;

  if (main_loop_)
    g_main_loop_unref(main_loop_);
  main_loop_ = g_main_loop_new(NULL, false);
  if (!main_loop_) {
    LOG(ERROR) << "Failed to create main loop";
    return false;
  }
  return true;
}

bool SessionManagerService::Run() {
  if (!main_loop_) {
    LOG(ERROR) << "You must have a main loop to call Run.";
    return false;
  }

  int pipefd[2];
  int ret = pipe(pipefd);
  if (ret < 0) {
    PLOG(DFATAL) << "Failed to create pipe";
  } else {
    g_shutdown_pipe_read_fd = pipefd[0];
    g_shutdown_pipe_write_fd = pipefd[1];
    g_io_add_watch_full(g_io_channel_unix_new(g_shutdown_pipe_read_fd),
                        G_PRIORITY_HIGH_IDLE,
                        GIOCondition(G_IO_IN | G_IO_PRI | G_IO_HUP),
                        HandleKill,
                        this,
                        NULL);
  }

  if (ShouldRunChildren()) {
    RunChildren();
  } else {
    AllowGracefulExit();
  }

  // TODO(cmasone): A corrupted owner key means that the user needs to go
  //                to recovery mode.  How to tell them that from here?
  CHECK(key_->PopulateFromDiskIfPossible());

  g_main_loop_run(main_loop_);

  CleanupChildren(kKillTimeout);

  return true;
}

bool SessionManagerService::ShouldRunChildren() {
  return !file_checker_.get() || !file_checker_->exists();
}

bool SessionManagerService::ShouldStopChild(ChildJobInterface* child_job) {
  return child_job->ShouldStop();
}

bool SessionManagerService::Shutdown() {
  if (session_started_) {
    DLOG(INFO) << "emitting D-Bus signal SessionStateChanged:stopped";
    if (signals_[kSignalSessionStateChanged]) {
      g_signal_emit(session_manager_,
                    signals_[kSignalSessionStateChanged],
                    0, "stopped", current_user_.c_str());
    }
  }

  // Even if we haven't gotten around to processing a persist task.
  store_->Persist();

  return chromeos::dbus::AbstractDbusService::Shutdown();
}

void SessionManagerService::RunChildren() {
  bootstat_log("chrome-exec");
  for (size_t i_child = 0; i_child < child_jobs_.size(); ++i_child) {
    ChildJobInterface* child_job = child_jobs_[i_child];
    LOG(INFO) << "Running child " << child_job->GetName() << "...";
    child_pids_[i_child] = RunChild(child_job);
  }
}

int SessionManagerService::RunChild(ChildJobInterface* child_job) {
  child_job->RecordTime();
  int pid = fork();
  if (pid == 0) {
    child_job->Run();
    exit(1);  // Run() is not supposed to return.
  }
  g_child_watch_add_full(G_PRIORITY_HIGH_IDLE,
                         pid,
                         HandleChildExit,
                         this,
                         NULL);
  return pid;
}

bool SessionManagerService::IsKnownChild(int pid) {
  return std::find(child_pids_.begin(), child_pids_.end(), pid) !=
      child_pids_.end();
}

void SessionManagerService::AllowGracefulExit() {
  shutting_down_ = true;
  if (exit_on_child_done_) {
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                    ServiceShutdown,
                    this,
                    NULL);
  }
}

///////////////////////////////////////////////////////////////////////////////
// SessionManagerService commands

gboolean SessionManagerService::EmitLoginPromptReady(gboolean* OUT_emitted,
                                                     GError** error) {
  bootstat_log("login-prompt-ready");
  // TODO(derat): Stop emitting this signal once no one's listening for it.
  // Jobs that want to run after we're done booting should wait for
  // login-prompt-visible or boot-complete.
  *OUT_emitted =
      upstart_signal_emitter_->EmitSignal("login-prompt-ready", "", error);
  return *OUT_emitted;
}

gboolean SessionManagerService::EmitLoginPromptVisible(GError** error) {
  bootstat_log("login-prompt-visible");
  return upstart_signal_emitter_->EmitSignal("login-prompt-visible", "", error);
}

gboolean SessionManagerService::EnableChromeTesting(gchar** OUT_filepath,
                                                    GError** error) {
  for (size_t i_child = 0; i_child < child_jobs_.size(); ++i_child) {
    ChildJobInterface* child_job = child_jobs_[i_child];
    if (child_job->GetName() == "chrome") {
      // kill chrome
      uid_t to_kill_as = getuid();
      if (child_job->IsDesiredUidSet())
        to_kill_as = child_job->GetDesiredUid();
      system_->kill(-child_pids_[i_child], to_kill_as, SIGKILL);

      // create a write-only temporary directory to put the testing channel in
      FilePath temp_dir_path;
      if (!file_util::CreateNewTempDirectory(
          FILE_PATH_LITERAL(kChromeTestingPrefix), &temp_dir_path))
        return FALSE;
      if (chmod(temp_dir_path.value().c_str(), 0003))
        return FALSE;

      // get a temporary filename in the temporary directory
      char* file_path = tempnam(temp_dir_path.value().c_str(),
                                kChromeTestingPrefix);
      if (!file_path)
        return FALSE;

      // run chrome
      child_job->AddChromeTestingArgument(file_path);
      child_pids_[i_child] = RunChild(child_job);

      *OUT_filepath = g_strdup(file_path);

      free(file_path);

      return TRUE;
    }
  }

  return FALSE;
}

gboolean SessionManagerService::StartSession(gchar* email_address,
                                             gchar* unique_identifier,
                                             gboolean* OUT_done,
                                             GError** error) {
  if (session_started_) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_SESSION_EXISTS,
              "Can't start a session while a session is already active.");
    *OUT_done = FALSE;
    return FALSE;
  }
  if (!ValidateAndCacheUserEmail(email_address, error)) {
    *OUT_done = FALSE;
    return FALSE;
  }
  // If the current user is the owner, and isn't whitelisted or set
  // as the cros.device.owner pref, then do so.  This attempt only succeeds
  // if the current user has access to the private half of the owner's
  // registered public key.
  StoreOwnerProperties(NULL);
  // Now, the flip side...if we believe the current user to be the owner
  // based on the cros.owner.device setting, and he DOESN'T have the private
  // half of the public key, we must mitigate.
  if (CurrentUserIsOwner(error) &&
      !CurrentUserHasOwnerKey(key_->public_key_der(), error)) {
    if (!(*OUT_done = mitigator_->Mitigate()))
      return FALSE;
  }

  if (set_uid_) {
    *OUT_done =
        upstart_signal_emitter_->EmitSignal(
            "start-user-session",
            StringPrintf("CHROMEOS_USER=%s USER_ID=%d",
                         current_user_.c_str(), uid_),
            error);
  } else {
    *OUT_done =
        upstart_signal_emitter_->EmitSignal(
            "start-user-session",
            StringPrintf("CHROMEOS_USER=%s", current_user_.c_str()),
            error);
  }

  if (*OUT_done) {
    for (size_t i_child = 0; i_child < child_jobs_.size(); ++i_child) {
      ChildJobInterface* child_job = child_jobs_[i_child];
      child_job->StartSession(current_user_);
    }
    session_started_ = true;
    DLOG(INFO) << "emitting D-Bus signal SessionStateChanged:started";
    if (signals_[kSignalSessionStateChanged]) {
      g_signal_emit(session_manager_,
                    signals_[kSignalSessionStateChanged],
                    0, "started", current_user_.c_str());
    }
  }

  return *OUT_done;
}

gboolean SessionManagerService::StopSession(gchar* unique_identifier,
                                            gboolean* OUT_done,
                                            GError** error) {
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                  ServiceShutdown,
                  this,
                  NULL);
  // TODO(cmasone): re-enable these when we try to enable logout without exiting
  //                the session manager
  // child_job_->StopSession();
  // session_started_ = false;
  return *OUT_done = TRUE;
}

gboolean SessionManagerService::SetOwnerKey(GArray* public_key_der,
                                            GError** error) {
  LOG(INFO) << "key size is " << public_key_der->len;

  if (!session_started_) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_ILLEGAL_PUBKEY,
              "Illegal attempt to set the owner's public key.");
    return FALSE;
  }

  std::vector<uint8> pub_key;
  NssUtil::KeyFromBuffer(public_key_der, &pub_key);

  if (!CurrentUserHasOwnerKey(pub_key, error)) {
    return FALSE;  // error set by CurrentUserHasOwnerKey()
  }

  if (!key_->PopulateFromBuffer(pub_key)) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_ILLEGAL_PUBKEY,
              "Illegal attempt to set the owner's public key.");
    return FALSE;
  }
  g_idle_add_full(G_PRIORITY_HIGH_IDLE,
                  PersistKey,
                  new PersistKeyData(system_.get(), key_.get()),
                  NULL);
  return StoreOwnerProperties(error);
}

gboolean SessionManagerService::Unwhitelist(gchar* email_address,
                                            GArray* signature,
                                            GError** error) {
  LOG(INFO) << "Unwhitelisting " << email_address;
  if (!key_->IsPopulated()) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_NO_OWNER_KEY,
              "Attempt to unwhitelist before owner's key is set.");
    return FALSE;
  }
  if (!key_->Verify(email_address,
                    strlen(email_address),
                    signature->data,
                    signature->len)) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_VERIFY_FAIL,
              "Signature could not be verified.");
    return FALSE;
  }
  store_->Unwhitelist(email_address);
  g_idle_add_full(G_PRIORITY_HIGH_IDLE,
                  PersistWhitelist,
                  new PersistStoreData(system_.get(), store_.get()),
                  NULL);
  return TRUE;
}

gboolean SessionManagerService::CheckWhitelist(gchar* email_address,
                                               GArray** OUT_signature,
                                               GError** error) {
  std::string encoded;
  if (!store_->GetFromWhitelist(email_address, &encoded)) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_ILLEGAL_USER,
              "The user is not whitelisted.");
    return FALSE;
  }
  std::string decoded;
  if (!base::Base64Decode(encoded, &decoded)) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_DECODE_FAIL,
              "Signature could not be decoded.");
    return FALSE;
  }

  *OUT_signature = g_array_sized_new(FALSE, FALSE, 1, decoded.length());
  g_array_append_vals(*OUT_signature, decoded.c_str(), decoded.length());
  return TRUE;
}

gboolean SessionManagerService::EnumerateWhitelisted(gchar*** OUT_whitelist,
                                                     GError** error) {
  std::vector<std::string> the_whitelisted;
  store_->EnumerateWhitelisted(&the_whitelisted);
  uint num_whitelisted = the_whitelisted.size();
  *OUT_whitelist = g_new(gchar*, num_whitelisted + 1);
  (*OUT_whitelist)[num_whitelisted] = NULL;

  for (uint i = 0; i < num_whitelisted; ++i) {
    (*OUT_whitelist)[i] = g_strndup(the_whitelisted[i].c_str(),
                                    the_whitelisted[i].length());
  }
  return TRUE;
}

gboolean SessionManagerService::Whitelist(gchar* email_address,
                                          GArray* signature,
                                          GError** error) {
  LOG(INFO) << "Whitelisting " << email_address;
  if (!key_->IsPopulated()) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_NO_OWNER_KEY,
              "Attempt to whitelist before owner's key is set.");
    return FALSE;
  }
  if (!key_->Verify(email_address,
                    strlen(email_address),
                    signature->data,
                    signature->len)) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_VERIFY_FAIL,
              "Signature could not be verified.");
    return FALSE;
  }
  std::string data(signature->data, signature->len);
  return WhitelistHelper(email_address, data, error);
}

gboolean SessionManagerService::StoreProperty(gchar* name,
                                              gchar* value,
                                              GArray* signature,
                                              GError** error) {
  LOG(INFO) << "Setting pref " << name << "=" << value;
  if (!key_->IsPopulated()) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_NO_OWNER_KEY,
              "Attempt to store property before owner's key is set.");
    return FALSE;
  }
  std::string was_signed = base::StringPrintf("%s=%s", name, value);
  if (!key_->Verify(was_signed.c_str(),
                    was_signed.length(),
                    signature->data,
                    signature->len)) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_VERIFY_FAIL,
              "Signature could not be verified.");
    return FALSE;
  }
  std::string data(signature->data, signature->len);
  return SetPropertyHelper(name, value, data, error);
}

gboolean SessionManagerService::RetrieveProperty(gchar* name,
                                                 gchar** OUT_value,
                                                 GArray** OUT_signature,
                                                 GError** error) {
  std::string value;
  std::string decoded;
  if (!GetPropertyHelper(name, &value, &decoded, error))
    return FALSE;

  *OUT_value = g_strdup_printf("%.*s", value.length(), value.c_str());
  *OUT_signature = g_array_sized_new(FALSE, FALSE, 1, decoded.length());
  g_array_append_vals(*OUT_signature, decoded.c_str(), decoded.length());
  return TRUE;
}

gboolean SessionManagerService::LockScreen(GError** error) {
  screen_locked_ = TRUE;
  system_->SendSignalToChromium(chromium::kLockScreenSignal, NULL);
  LOG(INFO) << "LockScreen";
  return TRUE;
}

gboolean SessionManagerService::UnlockScreen(GError** error) {
  screen_locked_ = FALSE;
  system_->SendSignalToChromium(chromium::kUnlockScreenSignal, NULL);
  LOG(INFO) << "UnlockScreen";
  return TRUE;
}

gboolean SessionManagerService::RestartJob(gint pid,
                                           gchar* arguments,
                                           gboolean* OUT_done,
                                           GError** error) {
  int child_pid = pid;
  std::vector<int>::iterator child_pid_it =
      std::find(child_pids_.begin(), child_pids_.end(), child_pid);
  size_t child_index = child_pid_it - child_pids_.begin();

  if (child_pid_it == child_pids_.end() ||
      child_jobs_[child_index]->GetName() != "chrome") {
    // If we didn't find the pid, or we don't think that job was chrome...
    *OUT_done = FALSE;
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_UNKNOWN_PID,
              "Provided pid is unknown.");
    return FALSE;
  }

  // Waiting for Chrome to shutdown takes too much time.
  // We're killing it immediately hoping that data Chrome uses before
  // logging in is not corrupted.
  // TODO(avayvod): Remove RestartJob when crosbug.com/6924 is fixed.
  uid_t to_kill_as = getuid();
  if (child_jobs_[child_index]->IsDesiredUidSet())
    to_kill_as = child_jobs_[child_index]->GetDesiredUid();
  system_->kill(-child_pid, to_kill_as, SIGKILL);

  char arguments_buffer[kMaxArgumentsSize + 1];
  snprintf(arguments_buffer, sizeof(arguments_buffer), "%s", arguments);
  arguments_buffer[kMaxArgumentsSize] = '\0';
  string arguments_string(arguments_buffer);

  child_jobs_[child_index]->SetArguments(arguments_string);
  child_pids_[child_index] = RunChild(child_jobs_[child_index]);

  // To set "logged-in" state for BWSI mode.
  return StartSession(const_cast<gchar*>(kIncognitoUser), NULL,
                      OUT_done, error);
}

gboolean SessionManagerService::RestartEntd(GError** error) {
  LOG(INFO) << "Restarting entd.";
  // Shutdown entd if it is currently running, blocking this thread and
  // method call until it has finished shutting down.
  int stop_status = system("/sbin/initctl stop entd");
  // Stop may have failed, but it may be ok if not already running.  Error
  // messages will go to session manager log.
  LOG_IF(INFO, stop_status != 0)
      << "Could not stop entd, likely was not running.";
  string command = StringPrintf("/sbin/initctl start entd "
                                "CHROMEOS_USER=%s",
                                current_user_.c_str());
  // Start entd with the current user passed in, blocking this thread
  // and method call until it has finished starting.
  bool restarted = (system(command.c_str()) == 0);
  LOG(INFO) << "Restart was " << (restarted ? "" : "not ")
            << "successful.";
  return restarted;
}

///////////////////////////////////////////////////////////////////////////////
// glib event handlers

void SessionManagerService::HandleChildExit(GPid pid,
                                            gint status,
                                            gpointer data) {
  // If I could wait for descendants here, I would.  Instead, I kill them.
  kill(-pid, SIGKILL);

  DLOG(INFO) << "Handling child process exit.";
  if (WIFSIGNALED(status)) {
    DLOG(INFO) << "  Exited with signal " << WTERMSIG(status);
  } else if (WIFEXITED(status)) {
    DLOG(INFO) << "  Exited with exit code " << WEXITSTATUS(status);
    CHECK(WEXITSTATUS(status) != ChildJobInterface::kCantSetUid);
    CHECK(WEXITSTATUS(status) != ChildJobInterface::kCantExec);
  } else {
    DLOG(INFO) << "  Exited...somehow, without an exit code or a signal??";
  }

  // If the child _ever_ exits uncleanly, we want to start it up again.
  SessionManagerService* manager = static_cast<SessionManagerService*>(data);

  // Do nothing if already shutting down.
  if (manager->shutting_down_)
    return;

  int i_child = -1;
  for (int i = 0; i < manager->child_pids_.size(); ++i) {
    if (manager->child_pids_[i] == pid) {
      i_child = i;
      manager->child_pids_[i] = -1;
      break;
    }
  }

  ChildJobInterface* child_job = i_child >= 0
                                 ? manager->child_jobs_[i_child]
                                 : NULL;

  LOG(ERROR) << StringPrintf(
      "Process %s(%d) exited.",
      child_job ? child_job->GetName().c_str() : "",
      pid);
  if (manager->screen_locked_) {
    LOG(ERROR) << "Screen locked, shutting down",
    ServiceShutdown(data);
    return;
  }

  if (child_job) {
    if (manager->ShouldStopChild(child_job)) {
      ServiceShutdown(data);
    } else if (manager->ShouldRunChildren()) {
      // TODO(cmasone): deal with fork failing in RunChild()
      LOG(INFO) << StringPrintf(
          "Running child %s again...", child_job->GetName().data());
      manager->child_pids_[i_child] = manager->RunChild(child_job);
    } else {
      LOG(INFO) << StringPrintf(
          "Should NOT run %s again...", child_job->GetName().data());
      manager->AllowGracefulExit();
    }
  } else {
    LOG(ERROR) << "Couldn't find pid of exiting child: " << pid;
  }
}

gboolean SessionManagerService::HandleKill(GIOChannel* source,
                                           GIOCondition condition,
                                           gpointer data) {
  // We only get called if there's data on the pipe.  If there's data, we're
  // supposed to exit.  So, don't even bother to read it.
  return ServiceShutdown(data);
}

gboolean SessionManagerService::ServiceShutdown(gpointer data) {
  SessionManagerService* manager = static_cast<SessionManagerService*>(data);
  manager->Shutdown();
  LOG(INFO) << "SessionManagerService exiting";
  return FALSE;  // So that the event source that called this gets removed.
}

gboolean SessionManagerService::PersistKey(gpointer data) {
  PersistKeyData* key_data = static_cast<PersistKeyData*>(data);
  LOG(INFO) << "Persisting Owner key to disk.";
  if (key_data->to_persist->Persist())
    key_data->signaler->SendSignalToChromium(chromium::kOwnerKeySetSignal,
                                             "success");
  else
    key_data->signaler->SendSignalToChromium(chromium::kOwnerKeySetSignal,
                                             "failure");
  delete key_data;
  return FALSE;  // So that the event source that called this gets removed.
}

gboolean SessionManagerService::PersistWhitelist(gpointer data) {
  PersistStoreData* store_data = static_cast<PersistStoreData*>(data);
  LOG(INFO) << "Persisting Whitelist to disk.";
  if (store_data->to_persist->Persist()) {
    store_data->signaler->SendSignalToChromium(
        chromium::kWhitelistChangeCompleteSignal,
        "success");
  } else {
    store_data->signaler->SendSignalToChromium(
        chromium::kWhitelistChangeCompleteSignal,
        "failure");
  }
  delete store_data;
  return FALSE;  // So that the event source that called this gets removed.
}

gboolean SessionManagerService::PersistStore(gpointer data) {
  PersistStoreData* store_data = static_cast<PersistStoreData*>(data);
  LOG(INFO) << "Persisting Store to disk.";
  if (store_data->to_persist->Persist()) {
    store_data->signaler->SendSignalToChromium(
        chromium::kPropertyChangeCompleteSignal,
        "success");
  } else {
    store_data->signaler->SendSignalToChromium(
        chromium::kPropertyChangeCompleteSignal,
        "failure");
  }
  delete store_data;
  return FALSE;  // So that the event source that called this gets removed.
}

// static
void SessionManagerService::SetGError(GError** error,
                                      ChromeOSLoginError code,
                                      const char* message) {
  LOG(ERROR) << message;
  g_set_error(error, CHROMEOS_LOGIN_ERROR, code, "Login error: %s", message);
}


///////////////////////////////////////////////////////////////////////////////
// Utility Methods

// This can probably be more efficient, if it needs to be.
// static
bool SessionManagerService::ValidateEmail(const string& email_address) {
  if (email_address.find_first_not_of(kLegalCharacters) != string::npos)
    return false;

  size_t at = email_address.find(kEmailSeparator);
  // it has NO @.
  if (at == string::npos)
    return false;

  // it has more than one @.
  if (email_address.find(kEmailSeparator, at+1) != string::npos)
    return false;

  return true;
}

// static
DBusHandlerResult SessionManagerService::FilterMessage(DBusConnection* conn,
                                                       DBusMessage* message,
                                                       void* data) {
  SessionManagerService* service = static_cast<SessionManagerService*>(data);
  if (::dbus_message_is_method_call(message,
                                    service->service_interface(),
                                    kSessionManagerRestartJob)) {
    const char* sender = ::dbus_message_get_sender(message);
    if (!sender) {
      LOG(ERROR) << "Call to RestartJob has no sender";
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    LOG(INFO) << "Received RestartJob from " << sender;
    DBusMessage* get_pid =
        ::dbus_message_new_method_call("org.freedesktop.DBus",
                                       "/org/freedesktop/DBus",
                                       "org.freedesktop.DBus",
                                       "GetConnectionUnixProcessID");
    CHECK(get_pid);
    ::dbus_message_append_args(get_pid,
                               DBUS_TYPE_STRING, &sender,
                               DBUS_TYPE_INVALID);
    DBusMessage* got_pid =
        ::dbus_connection_send_with_reply_and_block(conn, get_pid, -1, NULL);
    ::dbus_message_unref(get_pid);
    if (!got_pid) {
      LOG(ERROR) << "Could not look up sender of RestartJob";
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    uint32 pid;
    if (!::dbus_message_get_args(got_pid, NULL,
                                 DBUS_TYPE_UINT32, &pid,
                                 DBUS_TYPE_INVALID)) {
      ::dbus_message_unref(got_pid);
      LOG(ERROR) << "Could not extract pid of sender of RestartJob";
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    ::dbus_message_unref(got_pid);
    if (!service->IsKnownChild(pid)) {
      LOG(WARNING) << "Sender of RestartJob is no child of mine!";
      return DBUS_HANDLER_RESULT_HANDLED;
    }
  }
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void SessionManagerService::SetupHandlers() {
  // I have to ignore SIGUSR1, because Xorg sends it to this process when it's
  // got no clients and is ready for new ones.  If we don't ignore it, we die.
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_IGN;
  CHECK(sigaction(SIGUSR1, &action, NULL) == 0);

  action.sa_handler = SessionManagerService::do_nothing;
  CHECK(sigaction(SIGALRM, &action, NULL) == 0);

  // We need to handle SIGTERM, because that is how many POSIX-based distros ask
  // processes to quit gracefully at shutdown time.
  action.sa_handler = SIGTERMHandler;
  CHECK(sigaction(SIGTERM, &action, NULL) == 0);
  // Also handle SIGINT - when the user terminates the browser via Ctrl+C.
  // If the browser process is being debugged, GDB will catch the SIGINT first.
  action.sa_handler = SIGINTHandler;
  CHECK(sigaction(SIGINT, &action, NULL) == 0);
  // And SIGHUP, for when the terminal disappears. On shutdown, many Linux
  // distros send SIGHUP, SIGTERM, and then SIGKILL.
  action.sa_handler = SIGHUPHandler;
  CHECK(sigaction(SIGHUP, &action, NULL) == 0);
}

gboolean SessionManagerService::CurrentUserIsOwner(GError** error) {
  std::string value;
  std::string decoded;
  if (!GetPropertyHelper(kDeviceOwnerPref, &value, &decoded, error))
    return FALSE;
  std::string was_signed = base::StringPrintf("%s=%s",
                                              kDeviceOwnerPref,
                                              value.c_str());
  if (!key_->Verify(was_signed.c_str(),
                    was_signed.length(),
                    decoded.c_str(),
                    decoded.length())) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_VERIFY_FAIL,
              "Owner pref signature could not be verified.");
    return FALSE;
  }
  return value == current_user_;
}

gboolean SessionManagerService::CurrentUserHasOwnerKey(
    const std::vector<uint8>& pub_key,
    GError** error) {
  if (!nss_->OpenUserDB()) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_NO_USER_NSSDB,
              "Could not open the current user's NSS database.");
    return FALSE;
  }
  if (!nss_->GetPrivateKey(pub_key)) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_ILLEGAL_PUBKEY,
              "Could not verify that public key belongs to the owner.");
    return FALSE;
  }
  return TRUE;
}

gboolean SessionManagerService::ValidateAndCacheUserEmail(
    const gchar* email_address,
    GError** error) {
  // basic validity checking; avoid buffer overflows here, and
  // canonicalize the email address a little.
  char email[kMaxEmailSize + 1];
  snprintf(email, sizeof(email), "%s", email_address);
  email[kMaxEmailSize] = '\0';  // Just to be sure.
  string email_string(email);
  if (email_string != kIncognitoUser && !ValidateEmail(email_string)) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_INVALID_EMAIL,
              "Provided email address is not valid.  ASCII only.");
    return FALSE;
  }
  current_user_ = StringToLowerASCII(email_string);
  return TRUE;
}

void SessionManagerService::CleanupChildren(int timeout) {
  vector<pair<int, uid_t> > pids_to_kill;

  for (size_t i = 0; i < child_pids_.size(); ++i) {
    const int pid = child_pids_[i];
    if (pid < 0)
      continue;

    ChildJobInterface* job = child_jobs_[i];
    if (job->ShouldNeverKill())
      continue;

    const uid_t uid = job->IsDesiredUidSet() ? job->GetDesiredUid() : getuid();
    pids_to_kill.push_back(make_pair(pid, uid));
    system_->kill(pid, uid, (session_started_ ? SIGTERM : SIGKILL));
  }

  for (vector<pair<int, uid_t> >::const_iterator it = pids_to_kill.begin();
       it != pids_to_kill.end(); ++it) {
    const int pid = it->first;
    const uid_t uid = it->second;
    if (!system_->ChildIsGone(pid, timeout))
      system_->kill(pid, uid, SIGABRT);
  }
}

gboolean SessionManagerService::StoreOwnerProperties(GError** error) {
  if (!SignAndStoreProperty(kDeviceOwnerPref,
                            current_user_,
                            "Could not sign owner property.",
                            error)) {
    return FALSE;
  }
  return SignAndWhitelist(current_user_, "Could not whitelist owner.", error);
}

gboolean SessionManagerService::SignAndStoreProperty(const std::string& name,
                                                     const std::string& value,
                                                     const std::string& err_msg,
                                                     GError** error) {
  std::vector<uint8> signature;
  std::string to_sign = base::StringPrintf("%s=%s",
                                           kDeviceOwnerPref,
                                           current_user_.c_str());
  if (!key_->Sign(to_sign.c_str(), to_sign.length(), &signature)) {
    SetGError(error, CHROMEOS_LOGIN_ERROR_ILLEGAL_PUBKEY, err_msg.c_str());
    return FALSE;
  }
  std::string signature_string(reinterpret_cast<const char*>(&signature[0]),
                               signature.size());
  return SetPropertyHelper(kDeviceOwnerPref,
                           current_user_,
                           signature_string,
                           error);
}

gboolean SessionManagerService::SignAndWhitelist(const std::string& email,
                                                 const std::string& err_msg,
                                                 GError** error) {
  std::vector<uint8> signature;
  if (!key_->Sign(current_user_.c_str(), current_user_.length(), &signature)) {
    SetGError(error, CHROMEOS_LOGIN_ERROR_ILLEGAL_PUBKEY, err_msg.c_str());
    return FALSE;
  }
  std::string signature_string(reinterpret_cast<const char*>(&signature[0]),
                               signature.size());
  return WhitelistHelper(current_user_, signature_string, error);
}

gboolean SessionManagerService::SetPropertyHelper(const std::string& name,
                                                  const std::string& value,
                                                  const std::string& signature,
                                                  GError** error) {
  std::string encoded;
  if (!base::Base64Encode(signature, &encoded)) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_ENCODE_FAIL,
              "Signature could not be verified.");
    return FALSE;
  }
  store_->Set(name, value, encoded);
  g_idle_add_full(G_PRIORITY_HIGH_IDLE,
                  PersistStore,
                  new PersistStoreData(system_.get(), store_.get()),
                  NULL);
  return TRUE;
}

gboolean SessionManagerService::WhitelistHelper(const std::string& email,
                                                const std::string& signature,
                                                GError** error) {
  std::string encoded;
  if (!base::Base64Encode(signature, &encoded)) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_ENCODE_FAIL,
              "Signature could not be encoded.");
    return FALSE;
  }
  store_->Whitelist(email, encoded);
  g_idle_add_full(G_PRIORITY_HIGH_IDLE,
                  PersistWhitelist,
                  new PersistStoreData(system_.get(), store_.get()),
                  NULL);
  return TRUE;
}

gboolean SessionManagerService::GetPropertyHelper(const std::string& name,
                                                  std::string* OUT_value,
                                                  std::string* OUT_signature,
                                                  GError** error) {
  std::string encoded;
  if (!store_->Get(name, OUT_value, &encoded)) {
    std::string error_string =
        base::StringPrintf("The requested property %s is unknown.",
                           name.c_str());
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_UNKNOWN_PROPERTY,
              error_string.c_str());
    return FALSE;
  }
  if (!base::Base64Decode(encoded, OUT_signature)) {
    SetGError(error,
              CHROMEOS_LOGIN_ERROR_DECODE_FAIL,
              "Signature could not be decoded.");
    return FALSE;
  }
  return TRUE;
}

// static
std::vector<std::vector<std::string> > SessionManagerService::GetArgLists(
    std::vector<std::string> args) {
  std::vector<std::string> arg_list;
  std::vector<std::vector<std::string> > arg_lists;
  for (size_t i_arg = 0; i_arg < args.size(); ++i_arg) {
    if (args[i_arg] == "--") {
      if (arg_list.size()) {
        arg_lists.push_back(arg_list);
        arg_list.clear();
      }
    } else {
      arg_list.push_back(args[i_arg]);
    }
  }
  if (arg_list.size()) {
    arg_lists.push_back(arg_list);
  }
  return arg_lists;
}

}  // namespace login_manager
