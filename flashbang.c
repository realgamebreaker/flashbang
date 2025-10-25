#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <gst/pbutils/gstdiscoverer.h>
#include <cairo.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "assets.h"

typedef enum
{
    STATE_WAITING = 0,
    STATE_FLASHBANG,
    STATE_FADE_IN,
    STATE_SHOW,
    STATE_FADE_OUT
} FlashState;

typedef struct
{
    GtkWidget *window;
    GdkPixbuf *pixbuf;
    GstElement *player;
    FlashState state;
    gdouble image_alpha;
    gdouble window_alpha;
    gint64 start_time_us;
    gdouble audio_delay_ms;
    gdouble flashbang_duration_ms;
    gdouble fade_in_duration_ms;
    gdouble fade_out_duration_ms;
    gdouble show_duration_ms;
    gchar *audio_path;
    gchar *audio_uri;
} FlashbangApp;

static gint64
monotonic_ms(void)
{
    return g_get_monotonic_time() / 1000;
}

static gboolean
write_temp_file(const unsigned char *data, size_t len, gchar **path_out)
{
    if (!data || len == 0)
    {
        return FALSE;
    }

    GError *error = NULL;
    gint fd = g_file_open_tmp("flashbang-audio-XXXXXX", path_out, &error);
    if (fd == -1)
    {
        if (error)
        {
            g_warning("failed to create temp audio file: %s", error->message);
            g_clear_error(&error);
        }
        return FALSE;
    }

    gssize total = 0;
    while ((size_t)total < len)
    {
        gssize chunk = write(fd, data + total, len - total);
        if (chunk == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            g_warning("failed to write temp audio file: %s", g_strerror(errno));
            close(fd);
            g_remove(*path_out);
            g_free(*path_out);
            *path_out = NULL;
            return FALSE;
        }
        total += chunk;
    }

    close(fd);
    return TRUE;
}

static gdouble
probe_audio_duration_ms(const gchar *audio_uri)
{
    if (!audio_uri)
    {
        return 5000.0;
    }

    GError *error = NULL;
    GstDiscoverer *discoverer = gst_discoverer_new(GST_SECOND, &error);
    if (!discoverer)
    {
        if (error)
        {
            g_warning("failed to create discoverer: %s", error->message);
            g_clear_error(&error);
        }
        return 5000.0;
    }

    GstDiscovererInfo *info = gst_discoverer_discover_uri(discoverer, audio_uri, &error);
    if (!info)
    {
        if (error)
        {
            g_warning("failed to probe audio duration: %s", error->message);
            g_clear_error(&error);
        }
        g_object_unref(discoverer);
        return 5000.0;
    }

    GstClockTime duration_ns = gst_discoverer_info_get_duration(info);
    g_object_unref(info);
    g_object_unref(discoverer);

    if (duration_ns == GST_CLOCK_TIME_NONE)
    {
        return 5000.0;
    }

    return (gdouble)duration_ns / 1000000.0;
}

static gboolean
start_audio_playback(FlashbangApp *app)
{
    if (!app || !app->audio_uri)
    {
        return FALSE;
    }

    GstElement *player = gst_element_factory_make("playbin", NULL);
    if (!player)
    {
        g_warning("failed to create GStreamer playbin");
        return FALSE;
    }

    g_object_set(player, "uri", app->audio_uri, NULL);

    GstStateChangeReturn ret = gst_element_set_state(player, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_warning("failed to start audio playback");
        gst_object_unref(player);
        return FALSE;
    }

    app->player = player;
    return TRUE;
}

static gboolean
load_pixbuf_from_memory(FlashbangApp *app)
{
    if (!job_png || job_png_len == 0)
    {
        g_warning("embedded image data is missing");
        return FALSE;
    }

    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    GError *error = NULL;

    if (!gdk_pixbuf_loader_write(loader, job_png, job_png_len, &error))
    {
        if (error)
        {
            g_warning("failed to decode embedded image: %s", error->message);
            g_clear_error(&error);
        }
        g_object_unref(loader);
        return FALSE;
    }

    if (!gdk_pixbuf_loader_close(loader, &error))
    {
        if (error)
        {
            g_warning("failed to finalize image load: %s", error->message);
            g_clear_error(&error);
        }
        g_object_unref(loader);
        return FALSE;
    }

    GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (!pixbuf)
    {
        g_warning("embedded image produced no pixbuf");
        g_object_unref(loader);
        return FALSE;
    }

    app->pixbuf = g_object_ref(pixbuf);
    g_object_unref(loader);
    return TRUE;
}

static gdouble
elapsed_ms(const FlashbangApp *app)
{
    gint64 now_us = g_get_monotonic_time();
    return (now_us - app->start_time_us) / 1000.0;
}

static gboolean
update_cb(gpointer user_data)
{
    FlashbangApp *app = user_data;
    gdouble elapsed = elapsed_ms(app);

    switch (app->state)
    {
    case STATE_WAITING:
        app->window_alpha = 0.0;
        if (elapsed >= app->audio_delay_ms)
        {
            app->state = STATE_FLASHBANG;
            app->start_time_us = g_get_monotonic_time();
        }
        break;
    case STATE_FLASHBANG:
        app->image_alpha = 0.0;
        app->window_alpha = 1.0;
        if (elapsed >= app->flashbang_duration_ms)
        {
            app->state = STATE_FADE_IN;
            app->start_time_us = g_get_monotonic_time();
        }
        break;
    case STATE_FADE_IN:
        app->image_alpha = MIN(1.0, elapsed / app->fade_in_duration_ms);
        app->window_alpha = 1.0;
        if (elapsed >= app->fade_in_duration_ms)
        {
            app->state = STATE_SHOW;
            app->image_alpha = 1.0;
            app->start_time_us = g_get_monotonic_time();
        }
        break;
    case STATE_SHOW:
        app->image_alpha = 1.0;
        app->window_alpha = 1.0;
        if (elapsed >= app->show_duration_ms)
        {
            app->state = STATE_FADE_OUT;
            app->start_time_us = g_get_monotonic_time();
        }
        break;
    case STATE_FADE_OUT:
    {
        gdouble fade = MAX(0.0, 1.0 - elapsed / app->fade_out_duration_ms);
        app->image_alpha = fade;
        app->window_alpha = fade;
        if (elapsed >= app->fade_out_duration_ms)
        {
            gtk_main_quit();
            return G_SOURCE_REMOVE;
        }
        break;
    }
    default:
        break;
    }

    gtk_widget_queue_draw(app->window);
    return G_SOURCE_CONTINUE;
}

static gboolean
on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    if (event->keyval == GDK_KEY_Escape)
    {
        gtk_main_quit();
        return TRUE;
    }
    return FALSE;
}

static gboolean
on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    FlashbangApp *app = user_data;
    gint width = gtk_widget_get_allocated_width(widget);
    gint height = gtk_widget_get_allocated_height(widget);

    if (app->state == STATE_WAITING)
    {
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        return FALSE;
    }

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, app->window_alpha);
    cairo_paint(cr);

    if (app->state != STATE_FLASHBANG && app->image_alpha > 0.0 && app->pixbuf)
    {
        gint img_width = gdk_pixbuf_get_width(app->pixbuf);
        gint img_height = gdk_pixbuf_get_height(app->pixbuf);
        gdouble scale = MIN((gdouble)width / img_width, (gdouble)height / img_height);
        gdouble draw_width = img_width * scale;
        gdouble draw_height = img_height * scale;
        gdouble x = (width - draw_width) / 2.0;
        gdouble y = (height - draw_height) / 2.0;

        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, scale, scale);
        gdk_cairo_set_source_pixbuf(cr, app->pixbuf, 0.0, 0.0);
        cairo_paint_with_alpha(cr, app->image_alpha);
        cairo_restore(cr);
    }

    return FALSE;
}

static void
cleanup_app(FlashbangApp *app)
{
    if (!app)
    {
        return;
    }

    if (app->player)
    {
        gst_element_set_state(app->player, GST_STATE_NULL);
        gst_object_unref(app->player);
        app->player = NULL;
    }

    if (app->audio_path)
    {
        g_remove(app->audio_path);
        g_free(app->audio_path);
        app->audio_path = NULL;
    }

    if (app->audio_uri)
    {
        g_free(app->audio_uri);
        app->audio_uri = NULL;
    }

    if (app->pixbuf)
    {
        g_object_unref(app->pixbuf);
        app->pixbuf = NULL;
    }
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    gst_init(&argc, &argv);

    FlashbangApp *app = g_new0(FlashbangApp, 1);
    app->audio_delay_ms = 1000.0;
    app->flashbang_duration_ms = 300.0;
    app->fade_in_duration_ms = 1000.0;
    app->fade_out_duration_ms = 1000.0;
    app->state = STATE_WAITING;
    app->image_alpha = 0.0;
    app->window_alpha = 0.0;

    if (!write_temp_file(flashbang_mp3, flashbang_mp3_len, &app->audio_path))
    {
        g_printerr("embedded audio data is missing or invalid\n");
        cleanup_app(app);
        g_free(app);
        return EXIT_FAILURE;
    }

    GError *uri_error = NULL;
    app->audio_uri = g_filename_to_uri(app->audio_path, NULL, &uri_error);
    if (!app->audio_uri)
    {
        if (uri_error)
        {
            g_printerr("failed to create file URI: %s\n", uri_error->message);
            g_clear_error(&uri_error);
        }
        cleanup_app(app);
        g_free(app);
        return EXIT_FAILURE;
    }

    gdouble audio_duration_ms = probe_audio_duration_ms(app->audio_uri);
    app->show_duration_ms = MAX(0.0, audio_duration_ms - app->audio_delay_ms - app->flashbang_duration_ms - app->fade_in_duration_ms - app->fade_out_duration_ms);

    if (!load_pixbuf_from_memory(app))
    {
        cleanup_app(app);
        g_free(app);
        return EXIT_FAILURE;
    }

    if (!start_audio_playback(app))
    {
        cleanup_app(app);
        g_free(app);
        return EXIT_FAILURE;
    }

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "Flashbang Job");
    gtk_window_set_decorated(GTK_WINDOW(app->window), FALSE);
    gtk_window_fullscreen(GTK_WINDOW(app->window));
    gtk_window_set_keep_above(GTK_WINDOW(app->window), TRUE);

    GdkScreen *screen = gtk_widget_get_screen(app->window);
    if (screen)
    {
        GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
        if (visual)
        {
            gtk_widget_set_visual(app->window, visual);
        }
    }

    gtk_widget_set_app_paintable(app->window, TRUE);

    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app->window, "key-press-event", G_CALLBACK(on_key_press), app);
    g_signal_connect(app->window, "draw", G_CALLBACK(on_draw), app);

    app->start_time_us = g_get_monotonic_time();
    g_timeout_add(16, update_cb, app);

    gtk_widget_show_all(app->window);
    gtk_main();

    cleanup_app(app);
    g_free(app);
    return EXIT_SUCCESS;
}
