/*
 *  Copyright (C) 2007 OpenedHand
 *  Copyright (C) 2007 Alp Toker <alp@atoker.com>
 *  Copyright (C) 2009, 2010, 2011, 2012 Igalia S.L
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

/*
 *
 * WebKitVideoSink is a GStreamer sink element that triggers
 * repaints in the WebKit GStreamer media player for the
 * current video buffer.
 */

#include "VideoSinkGStreamer.h"

#include "GStreamerUtilities.h"
#include <stdbool.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/video/gstvideometa.h>

// CAIRO_FORMAT_RGB24 used to render the video buffers is little/big endian dependant.
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define GST_CAPS_FORMAT "{ BGRx, BGRA }"
#else
#define GST_CAPS_FORMAT "{ xRGB, ARGB }"
#endif
#if GST_CHECK_VERSION(1, 1, 0)
#define GST_FEATURED_CAPS GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_META_GST_VIDEO_GL_TEXTURE_UPLOAD_META, "NV12") ";"
#else
#define GST_FEATURED_CAPS
#endif

#define WEBKIT_VIDEO_SINK_PAD_CAPS GST_FEATURED_CAPS GST_VIDEO_CAPS_MAKE(GST_CAPS_FORMAT)

static GstStaticPadTemplate s_sinkTemplate = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(WEBKIT_VIDEO_SINK_PAD_CAPS));


GST_DEBUG_CATEGORY_STATIC(webkitVideoSinkDebug);
#define GST_CAT_DEFAULT webkitVideoSinkDebug

enum {
    REPAINT_REQUESTED,
    LAST_SIGNAL
};

enum {
    PROP_0,
    PROP_CAPS
};

static guint webkitVideoSinkSignals[LAST_SIGNAL] = { 0, };

struct _WebKitVideoSinkPrivate {
    GstBuffer* buffer;
    guint timeoutId;
    GMutex bufferMutex;
    GCond dataCondition;

    GstVideoInfo info;

    GstCaps* currentCaps;

    // If this is TRUE all processing should finish ASAP
    // This is necessary because there could be a race between
    // unlock() and render(), where unlock() wins, signals the
    // GCond, then render() tries to render a frame although
    // everything else isn't running anymore. This will lead
    // to deadlocks because render() holds the stream lock.
    //
    // Protected by the buffer mutex
    bool unlocked;
};

static void print_buffer_metadata(WebKitVideoSink* sink, GstBuffer* buffer)
{
    gchar dts_str[64], pts_str[64], dur_str[64];
    gchar flag_str[100];

    if (GST_BUFFER_DTS (buffer) != GST_CLOCK_TIME_NONE) {
        g_snprintf (dts_str, sizeof (dts_str), "%" GST_TIME_FORMAT,
                    GST_TIME_ARGS (GST_BUFFER_DTS (buffer)));
    } else {
        g_strlcpy (dts_str, "none", sizeof (dts_str));
    }

    if (GST_BUFFER_PTS (buffer) != GST_CLOCK_TIME_NONE) {
        g_snprintf (pts_str, sizeof (pts_str), "%" GST_TIME_FORMAT,
                    GST_TIME_ARGS (GST_BUFFER_PTS (buffer)));
    } else {
        g_strlcpy (pts_str, "none", sizeof (pts_str));
    }

    if (GST_BUFFER_DURATION (buffer) != GST_CLOCK_TIME_NONE) {
        g_snprintf (dur_str, sizeof (dur_str), "%" GST_TIME_FORMAT,
                    GST_TIME_ARGS (GST_BUFFER_DURATION (buffer)));
    } else {
        g_strlcpy (dur_str, "none", sizeof (dur_str));
    }

    {
        const char *flag_list[15] = {
            "", "", "", "", "live", "decode-only", "discont", "resync", "corrupted",
            "marker", "header", "gap", "droppable", "delta-unit", "in-caps"
        };
        guint i;
        char *end = flag_str;
        end[0] = '\0';
        for (i = 0; i < G_N_ELEMENTS (flag_list); i++) {
            if (GST_MINI_OBJECT_CAST (buffer)->flags & (1 << i)) {
                strcpy (end, flag_list[i]);
                end += strlen (end);
                end[0] = ' ';
                end[1] = '\0';
                end++;
            }
        }
    }

    g_printerr ("chain   ******* (%s:%s) (%u bytes, dts: %s, pts: %s"
                ", duration: %s, offset: %" G_GINT64_FORMAT ", offset_end: %"
                G_GINT64_FORMAT ", flags: %08x %s) %p\n",
                    GST_DEBUG_PAD_NAME (GST_BASE_SINK_CAST (sink)->sinkpad),
                (guint) gst_buffer_get_size (buffer), dts_str, pts_str,
                dur_str, GST_BUFFER_OFFSET (buffer), GST_BUFFER_OFFSET_END (buffer),
                GST_MINI_OBJECT_CAST (buffer)->flags, flag_str, buffer);
}

#define webkit_video_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(WebKitVideoSink, webkit_video_sink, GST_TYPE_VIDEO_SINK, GST_DEBUG_CATEGORY_INIT(webkitVideoSinkDebug, "webkitsink", 0, "webkit video sink"))

static void webkit_video_sink_init(WebKitVideoSink* sink)
{
    sink->priv = G_TYPE_INSTANCE_GET_PRIVATE(sink, WEBKIT_TYPE_VIDEO_SINK, WebKitVideoSinkPrivate);

    g_cond_init(&sink->priv->dataCondition);
    g_mutex_init(&sink->priv->bufferMutex);

    gst_video_info_init(&sink->priv->info);
}

static gboolean webkitVideoSinkTimeoutCallback(gpointer data)
{
    WebKitVideoSink* sink = data;
    WebKitVideoSinkPrivate* priv = sink->priv;

    g_mutex_lock(&priv->bufferMutex);
    GstBuffer* buffer = priv->buffer;
    priv->buffer = 0;
    priv->timeoutId = 0;

    if (!buffer || priv->unlocked || G_UNLIKELY(!GST_IS_BUFFER(buffer))) {
        g_cond_signal(&priv->dataCondition);
        g_mutex_unlock(&priv->bufferMutex);
        return FALSE;
    }

    g_signal_emit(sink, webkitVideoSinkSignals[REPAINT_REQUESTED], 0, buffer);
    gst_buffer_unref(buffer);
    g_cond_signal(&priv->dataCondition);
    g_mutex_unlock(&priv->bufferMutex);

    return FALSE;
}

static GstFlowReturn webkitVideoSinkRender(GstBaseSink* baseSink, GstBuffer* buffer)
{
    WebKitVideoSink* sink = WEBKIT_VIDEO_SINK(baseSink);
    WebKitVideoSinkPrivate* priv = sink->priv;

    g_mutex_lock(&priv->bufferMutex);

    if (priv->unlocked) {
        g_mutex_unlock(&priv->bufferMutex);
        return GST_FLOW_OK;
    }

    priv->buffer = gst_buffer_ref(buffer);

    GstCaps* caps;
    // The video info structure is valid only if the sink handled an allocation query.
    if (GST_VIDEO_INFO_FORMAT(&priv->info) != GST_VIDEO_FORMAT_UNKNOWN)
        caps = gst_video_info_to_caps(&priv->info);
    else
        caps = priv->currentCaps;

    GstVideoFormat format;
    IntSize size;
    int pixelAspectRatioNumerator, pixelAspectRatioDenominator, stride;
    if (!getVideoSizeAndFormatFromCaps(caps, &size, &format, &pixelAspectRatioNumerator, &pixelAspectRatioDenominator, &stride)) {
        gst_caps_unref(caps);
        gst_buffer_unref(buffer);
        g_mutex_unlock(&priv->bufferMutex);
        return GST_FLOW_ERROR;
    }

    gst_caps_unref(caps);

    // Cairo's ARGB has pre-multiplied alpha while GStreamer's doesn't.
    // Here we convert to Cairo's ARGB.
    if (format == GST_VIDEO_FORMAT_ARGB || format == GST_VIDEO_FORMAT_BGRA) {
        // Because GstBaseSink::render() only owns the buffer reference in the
        // method scope we can't use gst_buffer_make_writable() here. Also
        // The buffer content should not be changed here because the same buffer
        // could be passed multiple times to this method (in theory).

        GstBuffer* newBuffer = createGstBuffer(buffer);

        // Check if allocation failed.
        if (G_UNLIKELY(!newBuffer)) {
            g_mutex_unlock(&priv->bufferMutex);
            return GST_FLOW_ERROR;
        }

        // We don't use Color::premultipliedARGBFromColor() here because
        // one function call per video pixel is just too expensive:
        // For 720p/PAL for example this means 1280*720*25=23040000
        // function calls per second!
        GstMapInfo sourceInfo;
        GstMapInfo destinationInfo;
        gst_buffer_map(buffer, &sourceInfo, GST_MAP_READ);
        const guint8* source = sourceInfo.data;
        gst_buffer_map(newBuffer, &destinationInfo, GST_MAP_WRITE);
        guint8* destination = destinationInfo.data;

        for (int x = 0; x < size.Height; x++) {
            for (int y = 0; y < size.Width; y++) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
                unsigned short alpha = source[3];
                destination[0] = (source[0] * alpha + 128) / 255;
                destination[1] = (source[1] * alpha + 128) / 255;
                destination[2] = (source[2] * alpha + 128) / 255;
                destination[3] = alpha;
#else
                unsigned short alpha = source[0];
                destination[0] = alpha;
                destination[1] = (source[1] * alpha + 128) / 255;
                destination[2] = (source[2] * alpha + 128) / 255;
                destination[3] = (source[3] * alpha + 128) / 255;
#endif
                source += 4;
                destination += 4;
            }
        }

        gst_buffer_unmap(buffer, &sourceInfo);
        gst_buffer_unmap(newBuffer, &destinationInfo);
        gst_buffer_unref(buffer);
        buffer = priv->buffer = newBuffer;
    }

    // This should likely use a lower priority, but glib currently starves
    // lower priority sources.
    // See: https://bugzilla.gnome.org/show_bug.cgi?id=610830.
    priv->timeoutId = g_timeout_add_full(G_PRIORITY_DEFAULT, 0, webkitVideoSinkTimeoutCallback,
                                         gst_object_ref(sink), (GDestroyNotify) gst_object_unref);
    g_source_set_name_by_id(priv->timeoutId, "[WebKit] webkitVideoSinkTimeoutCallback");

    print_buffer_metadata(sink, buffer);

    g_cond_wait(&priv->dataCondition, &priv->bufferMutex);
    g_mutex_unlock(&priv->bufferMutex);
    return GST_FLOW_OK;
}

static void webkitVideoSinkDispose(GObject* object)
{
    WebKitVideoSink* sink = WEBKIT_VIDEO_SINK(object);
    WebKitVideoSinkPrivate* priv = sink->priv;

    g_cond_clear(&priv->dataCondition);
    g_mutex_clear(&priv->bufferMutex);

    G_OBJECT_CLASS(parent_class)->dispose(object);
}

static void webkitVideoSinkGetProperty(GObject* object, guint propertyId, GValue* value, GParamSpec* parameterSpec)
{
    WebKitVideoSink* sink = WEBKIT_VIDEO_SINK(object);
    WebKitVideoSinkPrivate* priv = sink->priv;

    switch (propertyId) {
    case PROP_CAPS: {
        GstCaps* caps = priv->currentCaps;
        if (caps)
            gst_caps_ref(caps);
        g_value_take_boxed(value, caps);
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propertyId, parameterSpec);
    }
}

static void unlockBufferMutex(WebKitVideoSinkPrivate* priv)
{
    g_mutex_lock(&priv->bufferMutex);

    if (priv->buffer) {
        gst_buffer_unref(priv->buffer);
        priv->buffer = 0;
    }

    priv->unlocked = true;

    g_cond_signal(&priv->dataCondition);
    g_mutex_unlock(&priv->bufferMutex);
}

static gboolean webkitVideoSinkUnlock(GstBaseSink* baseSink)
{
    WebKitVideoSink* sink = WEBKIT_VIDEO_SINK(baseSink);

    unlockBufferMutex(sink->priv);

    return GST_CALL_PARENT_WITH_DEFAULT(GST_BASE_SINK_CLASS, unlock, (baseSink), TRUE);
}

static gboolean webkitVideoSinkUnlockStop(GstBaseSink* baseSink)
{
    WebKitVideoSinkPrivate* priv = WEBKIT_VIDEO_SINK(baseSink)->priv;

    g_mutex_lock(&priv->bufferMutex);
    priv->unlocked = false;
    g_mutex_unlock(&priv->bufferMutex);

    return GST_CALL_PARENT_WITH_DEFAULT(GST_BASE_SINK_CLASS, unlock_stop, (baseSink), TRUE);
}

static gboolean webkitVideoSinkStop(GstBaseSink* baseSink)
{
    WebKitVideoSinkPrivate* priv = WEBKIT_VIDEO_SINK(baseSink)->priv;

    unlockBufferMutex(priv);

    if (priv->currentCaps) {
        gst_caps_unref(priv->currentCaps);
        priv->currentCaps = 0;
    }

    return TRUE;
}

static gboolean webkitVideoSinkStart(GstBaseSink* baseSink)
{
    WebKitVideoSinkPrivate* priv = WEBKIT_VIDEO_SINK(baseSink)->priv;

    g_mutex_lock(&priv->bufferMutex);
    priv->unlocked = false;
    g_mutex_unlock(&priv->bufferMutex);
    return TRUE;
}

static gboolean webkitVideoSinkSetCaps(GstBaseSink* baseSink, GstCaps* caps)
{
    WebKitVideoSink* sink = WEBKIT_VIDEO_SINK(baseSink);
    WebKitVideoSinkPrivate* priv = sink->priv;

    GST_DEBUG_OBJECT(sink, "Current caps %" GST_PTR_FORMAT ", setting caps %" GST_PTR_FORMAT, priv->currentCaps, caps);

    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps)) {
        GST_ERROR_OBJECT(sink, "Invalid caps %" GST_PTR_FORMAT, caps);
        return FALSE;
    }

    gst_caps_replace(&priv->currentCaps, caps);
    return TRUE;
}

static gboolean webkitVideoSinkProposeAllocation(GstBaseSink* baseSink, GstQuery* query)
{
    GstCaps* caps;
    gst_query_parse_allocation(query, &caps, 0);
    if (!caps)
        return FALSE;

    WebKitVideoSink* sink = WEBKIT_VIDEO_SINK(baseSink);
    if (!gst_video_info_from_caps(&sink->priv->info, caps))
        return FALSE;

    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, 0);
    gst_query_add_allocation_meta(query, GST_VIDEO_CROP_META_API_TYPE, 0);
#if GST_CHECK_VERSION(1, 1, 0)
    gst_query_add_allocation_meta(query, GST_VIDEO_GL_TEXTURE_UPLOAD_META_API_TYPE, 0);
#endif
    return TRUE;
}

static void webkit_video_sink_class_init(WebKitVideoSinkClass* klass)
{
    GObjectClass* gobjectClass = G_OBJECT_CLASS(klass);
    GstBaseSinkClass* baseSinkClass = GST_BASE_SINK_CLASS(klass);
    GstElementClass* elementClass = GST_ELEMENT_CLASS(klass);

    gst_element_class_add_pad_template(elementClass, gst_static_pad_template_get(&s_sinkTemplate));
    gst_element_class_set_metadata(elementClass, "WebKit video sink", "Sink/Video", "Sends video data from a GStreamer pipeline to WebKit", "Igalia, Alp Toker <alp@atoker.com>");

    g_type_class_add_private(klass, sizeof(WebKitVideoSinkPrivate));

    gobjectClass->dispose = webkitVideoSinkDispose;
    gobjectClass->get_property = webkitVideoSinkGetProperty;

    baseSinkClass->unlock = webkitVideoSinkUnlock;
    baseSinkClass->unlock_stop = webkitVideoSinkUnlockStop;
    baseSinkClass->render = webkitVideoSinkRender;
    baseSinkClass->preroll = webkitVideoSinkRender;
    baseSinkClass->stop = webkitVideoSinkStop;
    baseSinkClass->start = webkitVideoSinkStart;
    baseSinkClass->set_caps = webkitVideoSinkSetCaps;
    baseSinkClass->propose_allocation = webkitVideoSinkProposeAllocation;

    g_object_class_install_property(gobjectClass, PROP_CAPS,
        g_param_spec_boxed("current-caps", "Current-Caps", "Current caps", GST_TYPE_CAPS, G_PARAM_READABLE));

    webkitVideoSinkSignals[REPAINT_REQUESTED] = g_signal_new("repaint-requested",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
            0, // Class offset
            0, // Accumulator
            0, // Accumulator data
            g_cclosure_marshal_generic,
            G_TYPE_NONE, // Return type
            1, // Only one parameter
            GST_TYPE_BUFFER);
}
