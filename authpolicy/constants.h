// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AUTHPOLICY_CONSTANTS_H_
#define AUTHPOLICY_CONSTANTS_H_

namespace authpolicy {

// Commands for the parser.
const char kCmdParseRealmInfo[] = "parse_realm_info";
const char kCmdParseWorkgroup[] = "parse_workgroup";
const char kCmdParseAccountInfo[] = "parse_account_info";
const char kCmdParseUserGpoList[] = "parse_user_gpo_list";
const char kCmdParseDeviceGpoList[] = "parse_device_gpo_list";
const char kCmdParseUserPreg[] = "parse_user_preg";
const char kCmdParseDevicePreg[] = "parse_device_preg";

enum ExitCodes {
  EXIT_CODE_OK = 0,
  EXIT_CODE_BAD_COMMAND,
  EXIT_CODE_FIND_TOKEN_FAILED,
  EXIT_CODE_READ_INPUT_FAILED,
  EXIT_CODE_PARSE_INPUT_FAILED,
  EXIT_CODE_WRITE_OUTPUT_FAILED,
};

enum class PolicyScope {
  USER,     // User policy
  MACHINE,  // Machine/device policy
};

}  // namespace authpolicy

#endif  // AUTHPOLICY_CONSTANTS_H_
