#include <stdbool.h>
#include <assert.h>
#include <gst/gst.h>

#include "GStreamerUtilities.h"

typedef struct {
    GstElement* playBin;
    GstElement* fpsSink;
    GstElement* webkitVideoSink;
    char* url;
    GMainLoop* loop;
    guint repaintHandler;
} MediaPlayerPrivateGStreamer;

static void didEnd(MediaPlayerPrivateGStreamer *m)
{
    g_main_loop_quit (m->loop);
}

static void mediaPlayerPrivateRepaintCallback(GstElement *sink, GstBuffer *buffer, MediaPlayerPrivateGStreamer* m)
{
    g_printerr(".");
}

static gboolean mediaPlayerPrivateMessageCallback(GstBus* bus, GstMessage* message, MediaPlayerPrivateGStreamer* m)
{
    GError* err;
    gchar* debug;
    GstState currentState;

    // We ignore state changes from internal elements. They are forwarded to playbin2 anyway.
    bool messageSourceIsPlaybin = GST_MESSAGE_SRC(message) == GST_OBJECT(m->playBin);

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR:
        gst_message_parse_error(message, &err, &debug);
        g_printerr("Error (%s) %d: %s (url=%s)", GST_OBJECT_NAME(message->src), err->code, err->message, m->url);

        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m->playBin), GST_DEBUG_GRAPH_SHOW_ALL, "webkit-video.error");

        break;
    case GST_MESSAGE_EOS:
        didEnd(m);
        break;
    case GST_MESSAGE_STATE_CHANGED: {
        if (!messageSourceIsPlaybin)
            break;

        // Construct a filename for the graphviz dot file output.
        GstState newState;
        gst_message_parse_state_changed(message, &currentState, &newState, NULL);
        char *dotFileName = g_strdup_printf("webkit-video.%s_%s", gst_element_state_get_name(currentState), gst_element_state_get_name(newState));
        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m->playBin), GST_DEBUG_GRAPH_SHOW_ALL, dotFileName);
        g_free(dotFileName);

        break;
    }
    default:
        g_debug("Unhandled GStreamer message type: %s", GST_MESSAGE_TYPE_NAME(message));
        break;
    }
    return TRUE;
}

static GstElement* createVideoSink(MediaPlayerPrivateGStreamer *m)
{
    GstElement* videoSink = NULL;

    m->webkitVideoSink = gst_element_factory_make("wkvsink", "wkvsink");
    m->repaintHandler = g_signal_connect(m->webkitVideoSink, "repaint-requested", G_CALLBACK(mediaPlayerPrivateRepaintCallback), m);

    m->fpsSink = gst_element_factory_make("fpsdisplaysink", "sink");
    if (m->fpsSink) {
        g_object_set(m->fpsSink, "silent", TRUE, "text-overlay", FALSE, "video-sink", videoSink, NULL);
        videoSink = m->fpsSink;
    }

    if (!m->fpsSink)
        videoSink = m->webkitVideoSink;

    assert(videoSink);

    return videoSink;
}

static void createGSTPlayBin(MediaPlayerPrivateGStreamer *m)
{
    assert(!m->playBin);

    m->playBin = gst_element_factory_make("playbin", "play");

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m->playBin));
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message", G_CALLBACK(mediaPlayerPrivateMessageCallback), m);
    g_object_unref(bus);

    g_object_set(m->playBin, "video-sink", createVideoSink(m), NULL);
}

static void destroy(MediaPlayerPrivateGStreamer *m)
{
    assert(m);

    if (m->repaintHandler != 0)
        g_signal_handler_disconnect(m->webkitVideoSink, m->repaintHandler);

    if (m->playBin) {
        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(GST_PIPELINE(m->playBin)));
        g_signal_handlers_disconnect_by_func(bus, mediaPlayerPrivateMessageCallback, m);
        gst_bus_remove_signal_watch(bus);

        gst_element_set_state(m->playBin, GST_STATE_NULL);

        gst_object_unref(m->playBin);
    }

    if (m->loop)
        g_main_loop_unref(m->loop);
}

int
main(int argc, char **argv)
{
    if (!initializeGStreamer(&argc, &argv))
        return -1;

    MediaPlayerPrivateGStreamer *m = g_new0(MediaPlayerPrivateGStreamer, 1);

    int i = 1;
    while(--argc) {

        m->url = argv[i++];

        createGSTPlayBin(m);

        m->loop = g_main_loop_new(NULL, TRUE);
        g_main_loop_run(m->loop);
    }

    destroy(m);

    return 0;
}
