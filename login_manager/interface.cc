// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "login_manager/interface.h"

namespace login_manager {
namespace gobject {

// Register with the glib type system.
// This macro automatically defines a number of functions and variables
// which are required to make session_manager functional as a GObject:
// - session_manager_parent_class
// - session_manager_get_type()
// - dbus_glib_session_manager_object_info
// It also ensures that the structs are setup so that the initialization
// functions are called in the appropriate way by g_object_new().
G_DEFINE_TYPE(SessionManager, session_manager, G_TYPE_OBJECT);

// This file is eerily reminiscent of cryptohome/interface.cc.
// We can probably re-use code somehow.

// TODO(cmasone): use a macro to allow this to be re-used?
GObject* session_manager_constructor(GType gtype,
                                     guint n_properties,
                                     GObjectConstructParam *properties) {
  GObject* obj;
  GObjectClass* parent_class;
  // Instantiate using the parent class, then extend for local properties.
  parent_class = G_OBJECT_CLASS(session_manager_parent_class);
  obj = parent_class->constructor(gtype, n_properties, properties);

  SessionManager* session_manager = reinterpret_cast<SessionManager*>(obj);
  session_manager->service = NULL;

  // We don't have any thing we care to expose to the glib class system.
  return obj;
}

void session_manager_class_init(SessionManagerClass *real_class) {
  // Called once to configure the "class" structure.
  GObjectClass* gobject_class = G_OBJECT_CLASS(real_class);
  gobject_class->constructor = session_manager_constructor;
}

void session_manager_init(SessionManager *self) { }

// TODO(wad) add error messaging
#define SESSION_MANAGER_WRAP_METHOD(_NAME, args...) \
  if (!self->service) { \
    return FALSE; \
  } \
  return self->service->_NAME(args, error);

gboolean session_manager_emit_login_prompt_ready(SessionManager* self,
                                                 gboolean* OUT_emitted,
                                                 GError** error) {
  SESSION_MANAGER_WRAP_METHOD(EmitLoginPromptReady, OUT_emitted);
}
gboolean session_manager_start_session(SessionManager* self,
                                       gchar* email_address,
                                       gchar* unique_identifier,
                                       gboolean* OUT_done,
                                       GError** error) {
  SESSION_MANAGER_WRAP_METHOD(StartSession,
                              email_address,
                              unique_identifier,
                              OUT_done);
}
gboolean session_manager_stop_session(SessionManager* self,
                                      gchar* unique_identifier,
                                      gboolean* OUT_done,
                                      GError** error) {
  SESSION_MANAGER_WRAP_METHOD(StopSession, unique_identifier, OUT_done);
}
#undef SESSION_MANAGER_WRAP_METHOD

}  // namespace gobject
}  // namespace login_manager
