// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/openvpn_driver.h"

#include <base/logging.h>
#include <chromeos/dbus/service_constants.h>

#include "shill/error.h"

using std::string;
using std::vector;

namespace shill {

OpenVPNDriver::OpenVPNDriver(const KeyValueStore &args)
    : args_(args) {}

OpenVPNDriver::~OpenVPNDriver() {}

void OpenVPNDriver::Connect(Error *error) {
  error->Populate(Error::kNotSupported);
}

void OpenVPNDriver::InitOptions(vector<string> *options, Error *error) {
  string vpnhost;
  if (args_.ContainsString(flimflam::kProviderHostProperty)) {
    vpnhost = args_.GetString(flimflam::kProviderHostProperty);
  }
  if (vpnhost.empty()) {
    Error::PopulateAndLog(
        error, Error::kInvalidArguments, "VPN host not specified.");
    return;
  }
  options->push_back("--client");
  options->push_back("--tls-client");
  options->push_back("--remote");
  options->push_back(vpnhost);
  options->push_back("--nobind");
  options->push_back("--persist-key");
  options->push_back("--persist-tun");

  // TODO(petkov): Add "--dev <interface_name>". For OpenVPN, the interface will
  // be the tunnel device (crosbug.com/26841).
  options->push_back("--dev-type");
  options->push_back("tun");
  options->push_back("--syslog");

  // TODO(petkov): Enable verbosity based on shill logging options too.
  AppendValueOption("OpenVPN.Verb", "--verb", options);

  AppendValueOption("VPN.MTU", "--mtu", options);
  AppendValueOption(flimflam::kOpenVPNProtoProperty, "--proto", options);
  AppendValueOption(flimflam::kOpenVPNPortProperty, "--port", options);
  AppendValueOption("OpenVPN.TLSAuth", "--tls-auth", options);

  // TODO(petkov): Implement this.
  LOG_IF(ERROR, args_.ContainsString(flimflam::kOpenVPNTLSAuthContentsProperty))
      << "Support for --tls-auth not implemented yet.";

  AppendValueOption(
      flimflam::kOpenVPNTLSRemoteProperty, "--tls-remote", options);
  AppendValueOption(flimflam::kOpenVPNCipherProperty, "--cipher", options);
  AppendValueOption(flimflam::kOpenVPNAuthProperty, "--auth", options);
  AppendFlag(flimflam::kOpenVPNAuthNoCacheProperty, "--auth-nocache", options);
  AppendValueOption(
      flimflam::kOpenVPNAuthRetryProperty, "--auth-retry", options);
  AppendFlag(flimflam::kOpenVPNCompLZOProperty, "--comp-lzo", options);
  AppendFlag(flimflam::kOpenVPNCompNoAdaptProperty, "--comp-noadapt", options);
  AppendFlag(
      flimflam::kOpenVPNPushPeerInfoProperty, "--push-peer-info", options);
  AppendValueOption(flimflam::kOpenVPNRenegSecProperty, "--reneg-sec", options);
  AppendValueOption(flimflam::kOpenVPNShaperProperty, "--shaper", options);
  AppendValueOption(flimflam::kOpenVPNServerPollTimeoutProperty,
                    "--server-poll-timeout", options);

  // TODO(petkov): Implement this.
  LOG_IF(ERROR, args_.ContainsString(flimflam::kOpenVPNCaCertNSSProperty))
      << "Support for NSS CA not implemented yet.";

  // Client-side ping support.
  AppendValueOption("OpenVPN.Ping", "--ping", options);
  AppendValueOption("OpenVPN.PingExit", "--ping-exit", options);
  AppendValueOption("OpenVPN.PingRestart", "--ping-restart", options);

  AppendValueOption(flimflam::kOpenVPNCaCertProperty, "--ca", options);
  AppendValueOption("OpenVPN.Cert", "--cert", options);
  AppendValueOption(
      flimflam::kOpenVPNNsCertTypeProperty, "--ns-cert-type", options);
  AppendValueOption("OpenVPN.Key", "--key", options);

  // TODO(petkov): Implement this.
  LOG_IF(ERROR, args_.ContainsString(flimflam::kOpenVPNClientCertIdProperty))
      << "Support for PKCS#11 (--pkcs11-id and --pkcs11-providers) "
      << "not implemented yet.";

  // TLS suport.
  string remote_cert_tls;
  if (args_.ContainsString(flimflam::kOpenVPNRemoteCertTLSProperty)) {
    remote_cert_tls = args_.GetString(flimflam::kOpenVPNRemoteCertTLSProperty);
  }
  if (remote_cert_tls.empty()) {
    remote_cert_tls = "server";
  }
  if (remote_cert_tls != "none") {
    options->push_back("--remote-cert-tls");
    options->push_back(remote_cert_tls);
  }

  // This is an undocumented command line argument that works like a .cfg file
  // entry. TODO(sleffler): Maybe roll this into --tls-auth?
  AppendValueOption(
      flimflam::kOpenVPNKeyDirectionProperty, "--key-direction", options);
  // TODO(sleffler): Support more than one eku parameter.
  AppendValueOption(
      flimflam::kOpenVPNRemoteCertEKUProperty, "--remote-cert-eku", options);
  AppendValueOption(
      flimflam::kOpenVPNRemoteCertKUProperty, "--remote-cert-ku", options);

  // TODO(petkov): Setup management control channel and add the approprate
  // options (crosbug.com/26994).

  // TODO(petkov): Setup openvpn-script options and DBus info required to send
  // back Layer 3 configuration (crosbug.com/26993).

  // Disable openvpn handling since we do route+ifconfig work.
  options->push_back("--route-noexec");
  options->push_back("--ifconfig-noexec");

  // Drop root privileges on connection and enable callback scripts to send
  // notify messages.
  options->push_back("--user");
  options->push_back("openvpn");
  options->push_back("--group");
  options->push_back("openvpn");
}

void OpenVPNDriver::AppendValueOption(
    const string &property, const string &option, vector<string> *options) {
  string value;
  if (args_.ContainsString(property)) {
    value = args_.GetString(property);
  }
  if (!value.empty()) {
    options->push_back(option);
    options->push_back(value);
  }
}

void OpenVPNDriver::AppendFlag(
    const string &property, const string &option, vector<string> *options) {
  if (args_.ContainsString(property)) {
    options->push_back(option);
  }
}

}  // namespace shill
