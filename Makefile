CFLAGS := -O2 -ggdb -Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes -ansi -std=c99
LDFLAGS := -Wl,--no-undefined -Wl,--as-needed

override CFLAGS += -D_GNU_SOURCE -DGST_DISABLE_DEPRECATED

GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0)
GST_LIBS := $(shell pkg-config --libs gstreamer-1.0 gstreamer-base-1.0  gstreamer-video-1.0)

all:

version := $(shell ./get-version)
pluginsdir := $(shell pkg-config --variable=pluginsdir gstreamer-1.0)

D = $(DESTDIR)$(pluginsdir)

# plugin

libgstwk.so: VideoSinkGStreamer.o GStreamerUtilities.o plugin.o
libgstwk.so: override CFLAGS += $(GST_CFLAGS) -fPIC \
	-D VERSION='"$(version)"' -I./include
libgstwk.so: override LIBS += $(GST_LIBS)

targets += libgstwk.so

wkplayer: GStreamerUtilities.o player.o
wkplayer: override CFLAGS += $(GST_CFLAGS)
wkplayer: override LIBS += $(GST_LIBS)

bins += wkplayer

all: $(targets) $(bins)

# pretty print
ifndef V
QUIET_CC    = @echo '   CC         '$@;
QUIET_LINK  = @echo '   LINK       '$@;
QUIET_CLEAN = @echo '   CLEAN      '$@;
endif

install: $(targets)
	install -m 755 -D libgstwk.so $(D)

$(bins):
	$(QUIET_LINK)$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

%.o:: %.c
	$(QUIET_CC)$(CC) $(CFLAGS) -MMD -o $@ -c $<

%.so::
	$(QUIET_LINK)$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

clean:
	$(QUIET_CLEAN)$(RM) -v $(targets) *.o *.d

dist: base := gst-wk-$(version)
dist:
	git archive --format=tar --prefix=$(base)/ HEAD > /tmp/$(base).tar
	mkdir -p $(base)
	echo $(version) > $(base)/.version
	chmod 664 $(base)/.version
	tar --append -f /tmp/$(base).tar --owner root --group root $(base)/.version
	rm -r $(base)
	gzip /tmp/$(base).tar

-include *.d
