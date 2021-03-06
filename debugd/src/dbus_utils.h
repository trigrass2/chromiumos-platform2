// Copyright (c) 2009-2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEBUGD_SRC_DBUS_UTILS_H_
#define DEBUGD_SRC_DBUS_UTILS_H_

#include <map>
#include <memory>
#include <string>

#include <base/values.h>

namespace base {
class Value;
}  // namespace base

namespace DBus {
class Message;
class Variant;
}  // namespace DBus

namespace debugd {

// Convert a DBus message into a Value.
//
// Parameters
//  message - Message to convert.
//  result - Result pointer.
// Returns
//  True if conversion succeeded, false if it didn't.
bool DBusMessageToValue(const DBus::Message& message,
                        std::unique_ptr<base::Value>* result);

// Convert a DBus property map to a Value.
//
// Parameters
//  message - Message to convert.
//  result - Result pointer.
// Returns
//  True if conversion succeeded, false if it didn't.
bool DBusPropertyMapToValue(
    const std::map<std::string, DBus::Variant>& properties,
    std::unique_ptr<base::Value>* result);

}  // namespace debugd


#endif  // DEBUGD_SRC_DBUS_UTILS_H_
