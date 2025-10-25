CC ?= cc
PKGS := gtk+-3.0 gdk-pixbuf-2.0 gstreamer-1.0 gstreamer-pbutils-1.0
CFLAGS ?=
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -O2 $(shell pkg-config --cflags $(PKGS))
LDFLAGS ?=
LDFLAGS += $(shell pkg-config --libs $(PKGS))

SOURCES := flashbang.c assets.c
BUILDDIR := build
OBJECTS := $(SOURCES:%.c=$(BUILDDIR)/%.o)
TARGET := flashbang
TARGET_STATIC := flashbang-static

.PHONY: all clean standalone

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

standalone: CFLAGS += -static
standalone: LDFLAGS += -static
standalone: $(TARGET_STATIC)

$(TARGET_STATIC): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILDDIR)/%.o: %.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) $(TARGET) $(TARGET_STATIC)
