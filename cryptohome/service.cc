// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "service.h"

#include <stdio.h>
#include <stdlib.h>

#include <base/file_util.h>
#include <base/logging.h>
#include <base/string_util.h>
#include <base/time.h>
#include <chromeos/dbus/dbus.h>

#include "cryptohome/marshal.glibmarshal.h"
#include "cryptohome_event_source.h"
#include "interface.h"
#include "crypto.h"
#include "mount.h"
#include "secure_blob.h"
#include "tpm.h"
#include "username_passkey.h"
#include "vault_keyset.pb.h"

// Forcibly namespace the dbus-bindings generated server bindings instead of
// modifying the files afterward.
namespace cryptohome {  // NOLINT
namespace gobject {  // NOLINT
#include "cryptohome/bindings/server.h"
}  // namespace gobject
}  // namespace cryptohome

namespace cryptohome {

const int kDefaultRandomSeedLength = 64;
const char* kMountThreadName = "MountThread";
const char* kTpmInitStatusEventType = "TpmInitStatus";

// The default entropy source to seed with random data from the TPM on startup.
const char* kDefaultEntropySource = "/dev/urandom";

class TpmInitStatus : public CryptohomeEventBase {
 public:
  TpmInitStatus()
      : took_ownership_(false),
        status_(false) { }
  virtual ~TpmInitStatus() { }

  virtual const char* GetEventName() {
    return kTpmInitStatusEventType;
  }

  void set_took_ownership(bool value) {
    took_ownership_ = value;
  }

  bool get_took_ownership() {
    return took_ownership_;
  }

  void set_status(bool value) {
    status_ = value;
  }

  bool get_status() {
    return status_;
  }

 private:
  bool took_ownership_;
  bool status_;
};

Service::Service()
    : loop_(NULL),
      cryptohome_(NULL),
      system_salt_(),
      default_mount_(new cryptohome::Mount()),
      mount_(default_mount_.get()),
      default_tpm_init_(new TpmInit()),
      tpm_init_(default_tpm_init_.get()),
      initialize_tpm_(true),
      mount_thread_(kMountThreadName),
      async_complete_signal_(-1),
      tpm_init_signal_(-1),
      event_source_() {
}

Service::~Service() {
  if (loop_)
    g_main_loop_unref(loop_);
  if (cryptohome_)
    g_object_unref(cryptohome_);
  if (mount_thread_.IsRunning()) {
    mount_thread_.Stop();
  }
}

bool Service::Initialize() {
  bool result = true;

  mount_->Init();
  Tpm* tpm = const_cast<Tpm*>(mount_->get_crypto()->get_tpm());
  if (tpm && initialize_tpm_) {
    tpm_init_->set_tpm(tpm);
    tpm_init_->Init(this);
    if (!SeedUrandom()) {
      LOG(ERROR) << "FAILED TO SEED /dev/urandom AT START";
    }
  }

  // Install the type-info for the service with dbus.
  dbus_g_object_type_install_info(gobject::cryptohome_get_type(),
                                  &gobject::dbus_glib_cryptohome_object_info);
  if (!Reset()) {
    result = false;
  }

  async_complete_signal_ = g_signal_new("async_call_status",
                                        gobject::cryptohome_get_type(),
                                        G_SIGNAL_RUN_LAST,
                                        0,
                                        NULL,
                                        NULL,
                                        cryptohome_VOID__INT_BOOLEAN_INT,
                                        G_TYPE_NONE,
                                        3,
                                        G_TYPE_INT,
                                        G_TYPE_BOOLEAN,
                                        G_TYPE_INT);

  tpm_init_signal_ = g_signal_new("tpm_init_status",
                                  gobject::cryptohome_get_type(),
                                  G_SIGNAL_RUN_LAST,
                                  0,
                                  NULL,
                                  NULL,
                                  cryptohome_VOID__BOOLEAN_BOOLEAN_BOOLEAN,
                                  G_TYPE_NONE,
                                  3,
                                  G_TYPE_BOOLEAN,
                                  G_TYPE_BOOLEAN,
                                  G_TYPE_BOOLEAN);

  mount_thread_.Start();

  return result;
}

bool Service::SeedUrandom() {
  SecureBlob random;
  if (!tpm_init_->GetRandomData(kDefaultRandomSeedLength, &random)) {
    LOG(ERROR) << "Could not get random data from the TPM";
    return false;
  }
  size_t written = file_util::WriteFile(FilePath(kDefaultEntropySource),
      static_cast<const char*>(random.const_data()), random.size());
  if (written != random.size()) {
    LOG(ERROR) << "Error writing data to /dev/urandom";
    return false;
  }
  return true;
}

bool Service::Reset() {
  if (cryptohome_)
    g_object_unref(cryptohome_);
  cryptohome_ = reinterpret_cast<gobject::Cryptohome*>(
      g_object_new(gobject::cryptohome_get_type(), NULL));
  // Allow references to this instance.
  cryptohome_->service = this;

  if (loop_) {
    ::g_main_loop_unref(loop_);
  }
  loop_ = g_main_loop_new(NULL, false);
  if (!loop_) {
    LOG(ERROR) << "Failed to create main loop";
    return false;
  }
  // Install the local event source for handling async results
  event_source_.Reset(this, g_main_loop_get_context(loop_));
  return true;
}

void Service::MountTaskObserve(const MountTaskResult& result) {
  // The event source will free this object
  event_source_.AddEvent(new MountTaskResult(result));
}

void Service::NotifyEvent(CryptohomeEventBase* event) {
  if (!strcmp(event->GetEventName(), kMountTaskResultEventType)) {
    MountTaskResult* result = static_cast<MountTaskResult*>(event);
    g_signal_emit(cryptohome_, async_complete_signal_, 0, result->sequence_id(),
                  result->return_status(), result->return_code());
  } else if (!strcmp(event->GetEventName(), kTpmInitStatusEventType)) {
    TpmInitStatus* result = static_cast<TpmInitStatus*>(event);
    g_signal_emit(cryptohome_, tpm_init_signal_, 0, tpm_init_->IsTpmReady(),
                  tpm_init_->IsTpmEnabled(), result->get_took_ownership());
  }
}

void Service::InitializeTpmComplete(bool status, bool took_ownership) {
  if (took_ownership) {
    MountTaskResult ignored_result;
    base::WaitableEvent event(true, false);
    MountTaskResetTpmContext* mount_task =
        new MountTaskResetTpmContext(NULL, mount_);
    mount_task->set_result(&ignored_result);
    mount_task->set_complete_event(&event);
    mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
    event.Wait();
  }
  // The event source will free this object
  TpmInitStatus* tpm_init_status = new TpmInitStatus();
  tpm_init_status->set_status(status);
  tpm_init_status->set_took_ownership(took_ownership);
  event_source_.AddEvent(tpm_init_status);
}

gboolean Service::CheckKey(gchar *userid,
                           gchar *key,
                           gboolean *OUT_result,
                           GError **error) {
  UsernamePasskey credentials(userid, SecureBlob(key, strlen(key)));

  MountTaskResult result;
  base::WaitableEvent event(true, false);
  MountTaskTestCredentials* mount_task =
      new MountTaskTestCredentials(NULL, mount_, credentials);
  mount_task->set_result(&result);
  mount_task->set_complete_event(&event);
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  event.Wait();
  *OUT_result = result.return_status();
  return TRUE;
}

gboolean Service::AsyncCheckKey(gchar *userid,
                                gchar *key,
                                gint *OUT_async_id,
                                GError **error) {
  UsernamePasskey credentials(userid, SecureBlob(key, strlen(key)));

  // Freed by the message loop
  MountTaskTestCredentials* mount_task = new MountTaskTestCredentials(
      this,
      mount_,
      credentials);
  *OUT_async_id = mount_task->sequence_id();
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  return TRUE;
}

gboolean Service::MigrateKey(gchar *userid,
                             gchar *from_key,
                             gchar *to_key,
                             gboolean *OUT_result,
                             GError **error) {
  UsernamePasskey credentials(userid, SecureBlob(to_key, strlen(to_key)));

  MountTaskResult result;
  base::WaitableEvent event(true, false);
  MountTaskMigratePasskey* mount_task =
      new MountTaskMigratePasskey(NULL, mount_, credentials, from_key);
  mount_task->set_result(&result);
  mount_task->set_complete_event(&event);
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  event.Wait();
  *OUT_result = result.return_status();
  return TRUE;
}

gboolean Service::AsyncMigrateKey(gchar *userid,
                                  gchar *from_key,
                                  gchar *to_key,
                                  gint *OUT_async_id,
                                  GError **error) {
  UsernamePasskey credentials(userid, SecureBlob(to_key, strlen(to_key)));

  MountTaskMigratePasskey* mount_task =
      new MountTaskMigratePasskey(this, mount_, credentials, from_key);
  *OUT_async_id = mount_task->sequence_id();
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  return TRUE;
}

gboolean Service::Remove(gchar *userid,
                         gboolean *OUT_result,
                         GError **error) {
  UsernamePasskey credentials(userid, chromeos::Blob());

  MountTaskResult result;
  base::WaitableEvent event(true, false);
  MountTaskRemove* mount_task =
      new MountTaskRemove(this, mount_, credentials);
  mount_task->set_result(&result);
  mount_task->set_complete_event(&event);
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  event.Wait();
  *OUT_result = result.return_status();
  return TRUE;
}

gboolean Service::AsyncRemove(gchar *userid,
                              gint *OUT_async_id,
                              GError **error) {
  UsernamePasskey credentials(userid, chromeos::Blob());

  MountTaskRemove* mount_task =
      new MountTaskRemove(this, mount_, credentials);
  *OUT_async_id = mount_task->sequence_id();
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  return TRUE;
}

gboolean Service::GetSystemSalt(GArray **OUT_salt, GError **error) {
  if (system_salt_.size() == 0) {
    mount_->GetSystemSalt(&system_salt_);
  }
  *OUT_salt = g_array_new(false, false, 1);
  g_array_append_vals(*OUT_salt, &system_salt_.front(), system_salt_.size());

  return TRUE;
}

gboolean Service::IsMounted(gboolean *OUT_is_mounted, GError **error) {
  *OUT_is_mounted = mount_->IsCryptohomeMounted();
  return TRUE;
}

gboolean Service::Mount(gchar *userid,
                        gchar *key,
                        gboolean create_if_missing,
                        gboolean deprecated_replace_tracked_subdirectories,
                        gchar** deprecated_tracked_subdirectories,
                        gint *OUT_error_code,
                        gboolean *OUT_result,
                        GError **error) {
  UsernamePasskey credentials(userid, SecureBlob(key, strlen(key)));

  if (mount_->IsCryptohomeMounted()) {
    if (mount_->IsCryptohomeMountedForUser(credentials)) {
      LOG(INFO) << "Cryptohome already mounted for this user";
      *OUT_error_code = Mount::MOUNT_ERROR_NONE;
      *OUT_result = TRUE;
      return TRUE;
    } else {
      if (!mount_->UnmountCryptohome()) {
        LOG(ERROR) << "Could not unmount cryptohome from previous user";
        *OUT_error_code = Mount::MOUNT_ERROR_MOUNT_POINT_BUSY;
        *OUT_result = FALSE;
        return TRUE;
      }
    }
  }

  MountTaskResult result;
  base::WaitableEvent event(true, false);
  Mount::MountArgs mount_args;
  mount_args.create_if_missing = create_if_missing;
  MountTaskMount* mount_task = new MountTaskMount(NULL,
                                                  mount_,
                                                  credentials,
                                                  mount_args);
  mount_task->set_result(&result);
  mount_task->set_complete_event(&event);
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  event.Wait();
  *OUT_error_code = result.return_code();
  *OUT_result = result.return_status();
  return TRUE;
}

gboolean Service::AsyncMount(gchar *userid,
                             gchar *key,
                             gboolean create_if_missing,
                             gboolean deprecated_replace_tracked_subdirectories,
                             gchar** deprecated_tracked_subdirectories,
                             gint *OUT_async_id,
                             GError **error) {
  UsernamePasskey credentials(userid, SecureBlob(key, strlen(key)));

  if (mount_->IsCryptohomeMounted()) {
    if (mount_->IsCryptohomeMountedForUser(credentials)) {
      LOG(INFO) << "Cryptohome already mounted for this user";
      MountTaskNop* mount_task = new MountTaskNop(this);
      mount_task->result()->set_return_code(Mount::MOUNT_ERROR_NONE);
      mount_task->result()->set_return_status(true);
      *OUT_async_id = mount_task->sequence_id();
      mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
      return TRUE;
    } else {
      if (!mount_->UnmountCryptohome()) {
        LOG(ERROR) << "Could not unmount cryptohome from previous user";
        MountTaskNop* mount_task = new MountTaskNop(this);
        mount_task->result()->set_return_code(
            Mount::MOUNT_ERROR_MOUNT_POINT_BUSY);
        mount_task->result()->set_return_status(false);
        *OUT_async_id = mount_task->sequence_id();
        mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
        return TRUE;
      }
    }
  }

  Mount::MountArgs mount_args;
  mount_args.create_if_missing = create_if_missing;
  MountTaskMount* mount_task = new MountTaskMount(this,
                                                  mount_,
                                                  credentials,
                                                  mount_args);
  *OUT_async_id = mount_task->sequence_id();
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  return TRUE;
}

gboolean Service::MountGuest(gint *OUT_error_code,
                             gboolean *OUT_result,
                             GError **error) {
  if (mount_->IsCryptohomeMounted()) {
    if (!mount_->UnmountCryptohome()) {
      LOG(ERROR) << "Could not unmount cryptohome from previous user";
      *OUT_error_code = Mount::MOUNT_ERROR_MOUNT_POINT_BUSY;
      *OUT_result = FALSE;
      return TRUE;
    }
  }

  MountTaskResult result;
  base::WaitableEvent event(true, false);
  MountTaskMountGuest* mount_task = new MountTaskMountGuest(NULL, mount_);
  mount_task->set_result(&result);
  mount_task->set_complete_event(&event);
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  event.Wait();
  *OUT_error_code = result.return_code();
  *OUT_result = result.return_status();
  return TRUE;
}

gboolean Service::AsyncMountGuest(gint *OUT_async_id,
                                  GError **error) {
  if (mount_->IsCryptohomeMounted()) {
    if (!mount_->UnmountCryptohome()) {
      LOG(ERROR) << "Could not unmount cryptohome from previous user";
      MountTaskNop* mount_task = new MountTaskNop(this);
      mount_task->result()->set_return_code(
          Mount::MOUNT_ERROR_MOUNT_POINT_BUSY);
      mount_task->result()->set_return_status(false);
      *OUT_async_id = mount_task->sequence_id();
      mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
      return TRUE;
    }
  }

  MountTaskMountGuest* mount_task = new MountTaskMountGuest(this, mount_);
  *OUT_async_id = mount_task->sequence_id();
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  return TRUE;
}

gboolean Service::Unmount(gboolean *OUT_result, GError **error) {
  if (mount_->IsCryptohomeMounted()) {
    *OUT_result = mount_->UnmountCryptohome();
  } else {
    *OUT_result = true;
  }
  return TRUE;
}

gboolean Service::RemoveTrackedSubdirectories(gboolean *OUT_result,
                                              GError **error) {
  MountTaskResult result;
  base::WaitableEvent event(true, false);
  MountTaskRemoveTrackedSubdirectories* mount_task =
      new MountTaskRemoveTrackedSubdirectories(this, mount_);
  mount_task->set_result(&result);
  mount_task->set_complete_event(&event);
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  event.Wait();
  *OUT_result = result.return_status();
  return TRUE;
}

gboolean Service::AsyncRemoveTrackedSubdirectories(gint *OUT_async_id,
                                                   GError **error) {
  MountTaskRemoveTrackedSubdirectories* mount_task =
      new MountTaskRemoveTrackedSubdirectories(this, mount_);
  *OUT_async_id = mount_task->sequence_id();
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  return TRUE;
}

gboolean Service::DoAutomaticFreeDiskSpaceControl(gboolean *OUT_result,
                                                  GError **error) {
  MountTaskResult result;
  base::WaitableEvent event(true, false);
  MountTaskAutomaticFreeDiskSpace* mount_task =
      new MountTaskAutomaticFreeDiskSpace(this, mount_);
  mount_task->set_result(&result);
  mount_task->set_complete_event(&event);
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  event.Wait();
  *OUT_result = result.return_status();
  return TRUE;
}

gboolean Service::AsyncDoAutomaticFreeDiskSpaceControl(gint *OUT_async_id,
                                                       GError **error) {
  MountTaskAutomaticFreeDiskSpace* mount_task =
      new MountTaskAutomaticFreeDiskSpace(this, mount_);
  *OUT_async_id = mount_task->sequence_id();
  mount_thread_.message_loop()->PostTask(FROM_HERE, mount_task);
  return TRUE;
}

gboolean Service::TpmIsReady(gboolean* OUT_ready, GError** error) {
  *OUT_ready = tpm_init_->IsTpmReady();
  return TRUE;
}

gboolean Service::TpmIsEnabled(gboolean* OUT_enabled, GError** error) {
  *OUT_enabled = tpm_init_->IsTpmEnabled();
  return TRUE;
}

gboolean Service::TpmGetPassword(gchar** OUT_password, GError** error) {
  SecureBlob password;
  if (!tpm_init_->GetTpmPassword(&password)) {
    *OUT_password = NULL;
    return TRUE;
  }
  *OUT_password = g_strdup_printf("%.*s", password.size(),
                                  static_cast<char*>(password.data()));
  return TRUE;
}

gboolean Service::TpmIsOwned(gboolean* OUT_owned, GError** error) {
  *OUT_owned = tpm_init_->IsTpmOwned();
  return TRUE;
}

gboolean Service::TpmIsBeingOwned(gboolean* OUT_owning, GError** error) {
  *OUT_owning = tpm_init_->IsTpmBeingOwned();
  return TRUE;
}

gboolean Service::TpmCanAttemptOwnership(GError** error) {
  if (!tpm_init_->HasInitializeBeenCalled()) {
    tpm_init_->StartInitializeTpm();
  }
  return TRUE;
}

gboolean Service::TpmClearStoredPassword(GError** error) {
  tpm_init_->ClearStoredTpmPassword();
  return TRUE;
}

gboolean Service::GetStatusString(gchar** OUT_status, GError** error) {
  Tpm::TpmStatusInfo tpm_status;
  mount_->get_crypto()->EnsureTpm(false);
  Tpm* tpm = const_cast<Tpm*>(mount_->get_crypto()->get_tpm());

  if (tpm) {
    tpm->GetStatus(true, &tpm_status);
  } else {
    Tpm::GetSingleton()->GetStatus(true, &tpm_status);
  }

  if (tpm_init_) {
    tpm_status.Enabled = tpm_init_->IsTpmEnabled();
    tpm_status.BeingOwned = tpm_init_->IsTpmBeingOwned();
    tpm_status.Owned = tpm_init_->IsTpmOwned();
  }

  std::string user_data;
  UserSession* session = mount_->get_current_user();
  if (session) {
    do {
      std::string obfuscated_user;
      session->GetObfuscatedUsername(&obfuscated_user);
      if (!obfuscated_user.length()) {
        break;
      }
      std::string vault_path = StringPrintf("%s/%s/master.0",
                                            mount_->get_shadow_root().c_str(),
                                            obfuscated_user.c_str());
      FilePath vault_file(vault_path);
      file_util::FileInfo file_info;
      if(!file_util::GetFileInfo(vault_file, &file_info)) {
        break;
      }
      SecureBlob contents;
      if (!Mount::LoadFileBytes(vault_file, &contents)) {
        break;
      }
      cryptohome::SerializedVaultKeyset serialized;
      if (!serialized.ParseFromArray(
          static_cast<const unsigned char*>(&contents[0]), contents.size())) {
        break;
      }
      base::Time::Exploded exploded;
      file_info.last_modified.UTCExplode(&exploded);
      user_data = StringPrintf(
          "User Session:\n"
          "  Keyset Was TPM Wrapped..........: %s\n"
          "  Keyset Was Scrypt Wrapped.......: %s\n"
          "  Keyset Last Modified............: %02d-%02d-%04d %02d:%02d:%02d"
          " (UTC)\n",
          ((serialized.flags() &
            cryptohome::SerializedVaultKeyset::TPM_WRAPPED) ? "1" : "0"),
          ((serialized.flags() &
            cryptohome::SerializedVaultKeyset::SCRYPT_WRAPPED) ? "1" : "0"),
          exploded.month, exploded.day_of_month, exploded.year,
          exploded.hour, exploded.minute, exploded.second);

    } while(false);
  }

  *OUT_status = g_strdup_printf(
      "TPM Status:\n"
      "  Enabled.........................: %s\n"
      "  Owned...........................: %s\n"
      "  Being Owned.....................: %s\n"
      "  Can Connect.....................: %s\n"
      "  Can Load SRK....................: %s\n"
      "  Can Load SRK Public.............: %s\n"
      "  Has Cryptohome Key..............: %s\n"
      "  Can Encrypt.....................: %s\n"
      "  Can Decrypt.....................: %s\n"
      "  Instance Context................: %s\n"
      "  Instance Key Handle.............: %s\n"
      "  Last Error......................: %08x\n"
      "%s"
      "Mount Status:\n"
      "  Vault Is Mounted................: %s\n",
      (tpm_status.Enabled ? "1" : "0"),
      (tpm_status.Owned ? "1" : "0"),
      (tpm_status.BeingOwned ? "1" : "0"),
      (tpm_status.CanConnect ? "1" : "0"),
      (tpm_status.CanLoadSrk ? "1" : "0"),
      (tpm_status.CanLoadSrkPublicKey ? "1" : "0"),
      (tpm_status.HasCryptohomeKey ? "1" : "0"),
      (tpm_status.CanEncrypt ? "1" : "0"),
      (tpm_status.CanDecrypt ? "1" : "0"),
      (tpm_status.ThisInstanceHasContext ? "1" : "0"),
      (tpm_status.ThisInstanceHasKeyHandle ? "1" : "0"),
      tpm_status.LastTpmError,
      user_data.c_str(),
      (mount_->IsCryptohomeMounted() ? "1" : "0"));
  return TRUE;
}

}  // namespace cryptohome
