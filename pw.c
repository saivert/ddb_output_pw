/*
    PipeWire output plugin for DeaDBeeF Player
    Copyright (C) 2020 Nicolai Syvertsen <saivert@saivert.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#  include "../config.h"
#endif

#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>

#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <deadbeef/deadbeef.h>

#define OP_ERROR_SUCCESS 0
#define OP_ERROR_INTERNAL -1

#ifdef DDBPW_DEBUG
#define trace(...) { fprintf(stdout, __VA_ARGS__); }
#else
#define trace(fmt,...)
#endif

#define log_err(...) { deadbeef->log_detailed (&plugin.plugin, DDB_LOG_LAYER_DEFAULT, __VA_ARGS__); }

DB_functions_t * deadbeef;
static DB_output_t plugin;

#define PW_PLUGIN_ID "pipewire"



static ddb_waveformat_t requested_fmt;
static int state=OUTPUT_STATE_STOPPED;
static uintptr_t mutex;

struct data {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
};

struct data data = { 0, };

static int ddbpw_init();

static int ddbpw_free();

static int ddbpw_setformat(ddb_waveformat_t *fmt);

static int ddbpw_play();

static int ddbpw_stop();

static int ddbpw_pause();

static int ddbpw_unpause();

static int ddbpw_set_spec(ddb_waveformat_t *fmt);



static void on_process(void *userdata)
{
    struct data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    int n_frames, stride;
    int16_t *dst;

    if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    if ((dst = buf->datas[0].data) == NULL)
        return;

    stride = 1;
    n_frames = 4096;

    int len = n_frames * stride;

    if (deadbeef->streamer_ok_to_read(len)) {
        int bytesread = deadbeef->streamer_read (buf->datas[0].data , len);

        if (bytesread < len) {
            memset (buf->datas[0].data + len, 0, len-bytesread);
        }
    } 

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size = n_frames * stride;

    pw_stream_queue_buffer(data->stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

static int ddbpw_init(void)
{
    trace ("ddbpw_init\n");

    state = OUTPUT_STATE_STOPPED;

    if (requested_fmt.samplerate != 0) {
        memcpy (&plugin.fmt, &requested_fmt, sizeof (ddb_waveformat_t));
    }


    data.loop = pw_thread_loop_new("ddb_out_pw", NULL);

    data.stream = pw_stream_new_simple(
            pw_thread_loop_get_loop(data.loop),
            "audio-src",
            pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio",
                PW_KEY_MEDIA_CATEGORY, "Playback",
                PW_KEY_MEDIA_ROLE, "Music",
                NULL),
            &stream_events,
            &data);



    return OP_ERROR_SUCCESS;
}

static int ddbpw_setformat (ddb_waveformat_t *fmt)
{
    int st = state;
    memcpy (&requested_fmt, fmt, sizeof (ddb_waveformat_t));
    if (!data.loop
        || !memcmp (fmt, &plugin.fmt, sizeof (ddb_waveformat_t))) {
        return 0;
    }

    ddbpw_free ();
    ddbpw_init ();
    int res = 0;
    if (st == OUTPUT_STATE_PLAYING) {
        res = ddbpw_play ();
    }
    else if (st == OUTPUT_STATE_PAUSED) {
        res = ddbpw_pause ();
    }

    return res;
}

static int ddbpw_free(void)
{
    trace("ddbpw_free\n");

    state = OUTPUT_STATE_STOPPED;
    if (!data.loop) {
        return 0;
    }

    pw_stream_destroy(data.stream);
    data.stream = NULL;

    pw_thread_loop_destroy(data.loop);
    data.loop = NULL;

    return OP_ERROR_SUCCESS;
}

static int ddbpw_set_spec(ddb_waveformat_t *fmt)
{
    memcpy (&plugin.fmt, fmt, sizeof (ddb_waveformat_t));
    if (!plugin.fmt.channels) {
        // generic format
        plugin.fmt.bps = 16;
        plugin.fmt.is_float = 0;
        plugin.fmt.channels = 2;
        plugin.fmt.samplerate = 44100;
        plugin.fmt.channelmask = 3;
    }
    if (plugin.fmt.samplerate > 192000) {
        plugin.fmt.samplerate = 192000;
    }

    trace ("format %dbit %s %dch %dHz channelmask=%X\n", plugin.fmt.bps, plugin.fmt.is_float ? "float" : "int", plugin.fmt.channels, plugin.fmt.samplerate, plugin.fmt.channelmask);

    enum spa_audio_format pwfmt;

    switch (plugin.fmt.bps) {
    case 8:
        pwfmt = SPA_AUDIO_FORMAT_S8;
        break;
    case 16:
        pwfmt = SPA_AUDIO_FORMAT_S16_LE;
        break;
    case 24:
        pwfmt = SPA_AUDIO_FORMAT_S24_LE;
        break;
    case 32:
        if (plugin.fmt.is_float) {
            pwfmt = SPA_AUDIO_FORMAT_F32_LE;
        }
        else {
            pwfmt = SPA_AUDIO_FORMAT_S32_LE;
        }
        break;
    default:
        return -1;
    };


    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
            &SPA_AUDIO_INFO_RAW_INIT(
                .format = pwfmt,
                .channels = plugin.fmt.channels,
                .rate = plugin.fmt.samplerate ));

    pw_stream_connect(data.stream,
              PW_DIRECTION_OUTPUT,
              PW_ID_ANY,
              PW_STREAM_FLAG_AUTOCONNECT |
              PW_STREAM_FLAG_MAP_BUFFERS |
              PW_STREAM_FLAG_RT_PROCESS,
              params, 1);

    pw_thread_loop_start(data.loop);

    state = DDB_PLAYBACK_STATE_PLAYING;

    return OP_ERROR_SUCCESS;
}

static int ddbpw_play(void)
{
    trace ("ddbpw_play\n");

    if (!data.loop) {
        ddbpw_init();
    }


    int ret = ddbpw_set_spec(&plugin.fmt);
    if (ret != 0) {
        ddbpw_free();
    }
    return ret;
}

static int ddbpw_stop(void)
{
    ddbpw_free();

    return OP_ERROR_SUCCESS;
}

static int ddbpw_pause(void)
{
    if (state == DDB_PLAYBACK_STATE_STOPPED) {
        return -1;
    }
    // set pause state
    state = DDB_PLAYBACK_STATE_PAUSED;

    pw_stream_set_active(data.stream, 0);
    return OP_ERROR_SUCCESS;
}

static int ddbpw_unpause(void)
{
    // unset pause state
    if (state == DDB_PLAYBACK_STATE_PAUSED) {
        state = DDB_PLAYBACK_STATE_PLAYING;
    }
    pw_stream_set_active(data.stream, 1);
    return OP_ERROR_SUCCESS;
}


static ddb_playback_state_t ddbpw_get_state(void)
{
    return state;
}



static int ddbpw_plugin_start(void)
{
    mutex = deadbeef->mutex_create();
    int argc = 1;
    const char **argv = alloca (sizeof (char *) * argc);
    argv[0] = "deadbeef";

    pw_init(&argc, (char ***)&argv);
    return 0;
}

static int ddbpw_plugin_stop(void)
{
    deadbeef->mutex_free(mutex);

    return 0;
}

DB_plugin_t * ddb_out_pw_load(DB_functions_t *api)
{
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}

static int
ddbpw_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    switch (id) {
    case DB_EV_SONGSTARTED:
    case DB_EV_VOLUMECHANGED:
    case DB_EV_CONFIGCHANGED:
        break;
    }
    return 0;
}

static DB_output_t plugin =
{
    .plugin.api_vmajor = 1,
    .plugin.api_vminor = 0,
    .plugin.version_major = 0,
    .plugin.version_minor = 1,
    .plugin.flags = DDB_PLUGIN_FLAG_LOGGING,
    .plugin.type = DB_PLUGIN_OUTPUT,
    .plugin.id = PW_PLUGIN_ID,
    .plugin.name = "PipeWire output plugin",
    .plugin.descr = "This is a new PipeWire plugin",
    .plugin.copyright =
        "PulseAudio output plugin for DeaDBeeF Player\n"
        "Copyright (C) 2020 Nicolai Syvertsen <saivert@saivert.com>\n"
        "\n"
        "This program is free software; you can redistribute it and/or\n"
        "modify it under the terms of the GNU General Public License\n"
        "as published by the Free Software Foundation; either version 2\n"
        "of the License, or (at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program; if not, write to the Free Software\n"
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n",
    .plugin.website = "http://saivert.com",
    .plugin.start = ddbpw_plugin_start,
    .plugin.stop = ddbpw_plugin_stop,
    .plugin.configdialog = NULL, /* settings_dlg, */
    .plugin.message = ddbpw_message,
    .init = ddbpw_init,
    .free = ddbpw_free,
    .setformat = ddbpw_setformat,
    .play = ddbpw_play,
    .stop = ddbpw_stop,
    .pause = ddbpw_pause,
    .unpause = ddbpw_unpause,
    .state = ddbpw_get_state,
    .enum_soundcards = NULL,
    .has_volume = 0,
};
