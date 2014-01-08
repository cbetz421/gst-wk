#include <stdbool.h>
#include <assert.h>
#include <gst/gst.h>

#include "GStreamerUtilities.h"

// GstPlayFlags flags from playbin2. It is the policy of GStreamer to
// not publicly expose element-specific enums. That's why this
// GstPlayFlags enum has been copied here.
typedef enum {
    GST_PLAY_FLAG_VIDEO         = 0x00000001,
    GST_PLAY_FLAG_AUDIO         = 0x00000002,
    GST_PLAY_FLAG_TEXT          = 0x00000004,
    GST_PLAY_FLAG_VIS           = 0x00000008,
    GST_PLAY_FLAG_SOFT_VOLUME   = 0x00000010,
    GST_PLAY_FLAG_NATIVE_AUDIO  = 0x00000020,
    GST_PLAY_FLAG_NATIVE_VIDEO  = 0x00000040,
    GST_PLAY_FLAG_DOWNLOAD      = 0x00000080,
    GST_PLAY_FLAG_BUFFERING     = 0x000000100
} GstPlayFlags;

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
    assert(m->webkitVideoSink);
    g_object_set(m->webkitVideoSink, "silent", TRUE, NULL);
    m->repaintHandler = g_signal_connect(m->webkitVideoSink, "repaint-requested", G_CALLBACK(mediaPlayerPrivateRepaintCallback), m);

    m->fpsSink = gst_element_factory_make("fpsdisplaysink", "sink");
    if (m->fpsSink) {
        g_object_set(m->fpsSink, "silent", TRUE, "text-overlay", FALSE, "video-sink", m->webkitVideoSink, NULL);
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

static bool changePipelineState(MediaPlayerPrivateGStreamer* m, GstState newState)
{
    assert(m->playBin);

    GstState currentState;
    GstState pending;

    gst_element_get_state(m->playBin, &currentState, &pending, 0);
    if (currentState == newState || pending == newState) {
        return true;
    }

    GstStateChangeReturn setStateResult = gst_element_set_state(m->playBin, newState);
    GstState pausedOrPlaying = newState == GST_STATE_PLAYING ? GST_STATE_PAUSED : GST_STATE_PLAYING;
    if (currentState != pausedOrPlaying && setStateResult == GST_STATE_CHANGE_FAILURE) {
        return false;
    }

    return true;
}

static void setDownloadBuffering(MediaPlayerPrivateGStreamer* m)
{
   assert(m->playBin);

    GstPlayFlags flags;
    g_object_get(m->playBin, "flags", &flags, NULL);

    if (flags & GST_PLAY_FLAG_DOWNLOAD)
        return;

    g_object_set(m->playBin, "flags", flags | GST_PLAY_FLAG_DOWNLOAD, NULL);
}


static void load(MediaPlayerPrivateGStreamer *m, const char* uri)
{
    assert(m->playBin);

    g_free(m->url);
    m->url = g_strdup(uri);
    g_object_set(m->playBin, "uri", uri, NULL);

    /* commitLoad */
    changePipelineState(m, GST_STATE_PAUSED);
    setDownloadBuffering(m);
}

static void play(MediaPlayerPrivateGStreamer *m)
{
    if (!changePipelineState(m, GST_STATE_PLAYING)) {
        g_printerr("Play failed!\n");
    }
}

static void destroy(MediaPlayerPrivateGStreamer *m)
{
    assert(m);

    g_free(m->url);

    if (m->repaintHandler != 0)
        g_signal_handler_disconnect(m->webkitVideoSink, m->repaintHandler);

    if (m->playBin) {
        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(GST_PIPELINE(m->playBin)));
        g_signal_handlers_disconnect_by_func(bus, mediaPlayerPrivateMessageCallback, m);
        gst_bus_remove_signal_watch(bus);
        g_object_unref(bus);

        gst_element_set_state(m->playBin, GST_STATE_NULL);

        gst_object_unref(m->playBin);
    }

    if (m->loop)
        g_main_loop_unref(m->loop);
}

static gboolean
launch(gpointer data)
{
    play((MediaPlayerPrivateGStreamer *) data);

    return FALSE;
}

int
main(int argc, char **argv)
{
    if (!initializeGStreamer(&argc, &argv))
        return -1;

    MediaPlayerPrivateGStreamer *m = g_new0(MediaPlayerPrivateGStreamer, 1);
    createGSTPlayBin(m);

    int i = 1;
    while(--argc) {
        load(m, argv[i--]);
        m->loop = g_main_loop_new(NULL, TRUE);
        g_idle_add(launch, m);
        g_main_loop_run(m->loop);
    }

    destroy(m);

    return 0;
}
