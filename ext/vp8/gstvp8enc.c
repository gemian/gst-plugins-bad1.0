/* VP8
 * Copyright (C) 2006 David Schleef <ds@schleef.org>
 * Copyright (C) 2010 Entropy Wave Inc
 * Copyright (C) 2010 Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */
/**
 * SECTION:element-vp8enc
 * @see_also: vp8dec, webmmux, oggmux
 *
 * This element encodes raw video into a VP8 stream.
 * <ulink url="http://www.webmproject.org">VP8</ulink> is a royalty-free
 * video codec maintained by <ulink url="http://www.google.com/">Google
 * </ulink>. It's the successor of On2 VP3, which was the base of the
 * Theora video codec.
 *
 * To control the quality of the encoding, the #GstVP8Enc::bitrate and
 * #GstVP8Enc::quality properties can be used. These two properties are
 * mutualy exclusive. Setting the bitrate property will produce a constant
 * bitrate (CBR) stream while setting the quality property will produce a
 * variable bitrate (VBR) stream.
 *
 * <refsect2>
 * <title>Example pipeline</title>
 * |[
 * gst-launch -v videotestsrc num-buffers=1000 ! vp8enc ! webmmux ! filesink location=videotestsrc.webm
 * ]| This example pipeline will encode a test video source to VP8 muxed in an
 * WebM container.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_VP8_ENCODER

#include <gst/tag/tag.h>
#include <string.h>

#include "gstvp8utils.h"
#include "gstvp8enc.h"

GST_DEBUG_CATEGORY_STATIC (gst_vp8enc_debug);
#define GST_CAT_DEFAULT gst_vp8enc_debug

typedef struct
{
  vpx_image_t *image;
  GList *invisible;
} GstVP8EncCoderHook;

#define DEFAULT_BITRATE 0
#define DEFAULT_MODE VPX_VBR
#define DEFAULT_QUALITY 5
#define DEFAULT_ERROR_RESILIENT FALSE
#define DEFAULT_MAX_LATENCY 10
#define DEFAULT_MAX_KEYFRAME_DISTANCE 60
#define DEFAULT_SPEED 0
#define DEFAULT_THREADS 1
#define DEFAULT_MULTIPASS_MODE VPX_RC_ONE_PASS
#define DEFAULT_MULTIPASS_CACHE_FILE NULL
#define DEFAULT_AUTO_ALT_REF_FRAMES FALSE

enum
{
  PROP_0,
  PROP_BITRATE,
  PROP_MODE,
  PROP_QUALITY,
  PROP_ERROR_RESILIENT,
  PROP_MAX_LATENCY,
  PROP_MAX_KEYFRAME_DISTANCE,
  PROP_SPEED,
  PROP_THREADS,
  PROP_MULTIPASS_MODE,
  PROP_MULTIPASS_CACHE_FILE,
  PROP_AUTO_ALT_REF_FRAMES
};

#define GST_VP8_ENC_MODE_TYPE (gst_vp8_enc_mode_get_type())
static GType
gst_vp8_enc_mode_get_type (void)
{
  static const GEnumValue values[] = {
    {VPX_VBR, "Variable Bit Rate (VBR) mode", "vbr"},
    {VPX_CBR, "Constant Bit Rate (CBR) mode", "cbr"},
    {0, NULL, NULL}
  };
  static volatile GType id = 0;

  if (g_once_init_enter ((gsize *) & id)) {
    GType _id;

    _id = g_enum_register_static ("GstVP8EncMode", values);

    g_once_init_leave ((gsize *) & id, _id);
  }

  return id;
}

#define GST_VP8_ENC_MULTIPASS_MODE_TYPE (gst_vp8_enc_multipass_mode_get_type())
static GType
gst_vp8_enc_multipass_mode_get_type (void)
{
  static const GEnumValue values[] = {
    {VPX_RC_ONE_PASS, "One pass encoding (default)", "one-pass"},
    {VPX_RC_FIRST_PASS, "First pass of multipass encoding", "first-pass"},
    {VPX_RC_LAST_PASS, "Last pass of multipass encoding", "last-pass"},
    {0, NULL, NULL}
  };
  static volatile GType id = 0;

  if (g_once_init_enter ((gsize *) & id)) {
    GType _id;

    _id = g_enum_register_static ("GstVP8EncMultipassMode", values);

    g_once_init_leave ((gsize *) & id, _id);
  }

  return id;
}

static void gst_vp8_enc_finalize (GObject * object);
static void gst_vp8_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_vp8_enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_vp8_enc_start (GstBaseVideoEncoder * encoder);
static gboolean gst_vp8_enc_stop (GstBaseVideoEncoder * encoder);
static gboolean gst_vp8_enc_set_format (GstBaseVideoEncoder *
    base_video_encoder, GstVideoState * state);
static gboolean gst_vp8_enc_finish (GstBaseVideoEncoder * base_video_encoder);
static gboolean gst_vp8_enc_handle_frame (GstBaseVideoEncoder *
    base_video_encoder, GstVideoFrame * frame);
static GstFlowReturn gst_vp8_enc_shape_output (GstBaseVideoEncoder * encoder,
    GstVideoFrame * frame);
static gboolean gst_vp8_enc_sink_event (GstBaseVideoEncoder *
    base_video_encoder, GstEvent * event);
static GstCaps *gst_vp8_enc_get_caps (GstBaseVideoEncoder * base_video_encoder);

static GstStaticPadTemplate gst_vp8_enc_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_YUV ("I420"))
    );

static GstStaticPadTemplate gst_vp8_enc_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-vp8")
    );

static void
do_init (GType vp8enc_type)
{
  static const GInterfaceInfo tag_setter_info = { NULL, NULL, NULL };
  const GInterfaceInfo preset_interface_info = {
    NULL,                       /* interface_init */
    NULL,                       /* interface_finalize */
    NULL                        /* interface_data */
  };

  g_type_add_interface_static (vp8enc_type, GST_TYPE_TAG_SETTER,
      &tag_setter_info);
  g_type_add_interface_static (vp8enc_type, GST_TYPE_PRESET,
      &preset_interface_info);
}

GST_BOILERPLATE_FULL (GstVP8Enc, gst_vp8_enc, GstBaseVideoEncoder,
    GST_TYPE_BASE_VIDEO_ENCODER, do_init);

static void
gst_vp8_enc_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_vp8_enc_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_vp8_enc_sink_template));

  gst_element_class_set_details_simple (element_class,
      "On2 VP8 Encoder",
      "Codec/Encoder/Video",
      "Encode VP8 video streams", "David Schleef <ds@entropywave.com>");
}

static void
gst_vp8_enc_class_init (GstVP8EncClass * klass)
{
  GObjectClass *gobject_class;
  GstBaseVideoEncoderClass *base_video_encoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  base_video_encoder_class = GST_BASE_VIDEO_ENCODER_CLASS (klass);

  gobject_class->set_property = gst_vp8_enc_set_property;
  gobject_class->get_property = gst_vp8_enc_get_property;
  gobject_class->finalize = gst_vp8_enc_finalize;

  base_video_encoder_class->start = gst_vp8_enc_start;
  base_video_encoder_class->stop = gst_vp8_enc_stop;
  base_video_encoder_class->handle_frame = gst_vp8_enc_handle_frame;
  base_video_encoder_class->set_format = gst_vp8_enc_set_format;
  base_video_encoder_class->finish = gst_vp8_enc_finish;
  base_video_encoder_class->shape_output = gst_vp8_enc_shape_output;
  base_video_encoder_class->event = gst_vp8_enc_sink_event;
  base_video_encoder_class->get_caps = gst_vp8_enc_get_caps;

  g_object_class_install_property (gobject_class, PROP_BITRATE,
      g_param_spec_int ("bitrate", "Bit rate",
          "Bit rate (in bits/sec)",
          0, 1000000000, DEFAULT_BITRATE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_MODE,
      g_param_spec_enum ("mode", "Mode",
          "Mode",
          GST_VP8_ENC_MODE_TYPE, DEFAULT_MODE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_QUALITY,
      g_param_spec_double ("quality", "Quality",
          "Quality",
          0, 10.0, DEFAULT_QUALITY,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_ERROR_RESILIENT,
      g_param_spec_boolean ("error-resilient", "Error Resilient",
          "Encode streams that are error resilient",
          DEFAULT_ERROR_RESILIENT,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_MAX_LATENCY,
      g_param_spec_int ("max-latency", "Max latency",
          "Number of frames in encoder queue",
          0, 25, DEFAULT_MAX_LATENCY,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_MAX_KEYFRAME_DISTANCE,
      g_param_spec_int ("max-keyframe-distance", "Maximum Key frame distance",
          "Maximum distance between key frames",
          0, 9999, DEFAULT_MAX_KEYFRAME_DISTANCE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_SPEED,
      g_param_spec_int ("speed", "Speed",
          "Speed",
          0, 2, DEFAULT_SPEED,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_THREADS,
      g_param_spec_int ("threads", "Threads",
          "Threads",
          1, 64, DEFAULT_THREADS,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_MULTIPASS_MODE,
      g_param_spec_enum ("multipass-mode", "Multipass Mode",
          "Multipass encode mode",
          GST_VP8_ENC_MULTIPASS_MODE_TYPE, DEFAULT_MULTIPASS_MODE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_MULTIPASS_CACHE_FILE,
      g_param_spec_string ("multipass-cache-file", "Multipass Cache File",
          "Multipass cache file",
          DEFAULT_MULTIPASS_CACHE_FILE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_AUTO_ALT_REF_FRAMES,
      g_param_spec_boolean ("auto-alt-ref-frames", "Auto Alt Ref Frames",
          "Automatically create alternative reference frames",
          DEFAULT_AUTO_ALT_REF_FRAMES,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));


  GST_DEBUG_CATEGORY_INIT (gst_vp8enc_debug, "vp8enc", 0, "VP8 Encoder");
}

static void
gst_vp8_enc_init (GstVP8Enc * gst_vp8_enc, GstVP8EncClass * klass)
{

  GST_DEBUG_OBJECT (gst_vp8_enc, "init");

  gst_vp8_enc->bitrate = DEFAULT_BITRATE;
  gst_vp8_enc->mode = DEFAULT_MODE;
  gst_vp8_enc->quality = DEFAULT_QUALITY;
  gst_vp8_enc->error_resilient = DEFAULT_ERROR_RESILIENT;
  gst_vp8_enc->max_latency = DEFAULT_MAX_LATENCY;
  gst_vp8_enc->max_keyframe_distance = DEFAULT_MAX_KEYFRAME_DISTANCE;
  gst_vp8_enc->multipass_mode = DEFAULT_MULTIPASS_MODE;
  gst_vp8_enc->multipass_cache_file = DEFAULT_MULTIPASS_CACHE_FILE;
  gst_vp8_enc->auto_alt_ref_frames = DEFAULT_AUTO_ALT_REF_FRAMES;
}

static void
gst_vp8_enc_finalize (GObject * object)
{
  GstVP8Enc *gst_vp8_enc;

  GST_DEBUG_OBJECT (object, "finalize");

  g_return_if_fail (GST_IS_VP8_ENC (object));
  gst_vp8_enc = GST_VP8_ENC (object);

  g_free (gst_vp8_enc->multipass_cache_file);
  gst_vp8_enc->multipass_cache_file = NULL;

  G_OBJECT_CLASS (parent_class)->finalize (object);

}

static void
gst_vp8_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVP8Enc *gst_vp8_enc;

  g_return_if_fail (GST_IS_VP8_ENC (object));
  gst_vp8_enc = GST_VP8_ENC (object);

  GST_DEBUG_OBJECT (object, "gst_vp8_enc_set_property");
  switch (prop_id) {
    case PROP_BITRATE:
      gst_vp8_enc->bitrate = g_value_get_int (value);
      break;
    case PROP_MODE:
      gst_vp8_enc->mode = g_value_get_enum (value);
      break;
    case PROP_QUALITY:
      gst_vp8_enc->quality = g_value_get_double (value);
      break;
    case PROP_ERROR_RESILIENT:
      gst_vp8_enc->error_resilient = g_value_get_boolean (value);
      break;
    case PROP_MAX_LATENCY:
      gst_vp8_enc->max_latency = g_value_get_int (value);
      break;
    case PROP_MAX_KEYFRAME_DISTANCE:
      gst_vp8_enc->max_keyframe_distance = g_value_get_int (value);
      break;
    case PROP_SPEED:
      gst_vp8_enc->speed = g_value_get_int (value);
      break;
    case PROP_THREADS:
      gst_vp8_enc->threads = g_value_get_int (value);
      break;
    case PROP_MULTIPASS_MODE:
      gst_vp8_enc->multipass_mode = g_value_get_enum (value);
      break;
    case PROP_MULTIPASS_CACHE_FILE:
      if (gst_vp8_enc->multipass_cache_file)
        g_free (gst_vp8_enc->multipass_cache_file);
      gst_vp8_enc->multipass_cache_file = g_value_dup_string (value);
      break;
    case PROP_AUTO_ALT_REF_FRAMES:
      gst_vp8_enc->auto_alt_ref_frames = g_value_get_boolean (value);
      break;
    default:
      break;
  }
}

static void
gst_vp8_enc_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstVP8Enc *gst_vp8_enc;

  g_return_if_fail (GST_IS_VP8_ENC (object));
  gst_vp8_enc = GST_VP8_ENC (object);

  switch (prop_id) {
    case PROP_BITRATE:
      g_value_set_int (value, gst_vp8_enc->bitrate);
      break;
    case PROP_MODE:
      g_value_set_enum (value, gst_vp8_enc->mode);
      break;
    case PROP_QUALITY:
      g_value_set_double (value, gst_vp8_enc->quality);
      break;
    case PROP_ERROR_RESILIENT:
      g_value_set_boolean (value, gst_vp8_enc->error_resilient);
      break;
    case PROP_MAX_LATENCY:
      g_value_set_int (value, gst_vp8_enc->max_latency);
      break;
    case PROP_MAX_KEYFRAME_DISTANCE:
      g_value_set_int (value, gst_vp8_enc->max_keyframe_distance);
      break;
    case PROP_SPEED:
      g_value_set_int (value, gst_vp8_enc->speed);
      break;
    case PROP_THREADS:
      g_value_set_int (value, gst_vp8_enc->threads);
      break;
    case PROP_MULTIPASS_MODE:
      g_value_set_enum (value, gst_vp8_enc->multipass_mode);
      break;
    case PROP_MULTIPASS_CACHE_FILE:
      g_value_set_string (value, gst_vp8_enc->multipass_cache_file);
      break;
    case PROP_AUTO_ALT_REF_FRAMES:
      g_value_set_boolean (value, gst_vp8_enc->auto_alt_ref_frames);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_vp8_enc_start (GstBaseVideoEncoder * base_video_encoder)
{
  GST_DEBUG_OBJECT (base_video_encoder, "start");

  return TRUE;
}

static gboolean
gst_vp8_enc_stop (GstBaseVideoEncoder * base_video_encoder)
{
  GstVP8Enc *encoder;

  GST_DEBUG_OBJECT (base_video_encoder, "stop");

  encoder = GST_VP8_ENC (base_video_encoder);

  if (encoder->inited) {
    vpx_codec_destroy (&encoder->encoder);
    encoder->inited = FALSE;
  }

  if (encoder->first_pass_cache_content) {
    g_byte_array_free (encoder->first_pass_cache_content, TRUE);
    encoder->first_pass_cache_content = NULL;
  }

  if (encoder->last_pass_cache_content.buf) {
    g_free (encoder->last_pass_cache_content.buf);
    encoder->last_pass_cache_content.buf = NULL;
    encoder->last_pass_cache_content.sz = 0;
  }

  gst_tag_setter_reset_tags (GST_TAG_SETTER (encoder));

  return TRUE;
}

static gboolean
gst_vp8_enc_set_format (GstBaseVideoEncoder * base_video_encoder,
    GstVideoState * state)
{
  GstVP8Enc *encoder;
  vpx_codec_enc_cfg_t cfg;
  vpx_codec_err_t status;

  encoder = GST_VP8_ENC (base_video_encoder);
  GST_DEBUG_OBJECT (base_video_encoder, "set_format");

  if (encoder->inited) {
    GST_DEBUG_OBJECT (base_video_encoder, "refusing renegotiation");
    return FALSE;
  }

  status = vpx_codec_enc_config_default (&vpx_codec_vp8_cx_algo, &cfg, 0);
  if (status != VPX_CODEC_OK) {
    GST_ELEMENT_ERROR (encoder, LIBRARY, INIT,
        ("Failed to get default encoder configuration"), ("%s",
            gst_vpx_error_name (status)));
    return FALSE;
  }

  cfg.g_w = state->width;
  cfg.g_h = state->height;
  cfg.g_timebase.num = state->fps_d;
  cfg.g_timebase.den = state->fps_n;

  cfg.g_error_resilient = encoder->error_resilient;
  cfg.g_lag_in_frames = encoder->max_latency;
  cfg.g_threads = encoder->threads;
  cfg.rc_end_usage = encoder->mode;
  if (encoder->bitrate) {
    cfg.rc_target_bitrate = encoder->bitrate / 1000;
  } else {
    cfg.rc_min_quantizer = 63 - encoder->quality * 5.0;
    cfg.rc_max_quantizer = 63 - encoder->quality * 5.0;
    cfg.rc_target_bitrate = encoder->bitrate;
  }

  cfg.kf_mode = VPX_KF_AUTO;
  cfg.kf_min_dist = 0;
  cfg.kf_max_dist = encoder->max_keyframe_distance;

  cfg.g_pass = encoder->multipass_mode;
  if (encoder->multipass_mode == VPX_RC_FIRST_PASS) {
    encoder->first_pass_cache_content = g_byte_array_sized_new (4096);
  } else if (encoder->multipass_mode == VPX_RC_LAST_PASS) {
    GError *err = NULL;

    if (!encoder->multipass_cache_file) {
      GST_ELEMENT_ERROR (encoder, RESOURCE, OPEN_READ,
          ("No multipass cache file provided"), (NULL));
      return FALSE;
    }

    if (!g_file_get_contents (encoder->multipass_cache_file,
            (gchar **) & encoder->last_pass_cache_content.buf,
            &encoder->last_pass_cache_content.sz, &err)) {
      GST_ELEMENT_ERROR (encoder, RESOURCE, OPEN_READ,
          ("Failed to read multipass cache file provided"), ("%s",
              err->message));
      g_error_free (err);
      return FALSE;
    }
    cfg.rc_twopass_stats_in = encoder->last_pass_cache_content;
  }

  status = vpx_codec_enc_init (&encoder->encoder, &vpx_codec_vp8_cx_algo,
      &cfg, 0);
  if (status != VPX_CODEC_OK) {
    GST_ELEMENT_ERROR (encoder, LIBRARY, INIT,
        ("Failed to initialize encoder"), ("%s", gst_vpx_error_name (status)));
    return FALSE;
  }

  status = vpx_codec_control (&encoder->encoder, VP8E_SET_CPUUSED, 0);
  if (status != VPX_CODEC_OK) {
    GST_WARNING_OBJECT (encoder, "Failed to set VP8E_SET_CPUUSED to 0: %s",
        gst_vpx_error_name (status));
  }

  status =
      vpx_codec_control (&encoder->encoder, VP8E_SET_ENABLEAUTOALTREF,
      (encoder->auto_alt_ref_frames ? 1 : 0));
  if (status != VPX_CODEC_OK) {
    GST_WARNING_OBJECT (encoder,
        "Failed to set VP8E_ENABLEAUTOALTREF to %d: %s",
        (encoder->auto_alt_ref_frames ? 1 : 0), gst_vpx_error_name (status));
  }

  gst_base_video_encoder_set_latency (base_video_encoder, 0,
      gst_util_uint64_scale (encoder->max_latency,
          state->fps_d * GST_SECOND, state->fps_n));
  encoder->inited = TRUE;

  return TRUE;
}

static GstCaps *
gst_vp8_enc_get_caps (GstBaseVideoEncoder * base_video_encoder)
{
  GstCaps *caps;
  const GstVideoState *state;
  GstTagList *tags = NULL;
  const GstTagList *iface_tags;
  GstBuffer *stream_hdr, *vorbiscomment;
  guint8 *data;
  GstStructure *s;
  GValue array = { 0 };
  GValue value = { 0 };

  state = gst_base_video_encoder_get_state (base_video_encoder);

  caps = gst_caps_new_simple ("video/x-vp8",
      "width", G_TYPE_INT, state->width,
      "height", G_TYPE_INT, state->height,
      "framerate", GST_TYPE_FRACTION, state->fps_n,
      state->fps_d,
      "pixel-aspect-ratio", GST_TYPE_FRACTION, state->par_n,
      state->par_d, NULL);

  s = gst_caps_get_structure (caps, 0);

  /* put buffers in a fixed list */
  g_value_init (&array, GST_TYPE_ARRAY);
  g_value_init (&value, GST_TYPE_BUFFER);

  /* Create Ogg stream-info */
  stream_hdr = gst_buffer_new_and_alloc (26);
  data = GST_BUFFER_DATA (stream_hdr);

  GST_WRITE_UINT8 (data, 0x4F);
  GST_WRITE_UINT32_BE (data + 1, 0x56503830);   /* "VP80" */
  GST_WRITE_UINT8 (data + 5, 0x01);     /* stream info header */
  GST_WRITE_UINT8 (data + 6, 1);        /* Major version 1 */
  GST_WRITE_UINT8 (data + 7, 0);        /* Minor version 0 */
  GST_WRITE_UINT16_BE (data + 8, state->width);
  GST_WRITE_UINT16_BE (data + 10, state->height);
  GST_WRITE_UINT24_BE (data + 12, state->par_n);
  GST_WRITE_UINT24_BE (data + 15, state->par_d);
  GST_WRITE_UINT32_BE (data + 18, state->fps_n);
  GST_WRITE_UINT32_BE (data + 22, state->fps_d);

  GST_BUFFER_FLAG_SET (stream_hdr, GST_BUFFER_FLAG_IN_CAPS);
  gst_value_set_buffer (&value, stream_hdr);
  gst_value_array_append_value (&array, &value);
  g_value_unset (&value);
  gst_buffer_unref (stream_hdr);

  iface_tags =
      gst_tag_setter_get_tag_list (GST_TAG_SETTER (base_video_encoder));
  if (iface_tags) {
    vorbiscomment =
        gst_tag_list_to_vorbiscomment_buffer ((iface_tags) ? iface_tags : tags,
        (const guint8 *) "OVP80\2 ", 7,
        "Encoded with GStreamer vp8enc " PACKAGE_VERSION);

    GST_BUFFER_FLAG_SET (vorbiscomment, GST_BUFFER_FLAG_IN_CAPS);

    g_value_init (&value, GST_TYPE_BUFFER);
    gst_value_set_buffer (&value, vorbiscomment);
    gst_value_array_append_value (&array, &value);
    g_value_unset (&value);
    gst_buffer_unref (vorbiscomment);
  }

  gst_structure_set_value (s, "streamheader", &array);
  g_value_unset (&array);

  return caps;
}

static void
gst_vp8_enc_process (GstVP8Enc * encoder)
{
  vpx_codec_iter_t iter = NULL;
  const vpx_codec_cx_pkt_t *pkt;
  GstBaseVideoEncoder *base_video_encoder;
  GstVP8EncCoderHook *hook;
  GstVideoFrame *frame;

  base_video_encoder = GST_BASE_VIDEO_ENCODER (encoder);

  pkt = vpx_codec_get_cx_data (&encoder->encoder, &iter);
  while (pkt != NULL) {
    GstBuffer *buffer;
    gboolean invisible;

    GST_DEBUG_OBJECT (encoder, "packet %u type %d", (guint) pkt->data.frame.sz,
        pkt->kind);

    if (pkt->kind == VPX_CODEC_STATS_PKT
        && encoder->multipass_mode == VPX_RC_FIRST_PASS) {
      GST_LOG_OBJECT (encoder, "handling STATS packet");

      g_byte_array_append (encoder->first_pass_cache_content,
          pkt->data.twopass_stats.buf, pkt->data.twopass_stats.sz);

      frame = gst_base_video_encoder_get_oldest_frame (base_video_encoder);
      if (frame != NULL) {
        buffer = gst_buffer_new ();
        GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_PREROLL);
        frame->src_buffer = buffer;
        gst_base_video_encoder_finish_frame (base_video_encoder, frame);
      }

      pkt = vpx_codec_get_cx_data (&encoder->encoder, &iter);
      continue;
    } else if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) {
      GST_LOG_OBJECT (encoder, "non frame pkt: %d", pkt->kind);
      pkt = vpx_codec_get_cx_data (&encoder->encoder, &iter);
      continue;
    }

    invisible = (pkt->data.frame.flags & VPX_FRAME_IS_INVISIBLE) != 0;
    frame = gst_base_video_encoder_get_oldest_frame (base_video_encoder);
    g_assert (frame != NULL);
    frame->is_sync_point = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
    hook = frame->coder_hook;

    buffer = gst_buffer_new_and_alloc (pkt->data.frame.sz);

    memcpy (GST_BUFFER_DATA (buffer), pkt->data.frame.buf, pkt->data.frame.sz);

    if (hook->image)
      g_slice_free (vpx_image_t, hook->image);
    hook->image = NULL;

    if (invisible) {
      hook->invisible = g_list_append (hook->invisible, buffer);
    } else {
      frame->src_buffer = buffer;
      gst_base_video_encoder_finish_frame (base_video_encoder, frame);
    }

    pkt = vpx_codec_get_cx_data (&encoder->encoder, &iter);
  }
}

static gboolean
gst_vp8_enc_finish (GstBaseVideoEncoder * base_video_encoder)
{
  GstVP8Enc *encoder;
  int flags = 0;
  vpx_codec_err_t status;

  GST_DEBUG_OBJECT (base_video_encoder, "finish");

  encoder = GST_VP8_ENC (base_video_encoder);

  status =
      vpx_codec_encode (&encoder->encoder, NULL, encoder->n_frames, 1, flags,
      0);
  if (status != 0) {
    GST_ERROR_OBJECT (encoder, "encode returned %d %s", status,
        gst_vpx_error_name (status));
    return FALSE;
  }

  /* dispatch remaining frames */
  gst_vp8_enc_process (encoder);

  if (encoder->multipass_mode == VPX_RC_FIRST_PASS
      && encoder->multipass_cache_file) {
    GError *err = NULL;

    if (!g_file_set_contents (encoder->multipass_cache_file,
            (const gchar *) encoder->first_pass_cache_content->data,
            encoder->first_pass_cache_content->len, &err)) {
      GST_ELEMENT_ERROR (encoder, RESOURCE, WRITE, (NULL),
          ("Failed to write multipass cache file: %s", err->message));
      g_error_free (err);
    }
  }

  return TRUE;
}

static vpx_image_t *
gst_vp8_enc_buffer_to_image (GstVP8Enc * enc, GstBuffer * buffer)
{
  vpx_image_t *image = g_slice_new0 (vpx_image_t);
  guint8 *data = GST_BUFFER_DATA (buffer);
  GstVideoState *state = &GST_BASE_VIDEO_CODEC (enc)->state;

  image->fmt = VPX_IMG_FMT_I420;
  image->bps = 12;
  image->x_chroma_shift = image->y_chroma_shift = 1;
  image->img_data = data;
  image->w = image->d_w = state->width;
  image->h = image->d_h = state->height;

  image->stride[VPX_PLANE_Y] =
      gst_video_format_get_row_stride (state->format, 0, state->width);
  image->stride[VPX_PLANE_U] =
      gst_video_format_get_row_stride (state->format, 1, state->width);
  image->stride[VPX_PLANE_V] =
      gst_video_format_get_row_stride (state->format, 2, state->width);
  image->planes[VPX_PLANE_Y] =
      data + gst_video_format_get_component_offset (state->format, 0,
      state->width, state->height);
  image->planes[VPX_PLANE_U] =
      data + gst_video_format_get_component_offset (state->format, 1,
      state->width, state->height);
  image->planes[VPX_PLANE_V] =
      data + gst_video_format_get_component_offset (state->format, 2,
      state->width, state->height);

  return image;
}

static const int speed_table[] = {
  VPX_DL_BEST_QUALITY,
  VPX_DL_GOOD_QUALITY,
  VPX_DL_REALTIME,
};

static gboolean
gst_vp8_enc_handle_frame (GstBaseVideoEncoder * base_video_encoder,
    GstVideoFrame * frame)
{
  GstVP8Enc *encoder;
  const GstVideoState *state;
  vpx_codec_err_t status;
  int flags = 0;
  vpx_image_t *image;
  GstVP8EncCoderHook *hook;

  GST_DEBUG_OBJECT (base_video_encoder, "handle_frame");

  encoder = GST_VP8_ENC (base_video_encoder);

  state = gst_base_video_encoder_get_state (base_video_encoder);
  encoder->n_frames++;

  GST_DEBUG_OBJECT (base_video_encoder, "size %d %d", state->width,
      state->height);

  image = gst_vp8_enc_buffer_to_image (encoder, frame->sink_buffer);

  hook = g_slice_new0 (GstVP8EncCoderHook);
  hook->image = image;
  frame->coder_hook = hook;

  if (frame->force_keyframe) {
    flags |= VPX_EFLAG_FORCE_KF;
  }

  status = vpx_codec_encode (&encoder->encoder, image,
      encoder->n_frames, 1, flags, speed_table[encoder->speed]);
  if (status != 0) {
    GST_ELEMENT_ERROR (encoder, LIBRARY, ENCODE,
        ("Failed to encode frame"), ("%s", gst_vpx_error_name (status)));
    g_slice_free (GstVP8EncCoderHook, hook);
    frame->coder_hook = NULL;
    g_slice_free (vpx_image_t, image);
    return FALSE;
  }

  gst_vp8_enc_process (encoder);

  return TRUE;
}

static guint64
_to_granulepos (guint64 frame_end_number, guint inv_count, guint keyframe_dist)
{
  guint64 granulepos;
  guint inv;

  inv = (inv_count == 0) ? 0x3 : inv_count - 1;

  granulepos = (frame_end_number << 32) | (inv << 30) | (keyframe_dist << 3);
  return granulepos;
}

static void
_gst_mini_object_unref0 (GstMiniObject * obj)
{
  if (obj)
    gst_mini_object_unref (obj);
}

static GstFlowReturn
gst_vp8_enc_shape_output (GstBaseVideoEncoder * base_video_encoder,
    GstVideoFrame * frame)
{
  GstVP8Enc *encoder;
  GstBuffer *buf;
  const GstVideoState *state;
  GstFlowReturn ret;
  GstVP8EncCoderHook *hook = frame->coder_hook;
  GList *l;
  gint inv_count;

  GST_DEBUG_OBJECT (base_video_encoder, "shape_output");

  encoder = GST_VP8_ENC (base_video_encoder);

  state = gst_base_video_encoder_get_state (base_video_encoder);

  g_assert (hook != NULL);

  for (inv_count = 0, l = hook->invisible; l; inv_count++, l = l->next) {
    buf = l->data;
    l->data = NULL;

    if (l == hook->invisible && frame->is_sync_point) {
      GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
      encoder->keyframe_distance = 0;
    } else {
      GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
      encoder->keyframe_distance++;
    }

    GST_BUFFER_OFFSET_END (buf) =
        _to_granulepos (frame->presentation_frame_number + 1,
        inv_count, encoder->keyframe_distance);
    GST_BUFFER_OFFSET (buf) =
        gst_util_uint64_scale (frame->presentation_frame_number + 1,
        GST_SECOND * state->fps_d, state->fps_n);

    gst_buffer_set_caps (buf,
        GST_PAD_CAPS (GST_BASE_VIDEO_CODEC_SRC_PAD (base_video_encoder)));
    ret = gst_pad_push (GST_BASE_VIDEO_CODEC_SRC_PAD (base_video_encoder), buf);

    if (ret != GST_FLOW_OK) {
      GST_WARNING_OBJECT (encoder, "flow error %d", ret);
      goto done;
    }
  }

  buf = frame->src_buffer;
  frame->src_buffer = NULL;

  if (!hook->invisible && frame->is_sync_point) {
    GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
    encoder->keyframe_distance = 0;
  } else {
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
    encoder->keyframe_distance++;
  }

  GST_BUFFER_OFFSET_END (buf) =
      _to_granulepos (frame->presentation_frame_number + 1, 0,
      encoder->keyframe_distance);
  GST_BUFFER_OFFSET (buf) =
      gst_util_uint64_scale (frame->presentation_frame_number + 1,
      GST_SECOND * state->fps_d, state->fps_n);

  ret = gst_pad_push (GST_BASE_VIDEO_CODEC_SRC_PAD (base_video_encoder), buf);
  if (ret != GST_FLOW_OK) {
    GST_WARNING_OBJECT (encoder, "flow error %d", ret);
  }

done:
  if (hook) {
    g_list_foreach (hook->invisible, (GFunc) _gst_mini_object_unref0, NULL);
    g_list_free (hook->invisible);
    g_slice_free (GstVP8EncCoderHook, hook);
    frame->coder_hook = NULL;
  }

  return ret;
}

static gboolean
gst_vp8_enc_sink_event (GstBaseVideoEncoder * benc, GstEvent * event)
{
  GstVP8Enc *enc = GST_VP8_ENC (benc);

  if (GST_EVENT_TYPE (event) == GST_EVENT_TAG) {
    GstTagList *list;
    GstTagSetter *setter = GST_TAG_SETTER (enc);
    const GstTagMergeMode mode = gst_tag_setter_get_tag_merge_mode (setter);

    gst_event_parse_tag (event, &list);
    gst_tag_setter_merge_tags (setter, list, mode);
  }

  /* just peeked, baseclass handles the rest */
  return FALSE;
}

#endif /* HAVE_VP8_ENCODER */
