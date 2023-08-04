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

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <pipewire/pipewire.h>

#include <errno.h>
#include <string.h>
#include <stdbool.h>
#ifdef DDB_IN_TREE
#include "../../deadbeef.h"
#else
#include <deadbeef/deadbeef.h>
#endif

#define OP_ERROR_SUCCESS 0
#define OP_ERROR_INTERNAL -1

#define CONFSTR_DDBPW_VOLUMECONTROL "pipewire.volumecontrol"
#define DDBPW_DEFAULT_VOLUMECONTROL 0
#define CONFSTR_DDBPW_REMOTENAME "pipewire.remotename"
#define CONFSTR_DDBPW_PROPS "pipewire.properties"
#define DDBPW_DEFAULT_REMOTENAME ""


#ifdef ENABLE_BUFFER_OPTION
#define CONFSTR_DDBPW_BUFLENGTH "pipewire.buflength"
#endif
#define DDBPW_DEFAULT_BUFLENGTH 25

#ifdef DDBPW_DEBUG
#define trace(...) { fprintf(stdout, __VA_ARGS__); }
#else
#define trace(fmt,...)
#endif

#define log_err(...) { deadbeef->log_detailed (&plugin.plugin, DDB_LOG_LAYER_DEFAULT, __VA_ARGS__); }

DB_functions_t * deadbeef;
static DB_output_t plugin;
static char plugin_description[1024];

#define PW_PLUGIN_ID "pipewire"

static const char application_title[] = "DeaDBeeF Music Player";
static const char application_id[] = "music.deadbeef.player";

static char *tfbytecode;

static ddb_waveformat_t requested_fmt;
static ddb_playback_state_t state=DDB_PLAYBACK_STATE_STOPPED;
static uintptr_t mutex;
static int _setformat_requested;
static float _initialvol;
static int _buffersize;
static int _stride;

struct data {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    int pw_has_init;
};

struct data data = { 0, };

static int ddbpw_init(void);

static int ddbpw_free(void);

static int ddbpw_setformat(ddb_waveformat_t *fmt);

static int ddbpw_play(void);

static int ddbpw_stop(void);

static int ddbpw_pause(void);

static int ddbpw_unpause(void);

static int ddbpw_set_spec(ddb_waveformat_t *fmt);

static void my_pw_init(void) {
    if (data.pw_has_init || state != DDB_PLAYBACK_STATE_STOPPED) {
        return;
    }
    pw_init(NULL, NULL);
    data.pw_has_init = 1;
}

static void my_pw_deinit(void) {
    if (!data.pw_has_init || state != DDB_PLAYBACK_STATE_STOPPED) {
        return;
    }
    pw_deinit();
    data.pw_has_init = 0;
}

static int _apply_format(struct spa_loop *loop,
        bool async,
        uint32_t seq,
        const void *_data,
        size_t size,
        void *user_data) {
    deadbeef->mutex_lock(mutex);

    pw_stream_disconnect(data.stream);
    ddbpw_set_spec(&requested_fmt);
    _setformat_requested = 0;

    deadbeef->mutex_unlock(mutex);

    trace("From inside loop invoke function! %d\n", seq);
    return 0;
}

static void on_process(void *userdata) {
#ifdef DDBPW_DEBUG
    static int counter;
#endif
    struct data *data = userdata;
    struct pw_buffer *b = NULL;
    struct spa_buffer *buf = NULL;
    int16_t *dst = NULL;

    if (!_setformat_requested) {

        if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
            pw_log_warn("out of buffers: %m");
            return;
        }

        buf = b->buffer;
        if ((dst = buf->datas[0].data) == NULL) {
            return;
        }

#ifdef ENABLE_BUFFER_OPTION
        uint32_t buffersize = _buffersize;
        uint32_t nframes = SPA_MIN(buffersize, buf->datas[0].maxsize/_stride);
#else
        uint32_t buffersize = _buffersize;
        uint32_t nframes = SPA_MIN(buffersize, buf->datas[0].maxsize/_stride);
#endif

#if PW_CHECK_VERSION(0, 3, 49)
        if (b->requested != 0) {
            nframes = SPA_MIN(b->requested, nframes);
        }
#endif

        int len = nframes * _stride;
        int bytesread=0;
        if (deadbeef->streamer_ok_to_read(-1)) {
            bytesread = deadbeef->streamer_read (buf->datas[0].data , len);
        }
        // if (bytesread != 0) {
        //     b->size = bytesread / _stride;
        // }
        if (bytesread < len) {
            spa_memzero(buf->datas[0].data+bytesread, len-bytesread);
        }

        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = _stride;
        buf->datas[0].chunk->size = bytesread;

        trace("%d len: %d stride: %d requested: %ld nframes: %d maxsize: %u (/ stride %d) _buffersize %d bytesread %d\n",
            counter++, len, _stride, b->requested, nframes, buf->datas[0].maxsize, buf->datas[0].maxsize / _stride, buffersize, bytesread);

        pw_stream_queue_buffer(data->stream, b);
    }
}

static void
set_volume(int dolock, float volume) {
    if (data.stream && state != DDB_PLAYBACK_STATE_STOPPED) {
        float vol[SPA_AUDIO_MAX_CHANNELS] = {0};

        for (int i = 0; i < plugin.fmt.channels; i++) {
            vol[i] = volume;
        }

        if (dolock) {
            pw_thread_loop_lock(data.loop);
        }
        pw_stream_set_control(data.stream, SPA_PROP_channelVolumes, plugin.fmt.channels, vol, 0);
        if (dolock) {
            pw_thread_loop_unlock(data.loop);
        }
    }
}

static void on_state_changed(void *_data, enum pw_stream_state old,
        enum pw_stream_state pwstate, const char *error) {
    trace("PipeWire: Stream state %s\n", pw_stream_state_as_string(pwstate));

    if (_setformat_requested) {
        return;
    }

    if (pwstate == PW_STREAM_STATE_ERROR || (state == DDB_PLAYBACK_STATE_PLAYING && pwstate == PW_STREAM_STATE_UNCONNECTED ) ) {
        log_err("PipeWire: Stream error: %s\n", error);
        deadbeef->sendmessage(DB_EV_STOP, 0, 0, 0);
    }
}

static void on_control_info(void *_data, uint32_t id, const struct pw_stream_control *control) {
#ifdef DDBPW_DEBUG

    fprintf(stderr, "PipeWire: Control %s", control->name);
    for (int i = 0; i < control->n_values; i++) {
        fprintf(stderr, " value[%d] = %f", i, control->values[i]);
    }
    fprintf(stderr, "\n");
#endif

    if (id == SPA_PROP_channelVolumes && plugin.has_volume) {
        float dbvol = deadbeef->volume_get_amp();
        int changedvolume = 0;
        for (int i = 0; i < control->n_values; i++) {
            if (control->values[i] != dbvol) {
                changedvolume = i;
                break;
            }
        }

        deadbeef->volume_set_amp(control->values[changedvolume]);
    }
}

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param) {
    if (id != SPA_PARAM_Format || param == NULL) {
        return;
    }

    if (plugin.has_volume) {
        set_volume(0, _initialvol);
    }

#ifdef ENABLE_BUFFER_OPTION
    {
        const struct spa_pod *params[1];
        uint8_t buffer[4096];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        int stride = plugin.fmt.channels * (plugin.fmt.bps/8);
        int size = _buffersize*stride;

        params[0] = spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
                SPA_PARAM_BUFFERS_size,    SPA_POD_Int(size),
                SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(stride));

        pw_stream_update_params(data.stream, params, 1);
    }
#endif

}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
    .state_changed = on_state_changed,
    .control_info = on_control_info,
    .param_changed = on_param_changed,
};

static void do_update_media_props(DB_playItem_t *track, struct pw_properties *props) {
    int rc = 0, notrackgiven=0;

    ddb_tf_context_t ctx = {
        ._size = sizeof(ddb_tf_context_t),
        .flags = DDB_TF_CONTEXT_NO_DYNAMIC,
        .plt = NULL,
        .iter = PL_MAIN
    };

    if (!track) {
        track = deadbeef->streamer_get_playing_track_safe();
        if (track == NULL) {
            return;
        }
        notrackgiven = 1;
    }

    struct spa_dict_item items[3] = {0};
    int n_items = 0;

    char buf[1000] = {0};
    const char *artist = NULL;
    const char *title = NULL;

    ctx.it = track;
    if (deadbeef->tf_eval(&ctx, tfbytecode, buf, sizeof(buf)) > 0) {
        items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_NAME, buf);
    }

    deadbeef->pl_lock();
    artist = deadbeef->pl_find_meta(track, "artist");
    title = deadbeef->pl_find_meta(track, "title");

    if (artist) {
        items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_ARTIST, artist);
    }

    if (title) {
        items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_TITLE, title);
    }

    if (props) {
        pw_properties_update(props, &SPA_DICT_INIT(items, n_items));
    } else {
        rc = pw_stream_update_properties(data.stream, &SPA_DICT_INIT(items, n_items));
        if (rc < 0) {
            trace("PipeWire: Error updating properties!\n");
        }
    }

    deadbeef->pl_unlock();
    if (notrackgiven) {
        deadbeef->pl_item_unref(track);
    }
}

static int ddbpw_init(void) {
    trace ("ddbpw_init\n");

    my_pw_init();

    state = DDB_PLAYBACK_STATE_STOPPED;
    _setformat_requested = 0;
    _buffersize = 0;

    if (requested_fmt.samplerate != 0) {
        memcpy (&plugin.fmt, &requested_fmt, sizeof (ddb_waveformat_t));
    }

    data.loop = pw_thread_loop_new("ddb_out_pw", NULL);

    char dev[256] = {0};
    char remote[256] = {0};
    char propstr[256] = {0};
    deadbeef->conf_get_str (PW_PLUGIN_ID "_soundcard", "default", dev, sizeof(dev));

    deadbeef->conf_get_str(CONFSTR_DDBPW_REMOTENAME, DDBPW_DEFAULT_REMOTENAME, remote, sizeof(remote));

    deadbeef->conf_get_str(CONFSTR_DDBPW_PROPS, "", propstr, sizeof(propstr));

    struct pw_properties *props = pw_properties_new(
            PW_KEY_REMOTE_NAME, (remote[0] ? remote: NULL),
            PW_KEY_NODE_NAME, application_title,
            PW_KEY_APP_NAME, application_title,
            PW_KEY_APP_ID, application_id,
            PW_KEY_APP_ICON_NAME, "deadbeef",
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music",
            PW_KEY_NODE_TARGET, (!strcmp(dev, "default")) ? NULL: dev,
            NULL);
    do_update_media_props(NULL, props);
    pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", plugin.fmt.samplerate);

    pw_properties_update_string(props, propstr, strlen(propstr));

    data.stream = pw_stream_new_simple(
            pw_thread_loop_get_loop(data.loop),
            application_title,
            props,
            &stream_events,
            &data);

    if (!data.stream) {
        log_err("PipeWire: Error creating stream!");
        return OP_ERROR_INTERNAL;
    }


    return OP_ERROR_SUCCESS;
}

static int ddbpw_setformat (ddb_waveformat_t *fmt) {
    trace("Pipewire: setformat called!\n");
    deadbeef->mutex_lock(mutex);
    _setformat_requested = 1;
    memcpy (&requested_fmt, fmt, sizeof (ddb_waveformat_t));

    if (data.stream == 0) {
        deadbeef->mutex_unlock(mutex);
        return 0;
    }
    pw_thread_loop_lock(data.loop);
    pw_stream_set_active(data.stream, false);
    pw_loop_invoke(pw_thread_loop_get_loop(data.loop), _apply_format, 1, NULL, 0, false, NULL);
    pw_thread_loop_unlock(data.loop);

    deadbeef->mutex_unlock(mutex);
    return 0;
}

static int ddbpw_free(void) {
    trace("ddbpw_free\n");

    state = DDB_PLAYBACK_STATE_STOPPED;

    if (!data.loop) {
        return 0;
    }
    deadbeef->mutex_lock(mutex);

    pw_thread_loop_stop(data.loop);

    pw_stream_destroy(data.stream);
    data.stream = NULL;

    pw_thread_loop_destroy(data.loop);
    data.loop = NULL;
    deadbeef->mutex_unlock(mutex);
    my_pw_deinit();
    return OP_ERROR_SUCCESS;
}

static void set_channel_map(int channels, struct spa_audio_info_raw* audio_info) {
    /* Following http://www.microsoft.com/whdc/device/audio/multichaud.mspx#EKLAC */

    switch (channels) {
    case 1:
        audio_info->position[0] = SPA_AUDIO_CHANNEL_MONO;
        return;

    case 18:
        audio_info->position[15] = SPA_AUDIO_CHANNEL_TRL;
        audio_info->position[16] = SPA_AUDIO_CHANNEL_TRC;
        audio_info->position[17] = SPA_AUDIO_CHANNEL_TRR;
        /* Fall through */

    case 15:
        audio_info->position[12] = SPA_AUDIO_CHANNEL_TFL;
        audio_info->position[13] = SPA_AUDIO_CHANNEL_TFC;
        audio_info->position[14] = SPA_AUDIO_CHANNEL_TFR;
        /* Fall through */

    case 12:
        audio_info->position[11] = SPA_AUDIO_CHANNEL_TC;
        /* Fall through */

    case 11:
        audio_info->position[9] = SPA_AUDIO_CHANNEL_SL;
        audio_info->position[10] = SPA_AUDIO_CHANNEL_SR;
        /* Fall through */

    case 9:
        audio_info->position[8] = SPA_AUDIO_CHANNEL_RC;
        /* Fall through */

    case 8:
        audio_info->position[6] = SPA_AUDIO_CHANNEL_FLC;
        audio_info->position[7] = SPA_AUDIO_CHANNEL_FRC;
        /* Fall through */

    case 6:
        audio_info->position[4] = SPA_AUDIO_CHANNEL_RL;
        audio_info->position[5] = SPA_AUDIO_CHANNEL_RR;
        /* Fall through */

    case 4:
        audio_info->position[3] = SPA_AUDIO_CHANNEL_LFE;
        /* Fall through */

    case 3:
        audio_info->position[2] = SPA_AUDIO_CHANNEL_FC;
        /* Fall through */

    case 2:
        audio_info->position[0] = SPA_AUDIO_CHANNEL_FL;
        audio_info->position[1] = SPA_AUDIO_CHANNEL_FR;

    }
}

static struct spa_pod * makeformat(ddb_waveformat_t *fmt, uint8_t *buffer, size_t buffer_size) {

    enum spa_audio_format pwfmt = 0;

    switch (fmt->bps) {
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
        if (fmt->is_float) {
            pwfmt = SPA_AUDIO_FORMAT_F32_LE;
        }
        else {
            pwfmt = SPA_AUDIO_FORMAT_S32_LE;
        }
        break;
    default:
        return NULL;
    };



    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, buffer_size);

    struct spa_audio_info_raw rawinfo =  SPA_AUDIO_INFO_RAW_INIT(
        .flags = 0,
        .format = pwfmt,
        .channels = fmt->channels,
        .rate = fmt->samplerate
    );

    set_channel_map(fmt->channels, &rawinfo);

    return spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &rawinfo);

}

static int ddbpw_set_spec(ddb_waveformat_t *fmt) {
    memcpy (&plugin.fmt, fmt, sizeof (ddb_waveformat_t));
    if (!plugin.fmt.channels) {
        // generic format
        plugin.fmt.bps = 16;
        plugin.fmt.is_float = 0;
        plugin.fmt.channels = 2;
        plugin.fmt.samplerate = 44100;
        plugin.fmt.channelmask = 3;
    }

    trace ("format %dbit %s %dch %dHz channelmask=%X\n", plugin.fmt.bps, plugin.fmt.is_float ? "float" : "int", plugin.fmt.channels, plugin.fmt.samplerate, plugin.fmt.channelmask);
    _stride = plugin.fmt.channels * (plugin.fmt.bps / 8);

    uint8_t spa_buffer[1024];
    const struct spa_pod *params[1] = {
        makeformat(&plugin.fmt, spa_buffer, sizeof (spa_buffer))
    };

    struct pw_properties *props = pw_properties_new(NULL, NULL);
#ifdef ENABLE_BUFFER_OPTION
    _buffersize = deadbeef->conf_get_int(CONFSTR_DDBPW_BUFLENGTH, DDBPW_DEFAULT_BUFLENGTH) * plugin.fmt.samplerate / 1000;
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%u", _buffersize, plugin.fmt.samplerate);
#else
    _buffersize = DDBPW_DEFAULT_BUFLENGTH * plugin.fmt.samplerate / 1000;
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%u", _buffersize, plugin.fmt.samplerate);
#endif

    pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", plugin.fmt.samplerate);
    pw_stream_update_properties(data.stream, &props->dict);
    pw_properties_free(props);

    if (0 != pw_stream_connect(data.stream,
                PW_DIRECTION_OUTPUT,
                PW_ID_ANY,
                PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_MAP_BUFFERS |
                PW_STREAM_FLAG_RT_PROCESS,
                params, 1)) {
        log_err("PipeWire: Error connecting stream!\n");
        if (pw_properties_get(pw_stream_get_properties(data.stream), PW_KEY_REMOTE_NAME)) {
            log_err("PipeWire: Please check if remote daemon name is valid and daemon is up.\n")
        }
        return OP_ERROR_INTERNAL;
    };


    state = DDB_PLAYBACK_STATE_PLAYING;

    return OP_ERROR_SUCCESS;
}

static void update_has_volume(void) {
    plugin.has_volume = deadbeef->conf_get_int(CONFSTR_DDBPW_VOLUMECONTROL, DDBPW_DEFAULT_VOLUMECONTROL);
}

static int ddbpw_play(void) {
    trace ("ddbpw_play\n");

    deadbeef->mutex_lock(mutex);

    update_has_volume();
    _initialvol = plugin.has_volume ? deadbeef->volume_get_amp() : 1.0f;

    if (!data.loop) {
        ddbpw_init();
    }


    int ret = ddbpw_set_spec(&plugin.fmt);
    pw_thread_loop_start(data.loop);
    if (ret != 0) {
        ddbpw_free();
    }
    deadbeef->mutex_unlock(mutex);
    return ret;
}

static int ddbpw_stop(void) {
    ddbpw_free();

    return OP_ERROR_SUCCESS;
}

static int ddbpw_pause(void) {
    if (!data.loop && ddbpw_play() != OP_ERROR_SUCCESS) {
        return OP_ERROR_INTERNAL;
    }

    // set pause state
    state = DDB_PLAYBACK_STATE_PAUSED;
    pw_thread_loop_lock(data.loop);
    pw_stream_flush(data.stream, 0);
    pw_stream_set_active(data.stream, 0);
    pw_thread_loop_unlock(data.loop);
    return OP_ERROR_SUCCESS;
}

static int ddbpw_unpause(void) {
    // unset pause state
    if (state == DDB_PLAYBACK_STATE_PAUSED) {
        state = DDB_PLAYBACK_STATE_PLAYING;
    }
    pw_thread_loop_lock(data.loop);
    pw_stream_set_active(data.stream, 1);
    pw_thread_loop_unlock(data.loop);
    return OP_ERROR_SUCCESS;
}


static ddb_playback_state_t ddbpw_get_state(void) {
    return state;
}



static int ddbpw_plugin_start(void) {
    mutex = deadbeef->mutex_create();

    tfbytecode = deadbeef->tf_compile("[%artist% - ]%title%");
    return 0;
}

static int ddbpw_plugin_stop(void) {
    deadbeef->mutex_free(mutex);
    deadbeef->tf_free(tfbytecode);
    return 0;
}

DB_plugin_t * ddb_out_pw_load(DB_functions_t *api) {
    deadbeef = api;
    snprintf(plugin_description, sizeof(plugin_description),
            "This is a PipeWire plugin.\nLinked to library version %s\n", pw_get_library_version());
    plugin.plugin.descr = plugin_description;
    return DB_PLUGIN (&plugin);
}

static int
ddbpw_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    switch (id) {
    case DB_EV_SONGSTARTED:
        if (state == DDB_PLAYBACK_STATE_PLAYING) {
            pw_thread_loop_lock(data.loop);
            do_update_media_props(((ddb_event_track_t *)ctx)->track, NULL);
            pw_thread_loop_unlock(data.loop);
        }
        break;
    case DB_EV_VOLUMECHANGED:
        if (plugin.has_volume) {
            set_volume(1, deadbeef->volume_get_amp());
        }
        break;
    case DB_EV_CONFIGCHANGED:
        update_has_volume();
        if (plugin.has_volume) {
            set_volume(1, deadbeef->volume_get_amp());
        } else {
            set_volume(1, 1.0f);
        }
        break;
    }
    return 0;
}

struct enum_card_userdata {
    void (*callback)(const char *name, const char *desc, void *);
    void *userdata;
};

static void registry_event_global(void *data, uint32_t id,
        uint32_t permissions, const char *type, uint32_t version,
        const struct spa_dict *props) {
    struct enum_card_userdata *enumuserdata = (struct enum_card_userdata *)data;

    if (!strcmp(type, PW_TYPE_INTERFACE_Node) && props) {
        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);

        if (media_class && (!strcmp(media_class, "Audio/Sink") || !strcmp(media_class, "Audio/Duplex"))) {
            const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
            const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            char name_buf[64];
            if (!name) {
                snprintf(name_buf, sizeof(name_buf), "%d", id);
                name = name_buf;
            }

            if (!desc) {
                desc = name;
            }

            // Avoid crazy long descriptions, they grow the GTK dropdown box in deadbeef GUI
            // Truncate with a middle ellipsis so we catch output port names that are always at the end
            char buf[256] = { 0 };
            if (strlen(desc) > 80) {
                strncpy (buf, desc, 38);
                strcat(buf, "...");
                strcat (buf, desc+strlen(desc)-38);

            }
            else {
                strcpy(buf, desc ? desc : "");
            }

            enumuserdata->callback(name, buf, enumuserdata->userdata);
        }
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
};

struct donedata {
    int pending;
    int done;
    struct pw_main_loop *loop;
};

void core_event_done(void *object, uint32_t id, int seq) {
    struct donedata *donedata = (struct donedata *)object;
    if (id == PW_ID_CORE && seq == donedata->pending) {
        donedata->done = 1;
        pw_main_loop_quit(donedata->loop);
    }
}

static int roundtrip(struct pw_core *core, struct pw_main_loop *loop) {
    struct spa_hook core_listener;
    struct donedata donedata = {0,0, loop};

    const struct pw_core_events core_events = {
        PW_VERSION_CORE_EVENTS,
        .done = core_event_done,
    };

    spa_zero(core_listener);
    pw_core_add_listener(core, &core_listener,
            &core_events, &donedata);

    donedata.pending = pw_core_sync(core, PW_ID_CORE, 0);

    while (!donedata.done) {
        pw_main_loop_run(loop);
    }
    spa_hook_remove(&core_listener);
    return 0;
}

static void
ddbpw_enum_soundcards(void (*callback)(const char *name, const char *desc, void *), void *userdata) {
    struct pw_main_loop *loop = NULL;
    struct pw_context *context = NULL;
    struct pw_core *core = NULL;
    struct pw_registry *registry = NULL;
    struct spa_hook registry_listener = {0};
    struct enum_card_userdata enumuserdata = {0};

    my_pw_init();

    loop = pw_main_loop_new(NULL /* properties */);
    context = pw_context_new(pw_main_loop_get_loop(loop),
            NULL /* properties */,
            0 /* user_data size */);
    if (!context) {
        return;
    }

    char remote[256] = { 0 };
    deadbeef->conf_get_str(CONFSTR_DDBPW_REMOTENAME, DDBPW_DEFAULT_REMOTENAME, remote, sizeof(remote));

    core = pw_context_connect(context,
            pw_properties_new(
                PW_KEY_REMOTE_NAME,  (remote[0] ? remote: NULL),
                NULL),
            0 /* user_data size */);
    if (!core) {
        return;
    }

    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY,
            0 /* user_data size */);
    if (!registry) {
        return;
    }

    enumuserdata.callback = callback;
    enumuserdata.userdata = userdata;

    spa_zero(registry_listener);
    pw_registry_add_listener(registry, &registry_listener,
            &registry_events, &enumuserdata);

    roundtrip(core, loop);

    pw_proxy_destroy((struct pw_proxy *)registry);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    my_pw_deinit();
}

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

static const char settings_dlg[] =
"property \"PipeWire remote daemon name (empty for default)\" entry " CONFSTR_DDBPW_REMOTENAME " " STR(DDBPW_DEFAULT_REMOTENAME) ";\n"
"property \"Custom properties (overrides existing ones):\" label l;\n"
"property \"\" entry " CONFSTR_DDBPW_PROPS " \"\" ;\n"
"property \"Use PipeWire volume control\" checkbox " CONFSTR_DDBPW_VOLUMECONTROL " " STR(DDBPW_DEFAULT_VOLUMECONTROL) ";\n"
#ifdef ENABLE_BUFFER_OPTION
"property \"Buffer length (ms)\" entry " CONFSTR_DDBPW_BUFLENGTH " " STR(DDBPW_DEFAULT_BUFLENGTH) ";\n"
#endif
;


static DB_output_t plugin = {
    .plugin.api_vmajor = DB_API_VERSION_MAJOR,
    .plugin.api_vminor = DB_API_VERSION_MINOR,
    .plugin.version_major = 0,
    .plugin.version_minor = 1,
    .plugin.flags = DDB_PLUGIN_FLAG_LOGGING,
    .plugin.type = DB_PLUGIN_OUTPUT,
    .plugin.id = PW_PLUGIN_ID,
    .plugin.name = "PipeWire output plugin dev",
    //.plugin.descr = "This is a new PipeWire plugin",
    .plugin.copyright =
        "Pipewire output plugin for DeaDBeeF Player\n"
        "Copyright (C) 2020-2021 Nicolai Syvertsen <saivert@saivert.com>\n"
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
    .plugin.configdialog = settings_dlg,
    .plugin.message = ddbpw_message,
    .init = ddbpw_init,
    .free = ddbpw_free,
    .setformat = ddbpw_setformat,
    .play = ddbpw_play,
    .stop = ddbpw_stop,
    .pause = ddbpw_pause,
    .unpause = ddbpw_unpause,
    .state = ddbpw_get_state,
    .enum_soundcards = ddbpw_enum_soundcards,
    .has_volume = DDBPW_DEFAULT_VOLUMECONTROL,
};
