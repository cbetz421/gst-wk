/*
 *  Copyright (C) 2012 Igalia S.L
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include "GStreamerUtilities.h"

#include <gst/gst.h>

bool getVideoSizeAndFormatFromCaps(GstCaps* caps, IntSize* size, GstVideoFormat* format, int* pixelAspectRatioNumerator, int* pixelAspectRatioDenominator, int* stride)
{
    GstVideoInfo info;

    if (!gst_caps_is_fixed(caps) || !gst_video_info_from_caps(&info, caps))
        return false;

    *format = GST_VIDEO_INFO_FORMAT(&info);
    size->Width = GST_VIDEO_INFO_WIDTH(&info);
    size->Height = GST_VIDEO_INFO_HEIGHT(&info);
    *pixelAspectRatioNumerator = GST_VIDEO_INFO_PAR_N(&info);
    *pixelAspectRatioDenominator = GST_VIDEO_INFO_PAR_D(&info);
    *stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);

    return true;
}

GstBuffer* createGstBuffer(GstBuffer* buffer)
{
    gsize bufferSize = gst_buffer_get_size(buffer);
    GstBuffer* newBuffer = gst_buffer_new_and_alloc(bufferSize);

    if (!newBuffer)
        return 0;

    gst_buffer_copy_into(newBuffer, buffer, GST_BUFFER_COPY_METADATA, 0, bufferSize);
    return newBuffer;
}
