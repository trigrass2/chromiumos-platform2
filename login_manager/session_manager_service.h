// Copyright (c) 2009-2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LOGIN_MANAGER_SESSION_MANAGER_H_
#define LOGIN_MANAGER_SESSION_MANAGER_H_

#include <gtest/gtest.h>

#include <errno.h>
#include <glib.h>
#include <signal.h>
#include <unistd.h>
#include <string>
#include <vector>

#include <base/basictypes.h>
#include <base/scoped_ptr.h>
#include <chromeos/dbus/abstract_dbus_service.h>
#include <chromeos/dbus/dbus.h>

#include "login_manager/child_job.h"
#include "login_manager/constants.h"

class CommandLine;

namespace login_manager {
namespace gobject {
struct SessionManager;
}  // namespace gobject

class ChildJob;

// Provides a wrapper for exporting SessionManagerInterface to
// D-Bus and entering the glib run loop.
//
// ::g_type_init() must be called before this class is used.
class SessionManagerService : public chromeos::dbus::AbstractDbusService {
 public:
  // Takes ownership of |child|.
  explicit SessionManagerService(ChildJob* child);
  SessionManagerService(ChildJob* child, bool exit_on_child_done);
  virtual ~SessionManagerService();

  ////////////////////////////////////////////////////////////////////////////
  // Implementing chromeos::dbus::AbstractDbusService
  virtual bool Initialize();
  virtual bool Reset();

  // Runs the command specified on the command line as |desired_uid_| and
  // watches it, restarting it whenever it exits abnormally -- UNLESS
  // |magic_chrome_file| exists.
  //
  // So, this function will run until one of the following occurs:
  // 1) the specified command exits normally
  // 2) |magic_chrome_file| exists AND the specified command exits for any
  //     reason
  // 3) We can't fork/exec/setuid to |desired_uid_|
  virtual bool Run();

  virtual const char* service_name() const {
    return kSessionManagerServiceName;
  }
  virtual const char* service_path() const {
    return kSessionManagerServicePath;
  }
  virtual const char* service_interface() const {
    return kSessionManagerInterface;
  }
  virtual GObject* service_object() const {
    return G_OBJECT(session_manager_.get());
  }


  // Returns true if |child_job_| believes it should be run.
  bool should_run_child() { return child_job_->ShouldRun(); }

  // Fork, then call child_job_->Run() in the child and set a
  // babysitter in the parent's glib default context that calls
  // HandleChildExit when the child is done.
  int RunChild();

  // Tell us that, if we want, we can cause a graceful exit from g_main_loop.
  void AllowGracefulExit();

  ////////////////////////////////////////////////////////////////////////////
  // SessionManagerService commands

  // Emits the "login-prompt-ready" upstart signal.
  gboolean EmitLoginPromptReady(gboolean *OUT_emitted, GError **error);

  // In addition to emitting "start-user-session", this function will
  // also call child_job_->Toggle().
  gboolean StartSession(gchar *email_address,
                        gchar *unique_identifier,
                        gboolean *OUT_done,
                        GError **error);

  // In addition to emitting "stop-user-session", this function will
  // also call child_job_->Toggle().
  gboolean StopSession(gchar *unique_identifier,
                       gboolean *OUT_done,
                       GError **error);

 protected:
  virtual GMainLoop* main_loop() { return main_loop_; }

 private:
  // |data| is a SessionManagerService*
  static void HandleChildExit(GPid pid,
                              gint status,
                              gpointer data);

  // So that we can enqueue an event that will exit the main loop.
  // |data| is a SessionManagerService*
  static gboolean ServiceShutdown(gpointer data);

  // Setup any necessary signal handlers.
  void SetupHandlers();

  scoped_ptr<ChildJob> child_job_;
  bool exit_on_child_done_;

  scoped_ptr<gobject::SessionManager> session_manager_;
  GMainLoop* main_loop_;

  FRIEND_TEST(SessionManagerTest, BadExitTest);
  FRIEND_TEST(SessionManagerTest, CleanExitTest);
  DISALLOW_COPY_AND_ASSIGN(SessionManagerService);
};
}  // namespace login_manager

#endif  // LOGIN_MANAGER_SESSION_MANAGER_H_
