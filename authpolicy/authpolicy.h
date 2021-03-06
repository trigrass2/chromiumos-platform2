// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AUTHPOLICY_AUTHPOLICY_H_
#define AUTHPOLICY_AUTHPOLICY_H_

#include <memory>
#include <string>
#include <vector>

#include <brillo/dbus/async_event_sequencer.h>
#include <dbus/object_proxy.h>

#include "authpolicy/authpolicy_metrics.h"
#include "authpolicy/org.chromium.AuthPolicy.h"
#include "authpolicy/samba_interface.h"

using brillo::dbus_utils::AsyncEventSequencer;

namespace authpolicy {

class AuthPolicyMetrics;
class PathService;

class AuthPolicy : public org::chromium::AuthPolicyAdaptor,
                   public org::chromium::AuthPolicyInterface {
 public:
  using PolicyResponseCallback =
      std::unique_ptr<brillo::dbus_utils::DBusMethodResponse<int32_t>>;

  // Helper method to get the D-Bus object for the given |object_manager|.
  static std::unique_ptr<brillo::dbus_utils::DBusObject> GetDBusObject(
      brillo::dbus_utils::ExportedObjectManager* object_manager);

  AuthPolicy(std::unique_ptr<brillo::dbus_utils::DBusObject> dbus_object,
             std::unique_ptr<AuthPolicyMetrics> metrics,
             std::unique_ptr<PathService> path_service);
  ~AuthPolicy() override = default;

  // Initializes internals. See SambaInterface::Initialize() for details.
  ErrorType Initialize(bool expect_config);

  // Register the D-Bus object and interfaces.
  void RegisterAsync(
      const AsyncEventSequencer::CompletionAction& completion_callback);

  // org::chromium::AuthPolicyInterface: (see org.chromium.AuthPolicy.xml).
  // |account_data_blob| is a serialized ActiveDirectoryAccountData protobuf.
  void AuthenticateUser(const std::string& user_principal_name,
                        const dbus::FileDescriptor& password_fd,
                        int32_t* error,
                        std::vector<uint8_t>* account_data_blob) override;

  int32_t JoinADDomain(const std::string& machine_name,
                       const std::string& user_principal_name,
                       const dbus::FileDescriptor& password_fd) override;

  void RefreshUserPolicy(PolicyResponseCallback callback,
                         const std::string& account_id) override;

  void RefreshDevicePolicy(PolicyResponseCallback callback) override;

 private:
  // Sends policy to SessionManager. Assumes |policy_blob| contains user policy
  // if account_id is not nullptr, otherwise assumes it's device policy.
  void StorePolicy(const std::string& policy_blob,
                   const std::string* account_id,
                   std::unique_ptr<ScopedTimerReporter> timer,
                   PolicyResponseCallback callback);

  // Response callback from SessionManager, logs the result and calls callback.
  void OnPolicyStored(bool is_user_policy,
                      std::unique_ptr<ScopedTimerReporter> timer,
                      PolicyResponseCallback callback,
                      dbus::Response* response);

  std::unique_ptr<AuthPolicyMetrics> metrics_;
  SambaInterface samba_;
  std::unique_ptr<brillo::dbus_utils::DBusObject> dbus_object_;
  dbus::ObjectProxy* session_manager_proxy_ = nullptr;
  base::WeakPtrFactory<AuthPolicy> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(AuthPolicy);
};

}  // namespace authpolicy

#endif  // AUTHPOLICY_AUTHPOLICY_H_
