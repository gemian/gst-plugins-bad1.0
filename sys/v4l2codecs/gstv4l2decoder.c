/* GStreamer
 * Copyright (C) 2020 Nicolas Dufresne <nicolas.dufresne@collabora.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstv4l2decoder.h"
#include "gstv4l2format.h"
#include "linux/media.h"
#include "linux/videodev2.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

GST_DEBUG_CATEGORY (v4l2_decoder_debug);
#define GST_CAT_DEFAULT v4l2_decoder_debug

enum
{
  PROP_0,
  PROP_MEDIA_DEVICE,
  PROP_VIDEO_DEVICE,
};

struct _GstV4l2Decoder
{
  GstObject parent;

  gboolean opened;
  gint media_fd;
  gint video_fd;

  /* properties */
  gchar *media_device;
  gchar *video_device;
};

G_DEFINE_TYPE_WITH_CODE (GstV4l2Decoder, gst_v4l2_decoder, GST_TYPE_OBJECT,
    GST_DEBUG_CATEGORY_INIT (v4l2_decoder_debug, "v4l2codecs-decoder", 0,
        "V4L2 stateless decoder helper"));

static void
gst_v4l2_decoder_finalize (GObject * obj)
{
  GstV4l2Decoder *self = GST_V4L2_DECODER (obj);

  gst_v4l2_decoder_close (self);

  g_free (self->media_device);
  g_free (self->video_device);

  G_OBJECT_CLASS (gst_v4l2_decoder_parent_class)->finalize (obj);
}

static void
gst_v4l2_decoder_init (GstV4l2Decoder * self)
{
}

static void
gst_v4l2_decoder_class_init (GstV4l2DecoderClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gst_v4l2_decoder_finalize;
  gobject_class->get_property = gst_v4l2_decoder_get_property;
  gobject_class->set_property = gst_v4l2_decoder_set_property;

  gst_v4l2_decoder_install_properties (gobject_class, 0, NULL);
}

GstV4l2Decoder *
gst_v4l2_decoder_new (GstV4l2CodecDevice * device)
{
  GstV4l2Decoder *decoder;

  g_return_val_if_fail (device->function == MEDIA_ENT_F_PROC_VIDEO_DECODER,
      NULL);

  decoder = g_object_new (GST_TYPE_V4L2_DECODER,
      "media-device", device->media_device_path,
      "video-device", device->video_device_path, NULL);

  return gst_object_ref_sink (decoder);
}

gboolean
gst_v4l2_decoder_open (GstV4l2Decoder * self)
{
  self->media_fd = open (self->media_device, 0);
  if (self->media_fd < 0) {
    GST_ERROR_OBJECT (self, "Failed to open '%s': %s",
        self->media_device, g_strerror (errno));
    return FALSE;
  }

  self->video_fd = open (self->video_device, 0);
  if (self->video_fd < 0) {
    GST_ERROR_OBJECT (self, "Failed to open '%s': %s",
        self->video_device, g_strerror (errno));
    return FALSE;
  }

  self->opened = TRUE;

  return TRUE;
}

gboolean
gst_v4l2_decoder_close (GstV4l2Decoder * self)
{
  if (self->media_fd)
    close (self->media_fd);
  if (self->video_fd)
    close (self->media_fd);

  self->media_fd = 0;
  self->video_fd = 0;
  self->opened = FALSE;

  return TRUE;
}

gboolean
gst_v4l2_decoder_enum_sink_fmt (GstV4l2Decoder * self, gint i,
    guint32 * out_fmt)
{
  struct v4l2_fmtdesc fmtdesc = { i, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, };
  gint ret;

  g_return_val_if_fail (self->opened, FALSE);

  ret = ioctl (self->video_fd, VIDIOC_ENUM_FMT, &fmtdesc);
  if (ret < 0) {
    if (errno != EINVAL)
      GST_ERROR_OBJECT (self, "VIDIOC_ENUM_FMT failed: %s", g_strerror (errno));
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "Found format %" GST_FOURCC_FORMAT " (%s)",
      GST_FOURCC_ARGS (fmtdesc.pixelformat), fmtdesc.description);
  *out_fmt = fmtdesc.pixelformat;

  return TRUE;
}

gboolean
gst_v4l2_decoder_set_sink_fmt (GstV4l2Decoder * self, guint32 pix_fmt,
    gint width, gint height)
{
  struct v4l2_format format = (struct v4l2_format) {
    .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    .fmt.pix_mp = (struct v4l2_pix_format_mplane) {
          .pixelformat = pix_fmt,
          .width = width,
          .height = height,
        },
  };
  gint ret;

  ret = ioctl (self->video_fd, VIDIOC_S_FMT, &format);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_S_FMT failed: %s", g_strerror (errno));
    return FALSE;
  }

  if (format.fmt.pix_mp.pixelformat != pix_fmt
      || format.fmt.pix_mp.width != width
      || format.fmt.pix_mp.height != height) {
    GST_WARNING_OBJECT (self, "Failed to set sink format to %"
        GST_FOURCC_FORMAT " %ix%i", GST_FOURCC_ARGS (pix_fmt), width, height);
    errno = EINVAL;
    return FALSE;
  }

  return TRUE;
}

gboolean
gst_v4l2_decoder_select_src_format (GstV4l2Decoder * self, GstVideoInfo * info)
{
  gint ret;
  struct v4l2_format fmt = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
  };

  ret = ioctl (self->video_fd, VIDIOC_G_FMT, &fmt);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_S_FMT failed: %s", g_strerror (errno));
    return FALSE;
  }

  if (!gst_v4l2_format_to_video_info (&fmt, info)) {
    GST_ERROR_OBJECT (self, "Unsupported V4L2 pixelformat %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (fmt.fmt.pix_mp.pixelformat));
    return FALSE;
  }

  GST_INFO_OBJECT (self, "Selected format %s %ix%i",
      gst_video_format_to_string (info->finfo->format),
      info->width, info->height);

  return TRUE;
}

static guint32
direction_to_buffer_type (GstPadDirection direction)
{
  if (direction == GST_PAD_SRC)
    return V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  else
    return V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}

gint
gst_v4l2_decoder_request_buffers (GstV4l2Decoder * self,
    GstPadDirection direction, guint num_buffers)
{
  gint ret;
  struct v4l2_requestbuffers reqbufs = {
    .count = num_buffers,
    .memory = V4L2_MEMORY_MMAP,
    .type = direction_to_buffer_type (direction),
  };

  GST_DEBUG_OBJECT (self, "Requesting %u buffers", num_buffers);

  ret = ioctl (self->video_fd, VIDIOC_REQBUFS, &reqbufs);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_REQBUFS failed: %s", g_strerror (errno));
    return ret;
  }

  return reqbufs.count;
}

gboolean
gst_v4l2_decoder_export_buffer (GstV4l2Decoder * self,
    GstPadDirection direction, gint index, gint * fds, gsize * sizes,
    gsize * offsets, guint * num_fds)
{
  gint i, ret;
  struct v4l2_plane planes[GST_VIDEO_MAX_PLANES] = { {0} };
  struct v4l2_buffer v4l2_buf = {
    .index = 0,
    .type = direction_to_buffer_type (direction),
    .length = GST_VIDEO_MAX_PLANES,
    .m.planes = planes,
  };

  ret = ioctl (self->video_fd, VIDIOC_QUERYBUF, &v4l2_buf);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_QUERYBUF failed: %s", g_strerror (errno));
    return FALSE;
  }

  *num_fds = v4l2_buf.length;
  for (i = 0; i < v4l2_buf.length; i++) {
    struct v4l2_plane *plane = v4l2_buf.m.planes + i;
    struct v4l2_exportbuffer expbuf = {
      .type = direction_to_buffer_type (direction),
      .index = index,
      .plane = i,
      .flags = O_CLOEXEC | O_RDWR,
    };

    ret = ioctl (self->video_fd, VIDIOC_EXPBUF, &expbuf);
    if (ret < 0) {
      gint j;
      GST_ERROR_OBJECT (self, "VIDIOC_EXPBUF failed: %s", g_strerror (errno));

      for (j = i - 1; j >= 0; j--)
        close (fds[j]);

      return FALSE;
    }

    fds[i] = expbuf.fd;
    sizes[i] = plane->length;
    offsets[i] = plane->data_offset;
  }

  return TRUE;
}

void
gst_v4l2_decoder_install_properties (GObjectClass * gobject_class,
    gint prop_offset, GstV4l2CodecDevice * device)
{
  const gchar *media_device_path = NULL;
  const gchar *video_device_path = NULL;

  if (device) {
    media_device_path = device->media_device_path;
    video_device_path = device->video_device_path;
  }

  g_object_class_install_property (gobject_class, PROP_MEDIA_DEVICE,
      g_param_spec_string ("media-device", "Media Device Path",
          "Path to the media device node", media_device_path,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_VIDEO_DEVICE,
      g_param_spec_string ("video-device", "Video Device Path",
          "Path to the video device node", video_device_path,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

void
gst_v4l2_decoder_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstV4l2Decoder *self = GST_V4L2_DECODER (object);

  switch (prop_id) {
    case PROP_MEDIA_DEVICE:
      g_free (self->media_device);
      self->media_device = g_value_dup_string (value);
      break;
    case PROP_VIDEO_DEVICE:
      g_free (self->video_device);
      self->video_device = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

void
gst_v4l2_decoder_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstV4l2Decoder *self = GST_V4L2_DECODER (object);

  switch (prop_id) {
    case PROP_MEDIA_DEVICE:
      g_value_set_string (value, self->media_device);
      break;
    case PROP_VIDEO_DEVICE:
      g_value_set_string (value, self->video_device);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}