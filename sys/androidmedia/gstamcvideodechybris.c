/*
 * Initially based on gstamcvideodec.c
 *
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 *
 * Copyright (C) 2012, Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * Copyright (C) 2012, Rafaël Carré <funman@videolanorg>
 *
 * Copyright (C) 2013, Canonical Ltd..
 *   Author: Jim Hodapp <jim.hodapp@canonical.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include <string.h>

#include <gst/mir/mirallocator.h>
#include <ext/hybris/mirpool.h>

#ifdef HAVE_ORC
#include <orc/orc.h>
#else
#define orc_memcpy memcpy
#endif

#include "gstamcvideodec.h"
#include "gstamc-constants.h"

GST_DEBUG_CATEGORY_STATIC (gst_amc_video_dec_debug_category);
#define GST_CAT_DEFAULT gst_amc_video_dec_debug_category

#define GST_VIDEO_DECODER_ERROR_FROM_ERROR(el, err) G_STMT_START { \
  gchar *__dbg = g_strdup (err->message);                               \
  GstVideoDecoder *__dec = GST_VIDEO_DECODER (el);                      \
  GST_WARNING_OBJECT (el, "error: %s", __dbg);                          \
  _gst_video_decoder_error (__dec, 1,                                   \
    err->domain, err->code,                                             \
    NULL, __dbg, __FILE__, GST_FUNCTION, __LINE__);                     \
  g_clear_error (&err); \
} G_STMT_END

#define WAIT_FOR_SRC_CAPS_MS 100
#define DEC_ALIGNMENT_BYTES 4096

typedef struct _BufferIdentification BufferIdentification;
struct _BufferIdentification
{
  guint64 timestamp;
};

static BufferIdentification *
buffer_identification_new (GstClockTime timestamp)
{
  BufferIdentification *id = g_slice_new (BufferIdentification);

  id->timestamp = timestamp;

  return id;
}

static void
buffer_identification_free (BufferIdentification * id)
{
  g_slice_free (BufferIdentification, id);
}

/* prototypes */
static void gst_amc_video_dec_finalize (GObject * object);

static GstStateChangeReturn
gst_amc_video_dec_change_state (GstElement * element,
    GstStateChange transition);

static gboolean gst_amc_video_dec_open (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_close (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_start (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_stop (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static gboolean gst_amc_video_dec_flush (GstVideoDecoder * decoder);
static GstFlowReturn gst_amc_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);
static GstFlowReturn gst_amc_video_dec_finish (GstVideoDecoder * decoder);
static void gst_amc_video_dec_set_context (GstElement * element,
    GstContext * context);
static gboolean gst_amc_video_dec_decide_allocation (GstVideoDecoder * bdec,
    GstQuery * query);

static GstFlowReturn gst_amc_video_dec_drain (GstAmcVideoDec * self,
    gboolean at_eos);

static gboolean gst_amc_video_dec_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_amc_video_dec_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event);

enum
{
  PROP_0
};

/* class initialization */

static void gst_amc_video_dec_class_init (GstAmcVideoDecClass * klass);
static void gst_amc_video_dec_init (GstAmcVideoDec * self);
static void gst_amc_video_dec_base_init (gpointer g_class);

static GstVideoDecoderClass *parent_class = NULL;

GType
gst_amc_video_dec_get_type (void)
{
  static volatile gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    static const GTypeInfo info = {
      sizeof (GstAmcVideoDecClass),
      gst_amc_video_dec_base_init,
      NULL,
      (GClassInitFunc) gst_amc_video_dec_class_init,
      NULL,
      NULL,
      sizeof (GstAmcVideoDec),
      0,
      (GInstanceInitFunc) gst_amc_video_dec_init,
      NULL
    };

    _type = g_type_register_static (GST_TYPE_VIDEO_DECODER, "GstAmcVideoDec",
        &info, 0);

    GST_DEBUG_CATEGORY_INIT (gst_amc_video_dec_debug_category, "amcvideodec", 0,
        "Android MediaCodec video decoder");

    g_once_init_leave (&type, _type);
  }
  return type;
}

static GstCaps *
create_sink_caps (const GstAmcCodecInfo * codec_info)
{
  GstCaps *ret;
  gint i;

  ret = gst_caps_new_empty ();

  for (i = 0; i < codec_info->n_supported_types; i++) {
    const GstAmcCodecType *type = &codec_info->supported_types[i];

    if (strcmp (type->mime, "video/mp4v-es") == 0) {
      GstStructure *tmp;

      tmp = gst_structure_new ("video/mpeg",
          "mpegversion", G_TYPE_INT, 4,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      ret = gst_caps_merge_structure (ret, tmp);

      tmp = gst_structure_new ("video/x-divx",
          "divxversion", GST_TYPE_INT_RANGE, 3, 5, NULL);
      ret = gst_caps_merge_structure (ret, tmp);

    } else if (strcmp (type->mime, "video/3gpp") == 0) {
      GstStructure *tmp;

      tmp = gst_structure_new ("video/x-h263",
          "variant", G_TYPE_STRING, "itu",
          NULL);
      ret = gst_caps_merge_structure (ret, tmp);

    } else if (strcmp (type->mime, "video/avc") == 0) {
      GstStructure *tmp;

      tmp = gst_structure_new ("video/x-h264",
          "stream-format", G_TYPE_STRING, "byte-stream",
          "alignment", G_TYPE_STRING, "au", NULL);
      ret = gst_caps_merge_structure (ret, tmp);

    } else if (strcmp (type->mime, "video/x-vnd.on2.vp8") == 0) {
      GstStructure *tmp;

      tmp = gst_structure_new_empty ("video/x-vp8");
      ret = gst_caps_merge_structure (ret, tmp);

    } else if (strcmp (type->mime, "video/x-vnd.on2.vp9") == 0) {
      GstStructure *tmp;

      tmp = gst_structure_new_empty ("video/x-vp9");
      ret = gst_caps_merge_structure (ret, tmp);

    } else if (strcmp (type->mime, "video/mpeg2") == 0) {
      GstStructure *tmp;

      tmp = gst_structure_new ("video/mpeg",
          "mpegversion", GST_TYPE_INT_RANGE, 1, 2,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      ret = gst_caps_merge_structure (ret, tmp);

    } else if (strcmp (type->mime, "video/hevc") == 0) {
      GstStructure *tmp;

      tmp = gst_structure_new ("video/x-h265",
          "stream-format", G_TYPE_STRING, "byte-stream",
          "alignment", G_TYPE_STRING, "au", NULL);
      ret = gst_caps_merge_structure (ret, tmp);

    } else if (strcmp (type->mime, "video/x-ms-wmv") == 0) {
      GstStructure *tmp;
      GValue list = { 0 };
      GValue val = { 0 };

      tmp = gst_structure_new ("video/x-wmv",
          "wmvversion", G_TYPE_INT, 3, NULL);

      g_value_init (&list, GST_TYPE_LIST);
      g_value_init (&val, G_TYPE_STRING);

      g_value_set_string (&val, "WVC1");
      gst_value_list_append_value (&list, &val);

      g_value_set_string (&val,"WMVA");
      gst_value_list_append_value (&list, &val);

      gst_structure_set_value (tmp, "format", &list);
      g_value_unset (&val);
      g_value_unset (&list);

      ret = gst_caps_merge_structure (ret, tmp);

    } else {
      GST_WARNING ("Unsupported mimetype '%s'", type->mime);
    }
  }

  return ret;
}

static const gchar *
get_caps_data (GstCaps * caps, int * buffsize)
{
  GstStructure *s;
  const gchar *name;

  s = gst_caps_get_structure (caps, 0);
  if (!s)
    return NULL;

  name = gst_structure_get_name (s);

  /* Retrieve max input size for frame. If present, increment size to make sure
   * it fulfills HW access requirements (required by arale decoder).
   */
  if (!gst_structure_get_int (s, "max-input-size", buffsize))
    *buffsize = 0;
  else
    *buffsize = (*buffsize + DEC_ALIGNMENT_BYTES - 1)
        & ~(DEC_ALIGNMENT_BYTES - 1);

  if (strcmp (name, "video/mpeg") == 0) {
    gint mpegversion;

    if (!gst_structure_get_int (s, "mpegversion", &mpegversion))
      return NULL;

    if (mpegversion == 4)
      return "video/mp4v-es";
    else if (mpegversion == 1 || mpegversion == 2)
      return "video/mpeg2";
  } else if (strcmp (name, "video/x-h263") == 0) {
    return "video/3gpp";
  } else if (strcmp (name, "video/x-h264") == 0) {
    return "video/avc";
  } else if (strcmp (name, "video/x-vp8") == 0) {
    return "video/x-vnd.on2.vp8";
  } else if (strcmp (name, "video/x-vp9") == 0) {
    return "video/x-vnd.on2.vp9";
  } else if (strcmp (name, "video/x-divx") == 0) {
    return "video/mp4v-es";
  } else if (strcmp (name, "video/x-h265") == 0) {
    return "video/hevc";
  } else if (strcmp (name, "video/x-wmv") == 0) {
    return "video/x-ms-wmv";
  }

  return NULL;
}

static GstCaps *
create_src_caps (const GstAmcCodecInfo * codec_info)
{
  GstCaps *ret;
  gint i;

  ret = gst_caps_new_empty ();

  for (i = 0; i < codec_info->n_supported_types; i++) {
    const GstAmcCodecType *type = &codec_info->supported_types[i];
    gint j;

    for (j = 0; j < type->n_color_formats; j++) {
      GstVideoFormat format;
      GstCaps *tmp;

      format = gst_amc_color_format_to_video_format (codec_info, type->mime, type->color_formats[j]);
      if (format == GST_VIDEO_FORMAT_UNKNOWN) {
        GST_WARNING ("Unknown color format 0x%08x", type->color_formats[j]);
        continue;
      }

      tmp = gst_caps_new_simple ("video/x-raw",
          "format", G_TYPE_STRING, gst_video_format_to_string (format),
          "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
          "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
          "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
      ret = gst_caps_merge (ret, tmp);
    }
  }

  return ret;
}

static void
gst_amc_video_dec_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstAmcVideoDecClass *amcvideodec_class = GST_AMC_VIDEO_DEC_CLASS (g_class);
  const GstAmcCodecInfo *codec_info;
  GstPadTemplate *templ;
  GstCaps *caps;
  gchar *longname;

  codec_info =
      g_type_get_qdata (G_TYPE_FROM_CLASS (g_class), gst_amc_codec_info_quark);
  /* This happens for the base class and abstract subclasses */
  if (!codec_info)
    return;

  amcvideodec_class->codec_info = codec_info;

  /* Add pad templates */
  caps = create_sink_caps (codec_info);
  templ = gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps);
  gst_element_class_add_pad_template (element_class, templ);
  gst_caps_unref (caps);

  caps = create_src_caps (codec_info);
  templ = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps);
  gst_element_class_add_pad_template (element_class, templ);
  gst_caps_unref (caps);

  longname = g_strdup_printf ("Android MediaCodec Hybris %s", codec_info->name);
  gst_element_class_set_metadata (element_class,
      codec_info->name,
      "Codec/Decoder/Video", longname, "Jim Hodapp <jim.hodapp@canonical.com>");
  g_free (longname);
}

static void
gst_amc_video_dec_class_init (GstAmcVideoDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *videodec_class = GST_VIDEO_DECODER_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->finalize = gst_amc_video_dec_finalize;

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_change_state);
  element_class->set_context =
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_set_context);

  videodec_class->start = GST_DEBUG_FUNCPTR (gst_amc_video_dec_start);
  videodec_class->stop = GST_DEBUG_FUNCPTR (gst_amc_video_dec_stop);
  videodec_class->open = GST_DEBUG_FUNCPTR (gst_amc_video_dec_open);
  videodec_class->close = GST_DEBUG_FUNCPTR (gst_amc_video_dec_close);
  videodec_class->flush = GST_DEBUG_FUNCPTR (gst_amc_video_dec_flush);
  videodec_class->set_format = GST_DEBUG_FUNCPTR (gst_amc_video_dec_set_format);
  videodec_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_handle_frame);
  videodec_class->finish = GST_DEBUG_FUNCPTR (gst_amc_video_dec_finish);
  videodec_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_decide_allocation);
}

static void
gst_amc_video_dec_init (GstAmcVideoDec * self)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (self);

  gst_video_decoder_set_packetized (decoder, TRUE);

  g_mutex_init (&self->drain_lock);
  g_cond_init (&self->drain_cond);

  g_mutex_init (&self->srccaps_lock);
  g_cond_init (&self->srccaps_cond);
  self->srccaps_set = FALSE;

  gst_pad_set_event_function (decoder->sinkpad,
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_sink_event));
  gst_pad_set_event_function (decoder->srcpad,
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_src_event));

  /* Start queueing/dequeueing operations with a timeout of 0 */
  self->current_timeout = 0;
}

static gboolean
gst_amc_video_dec_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstVideoDecoder *decoder;
  GstVideoDecoderClass *decoder_class;
  GstAmcVideoDec *self;
  gboolean ret = FALSE;

  decoder = GST_VIDEO_DECODER (parent);
  self = GST_AMC_VIDEO_DEC (decoder);
  decoder_class = GST_VIDEO_DECODER_GET_CLASS (decoder);

  GST_DEBUG_OBJECT (self, "received sink event %d, %s", GST_EVENT_TYPE (event),
      GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
    {
      /* For very short videos (<2s), it can happen that we receive EOS before
       * having set the src pad caps, which makes the pad return and error later
       * when we try to configure it as it is in EOS state. This leaves us
       * without output buffers and we cannot play the video.
       * This sequence can happen because we wait for an async event from the HW
       * decoder to set the src caps. Waiting a few milliseconds seems to solve
       * the issue, although it is not the perfect solution.
       * TODO: Find out if there is a better way to handle this. Is it possible
       * to set output buffers after other src caps?
       */
      gint64 end_time = g_get_monotonic_time ()
          + WAIT_FOR_SRC_CAPS_MS * G_TIME_SPAN_MILLISECOND;

      g_mutex_lock (&self->srccaps_lock);
      while (!self->srccaps_set) {
        if (!g_cond_wait_until (&self->srccaps_cond, &self->srccaps_lock,
                end_time)) {
          GST_DEBUG_OBJECT (self, "No src caps, not delaying EOS anymore");
          break;
        }
      }
      g_mutex_unlock (&self->srccaps_lock);
      break;
    }
    case GST_EVENT_SEGMENT:
      self->waiting_segment = FALSE;
    default:
      break;
  }

  if (decoder_class->sink_event)
    ret = decoder_class->sink_event (decoder, event);

  return ret;
}

static gboolean
gst_amc_video_dec_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstVideoDecoder *decoder;
  GstVideoDecoderClass *decoder_class;
  GstAmcVideoDec *self;
  gboolean ret = FALSE;

  decoder = GST_VIDEO_DECODER (parent);
  self = GST_AMC_VIDEO_DEC (decoder);
  decoder_class = GST_VIDEO_DECODER_GET_CLASS (decoder);

  GST_DEBUG_OBJECT (self, "received src event %d, %s", GST_EVENT_TYPE (event),
      GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      self->waiting_segment = TRUE;
      break;
    default:
      break;
  }

  if (decoder_class->src_event)
    ret = decoder_class->src_event (decoder, event);

  return ret;
}

static gboolean
gst_amc_video_dec_open (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (decoder);
  GstAmcVideoDecClass *klass = GST_AMC_VIDEO_DEC_GET_CLASS (self);
  GError *err = NULL;

  GST_DEBUG_OBJECT (self, "Opening hardware decoder");

  self->codec = gst_amc_codec_new (klass->codec_info->name, FALSE, &err);
  if (!self->codec) {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }
  self->started = FALSE;
  self->flushing = TRUE;

  self->num_outbuf_dequeue_tries = 0;

  return TRUE;
}

static gboolean
gst_amc_video_dec_close (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Closing decoder");

  if (self->codec)
    gst_amc_codec_free (self->codec);
  self->codec = NULL;

  self->started = FALSE;
  self->flushing = TRUE;

  GST_DEBUG_OBJECT (self, "Closed decoder");

  return TRUE;
}

static void
gst_amc_video_dec_finalize (GObject * object)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (object);

  g_mutex_clear (&self->drain_lock);
  g_cond_clear (&self->drain_cond);

  g_mutex_clear (&self->srccaps_lock);
  g_cond_clear (&self->srccaps_cond);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstStateChangeReturn
gst_amc_video_dec_change_state (GstElement * element, GstStateChange transition)
{
  GstAmcVideoDec *self;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GError *err = NULL;

  g_return_val_if_fail (GST_IS_AMC_VIDEO_DEC (element),
      GST_STATE_CHANGE_FAILURE);
  self = GST_AMC_VIDEO_DEC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      self->downstream_flow_ret = GST_FLOW_OK;
      self->draining = FALSE;
      self->started = FALSE;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_VIDEO_DECODER_STREAM_LOCK(self);
      self->flushing = TRUE;
      gst_amc_codec_flush (self->codec, &err);
      if (err)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      GST_VIDEO_DECODER_STREAM_UNLOCK(self);

      g_mutex_lock (&self->drain_lock);
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
      g_mutex_unlock (&self->drain_lock);
      break;
    default:
      break;
  }

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      self->downstream_flow_ret = GST_FLOW_FLUSHING;
      self->started = FALSE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  return ret;
}

#define MAX_FRAME_DIST_TIME  (5 * GST_SECOND)
#define MAX_FRAME_DIST_FRAMES (100)

static GstVideoCodecFrame *
_find_nearest_frame (GstAmcVideoDec * self, GstClockTime reference_timestamp)
{
  GList *l, *best_l = NULL;
  GList *finish_frames = NULL;
  GstVideoCodecFrame *best = NULL;
  guint64 best_timestamp = 0;
  guint64 best_diff = G_MAXUINT64;
  BufferIdentification *best_id = NULL;
  GList *frames;

  frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (self));

  for (l = frames; l; l = l->next) {
    GstVideoCodecFrame *tmp = l->data;
    BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
    guint64 timestamp, diff;

    /* This happens for frames that were just added but
     * which were not passed to the component yet. Ignore
     * them here!
     */
    if (!id)
      continue;

    timestamp = id->timestamp;

    if (timestamp > reference_timestamp)
      diff = timestamp - reference_timestamp;
    else
      diff = reference_timestamp - timestamp;

    if (best == NULL || diff < best_diff) {
      best = tmp;
      best_timestamp = timestamp;
      best_diff = diff;
      best_l = l;
      best_id = id;

      /* For frames without timestamp we simply take the first frame */
      if ((reference_timestamp == 0 && timestamp == 0) || diff == 0)
        break;
    }
  }

  if (best_id) {
    for (l = frames; l && l != best_l; l = l->next) {
      GstVideoCodecFrame *tmp = l->data;
      BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
      guint64 diff_time, diff_frames;

      /* As in previous loop, ignore frames that were just added but which were
       * not passed to the component yet.
       */
      if (!id)
        continue;

      if (id->timestamp > best_timestamp)
        break;

      if (id->timestamp == 0 || best_timestamp == 0)
        diff_time = 0;
      else
        diff_time = best_timestamp - id->timestamp;
      diff_frames = best->system_frame_number - tmp->system_frame_number;

      if (diff_time > MAX_FRAME_DIST_TIME
          || diff_frames > MAX_FRAME_DIST_FRAMES) {
        finish_frames =
            g_list_prepend (finish_frames, gst_video_codec_frame_ref (tmp));
      }
    }
  }

  if (finish_frames) {
    g_warning ("%s: Frames are too old, bug in decoder -- please file a bug",
        GST_ELEMENT_NAME (self));
    for (l = finish_frames; l; l = l->next) {
      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), l->data);
    }
  }

  if (best)
    gst_video_codec_frame_ref (best);

  g_list_foreach (frames, (GFunc) gst_video_codec_frame_unref, NULL);
  g_list_free (frames);

  return best;
}

static gboolean
gst_amc_video_dec_set_src_caps (GstAmcVideoDec * self, GstAmcFormat * format)
{
  GstVideoCodecState *output_state;
  gint color_format, width, height;
  gint stride, slice_height;
  gint crop_left, crop_right;
  gint crop_top, crop_bottom;
  GstVideoFormat gst_format;
  const gchar * mime;
  int buffsize;
  GstAmcVideoDecClass *klass = GST_AMC_VIDEO_DEC_GET_CLASS (self);

  GST_DEBUG_OBJECT (self, "Setting src caps");

  color_format = media_format_get_color_format (format->format);
  width = media_format_get_width (format->format);
  height = media_format_get_height (format->format);
  if (!color_format || !height || !width) {
    GST_ERROR_OBJECT (self, "Failed to get output format metadata");
    return FALSE;
  }

  if (strcmp (klass->codec_info->name, "OMX.k3.video.decoder.avc") == 0 &&
      color_format == COLOR_FormatYCbYCr)
    color_format = COLOR_TI_FormatYUV420PackedSemiPlanar;

  stride = media_format_get_stride (format->format);
  slice_height = media_format_get_slice_height (format->format);
  if (!stride || !slice_height) {
    GST_ERROR_OBJECT (self, "Failed to get stride and slice-height");
    return FALSE;
  }

  crop_left = media_format_get_crop_left (format->format);
  crop_right = media_format_get_crop_right (format->format);
  crop_top = media_format_get_crop_top (format->format);
  crop_bottom = media_format_get_crop_bottom (format->format);

  if (width == 0 || height == 0) {
    GST_ERROR_OBJECT (self, "Height or width not set");
    return FALSE;
  }

  if (crop_bottom)
    height = height - (height - crop_bottom - 1);
  if (crop_top)
    height = height - crop_top;

  if (crop_right)
    width = width - (width - crop_right - 1);
  if (crop_left)
    width = width - crop_left;

  mime = get_caps_data (self->input_state->caps, &buffsize);
  if (!mime) {
    GST_ERROR_OBJECT (self, "Failed to convert caps to mime");
    return FALSE;
  }

  gst_format =
      gst_amc_color_format_to_video_format (klass->codec_info, mime,
      color_format);
  if (gst_format == GST_VIDEO_FORMAT_UNKNOWN) {
    GST_ERROR_OBJECT (self, "Unknown color format 0x%08x", color_format);
    return FALSE;
  }

  output_state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (self),
      gst_format, width, height, self->input_state);

  /* FIXME: Special handling for multiview, untested */
  if (color_format == COLOR_QCOM_FormatYVU420SemiPlanar32mMultiView) {
    gst_video_multiview_video_info_change_mode (&output_state->info,
        GST_VIDEO_MULTIVIEW_MODE_TOP_BOTTOM, GST_VIDEO_MULTIVIEW_FLAGS_NONE);
  }

  self->format = gst_format;
  if (!gst_amc_color_format_info_set (&self->color_format_info,
          klass->codec_info, mime, color_format, width, height, stride,
          slice_height, crop_left, crop_right, crop_top, crop_bottom)) {
    GST_ERROR_OBJECT(self, "Failed to set up GstAmcColorFormatInfo");
  }

  gst_video_decoder_negotiate (GST_VIDEO_DECODER (self));
  gst_video_codec_state_unref (output_state);
  self->input_state_changed = FALSE;

  return TRUE;
}

/*
 * The format is called QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka.
 * Which is actually NV12 (interleaved U&V).
 */
#define TILE_WIDTH 64
#define TILE_HEIGHT 32
#define TILE_SIZE (TILE_WIDTH * TILE_HEIGHT)
#define TILE_GROUP_SIZE (4 * TILE_SIZE)

/* get frame tile coordinate. XXX: nothing to be understood here, don't try. */
static size_t
tile_pos (size_t x, size_t y, size_t w, size_t h)
{
  size_t flim = x + (y & ~1) * w;

  if (y & 1) {
    flim += (x & ~3) + 2;
  } else if ((h & 1) == 0 || y != (h - 1)) {
    flim += (x + 2) & ~3;
  }

  return flim;
}

gboolean
gst_amc_color_format_info_set (GstAmcColorFormatInfo * color_format_info,
    const GstAmcCodecInfo * codec_info, const gchar * mime, gint color_format,
    gint width, gint height, gint stride, gint slice_height, gint crop_left,
    gint crop_right, gint crop_top, gint crop_bottom)
{
  gint frame_size = 0;

  if (color_format == COLOR_FormatYCbYCr) {
    if (strcmp (codec_info->name, "OMX.k3.video.decoder.avc") == 0)
      color_format = COLOR_FormatYUV420SemiPlanar;
  }

  /* Samsung Galaxy S3 seems to report wrong strides.
   * I.e. BigBuckBunny 854x480 H264 reports a stride of 864 when it is
   * actually 854, so we use width instead of stride here.
   * This is obviously bound to break in the future. */
  if (g_str_has_prefix (codec_info->name, "OMX.SEC.")) {
    stride = width;
  }

  if (strcmp (codec_info->name, "OMX.k3.video.decoder.avc") == 0) {
    stride = width;
    slice_height = height;
  }

  if (slice_height == 0) {
    /* NVidia Tegra 3 on Nexus 7 does not set this */
    if (g_str_has_prefix (codec_info->name, "OMX.Nvidia."))
      slice_height = GST_ROUND_UP_32 (height);
  }

  if (width == 0 || height == 0) {
    GST_ERROR ("Width or height is 0");
    return FALSE;
  }

  switch (color_format) {
    case COLOR_FormatYUV420Planar:{
      if (stride == 0 || slice_height == 0) {
        GST_ERROR ("Stride or slice height is 0");
        return FALSE;
      }

      frame_size =
          stride * slice_height + 2 * (((stride + 1) / 2) * (slice_height +
              1) / 2);
      break;
    }
    case COLOR_TI_FormatYUV420PackedSemiPlanar:
    case COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced:{
      if (stride == 0 || slice_height == 0) {
        GST_ERROR ("Stride or slice height is 0");
        return FALSE;
      }

      frame_size =
          stride * (slice_height - crop_top / 2) +
          (GST_ROUND_UP_2 (stride) * ((slice_height + 1) / 2));
      break;
    }
    case COLOR_QCOM_FormatYUV420SemiPlanar:
    case COLOR_QCOM_FormatYVU420SemiPlanar32m:
    case COLOR_QCOM_FormatYVU420SemiPlanar32mMultiView:
    case COLOR_QCOM_FormatYUV420SemiPlanarUBWC:
    case COLOR_FormatYUV420SemiPlanar:{
      if (stride == 0 || slice_height == 0) {
        GST_ERROR ("Stride or slice height is 0");
        return FALSE;
      }

      frame_size = stride * slice_height + stride * ((slice_height + 1) / 2);
      break;
    }
    case COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka:{
      const size_t tile_w = (width - 1) / TILE_WIDTH + 1;
      const size_t tile_w_align = (tile_w + 1) & ~1;
      const size_t tile_h_luma = (height - 1) / TILE_HEIGHT + 1;
      frame_size =
          tile_pos (tile_w, tile_h_luma, tile_w_align, tile_h_luma) * TILE_SIZE;
      break;
    }
    default:
      GST_ERROR ("Unsupported color format %d", color_format);
      return FALSE;
      break;
  }

  color_format_info->color_format = color_format;
  color_format_info->width = width;
  color_format_info->height = height;
  color_format_info->stride = stride;
  color_format_info->slice_height = slice_height;
  color_format_info->crop_left = crop_left;
  color_format_info->crop_right = crop_right;
  color_format_info->crop_top = crop_top;
  color_format_info->crop_bottom = crop_bottom;
  color_format_info->frame_size = frame_size;

  return TRUE;
}

/* The weird handling of cropping, alignment and everything is taken from
 * platform/frameworks/media/libstagefright/colorconversion/ColorConversion.cpp
 */
static gboolean
gst_amc_video_dec_fill_buffer (GstAmcVideoDec * self, gint idx,
    const GstAmcBufferInfo * buffer_info, GstBuffer * outbuf)
{
  GstAmcVideoDecClass *klass = GST_AMC_VIDEO_DEC_GET_CLASS (self);
  GstAmcBuffer *buf = &self->output_buffers[idx];
  GstVideoCodecState *state =
      gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));
  GstVideoInfo *info = &state->info;
  gboolean ret = FALSE;
  GstMemory *mem = { NULL };
  gboolean do_hardware_rendering = FALSE;
  gint err = 0;

  GST_DEBUG_OBJECT (self, "%s", __PRETTY_FUNCTION__);

  if (idx >= self->n_output_buffers) {
    GST_ERROR_OBJECT (self, "Invalid output buffer index %d of %zu",
        idx, self->n_output_buffers);
    goto done;
  }

  if (!self->codec->codec_delegate) {
    GST_ERROR_OBJECT (self,
        "codec_delegate is NULL, can't decode/render properly");
    goto done;
  }

  GST_DEBUG_OBJECT (self,
      "buffer_info->size: %d, gst_buffer_get_size (outbuf): %zu",
      buffer_info->size, gst_buffer_get_size (outbuf));

  if (gst_buffer_n_memory (outbuf) >= 1 &&
      (mem = gst_buffer_peek_memory (outbuf, 0))
      && gst_is_mir_image_memory (mem)) {

    GST_DEBUG_OBJECT (self, "It is Mir image memory");
    do_hardware_rendering = gst_mir_do_hardware_render (mem);
  }

  GST_DEBUG_OBJECT (self, "hardware_rendering: %d", do_hardware_rendering);

  /* Same video format */
  if (do_hardware_rendering > 0) {

    if (gst_buffer_n_memory (outbuf) >= 1 && gst_is_mir_image_memory (mem)) {
      GST_DEBUG_OBJECT (self, "Doing hardware rendering");
      GST_DEBUG_OBJECT (self, "gst_mir_image_memory_get_codec: %p",
          self->codec->codec_delegate);

      ret = TRUE;
      goto done;
    }
  } else if (!do_hardware_rendering
      && buffer_info->size == gst_buffer_get_size (outbuf)) {
    GstMapInfo minfo;
    GST_DEBUG_OBJECT (self, "Doing fast software rendering");

    gst_buffer_map (outbuf, &minfo, GST_MAP_WRITE);
    orc_memcpy (minfo.data, buf->data + buffer_info->offset, buffer_info->size);
    gst_buffer_unmap (outbuf, &minfo);

    ret = TRUE;
  } else {
    /* Disabled for now since this plugin does not support non-hardware accelerated
     * video rendering at the moment.
     */
    GST_DEBUG_OBJECT (self,
        "Doing slow software line-by-line copying with format conversion");

    /* Different video format, try to convert */
    switch (self->color_format_info.color_format) {
      case COLOR_FormatYUV420Planar:{
        GstVideoFrame vframe;
        gint i, j, height;
        guint8 *src, *dest;
        gint stride, slice_height;
        gint src_stride, dest_stride;
        gint row_length;

        stride = self->color_format_info.stride;
        if (stride == 0) {
          GST_ERROR_OBJECT (self, "Stride not set");
          goto done;
        }

        slice_height = self->color_format_info.slice_height;
        if (slice_height == 0) {
          /* NVidia Tegra 3 on Nexus 7 does not set this */
          if (g_str_has_prefix (klass->codec_info->name, "OMX.Nvidia.")) {
            slice_height = GST_ROUND_UP_32 (self->color_format_info.height);
          } else {
            GST_ERROR_OBJECT (self, "Slice height not set");
            goto done;
          }
        }

        gst_video_frame_map (&vframe, info, outbuf, GST_MAP_WRITE);
        for (i = 0; i < 3; i++) {
          if (i == 0) {
            src_stride = stride;
            dest_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, i);
          } else {
            src_stride = (stride + 1) / 2;
            dest_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, i);
          }

          src = buf->data + buffer_info->offset;

          if (i == 0) {
            src += self->color_format_info.crop_top * stride;
            src += self->color_format_info.crop_left;
            row_length = self->color_format_info.width;
          } else if (i > 0) {
            /* skip the Y plane */
            src += slice_height * stride;

            /* crop_top/crop_left divided by two
             * because one byte of the U/V planes
             * corresponds to two pixels horizontally/vertically */
            src += self->color_format_info.crop_top / 2 * src_stride;
            src += self->color_format_info.crop_left / 2;
            row_length = (self->color_format_info.width + 1) / 2;
          }
          if (i == 2) {
            /* skip the U plane */
            src += ((slice_height + 1) / 2) * ((stride + 1) / 2);
          }

          dest = GST_VIDEO_FRAME_COMP_DATA (&vframe, i);
          height = GST_VIDEO_FRAME_COMP_HEIGHT (&vframe, i);

          for (j = 0; j < height; j++) {
            orc_memcpy (dest, src, row_length);
            src += src_stride;
            dest += dest_stride;
          }
        }
        gst_video_frame_unmap (&vframe);
        ret = TRUE;
        break;
      }
      case COLOR_TI_COLOR_FormatYUV420PackedSemiPlanar:
      case COLOR_TI_FormatYUV420PackedSemiPlanar:
      case COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced:{
        gint i, j, height;
        guint8 *src, *dest;
        gint src_stride, dest_stride;
        gint row_length;
        GstVideoFrame vframe;

        GST_DEBUG_OBJECT (self,
            "COLOR_TI_COLOR_FormatYUV420PackedSemiPlanar conversion");

        /* This should always be set */
        if (self->color_format_info.stride == 0
            || self->color_format_info.slice_height == 0) {
          GST_ERROR_OBJECT (self, "Stride or slice height not set");
          goto done;
        }

        /* FIXME: This does not work for odd widths or heights
         * but might as well be a bug in the codec */
        gst_video_frame_map (&vframe, info, outbuf, GST_MAP_WRITE);
        for (i = 0; i < 2; i++) {
          if (i == 0) {
            src_stride = self->color_format_info.stride;
            dest_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, i);
          } else {
            src_stride = GST_ROUND_UP_2 (self->color_format_info.stride);
            dest_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, i);
          }

          src = buf->data + buffer_info->offset;
#if 0
          GST_DEBUG_OBJECT (self, "buf->data: %p, buffer_info->offset: %d",
              buf->data, buffer_info->offset);
          GST_DEBUG_OBJECT (self, "buf->size: %d", buf->size);
#endif
          if (i == 0) {
            row_length = self->color_format_info.width;
          } else if (i == 1) {
            src += (self->color_format_info.slice_height
                    - self->color_format_info.crop_top / 2)
                    * self->color_format_info.stride;
            row_length = GST_ROUND_UP_2 (self->color_format_info.width);
          }

          dest = GST_VIDEO_FRAME_COMP_DATA (&vframe, i);
          height = GST_VIDEO_FRAME_COMP_HEIGHT (&vframe, i);

          for (j = 0; j < height; j++) {
#if 0
            GST_DEBUG_OBJECT (self,
                "Copying data row (dest: %p, src: %p, row_length: %d", dest,
                src, row_length);
#endif
            orc_memcpy (dest, src, row_length);
            src += src_stride;
            dest += dest_stride;
#if 0
            GST_DEBUG_OBJECT (self,
                "Finished copying data row (src_stride: %d, dst_stride: %d",
                src_stride, dest_stride);
#endif
          }
        }
        gst_video_frame_unmap (&vframe);
        ret = TRUE;
        break;
      }
      case COLOR_QCOM_FormatYUV420SemiPlanar:
      case COLOR_QCOM_FormatYVU420SemiPlanar32m:
      case COLOR_QCOM_FormatYVU420SemiPlanar32mMultiView:
      case COLOR_QCOM_FormatYUV420SemiPlanarUBWC:
      case COLOR_FormatYUV420SemiPlanar:{
        gint i, j, height;
        guint8 *src, *dest;
        gint src_stride, dest_stride, fixed_stride;
        gint row_length;
        GstVideoFrame vframe;

        /* This should always be set */
        if (self->color_format_info.stride == 0
                || self->color_format_info.slice_height == 0) {
          GST_ERROR_OBJECT (self, "Stride or slice height not set");
          goto done;
        }

        /* Samsung Galaxy S3 seems to report wrong strides.
           I.e. BigBuckBunny 854x480 H264 reports a stride of 864 when it is
           actually 854, so we use width instead of stride here.
           This is obviously bound to break in the future. */
        if (g_str_has_prefix (klass->codec_info->name, "OMX.SEC.")) {
          fixed_stride = self->color_format_info.width;
        } else {
          fixed_stride = self->color_format_info.stride;
        }

        gst_video_frame_map (&vframe, info, outbuf, GST_MAP_WRITE);

        for (i = 0; i < 2; i++) {
          src_stride = fixed_stride;
          dest_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, i);

          src = buf->data + buffer_info->offset;
          if (i == 0) {
            src += self->color_format_info.crop_top * fixed_stride;
            src += self->color_format_info.crop_left;
            row_length = self->color_format_info.width;
          } else if (i == 1) {
            src += self->color_format_info.slice_height * fixed_stride;
            src += self->color_format_info.crop_top * fixed_stride;
            src += self->color_format_info.crop_left;
            row_length = self->color_format_info.width;
          }

          dest = GST_VIDEO_FRAME_COMP_DATA (&vframe, i);
          height = GST_VIDEO_FRAME_COMP_HEIGHT (&vframe, i);

          for (j = 0; j < height; j++) {
            orc_memcpy (dest, src, row_length);
            src += src_stride;
            dest += dest_stride;
          }
        }
        gst_video_frame_unmap (&vframe);
        ret = TRUE;
        break;
      }
        /* FIXME: This should be in libgstvideo as MT12 or similar, see v4l2 */
      case COLOR_EXYNOS_FormatNV12Tiled:
      case COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka:{
        GstVideoFrame vframe;
        gint width = self->color_format_info.width;
        gint height = self->color_format_info.height;
        gint dest_luma_stride, dest_chroma_stride;
        guint8 *src = buf->data + buffer_info->offset;
        guint8 *dest_luma, *dest_chroma;
        gint y;
        const size_t tile_w = (width - 1) / TILE_WIDTH + 1;
        const size_t tile_w_align = (tile_w + 1) & ~1;
        const size_t tile_h_luma = (height - 1) / TILE_HEIGHT + 1;
        const size_t tile_h_chroma = (height / 2 - 1) / TILE_HEIGHT + 1;
        size_t luma_size = tile_w_align * tile_h_luma * TILE_SIZE;

        gst_video_frame_map (&vframe, info, outbuf, GST_MAP_WRITE);
        dest_luma = GST_VIDEO_FRAME_PLANE_DATA (&vframe, 0);
        dest_chroma = GST_VIDEO_FRAME_PLANE_DATA (&vframe, 1);
        dest_luma_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, 0);
        dest_chroma_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, 1);

        if ((luma_size % TILE_GROUP_SIZE) != 0)
          luma_size =
              (((luma_size - 1) / TILE_GROUP_SIZE) + 1) * TILE_GROUP_SIZE;

        for (y = 0; y < tile_h_luma; y++) {
          size_t row_width = width;
          gint x;

          for (x = 0; x < tile_w; x++) {
            size_t tile_width = row_width;
            size_t tile_height = height;
            gint luma_idx;
            gint chroma_idx;
            /* luma source pointer for this tile */
            const uint8_t *src_luma = src
                + tile_pos (x, y, tile_w_align, tile_h_luma) * TILE_SIZE;

            /* chroma source pointer for this tile */
            const uint8_t *src_chroma = src + luma_size
                + tile_pos (x, y / 2, tile_w_align, tile_h_chroma) * TILE_SIZE;
            if (y & 1)
              src_chroma += TILE_SIZE / 2;

            /* account for right columns */
            if (tile_width > TILE_WIDTH)
              tile_width = TILE_WIDTH;

            /* account for bottom rows */
            if (tile_height > TILE_HEIGHT)
              tile_height = TILE_HEIGHT;

            /* dest luma memory index for this tile */
            luma_idx = y * TILE_HEIGHT * dest_luma_stride + x * TILE_WIDTH;

            /* dest chroma memory index for this tile */
            /* XXX: remove divisions */
            chroma_idx =
                y * TILE_HEIGHT / 2 * dest_chroma_stride + x * TILE_WIDTH;

            tile_height /= 2;   // we copy 2 luma lines at once
            while (tile_height--) {
              memcpy (dest_luma + luma_idx, src_luma, tile_width);
              src_luma += TILE_WIDTH;
              luma_idx += dest_luma_stride;

              memcpy (dest_luma + luma_idx, src_luma, tile_width);
              src_luma += TILE_WIDTH;
              luma_idx += dest_luma_stride;

              memcpy (dest_chroma + chroma_idx, src_chroma, tile_width);
              src_chroma += TILE_WIDTH;
              chroma_idx += dest_chroma_stride;
            }
            row_width -= TILE_WIDTH;
          }
          height -= TILE_HEIGHT;
        }
        gst_video_frame_unmap (&vframe);
        ret = TRUE;
        break;

      }
      default:
        GST_ERROR_OBJECT (self, "Unsupported color format %d",
            self->color_format_info.color_format);
        goto done;
        break;
    }
  }

  /* Just release the output buffer back to the decoder */
  err =
      media_codec_release_output_buffer (self->codec->codec_delegate, 0, FALSE);
  if (err < 0)
    GST_WARNING_OBJECT (self,
        "Failed to release output buffer. Rendering will probably be affected (err: %d).",
        err);

done:
  gst_video_codec_state_unref (state);
  return ret;
}

static gboolean
gst_amc_video_dec_configure_self (GstVideoDecoder * decoder,
    GstAmcFormat * format)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (decoder);
  SurfaceTextureClientHybris surface_texture_client = NULL;
  guint8 i = 0;
  const guint8 MAX_TRIES = 3;
  GError *err = NULL;

  GST_DEBUG_OBJECT (self, "%s", __PRETTY_FUNCTION__);

  g_return_val_if_fail (decoder != NULL, FALSE);
  g_return_val_if_fail (format != NULL, FALSE);

  /* This is a hack that should be able to go away when media-hub is used
   * to instantiate the playbin pipeline instead of QtMultimedia. This is
   * only required for the faster devices so that an eglContext exists and is
   * valid for rendering, and a valid texture id can be passed to the hybris
   * layer.
   */
  while (i < MAX_TRIES) {
    GST_WARNING_OBJECT (self, "Waiting a bit for the texture id");
    g_usleep (G_USEC_PER_SEC / 5);
    ++i;
  }

  /* Try to get a SurfaceTextureClientHybris instance from mirsink */
  if (!gst_mir_ensure_surface_texture_client (self)) {
    GST_ERROR_OBJECT (decoder,
        "Failed to ensure a SurfaceTextureClientHybris instance");
    return FALSE;
  }

  surface_texture_client =
      gst_amc_codec_get_surface_texture_client (self->codec);
  GST_INFO_OBJECT (self, "surface_texture_client: %p", surface_texture_client);

  /* Configure the hardware decoder */
  if (!gst_amc_codec_configure (self->codec, format,
           surface_texture_client, 0, &err)) {
    GST_ERROR_OBJECT (self, "Failed to configure codec");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }

  if (!gst_amc_codec_start (self->codec, &err)) {
    GST_ERROR_OBJECT (self, "Failed to start codec");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }

  if (self->input_buffers)
    gst_amc_codec_free_buffers (self->input_buffers, self->n_input_buffers);
  self->input_buffers =
      gst_amc_codec_get_input_buffers (self->codec, &self->n_input_buffers);
  if (!self->input_buffers) {
    GST_ERROR_OBJECT (self, "Failed to get input buffers");
    return FALSE;
  }
  // JH added
  if (self->output_buffers)
    gst_amc_codec_free_buffers (self->output_buffers, self->n_output_buffers);
  self->output_buffers =
      gst_amc_codec_get_output_buffers (self->codec, &self->n_output_buffers);
  if (!self->output_buffers) {
    GST_ERROR_OBJECT (self, "Failed to get output buffers");
    return FALSE;
  }

  return TRUE;
}

static void
gst_amc_video_dec_loop (GstAmcVideoDec * self)
{
  GstVideoCodecFrame *frame;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstClockTimeDiff deadline;
  gboolean is_eos;
  GstAmcBufferInfo buffer_info;
  gint idx;
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (self);
  GError *err = NULL;

  GST_VIDEO_DECODER_STREAM_LOCK (self);

retry:
  GST_DEBUG_OBJECT (self, "Waiting for available output buffer");
  /* Unlock stream and pad to avoid deadlocks when there are no hybris
   * buffers
   */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SRC_PAD (decoder));
  /* Wait at most 100ms here, some codecs don't fail dequeueing if
   * the codec is flushing, causing deadlocks during shutdown */
  idx =
      gst_amc_codec_dequeue_output_buffer (self->codec, &buffer_info,
      self->current_timeout, &err);
  GST_PAD_STREAM_LOCK (GST_VIDEO_DECODER_SRC_PAD (decoder));
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  GST_DEBUG_OBJECT (self, "Tried to dequeue output buffer (idx: %d)", idx);
  if (idx < 0) {
    if (self->flushing || self->downstream_flow_ret == GST_FLOW_FLUSHING) {
      g_clear_error (&err);
      goto flushing;
    }

    switch (idx) {
      case INFO_OUTPUT_BUFFERS_CHANGED:{
        GST_INFO_OBJECT (self, "Output buffers have changed");

        /* Once the output buffers have changed, we don't need such an
         * aggressive timeout value anymore for queueing/dequeueing
         */
        self->current_timeout = 10000;

        if (self->output_buffers)
          gst_amc_codec_free_buffers (self->output_buffers,
              self->n_output_buffers);
        self->output_buffers =
            gst_amc_codec_get_output_buffers (self->codec,
            &self->n_output_buffers);
        if (!self->output_buffers)
          goto get_output_buffers_error;
        break;
      }
      case INFO_OUTPUT_FORMAT_CHANGED:{
        GstAmcFormat *format;
        /* gchar *format_string; */

        GST_INFO_OBJECT (self, "Output format has changed");

        format = gst_amc_codec_get_output_format (self->codec, &err);
        if (!format)
          goto format_error;

        /*
           format_string = gst_amc_format_to_string (format);
           GST_DEBUG_OBJECT (self, "Got new output format: %s", format_string);
           g_free (format_string);
         */

        if (!gst_amc_video_dec_set_src_caps (self, format)) {
          gst_amc_format_free (format);
          goto format_error;
        }
        gst_amc_format_free (format);

        if (self->output_buffers)
          gst_amc_codec_free_buffers (self->output_buffers,
              self->n_output_buffers);
        self->output_buffers =
            gst_amc_codec_get_output_buffers (self->codec,
            &self->n_output_buffers);
        if (!self->output_buffers)
          goto get_output_buffers_error;

        g_mutex_lock (&self->srccaps_lock);
        self->srccaps_set = TRUE;
        g_cond_signal (&self->srccaps_cond);
        g_mutex_unlock (&self->srccaps_lock);

        goto retry;
        break;
      }
      case INFO_TRY_AGAIN_LATER:
        GST_DEBUG_OBJECT (self,
            "Dequeueing output buffer timed out, trying again");
        goto retry;
        break;
      case G_MININT:
        GST_ERROR_OBJECT (self, "Failure dequeueing output buffer");
        goto dequeue_error;
        break;
      default:
        g_assert_not_reached ();
        break;
    }

    goto retry;
  }

  GST_DEBUG_OBJECT (self,
      "Got output buffer at index %d: size %d time %" G_GINT64_FORMAT
      " flags 0x%08x", idx, buffer_info.size, buffer_info.presentation_time_us,
      buffer_info.flags);

  frame =
      _find_nearest_frame (self,
      gst_util_uint64_scale (buffer_info.presentation_time_us, GST_USECOND, 1));

  is_eos = ! !(buffer_info.flags & BUFFER_FLAG_END_OF_STREAM);

  if (is_eos) {
    GST_DEBUG_OBJECT (self, "EOS detected");
    flow_ret = GST_FLOW_EOS;
  }

  GST_DEBUG_OBJECT (self, "Checking the max decode time deadline");
  if (frame
      && (deadline =
          gst_video_decoder_get_max_decode_time (GST_VIDEO_DECODER (self),
              frame)) < 0) {
    GST_WARNING_OBJECT (self,
        "Frame is too late, dropping (deadline %" GST_TIME_FORMAT ")",
        GST_TIME_ARGS (-deadline));
    flow_ret = gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);

    if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err)) {
      GST_ERROR_OBJECT (self, "Failed to release output buffer index %d", idx);
      if (err && !self->flushing)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      g_clear_error (&err);
    }

  } else if (!frame && buffer_info.size > 0) {
    GstBuffer *outbuf;

    /* This sometimes happens at EOS or if the input is not properly framed,
     * let's handle it gracefully by allocating a new buffer for the current
     * caps and filling it
     */
    GST_ERROR_OBJECT (self, "No corresponding frame found");

    outbuf =
        gst_video_decoder_allocate_output_buffer (GST_VIDEO_DECODER (self));

    if (!gst_amc_video_dec_fill_buffer (self, idx, &buffer_info, outbuf)) {
      gst_buffer_unref (outbuf);
      if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err)) {
        GST_ERROR_OBJECT (self, "Failed to release output buffer index %d",
            idx);
        if (err && !self->flushing)
          GST_ELEMENT_WARNING_FROM_ERROR (self, err);
        g_clear_error (&err);
      }
      goto invalid_buffer;
    }

    GST_BUFFER_PTS (outbuf) =
        gst_util_uint64_scale (buffer_info.presentation_time_us, GST_USECOND,
        1);
    flow_ret = gst_pad_push (GST_VIDEO_DECODER_SRC_PAD (self), outbuf);
  } else if (buffer_info.size >= 0 && is_eos == FALSE) {
    GST_DEBUG_OBJECT (self, "Allocating buffer from pool");
    /* Allocate buffer from the GstBufferPool */
    if ((flow_ret = gst_video_decoder_allocate_output_frame (GST_VIDEO_DECODER
                (self), frame)) != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "Failed to allocate buffer");
      goto flow_error;
    }

    GST_DEBUG_OBJECT (self, "Filling the output buffer");
    /* Stuff the raw decoded video data into the buffer */
    if (!gst_amc_video_dec_fill_buffer (self, idx, &buffer_info,
            frame->output_buffer)) {
      GST_ERROR_OBJECT (self, "Cannot fill output buffer index %d", idx);
      gst_buffer_replace (&frame->output_buffer, NULL);
      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
      if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err)) {
        GST_ERROR_OBJECT (self, "Failed to release output buffer index %d",
            idx);
        if (err && !self->flushing)
          GST_ELEMENT_WARNING_FROM_ERROR (self, err);
        g_clear_error (&err);
      }
      goto invalid_buffer;
    }

    /* Push the frame downstream through the pipeline */
    flow_ret = gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
  } else if (frame != NULL) {
    GST_ERROR_OBJECT (self, "Dropping frame");
    flow_ret = gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);

    if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err)) {
      GST_ERROR_OBJECT (self, "Failed to release output buffer index %d", idx);
      if (err && !self->flushing)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      g_clear_error (&err);
    }
  } else {
    GST_ERROR_OBJECT (self, "Unexpected condition output buffer index %d", idx);
  }

  if (is_eos || flow_ret == GST_FLOW_EOS) {
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    if (self->draining) {
      GST_DEBUG_OBJECT (self, "Drained");
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
    } else if (flow_ret == GST_FLOW_OK) {
      GST_DEBUG_OBJECT (self, "Component signalled EOS");
      flow_ret = GST_FLOW_EOS;
    }
    g_mutex_unlock (&self->drain_lock);
    GST_VIDEO_DECODER_STREAM_LOCK (self);
  } else {
    GST_DEBUG_OBJECT (self, "Finished frame: %s", gst_flow_get_name (flow_ret));
  }

  self->downstream_flow_ret = flow_ret;

  if (flow_ret == GST_FLOW_FLUSHING)
    goto flushing;
  if (flow_ret != GST_FLOW_OK)
    goto flow_error;

  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  return;

dequeue_error:
  {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    GST_ERROR_OBJECT (self, "dec_loop, dequeue_error");
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("Failed to dequeue output buffer"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }
get_output_buffers_error:
  {
    GST_ERROR_OBJECT (self, "dec_loop, get_output_buffers_error");
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("Failed to get output buffers"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }
format_error:
  {
    if (err)
      GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    else
      GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
          ("Failed to handle format"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }
flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- pausing video_decoder_src_pad task");
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_FLUSHING;
    g_mutex_lock (&self->drain_lock);
    if (self->draining) {
      GST_DEBUG_OBJECT (self, "EOS received while flushing");
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
    }
    g_mutex_unlock (&self->drain_lock);
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }
flow_error:
  {
    if (flow_ret == GST_FLOW_EOS) {
      GST_ERROR_OBJECT (self, "dec_loop, flow_error, EOS");
      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    } else if (flow_ret == GST_FLOW_NOT_LINKED || flow_ret < GST_FLOW_EOS) {
      GST_ERROR_OBJECT (self, "dec_loop, flow_error, %d", flow_ret);
      GST_ELEMENT_ERROR (self, STREAM, FAILED,
          ("Internal data stream error."), ("stream stopped, reason %s",
              gst_flow_get_name (flow_ret)));
      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    }
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }
invalid_buffer:
  {
    GST_ERROR_OBJECT (self, "dec_loop, invalid_buffer");
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Invalid sized input buffer"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }
}

static gboolean
gst_amc_video_dec_start (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;

  self = GST_AMC_VIDEO_DEC (decoder);
  GST_DEBUG_OBJECT (self, "Starting decoder");
  self->last_upstream_ts = 0;
  self->eos = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;
  self->started = FALSE;
  self->flushing = TRUE;
  self->waiting_segment = FALSE;
  self->mime = NULL;

  return TRUE;
}

static gboolean
gst_amc_video_dec_stop (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;
  GError *err = NULL;

  self = GST_AMC_VIDEO_DEC (decoder);
  GST_DEBUG_OBJECT (self, "Stopping decoder");
  self->flushing = TRUE;
  if (self->started) {
    gst_amc_codec_flush (self->codec, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
    gst_amc_codec_stop (self->codec, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
    self->started = FALSE;
    if (self->input_buffers)
      gst_amc_codec_free_buffers (self->input_buffers, self->n_input_buffers);
    self->input_buffers = NULL;
    if (self->output_buffers)
      gst_amc_codec_free_buffers (self->output_buffers, self->n_output_buffers);
    self->output_buffers = NULL;
  }
  gst_pad_stop_task (GST_VIDEO_DECODER_SRC_PAD (decoder));

  self->downstream_flow_ret = GST_FLOW_FLUSHING;
  self->eos = FALSE;
  g_mutex_lock (&self->drain_lock);
  self->draining = FALSE;
  g_cond_broadcast (&self->drain_cond);
  g_mutex_unlock (&self->drain_lock);
  g_free (self->codec_data);
  self->codec_data_size = 0;
  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = NULL;
  self->current_timeout = 0;
  GST_DEBUG_OBJECT (self, "Stopped decoder");
  return TRUE;
}

static gboolean
gst_amc_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstAmcVideoDec *self;
  GstAmcFormat *format;
  gboolean ret = TRUE;
  const gchar *mime;
  gboolean is_format_change = FALSE;
  gboolean needs_disable = FALSE;
  /* gchar *format_string; */
  guint8 *codec_data = NULL;
  gsize codec_data_size = 0;
  int buffsize;
  GError *err = NULL;

  self = GST_AMC_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Setting new caps %" GST_PTR_FORMAT, state->caps);

  /* Check if the caps change is a real format change or if only irrelevant
   * parts of the caps have changed or nothing at all.
   */
  is_format_change |= self->color_format_info.width != state->info.width;
  is_format_change |= self->color_format_info.height != state->info.height;
  if (state->codec_data) {
    GstMapInfo cminfo;

    GST_DEBUG_OBJECT (self, "state->codec_data present!");

    gst_buffer_map (state->codec_data, &cminfo, GST_MAP_READ);
    codec_data = g_memdup (cminfo.data, cminfo.size);
    codec_data_size = cminfo.size;

    is_format_change |= (!self->codec_data
        || self->codec_data_size != codec_data_size
        || memcmp (self->codec_data, codec_data, codec_data_size) != 0);
    gst_buffer_unmap (state->codec_data, &cminfo);
  } else if (self->codec_data) {
    GST_DEBUG_OBJECT (self, "format change");
    is_format_change |= TRUE;
  }

  needs_disable = self->started;

  /* If the component is not started and a real format change happens
   * we have to restart the component. If no real format change
   * happened we can just exit here.
   */
  if (needs_disable && !is_format_change) {
    g_free (codec_data);
    codec_data = NULL;
    codec_data_size = 0;

    /* Framerate or something minor changed */
    self->input_state_changed = TRUE;
    if (self->input_state)
      gst_video_codec_state_unref (self->input_state);
    self->input_state = gst_video_codec_state_ref (state);
    GST_DEBUG_OBJECT (self,
        "Already running and caps did not change the format");
    return TRUE;
  }

  if (needs_disable && is_format_change) {
    gst_amc_video_dec_drain (self, FALSE);
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    gst_amc_video_dec_stop (GST_VIDEO_DECODER (self));
    GST_VIDEO_DECODER_STREAM_LOCK (self);
    gst_amc_video_dec_close (GST_VIDEO_DECODER (self));
    if (!gst_amc_video_dec_open (GST_VIDEO_DECODER (self))) {
      GST_ERROR_OBJECT (self, "Failed to open codec again");
      return FALSE;
    }

    if (!gst_amc_video_dec_start (GST_VIDEO_DECODER (self))) {
      GST_ERROR_OBJECT (self, "Failed to start codec again");
    }
  }
  /* srcpad task is not running at this point */
  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = NULL;

  g_free (self->codec_data);
  self->codec_data = codec_data;
  self->codec_data_size = codec_data_size;

  GST_DEBUG_OBJECT (self, "codec_data_size: %zu", codec_data_size);

  mime = get_caps_data (state->caps, &buffsize);
  if (!mime) {
    GST_ERROR_OBJECT (self, "Failed to convert caps to mime");
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "caps are %" GST_PTR_FORMAT, state->caps);

  GST_DEBUG_OBJECT (self, "mime: '%s', width: %d, height %d, buffsize %d", mime,
      state->info.width, state->info.height, buffsize);
  format =
      gst_amc_format_new_video (mime, state->info.width, state->info.height,
          buffsize, &err);
  if (!format) {
    GST_ERROR_OBJECT (self, "Failed to create video format");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }

  /* FIXME: This buffer needs to be valid until the codec is stopped again */
  if (self->codec_data) {
    gst_amc_format_set_buffer (format, "csd-0", self->codec_data,
        self->codec_data_size, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
  }

  /* Configure the hardware codec with format */
  ret = gst_amc_video_dec_configure_self (decoder, format);
  if (!ret) {
    goto failed_configure;
  }

  gst_amc_format_free (format);
  format = NULL;

  self->mime = mime;
  self->started = TRUE;
  self->input_state = gst_video_codec_state_ref (state);
  self->input_state_changed = TRUE;

  /* Start the srcpad loop again */
  self->flushing = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;

  gst_pad_start_task (GST_VIDEO_DECODER_SRC_PAD (self),
      (GstTaskFunction) gst_amc_video_dec_loop, decoder, NULL);

  return ret;

failed_configure:
  GST_ERROR_OBJECT (self, "Failed to configure hardware codec");
  gst_amc_format_free (format);
  format = NULL;
  return FALSE;
}

static gboolean
gst_amc_video_dec_flush (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;
  GError *err = NULL;

  self = GST_AMC_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Flushing decoder");

  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Codec not started yet");
    return TRUE;
  }

  self->flushing = TRUE;
  gst_amc_codec_flush (self->codec, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);

  /* Wait until the srcpad loop is finished,
   * unlock GST_VIDEO_DECODER_STREAM_LOCK to prevent deadlocks
   * caused by using this lock from inside the loop function */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  GST_PAD_STREAM_LOCK (GST_VIDEO_DECODER_SRC_PAD (self));
  GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SRC_PAD (self));
  GST_VIDEO_DECODER_STREAM_LOCK (self);
  self->flushing = FALSE;

  /* Start the srcpad loop again */
  self->last_upstream_ts = 0;
  self->eos = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;
  gst_pad_start_task (GST_VIDEO_DECODER_SRC_PAD (self),
      (GstTaskFunction) gst_amc_video_dec_loop, decoder, NULL);

  GST_DEBUG_OBJECT (self, "Flushed decoder");

  return TRUE;
}

static unsigned
skip_forbidden_nalus(const GstAmcVideoDec *self, GstMapInfo *minfo,
    unsigned offset)
{
  /* If we have an Access Unit Delimiter NALU (6 bytes), we remove it from the
   * stream, because it is not handled well by MTK's decoder (if there are
   * PPS/SPS NALUs after it they are ignored), and it is useless anyway. To
   * detect it we search for type 9 NALU after a start code (0x00000001).
   */
  if (g_strcmp0("video/avc", self->mime) == 0 &&
      minfo->size - offset > 5 &&
      GST_READ_UINT32_BE (minfo->data + offset) == 0x01 &&
    (*(minfo->data + offset + 4) & 0x1F) == 9) {
    offset += 6;
  }

  /* The VPS (Video Parameter Set) nalus introduced in HEVC are not handled by
   * MTK decoder, we move to the following start code. VPS is not necessary for
   * the decoding process if there are no extensions (see
   * https://tools.ietf.org/html/draft-ietf-payload-rtp-h265-07).
   */
  if (g_strcmp0("video/hevc", self->mime) == 0 &&
      GST_READ_UINT32_BE (minfo->data + offset) == 0x01 &&
      (*(minfo->data + offset + 4) >> 1) == 32) {
    /* Jump start code and minimum length of VPS */
    offset += 10;
    for (; offset + 4 <= minfo->size; ++offset) {
      if (GST_READ_UINT32_BE (minfo->data + offset) == 0x01)
        break;
    }
  }

  return offset;
}

static GstFlowReturn
gst_amc_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  /* 1 sec (wait_buff_us * max_touts) is enough for the first video input buffer
   * from the hybris layer to appear.
   */
  static const gint64 wait_buff_us = 100000;
  static const int max_touts = 10;
  int num_touts = 0;
  GstAmcVideoDec *self;
  gint idx;
  GstAmcBuffer *buf;
  GstAmcBufferInfo buffer_info;
  guint offset = 0;
  GstClockTime timestamp, duration, timestamp_offset = 0;
  GstMapInfo minfo;
  gboolean first_buff = TRUE;
  GError *err = NULL;

  memset (&minfo, 0, sizeof (minfo));

  self = GST_AMC_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Handling frame");

  if (!self->started) {
    GST_ERROR_OBJECT (self, "Codec not started yet");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_NOT_NEGOTIATED;
  }

  if (self->eos) {
    GST_WARNING_OBJECT (self, "Got frame after EOS");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_EOS;
  }

  if (self->flushing)
    goto flushing;

  if (self->downstream_flow_ret != GST_FLOW_OK)
    goto downstream_error;

  timestamp = frame->pts;
  duration = frame->duration;

  gst_buffer_map (frame->input_buffer, &minfo, GST_MAP_READ);

  /* We assume here that the "forbidden" nalus are always at the beginning of
   * the frame, which might become false in the end.
   */
  offset = skip_forbidden_nalus(self, &minfo, offset);

  while (offset < minfo.size) {
    /* Make sure to release the base class stream lock, otherwise
     * _loop() can't call _finish_frame() and we might block forever
     * because no input buffers are released. We also unlock the pad
     * to reduce chances of dead-locking.
     */
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SINK_PAD (decoder));
    /* Wait at most 100ms here, some codecs don't fail dequeueing if
     * the codec is flushing, causing deadlocks during shutdown */
    idx =
        gst_amc_codec_dequeue_input_buffer (self->codec, wait_buff_us, &err);
    GST_PAD_STREAM_LOCK (GST_VIDEO_DECODER_SINK_PAD (decoder));
    GST_VIDEO_DECODER_STREAM_LOCK (self);

    GST_DEBUG_OBJECT (self, "Tried to dequeue input buffer idx: %d", idx);
    if (idx < 0) {
      if (self->flushing) {
        g_clear_error (&err);
        goto flushing;
      }
      switch (idx) {
        case INFO_TRY_AGAIN_LATER:
          GST_DEBUG_OBJECT (self, "Dequeueing input buffer timed out");
          /* We try to dequeue num_touts times before giving up. We call
           * several times gst_amc_codec_dequeue_input_buffer to minimise delay
           * when the element is flushed.
           */
          if (++num_touts >= max_touts) {
            num_touts = 0;
            /* We have been waiting for the segment event after a seek event for
             * too long. This usually happens because the pipeline is deadlocked
             * and some event has not been propagated. This event will not move
             * forward until the streaming thread unlocks all pads in the
             * pipeline, which will not happen unless we return from this
             * function. So we force that. The ideal solution would be to change
             * this element so it complies with gstreamer rules (like properlly
             * prerolling and avoiding these loops we have to grab hybris
             * buffers).
             */
            if (self->waiting_segment)
              goto timeout_error;
            else
              continue;
          } else {
            continue;
          }
        case G_MININT:
          GST_ERROR_OBJECT (self, "Failed to dequeue input buffer");
          goto dequeue_error;
        default:
          g_assert_not_reached ();
          break;
      }

      continue;
    }

    if (idx >= self->n_input_buffers)
      goto invalid_buffer_index;

    if (self->flushing)
      goto flushing;

    if (self->downstream_flow_ret != GST_FLOW_OK) {
      memset (&buffer_info, 0, sizeof (buffer_info));
      gst_amc_codec_queue_input_buffer (self->codec, idx, &buffer_info, &err);
      if (err && !self->flushing)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      g_clear_error (&err);
      goto downstream_error;
    }

    /* Now handle the frame */

    /* Copy the buffer content in chunks of size as requested
     * by the port */
    buf = &self->input_buffers[idx];

    memset (&buffer_info, 0, sizeof (buffer_info));
    buffer_info.offset = 0;
    buffer_info.size = MIN (minfo.size - offset, buf->size);

    orc_memcpy (buf->data, minfo.data + offset, buffer_info.size);

    /* Interpolate timestamps if we're passing the buffer
     * in multiple chunks */
    if (!first_buff && duration != GST_CLOCK_TIME_NONE) {
      timestamp_offset = gst_util_uint64_scale (offset, duration, minfo.size);
    }

    if (timestamp != GST_CLOCK_TIME_NONE) {
      buffer_info.presentation_time_us =
          gst_util_uint64_scale (timestamp + timestamp_offset, 1, GST_USECOND);
      self->last_upstream_ts = timestamp + timestamp_offset;
    }
    if (duration != GST_CLOCK_TIME_NONE)
      self->last_upstream_ts += duration;

    if (first_buff) {
      BufferIdentification *id =
          buffer_identification_new (timestamp + timestamp_offset);
      if (GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame))
        buffer_info.flags |= BUFFER_FLAG_SYNC_FRAME;
      gst_video_codec_frame_set_user_data (frame, id,
          (GDestroyNotify) buffer_identification_free);
    }

    offset += buffer_info.size;
    GST_DEBUG_OBJECT (self,
        "Queueing buffer %d: size %d time %" G_GINT64_FORMAT " flags 0x%08x",
        idx, buffer_info.size, buffer_info.presentation_time_us,
        buffer_info.flags);
    if (!gst_amc_codec_queue_input_buffer (self->codec, idx, &buffer_info,
            &err)) {
      if (self->flushing) {
        g_clear_error (&err);
        goto flushing;
      }
      goto queue_error;
    }

    first_buff = FALSE;
  }

  gst_buffer_unmap (frame->input_buffer, &minfo);
  gst_video_codec_frame_unref (frame);

  return self->downstream_flow_ret;

downstream_error:
  {
    GST_ERROR_OBJECT (self, "Downstream returned %s",
        gst_flow_get_name (self->downstream_flow_ret));
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return self->downstream_flow_ret;
  }
invalid_buffer_index:
  {
    GST_ERROR_OBJECT (self, "handle_frame, invalid_buffer_index");
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("Invalid input buffer index %d of %zu", idx, self->n_input_buffers));
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
timeout_error:
  {
    GST_ERROR_OBJECT (self, "handle_frame, timeout_error");
    self->waiting_segment = FALSE;
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
dequeue_error:
  {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
queue_error:
  {
    GST_VIDEO_DECODER_ERROR_FROM_ERROR (self, err);
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
flushing:
  {
    GST_ERROR_OBJECT (self, "Flushing -- returning FLUSHING");
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_FLUSHING;
  }
}

static GstFlowReturn
gst_amc_video_dec_finish (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;

  self = GST_AMC_VIDEO_DEC (decoder);

  return gst_amc_video_dec_drain (self, TRUE);
}

static GstFlowReturn
gst_amc_video_dec_drain (GstAmcVideoDec * self, gboolean at_eos)
{
  GstFlowReturn ret;
  gint idx;
  GError *err = NULL;

  GST_DEBUG_OBJECT (self, "Draining codec");
  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Codec not started yet");
    return GST_FLOW_OK;
  }

  /* Don't send EOS buffer twice, this doesn't work */
  if (self->eos) {
    GST_DEBUG_OBJECT (self, "Codec is EOS already");
    return GST_FLOW_OK;
  }
  if (at_eos)
    self->eos = TRUE;

  /* Make sure to release the base class stream lock, otherwise
   * _loop() can't call _finish_frame() and we might block forever
   * because no input buffers are released */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  /* Send an EOS buffer to the component and let the base
   * class drop the EOS event. We will send it later when
   * the EOS buffer arrives on the output port.
   * Wait at most 0.5s here. */
  idx = gst_amc_codec_dequeue_input_buffer (self->codec, self->current_timeout,
            &err);
  GST_DEBUG_OBJECT (self, "dequeued input buffer with idx: %d", idx);
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  if (idx >= 0 && idx < self->n_input_buffers) {
    GstAmcBufferInfo buffer_info;

    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = TRUE;

    memset (&buffer_info, 0, sizeof (buffer_info));
    buffer_info.size = 0;
    buffer_info.presentation_time_us =
        gst_util_uint64_scale (self->last_upstream_ts, 1, GST_USECOND);
    buffer_info.flags |= BUFFER_FLAG_END_OF_STREAM;

    if (gst_amc_codec_queue_input_buffer (self->codec, idx, &buffer_info, &err)) {
      GST_DEBUG_OBJECT (self, "Waiting until codec is drained");
      g_cond_wait (&self->drain_cond, &self->drain_lock);
      GST_DEBUG_OBJECT (self, "Drained codec");
      ret = GST_FLOW_OK;
    } else {
      GST_ERROR_OBJECT (self, "Failed to queue input buffer");
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      ret = GST_FLOW_ERROR;
    }

    g_mutex_unlock (&self->drain_lock);
    GST_VIDEO_DECODER_STREAM_LOCK (self);
  } else if (idx >= self->n_input_buffers) {
    GST_ERROR_OBJECT (self, "Invalid input buffer index %d of %zu",
        idx, self->n_input_buffers);
    ret = GST_FLOW_ERROR;
  } else {
    GST_ERROR_OBJECT (self, "Failed to acquire buffer for EOS: %d", idx);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
    ret = GST_FLOW_ERROR;
  }

  return ret;
}

static void
gst_amc_video_dec_set_context (GstElement * element, GstContext * context)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (element);
  SurfaceTextureClientHybris stc;

  GST_DEBUG_OBJECT (element, "%s", __PRETTY_FUNCTION__);

  if (self->codec == NULL) {
    GST_WARNING_OBJECT (self, "codec not set yet");
    return;
  }

  stc = gst_context_get_surface_texture_client (context);
  if (!stc)
    GST_ERROR_OBJECT (self,
        "Failed to get SurfaceTextureClient instance. Hardware video rendering will not function");

  GST_DEBUG_OBJECT (self, "stc: %p", stc);
  if (!gst_amc_codec_set_surface_texture_client (self->codec, stc))
    GST_ERROR_OBJECT (self,
        "Failed to set SurfaceTextureClientHybris instance for decoder. Hardware video rendering will not function");
}

static gboolean
gst_amc_video_dec_decide_allocation (GstVideoDecoder * bdec, GstQuery * query)
{
  GstBufferPool *pool;
  GstMirBufferPool *m_pool;
  GstStructure *config;
  GstCaps *caps;
  guint i, n;

  GST_DEBUG_OBJECT (bdec, "%s", __PRETTY_FUNCTION__);
  GST_DEBUG_OBJECT (bdec, "Deciding ALLOCATION params");

  /* Prefer a MirImage allocator if available */
  gst_query_parse_allocation (query, &caps, NULL);
  /* if (caps && gst_video_info_from_caps (&info, caps) && info.finfo->format == GST_VIDEO_FORMAT_RGBA) { */
  {
    GST_DEBUG_OBJECT (bdec, "Considering using the MirImage allocator");
    n = gst_query_get_n_allocation_params (query);
    for (i = 0; i < n; i++) {
      GstAllocator *allocator = NULL;
      GstAllocationParams params;

      gst_query_parse_nth_allocation_param (query, i, &allocator, &params);
      /* Select the MirImage allocator from the ALLOCATION query */
      if (allocator
          && g_strcmp0 (allocator->mem_type, GST_MIR_IMAGE_MEMORY_TYPE) == 0) {
        GST_DEBUG_OBJECT (bdec, "Found the MirImage allocator!");
        g_assert (allocator != NULL);
        gst_query_set_nth_allocation_param (query, 0, allocator, &params);
        while (gst_query_get_n_allocation_params (query) > 1)
          gst_query_remove_nth_allocation_param (query, 1);
        break;
      }
    }
  }

  if (!GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation (bdec, query))
    return FALSE;

  g_assert (gst_query_get_n_allocation_pools (query) > 0);
  gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);
  g_assert (pool != NULL);

  /* Add the codec_delegate instance to the current pool */
  m_pool = (GstMirBufferPool *) pool;
  m_pool->codec_delegate = (GST_AMC_VIDEO_DEC (bdec))->codec->codec_delegate;

  config = gst_buffer_pool_get_config (pool);
  if (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL)) {
    GST_DEBUG_OBJECT (bdec,
        "ALLOCATION query has GST_VIDEO_META_API_TYPE embedded");
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
  }
  gst_buffer_pool_set_config (pool, config);
  gst_object_unref (pool);

  return TRUE;
}
