// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unistd.h>

#include <memory>
#include <utility>

#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <brillo/flag_helper.h>
#include <brillo/syslog_logging.h>

#include "arc-networkd/helper_process.h"
#include "arc-networkd/ip_helper.h"
#include "arc-networkd/manager.h"
#include "arc-networkd/options.h"

int main(int argc, char* argv[]) {
  DEFINE_bool(log_to_stderr, false, "Log to both syslog and stderr");
  DEFINE_string(internal_interface, "br0",
                "Name of the host interface that connects to the guest");
  DEFINE_string(container_interface, "arc0",
                "Name of the guest interface that connects to the host");
  DEFINE_int32(con_netns, 0, "Container's network namespace (PID)");
  DEFINE_int32(
      ip_helper_fd,
      -1,
      "Control socket for starting an IpHelper subprocess. Used internally.");

  brillo::FlagHelper::Init(argc, argv, "ARC network daemon");

  int flags = brillo::kLogToSyslog | brillo::kLogHeader;
  if (FLAGS_log_to_stderr)
    flags |= brillo::kLogToStderr;
  brillo::InitLog(flags);

  arc_networkd::Options opt;
  opt.int_ifname = FLAGS_internal_interface;
  opt.con_ifname = FLAGS_container_interface;
  opt.con_netns = FLAGS_con_netns;

  if (FLAGS_ip_helper_fd >= 0) {
    base::ScopedFD fd(FLAGS_ip_helper_fd);
    arc_networkd::IpHelper ip_helper{opt, std::move(fd)};
    return ip_helper.Run();
  } else {
    std::unique_ptr<arc_networkd::HelperProcess> ip_helper(
        new arc_networkd::HelperProcess());
    ip_helper->Start(argc, argv, "--ip_helper_fd");

    arc_networkd::Manager manager{opt, std::move(ip_helper)};
    return manager.Run();
  }
}
