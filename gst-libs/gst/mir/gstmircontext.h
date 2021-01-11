/*
 * Provides a struct for transfering context between elements
 * Copyright (C) 2013 Collabora Ltd.
 *   @author: Jim Hodapp <jim.hodapp@canonical.com>
 * *
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_MIR_COMMON_H__
#define __GST_MIR_COMMON_H__

#include <hybris/media/surface_texture_client_hybris.h>

#include <gst/gst.h>

G_BEGIN_DECLS

#ifndef GST_MIR_API
#define GST_MIR_API GST_EXPORT
#endif

#define GST_MIR_CONTEXT_TYPE "gst.mir.MirContext"

GstContext *gst_mir_context_new_with_stc (
    SurfaceTextureClientHybris surface_texture_client);

/** Gets a context from upstream/downstream peers or from the app **/
GST_MIR_API
gboolean gst_mir_ensure_surface_texture_client (gpointer element);

GST_MIR_API
SurfaceTextureClientHybris
gst_context_get_surface_texture_client (GstContext * context);

G_END_DECLS

#endif /* __GST_MIR_COMMON_H__ */
