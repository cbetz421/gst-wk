#include "VideoSinkGStreamer.h"

static gboolean
webkit_plugin_init(GstPlugin* plugin)
{
    return gst_element_register(plugin,
                                "wkvsink",
                                GST_RANK_PRIMARY,
                                WEBKIT_TYPE_VIDEO_SINK);
}

GstPluginDesc gst_plugin_desc = {
    .major_version = 1,
    .minor_version = 3,
    .name = "webkit",
    .description = "simulated WebKit elements",
    .plugin_init = webkit_plugin_init,
    .version = VERSION,
    .license = "LGPL",
    .package = "gst-wk",
    .source = "gst-wk",
    .origin = "http://www.igalia.com",
};
