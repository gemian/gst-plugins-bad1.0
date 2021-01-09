/*
 * Copyright (C) 2012, Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifndef __GST_AMC_VIDEO_DEC_H__
#define __GST_AMC_VIDEO_DEC_H__

#include <gst/gst.h>
#ifdef HAVE_ANDROID_MEDIA
  #include <gst/gl/gl.h>
#endif

#include <gst/video/gstvideodecoder.h>

#ifdef HAVE_ANDROID_MEDIA_HYBRIS
  #include <gst/mir/gstmircontext.h>
#endif

#include "gstamc.h"
#ifdef HAVE_ANDROID_MEDIA
  #include "gstamcsurface.h"
#endif

G_BEGIN_DECLS

#define GST_TYPE_AMC_VIDEO_DEC \
  (gst_amc_video_dec_get_type())
#define GST_AMC_VIDEO_DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AMC_VIDEO_DEC,GstAmcVideoDec))
#define GST_AMC_VIDEO_DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AMC_VIDEO_DEC,GstAmcVideoDecClass))
#define GST_AMC_VIDEO_DEC_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_AMC_VIDEO_DEC,GstAmcVideoDecClass))
#define GST_IS_AMC_VIDEO_DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AMC_VIDEO_DEC))
#define GST_IS_AMC_VIDEO_DEC_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AMC_VIDEO_DEC))

typedef struct _GstAmcVideoDec GstAmcVideoDec;
typedef struct _GstAmcVideoDecClass GstAmcVideoDecClass;
typedef enum _GstAmcCodecConfig GstAmcCodecConfig;

enum _GstAmcCodecConfig
{
  AMC_CODEC_CONFIG_NONE,
  AMC_CODEC_CONFIG_WITH_SURFACE,
  AMC_CODEC_CONFIG_WITHOUT_SURFACE,
};

struct _GstAmcVideoDec
{
  GstVideoDecoder parent;

  /* < private > */
  GstAmcCodec *codec;
  GstAmcCodecConfig codec_config;
#ifdef HAVE_ANDROID_MEDIA_HYBRIS
  GstAmcBuffer *input_buffers, *output_buffers;
  gsize n_input_buffers, n_output_buffers;
#endif

  GstVideoCodecState *input_state;
  gboolean input_state_changed;

  /* Current timeout value to pass for doing
   * queueing/dequeueing of input/output buffers
   */
  gint64 current_timeout;

  /* Output format of the codec */
  GstVideoFormat format;
  GstAmcColorFormatInfo color_format_info;

  /* Output dimensions */
  guint width;
  guint height;

  guint8 *codec_data;
  gsize codec_data_size;
  /* TRUE if the component is configured and saw
   * the first buffer */
  gboolean started;
  gboolean flushing;

#ifdef HAVE_ANDROID_MEDIA_HYBRIS
  guint8 num_outbuf_dequeue_tries;
#endif

  GstClockTime last_upstream_ts;

  /* Draining state */
  GMutex drain_lock;
  GCond drain_cond;
  /* TRUE if EOS buffers shouldn't be forwarded */
  gboolean draining;
  /* TRUE if the component is drained currently */
  gboolean drained;
#ifdef HAVE_ANDROID_MEDIA_HYBRIS
  gboolean eos;
#endif

#ifdef HAVE_ANDROID_MEDIA
  GstAmcSurface *surface;

  GstGLDisplay *gl_display;
  GstGLContext *gl_context;
  GstGLContext *other_gl_context;
#endif

  gboolean downstream_supports_gl;
  GstFlowReturn downstream_flow_ret;

#ifdef HAVE_ANDROID_MEDIA
  jobject listener;
  jmethodID set_context_id;
#endif

  gboolean gl_mem_attached;
#ifdef HAVE_ANDROID_MEDIA
  GstGLMemory *oes_mem;
#endif
  GError *gl_error;
  GMutex gl_lock;
  GCond gl_cond;
  guint gl_last_rendered_frame;
  guint gl_pushed_frame_count; /* n buffers pushed */
  guint gl_ready_frame_count;  /* n buffers ready for GL access */
  guint gl_released_frame_count;  /* n buffers released */
  GQueue *gl_queue;

#ifdef HAVE_ANDROID_MEDIA_HYBRIS
  /* To wait for source caps to be set */
  GMutex srccaps_lock;
  GCond srccaps_cond;
  gboolean srccaps_set;
  gboolean waiting_segment;

  const gchar *mime;
#endif
};

struct _GstAmcVideoDecClass
{
  GstVideoDecoderClass parent_class;

  const GstAmcCodecInfo *codec_info;
};

GType gst_amc_video_dec_get_type (void);

G_END_DECLS

#endif /* __GST_AMC_VIDEO_DEC_H__ */
