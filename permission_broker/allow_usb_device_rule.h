// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERMISSION_BROKER_ALLOW_USB_DEVICE_RULE_H_
#define PERMISSION_BROKER_ALLOW_USB_DEVICE_RULE_H_

#include "permission_broker/usb_subsystem_udev_rule.h"

namespace permission_broker {

// AllowUsbDeviceRule encapsulates the policy that USB devices are allowed to be
// accessed. Any path passed to it that is owned by a device on the USB
// subsystem is |ALLOW|ed. All other paths are ignored.
class AllowUsbDeviceRule : public UsbSubsystemUdevRule {
 public:
  AllowUsbDeviceRule();
  ~AllowUsbDeviceRule() override = default;

  Result ProcessUsbDevice(struct udev_device *device) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(AllowUsbDeviceRule);
};

}  // namespace permission_broker

#endif  // PERMISSION_BROKER_ALLOW_USB_DEVICE_RULE_H_
