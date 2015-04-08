// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "psyche/psyched/germ_connection.h"

#include <base/logging.h>
#include <germ/constants.h>
#include <protobinder/binder_proxy.h>
#include <protobinder/iinterface.h>

#include "psyche/proto_bindings/germ.pb.h"
#include "psyche/proto_bindings/germ.pb.rpc.h"

using protobinder::BinderProxy;
using protobinder::BinderToInterface;

using germ::IGerm;

namespace psyche {

// static
const char* GermConnection::ResultToString(Result result) {
  switch (result) {
    case Result::SUCCESS:
      return "SUCCESS";
    case Result::NO_CONNECTION:
      return "NO_CONNECTION";
    case Result::RPC_ERROR:
      return "RPC_ERROR";
    case Result::LAUNCH_ERROR:
      return "LAUNCH_ERROR";
  }
  NOTREACHED() << "Invalid result " << static_cast<int>(result);
  return "INVALID";
}

GermConnection::GermConnection() : service_(germ::kGermServiceName) {
  service_.AddObserver(this);
}

GermConnection::~GermConnection() {
  service_.RemoveObserver(this);
}

void GermConnection::SetProxy(std::unique_ptr<protobinder::BinderProxy> proxy) {
  // TODO(mcolagrosso): Verify that the transaction is coming from the proper
  // UID and report failure if not. See http://brbug.com/787.
  service_.SetProxy(std::move(proxy));
}

void GermConnection::OnServiceProxyChange(ServiceInterface* service) {
  DCHECK_EQ(service, &service_);
  if (service->GetProxy()) {
    LOG(INFO) << "Got connection to germd";
    interface_.reset(BinderToInterface<IGerm>(service->GetProxy()));
  } else {
    LOG(WARNING) << "Lost connection to germd";
    interface_.reset();
  }
}

}  // namespace psyche
