/*
 * GStreamer Mir video sink
 * Copyright (C) 2013 Canonical Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

#ifndef __GST_HYBRIS_VIDEO_SINK_H__
#define __GST_HYBRIS_VIDEO_SINK_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/gstvideometa.h>

#include <application/description.h>
#include <application/instance.h>
#include <application/options.h>
#include <application/lifecycle_delegate.h>

#include <hybris/media/surface_texture_client_hybris.h>
//#include <gst/mir/gstmircontext.h>

#define GST_TYPE_HYBRIS_SINK \
        (gst_hybris_sink_get_type())
#define GST_HYBRIS_SINK(obj) \
        (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_HYBRIS_SINK,GstHybrisSink))
#define GST_HYBRIS_SINK_CLASS(klass) \
        (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_HYBRIS_SINK,GstHybrisSinkClass))
#define GST_IS_HYBRIS_SINK(obj) \
        (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_HYBRIS_SINK))
#define GST_IS_HYBRIS_SINK_CLASS(klass) \
        (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_HYBRIS_SINK))
#define GST_HYBRIS_SINK_GET_CLASS(inst) \
        (G_TYPE_INSTANCE_GET_CLASS ((inst), GST_TYPE_HYBRIS_SINK, GstHybrisSinkClass))

typedef struct _GstHybrisSink GstHybrisSink;
typedef struct _GstHybrisSinkClass GstHybrisSinkClass;

#include "mirpool.h"

struct session
{
#if 0
  UAUiSession *session;
  UAUiSessionProperties *properties;
#endif
  UApplicationDescription *app_description;
  UApplicationOptions *app_options;
  UApplicationInstance *app_instance;
  UApplicationLifecycleDelegate *app_lifecycle_delegate;
};

struct display
{
#if 0
  UAUiDisplay *display;
#endif
  int width;
  int height;
  uint32_t formats;
};

struct window
{
  struct display *display;
  int width;
  int height;
#if 0
  UAUiWindow *window;
  UAUiWindowProperties *properties;
#endif
  EGLNativeWindowType egl_native_window;
};

struct _GstHybrisSink
{
  GstVideoSink parent;

  SurfaceTextureClientHybris surface_texture_client;

  struct session *session;
  struct display *display;
  struct window *window;

  guint texture_id;

  GstBufferPool *pool;

  GMutex mir_lock;

  gint video_width;
  gint video_height;
};

struct _GstHybrisSinkClass
{
  GstVideoSinkClass parent;

  void (*frame_ready_changed) (GstHybrisSink *sink, gpointer renderer);
  void (*surface_texture_client_changed) (GstHybrisSink *sink, gpointer surface_texture_client, gpointer renderer);
};

GType gst_hybris_sink_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __GST_HYBRIS_VIDEO_SINK_H__ */
