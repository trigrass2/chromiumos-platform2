// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "webserver/webservd/protocol_handler.h"

#include <limits>
#include <microhttpd.h>

#include <base/bind.h>
#include <base/guid.h>
#include <base/logging.h>
#include <base/message_loop/message_loop.h>

#include "webserver/webservd/request.h"
#include "webserver/webservd/request_handler_interface.h"
#include "webserver/webservd/server_interface.h"

namespace webservd {

const char ProtocolHandler::kHttp[] = "http";
const char ProtocolHandler::kHttps[] = "https";

// Helper class to provide static callback methods to libmicrohttpd library,
// with the ability to access private methods of Server class.
class ServerHelper {
 public:
  static int ConnectionHandler(void *cls,
                               MHD_Connection* connection,
                               const char* url,
                               const char* method,
                               const char* version,
                               const char* upload_data,
                               size_t* upload_data_size,
                               void** con_cls) {
    auto handler = reinterpret_cast<ProtocolHandler*>(cls);
    if (nullptr == *con_cls) {
      std::string request_handler_id = handler->FindRequestHandler(url, method);
      std::unique_ptr<Request> request{new Request{
          request_handler_id, url, method, connection, handler
      }};
      if (!request->BeginRequestData())
        return MHD_NO;

      // Pass the raw pointer here in order to interface with libmicrohttpd's
      // old-style C API.
      *con_cls = request.release();
    } else {
      auto request = reinterpret_cast<Request*>(*con_cls);
      if (*upload_data_size) {
        if (!request->AddRequestData(upload_data, *upload_data_size))
          return MHD_NO;
        *upload_data_size = 0;
      } else {
        request->EndRequestData();
      }
    }
    return MHD_YES;
  }

  static void RequestCompleted(void* cls,
                               MHD_Connection* connection,
                               void** con_cls,
                               MHD_RequestTerminationCode toe) {
    auto request = reinterpret_cast<Request*>(*con_cls);
    *con_cls = nullptr;
    delete request;
  }
};

ProtocolHandler::ProtocolHandler(const std::string& id,
                                 ServerInterface* server_interface)
    : id_{id}, server_interface_{server_interface} {
  if (id_.empty())
    id_ = base::GenerateGUID();
}

ProtocolHandler::~ProtocolHandler() {
  Stop();
}

std::string ProtocolHandler::AddRequestHandler(
    const std::string& url,
    const std::string& method,
    std::unique_ptr<RequestHandlerInterface> handler) {
  std::string handler_id = base::GenerateGUID();
  request_handlers_.emplace(handler_id,
                            HandlerMapEntry{url, method, std::move(handler)});
  return handler_id;
}

bool ProtocolHandler::RemoveRequestHandler(const std::string& handler_id) {
  return request_handlers_.erase(handler_id) == 1;
}

std::string ProtocolHandler::FindRequestHandler(
    const base::StringPiece& url,
    const base::StringPiece& method) const {
  size_t score = std::numeric_limits<size_t>::max();
  std::string handler_id;
  for (const auto& pair : request_handlers_) {
    std::string handler_url = pair.second.url;
    bool url_match = (handler_url == url);
    bool method_match = (pair.second.method == method);

    // Try exact match first. If everything matches, we have our handler.
    if (url_match && method_match)
      return pair.first;

    // Calculate the current handler's similarity score. The lower the score
    // the better the match is...
    size_t current_score = 0;
    if (!url_match && !handler_url.empty() && handler_url.back() == '/') {
      if (url.starts_with(handler_url)) {
        url_match = true;
        // Use the difference in URL length as URL match quality proxy.
        // The longer URL, the more specific (better) match is.
        // Multiply by 2 to allow for extra score point for matching the method.
        current_score = (url.size() - handler_url.size()) * 2;
      }
    }

    if (!method_match && pair.second.method.empty()) {
      // If the handler didn't specify the method it handles, this means
      // it doesn't care. However this isn't the exact match, so bump
      // the score up one point.
      method_match = true;
      ++current_score;
    }

    if (url_match && method_match && current_score < score) {
      score = current_score;
      handler_id = pair.first;
    }
  }

  return handler_id;
}

bool ProtocolHandler::Start(uint16_t port) {
  return StartWithTLS(port, {}, {}, {});
}

bool ProtocolHandler::StartWithTLS(
    uint16_t port,
    const chromeos::SecureBlob& private_key,
    const chromeos::Blob& certificate,
    const chromeos::Blob& certificate_fingerprint) {
  if (server_) {
    LOG(ERROR) << "Web server is already running.";
    return false;
  }

  // Either both keys and certificate must be specified or both must be omitted.
  CHECK_EQ(private_key.empty(), certificate.empty());
  // Same with cert fingerprint.
  CHECK_EQ(certificate.empty(), certificate_fingerprint.empty());

  const bool use_tls = !private_key.empty();

  LOG(INFO) << "Starting " << (use_tls ? "HTTPS" : "HTTP")
            << " Server on port: " << port;

  port_ = port;
  protocol_ = (use_tls ? "https" : "http");
  certificate_fingerprint_ = certificate_fingerprint;

  auto callback_addr =
      reinterpret_cast<intptr_t>(&ServerHelper::RequestCompleted);
  uint32_t flags = MHD_NO_FLAG;
  if (server_interface_->UseDebugInfo())
    flags |= MHD_USE_DEBUG;

  std::vector<MHD_OptionItem> options{
    {MHD_OPTION_CONNECTION_LIMIT, 10, nullptr},
    {MHD_OPTION_CONNECTION_TIMEOUT, 60, nullptr},
    {MHD_OPTION_NOTIFY_COMPLETED, callback_addr, nullptr},
  };

  // libmicrohttpd expects both the key and certificate to be zero-terminated
  // strings. Make sure they are terminated properly.
  chromeos::SecureBlob private_key_copy = private_key;
  chromeos::Blob certificate_copy = certificate;
  private_key_copy.push_back(0);
  certificate_copy.push_back(0);

  if (use_tls) {
    flags |= MHD_USE_SSL;
    options.push_back(
        MHD_OptionItem{MHD_OPTION_HTTPS_MEM_KEY, 0, private_key_copy.data()});
    options.push_back(
        MHD_OptionItem{MHD_OPTION_HTTPS_MEM_CERT, 0, certificate_copy.data()});
  }

  options.push_back(MHD_OptionItem{MHD_OPTION_END, 0, nullptr});

  server_ = MHD_start_daemon(flags, port, nullptr, nullptr,
                             &ServerHelper::ConnectionHandler, this,
                             MHD_OPTION_ARRAY, options.data(), MHD_OPTION_END);
  if (!server_) {
    LOG(ERROR) << "Failed to start the web server on port " << port;
    return false;
  }
  server_interface_->ProtocolHandlerStarted(this);
  DoWork();
  LOG(INFO) << "Server started";
  return true;
}

bool ProtocolHandler::Stop() {
  if (server_) {
    LOG(INFO) << "Shutting down the web server...";
    MHD_stop_daemon(server_);
    server_ = nullptr;
    server_interface_->ProtocolHandlerStopped(this);
    LOG(INFO) << "Server shutdown complete";
  }
  port_ = 0;
  protocol_.clear();
  certificate_fingerprint_.clear();
  return true;
}

void ProtocolHandler::AddRequest(Request* request) {
  requests_.emplace(request->GetID(), request);
}

void ProtocolHandler::RemoveRequest(Request* request) {
  requests_.erase(request->GetID());
}

Request* ProtocolHandler::GetRequest(const std::string& request_id) const {
  auto p = requests_.find(request_id);
  return (p != requests_.end()) ? p->second : nullptr;
}

// A file descriptor watcher class that oversees I/O operation notification
// on particular socket file descriptor.
class ProtocolHandler::Watcher final
    : public base::MessageLoopForIO::Watcher{
 public:
  Watcher(ProtocolHandler* handler,
          int fd,
          base::MessageLoopForIO::Mode mode,
          base::MessageLoopForIO* message_loop)
      : handler_{handler} {
    message_loop->WatchFileDescriptor(fd, false, mode, &controller_, this);
  }

  // Overrides from base::MessageLoopForIO::Watcher.
  void OnFileCanReadWithoutBlocking(int fd) override {
    handler_->DoWork();
  }

  void OnFileCanWriteWithoutBlocking(int fd) override {
    handler_->DoWork();
  }

 private:
  ProtocolHandler* handler_;
  base::MessageLoopForIO::FileDescriptorWatcher controller_;

  DISALLOW_COPY_AND_ASSIGN(Watcher);
};

void ProtocolHandler::OnResponseDataReceived() {
  base::MessageLoopForIO::current()->PostTask(
      FROM_HERE,
      base::Bind(&ProtocolHandler::DoWork, weak_ptr_factory_.GetWeakPtr()));
}

void ProtocolHandler::DoWork() {
  base::MessageLoopForIO* message_loop = base::MessageLoopForIO::current();

  // Remove the old watchers first.
  watchers_.clear();

  // Check if there is any pending work to be done in libmicrohttpd.
  MHD_run(server_);

  // Get all the file descriptors from libmicrohttpd and watch for I/O
  // operations on them.
  fd_set rs;
  fd_set ws;
  fd_set es;
  int max_fd = MHD_INVALID_SOCKET;
  FD_ZERO(&rs);
  FD_ZERO(&ws);
  FD_ZERO(&es);
  CHECK_EQ(MHD_YES, MHD_get_fdset(server_, &rs, &ws, &es, &max_fd));
  for (int fd = 0; fd <= max_fd; fd++) {
    // libmicrohttpd is not using exception FDs, so lets put our expectations
    // upfront.
    CHECK(!FD_ISSET(fd, &es));
    if (FD_ISSET(fd, &rs) || FD_ISSET(fd, &ws)) {
      base::MessageLoopForIO::Mode mode = base::MessageLoopForIO::WATCH_READ;
      if (FD_ISSET(fd, &rs) && FD_ISSET(fd, &ws))
        mode = base::MessageLoopForIO::WATCH_READ_WRITE;
      else if (FD_ISSET(fd, &rs))
        mode = base::MessageLoopForIO::WATCH_READ;
      else if (FD_ISSET(fd, &ws))
        mode = base::MessageLoopForIO::WATCH_WRITE;
      // libmicrohttpd should never use any of stdin/stdout/stderr descriptors.
      CHECK_GT(fd, STDERR_FILENO);
      watchers_.emplace_back(new Watcher{this, fd, mode, message_loop});
    }
  }

  // Schedule a time-out timer, if asked by libmicrohttpd.
  MHD_UNSIGNED_LONG_LONG mhd_timeout = 0;
  if (!timer_scheduled_ && MHD_get_timeout(server_, &mhd_timeout) == MHD_YES) {
    timer_scheduled_ = true;
    message_loop->PostDelayedTask(
        FROM_HERE,
        base::Bind(&ProtocolHandler::DoWork,
                   weak_ptr_factory_.GetWeakPtr()),
        base::TimeDelta::FromMilliseconds(mhd_timeout));
  }
}

void ProtocolHandler::TimerCallback() {
  timer_scheduled_ = false;
  DoWork();
}

}  // namespace webservd