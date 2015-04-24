// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ATTESTATION_SERVER_CRYPTO_UTILITY_IMPL_H_
#define ATTESTATION_SERVER_CRYPTO_UTILITY_IMPL_H_

#include "attestation/server/crypto_utility.h"

#include <string>

#include "attestation/server/tpm_utility.h"

namespace attestation {

// An implementation of CryptoUtility.
class CryptoUtilityImpl : public CryptoUtility {
 public:
  // Does not take ownership of pointers.
  explicit CryptoUtilityImpl(TpmUtility* tpm_utility);
  ~CryptoUtilityImpl() override;

  // CryptoUtility methods.
  bool GetRandom(size_t num_bytes, std::string* random_data) const override;
  bool CreateSealedKey(std::string* aes_key, std::string* sealed_key) override;
  bool EncryptData(const std::string& data,
                   const std::string& aes_key,
                   const std::string& sealed_key,
                   std::string* encrypted_data) override;
  bool UnsealKey(const std::string& encrypted_data,
                 std::string* aes_key,
                 std::string* sealed_key) override;
  bool DecryptData(const std::string& encrypted_data,
                   const std::string& aes_key,
                   std::string* data) override;

 private:
  // Encrypts |data| using |key| and |iv| for AES in CBC mode with PKCS #5
  // padding and produces the |encrypted_data|. Returns true on success.
  bool AesEncrypt(const std::string& data,
                  const std::string& key,
                  const std::string& iv,
                  std::string* encrypted_data);

  // Decrypts |encrypted_data| using |key| and |iv| for AES in CBC mode with
  // PKCS #5 padding and produces the decrypted |data|. Returns true on success.
  bool AesDecrypt(const std::string& encrypted_data,
                  const std::string& key,
                  const std::string& iv,
                  std::string* data);

  // Computes and returns an HMAC of |data| using |key| and SHA-512.
  std::string HmacSha512(const std::string& data, const std::string& key);

  TpmUtility* tpm_utility_;
};

}  // namespace attestation

#endif  // ATTESTATION_SERVER_CRYPTO_UTILITY_IMPL_H_