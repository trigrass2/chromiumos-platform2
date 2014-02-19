// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_PLATFORM_UPDATE_ENGINE_POLICY_MANAGER_MOCK_POLICY_H
#define CHROMEOS_PLATFORM_UPDATE_ENGINE_POLICY_MANAGER_MOCK_POLICY_H

#include <gmock/gmock.h>

#include "update_engine/policy_manager/policy.h"

namespace chromeos_policy_manager {

// A mocked implementation of Policy.
class MockPolicy : public Policy {
public:
  MockPolicy() {}
  virtual ~MockPolicy() {}

  // Policy overrides.
  MOCK_CONST_METHOD3(UpdateCheckAllowed,
                     EvalStatus(EvaluationContext*, std::string*, bool*));

private:
  DISALLOW_COPY_AND_ASSIGN(MockPolicy);
};

}  // namespace chromeos_policy_manager

#endif  // CHROMEOS_PLATFORM_UPDATE_ENGINE_POLICY_MANAGER_MOCK_POLICY_H
