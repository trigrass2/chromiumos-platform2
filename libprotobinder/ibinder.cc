// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "libprotobinder/ibinder.h"

#include <stddef.h>

namespace protobinder {

IBinder::IBinder() {
}
IBinder::~IBinder() {
}

BinderHost* IBinder::GetBinderHost() {
  return NULL;
}

BinderProxy* IBinder::GetBinderProxy() {
  return NULL;
}

}  // namespace protobinder