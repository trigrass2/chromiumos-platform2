// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EASY_UNLOCK_FAKE_EASY_UNLOCK_SERVICE_H_
#define EASY_UNLOCK_FAKE_EASY_UNLOCK_SERVICE_H_

#include <vector>

#include "easy-unlock/easy_unlock_service.h"

namespace easy_unlock {

// EasyUnlock service to be used in unittests.
class FakeService : public Service {
 public:
  FakeService();
  virtual ~FakeService();

  // easy_unlock::Service overrides:
  virtual void GenerateEcP256KeyPair(std::vector<uint8_t>* private_key,
                                     std::vector<uint8_t>* public_key) OVERRIDE;
  virtual std::vector<uint8_t> PerformECDHKeyAgreement(
      const std::vector<uint8_t>& private_key,
      const std::vector<uint8_t>& public_key) OVERRIDE;
  virtual std::vector<uint8_t> CreateSecureMessage(
      const std::vector<uint8_t>& payload,
      const std::vector<uint8_t>& key,
      const std::vector<uint8_t>& associated_data,
      const std::vector<uint8_t>& public_metadata,
      const std::vector<uint8_t>& verification_key_id,
      easy_unlock_crypto::ServiceImpl::EncryptionType encryption_type,
      easy_unlock_crypto::ServiceImpl::SignatureType signature_type) OVERRIDE;
  virtual std::vector<uint8_t> UnwrapSecureMessage(
      const std::vector<uint8_t>& secure_message,
      const std::vector<uint8_t>& key,
      const std::vector<uint8_t>& associated_data,
      easy_unlock_crypto::ServiceImpl::EncryptionType encryption_type,
      easy_unlock_crypto::ServiceImpl::SignatureType signature_type) OVERRIDE;

 private:
  int private_key_count_;
  int public_key_count_;

  DISALLOW_COPY_AND_ASSIGN(FakeService);
};

}  // namespace easy_unlock

#endif  // EASY_UNLOCK_FAKE_EASY_UNLOCK_SERVICE_H_
