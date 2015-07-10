/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "alsa.hpp"
#include "soundio.hpp"
#include "os.hpp"
#include "atomics.hpp"

#include <alsa/asoundlib.h>
#include <sys/inotify.h>

static snd_pcm_stream_t stream_types[] = {SND_PCM_STREAM_PLAYBACK, SND_PCM_STREAM_CAPTURE};

static const int MAX_SAMPLE_RATE = 48000;

struct SoundIoAlsa {
    SoundIoOsMutex *mutex;
    SoundIoOsCond *cond;

    struct SoundIoOsThread *thread;
    atomic_flag abort_flag;
    int notify_fd;
    int notify_wd;
    atomic_bool have_devices_flag;
    int notify_pipe_fd[2];

    // this one is ready to be read with flush_events. protected by mutex
    struct SoundIoDevicesInfo *ready_devices_info;
};

static void wakeup_device_poll(SoundIoAlsa *sia) {
    ssize_t amt = write(sia->notify_pipe_fd[1], "a", 1);
    if (amt == -1) {
        assert(errno != EBADF);
        assert(errno != EIO);
        assert(errno != ENOSPC);
        assert(errno != EPERM);
        assert(errno != EPIPE);
    }
}

static void destroy_alsa(SoundIo *soundio) {
    SoundIoAlsa *sia = (SoundIoAlsa *)soundio->backend_data;
    if (!sia)
        return;

    if (sia->thread) {
        sia->abort_flag.clear();
        wakeup_device_poll(sia);
        soundio_os_thread_destroy(sia->thread);
    }

    if (sia->cond)
        soundio_os_cond_destroy(sia->cond);

    if (sia->mutex)
        soundio_os_mutex_destroy(sia->mutex);

    soundio_destroy_devices_info(sia->ready_devices_info);



    close(sia->notify_pipe_fd[0]);
    close(sia->notify_pipe_fd[1]);
    close(sia->notify_fd);

    destroy(sia);
    soundio->backend_data = nullptr;
}

static char * str_partition_on_char(char *str, char c) {
    while (*str) {
        if (*str == c) {
            *str = 0;
            return str + 1;
        }
        str += 1;
    }
    return nullptr;
}

static snd_pcm_stream_t purpose_to_stream(SoundIoDevicePurpose purpose) {
    switch (purpose) {
        case SoundIoDevicePurposeOutput: return SND_PCM_STREAM_PLAYBACK;
        case SoundIoDevicePurposeInput: return SND_PCM_STREAM_CAPTURE;
    }
    soundio_panic("invalid purpose");
}

static SoundIoChannelId from_alsa_chmap_pos(unsigned int pos) {
    switch ((snd_pcm_chmap_position)pos) {
        case SND_CHMAP_UNKNOWN: return SoundIoChannelIdInvalid;
        case SND_CHMAP_NA:      return SoundIoChannelIdInvalid;
        case SND_CHMAP_MONO:    return SoundIoChannelIdFrontCenter;
        case SND_CHMAP_FL:      return SoundIoChannelIdFrontLeft; // front left
        case SND_CHMAP_FR:      return SoundIoChannelIdFrontRight; // front right
        case SND_CHMAP_RL:      return SoundIoChannelIdBackLeft; // rear left
        case SND_CHMAP_RR:      return SoundIoChannelIdBackRight; // rear right
        case SND_CHMAP_FC:      return SoundIoChannelIdFrontCenter; // front center
        case SND_CHMAP_LFE:     return SoundIoChannelIdLfe; // LFE
        case SND_CHMAP_SL:      return SoundIoChannelIdSideLeft; // side left
        case SND_CHMAP_SR:      return SoundIoChannelIdSideRight; // side right
        case SND_CHMAP_RC:      return SoundIoChannelIdBackCenter; // rear center
        case SND_CHMAP_FLC:     return SoundIoChannelIdFrontLeftCenter; // front left center
        case SND_CHMAP_FRC:     return SoundIoChannelIdFrontRightCenter; // front right center
        case SND_CHMAP_RLC:     return SoundIoChannelIdBackLeftCenter; // rear left center
        case SND_CHMAP_RRC:     return SoundIoChannelIdBackRightCenter; // rear right center
        case SND_CHMAP_FLW:     return SoundIoChannelIdFrontLeftWide; // front left wide
        case SND_CHMAP_FRW:     return SoundIoChannelIdFrontRightWide; // front right wide
        case SND_CHMAP_FLH:     return SoundIoChannelIdFrontLeftHigh; // front left high
        case SND_CHMAP_FCH:     return SoundIoChannelIdFrontCenterHigh; // front center high
        case SND_CHMAP_FRH:     return SoundIoChannelIdFrontRightHigh; // front right high
        case SND_CHMAP_TC:      return SoundIoChannelIdTopCenter; // top center
        case SND_CHMAP_TFL:     return SoundIoChannelIdTopFrontLeft; // top front left
        case SND_CHMAP_TFR:     return SoundIoChannelIdTopFrontRight; // top front right
        case SND_CHMAP_TFC:     return SoundIoChannelIdTopFrontCenter; // top front center
        case SND_CHMAP_TRL:     return SoundIoChannelIdTopBackLeft; // top rear left
        case SND_CHMAP_TRR:     return SoundIoChannelIdTopBackRight; // top rear right
        case SND_CHMAP_TRC:     return SoundIoChannelIdTopBackCenter; // top rear center
        case SND_CHMAP_TFLC:    return SoundIoChannelIdTopFrontLeftCenter; // top front left center
        case SND_CHMAP_TFRC:    return SoundIoChannelIdTopFrontRightCenter; // top front right center
        case SND_CHMAP_TSL:     return SoundIoChannelIdTopSideLeft; // top side left
        case SND_CHMAP_TSR:     return SoundIoChannelIdTopSideRight; // top side right
        case SND_CHMAP_LLFE:    return SoundIoChannelIdLeftLfe; // left LFE
        case SND_CHMAP_RLFE:    return SoundIoChannelIdRightLfe; // right LFE
        case SND_CHMAP_BC:      return SoundIoChannelIdBottomCenter; // bottom center
        case SND_CHMAP_BLC:     return SoundIoChannelIdBottomLeftCenter; // bottom left center
        case SND_CHMAP_BRC:     return SoundIoChannelIdBottomRightCenter; // bottom right center
    }
    return SoundIoChannelIdInvalid;
}

static void get_channel_layout(SoundIoDevice *device, snd_pcm_chmap_t *chmap) {
    int channel_count = min((unsigned int)SOUNDIO_MAX_CHANNELS, chmap->channels);
    device->channel_layout.channel_count = channel_count;
    device->channel_layout.name = nullptr;
    for (int i = 0; i < channel_count; i += 1) {
        device->channel_layout.channels[i] = from_alsa_chmap_pos(chmap->pos[i]);
    }
    soundio_channel_layout_detect_builtin(&device->channel_layout);
}

static void handle_channel_maps(SoundIoDevice *device, snd_pcm_chmap_query_t **maps) {
    if (!maps)
        return;
    snd_pcm_chmap_query_t **p;
    snd_pcm_chmap_query_t *v;
    snd_pcm_chmap_t *best = nullptr;
    for (p = maps; (v = *p); p += 1) {
        if (!best || v->map.channels > best->channels)
            best = &v->map;
    }
    get_channel_layout(device, best);
    snd_pcm_free_chmaps(maps);
}

static int probe_device(SoundIoDevice *device, snd_pcm_chmap_query_t **maps) {
    int err;
    snd_pcm_t *handle;

    snd_pcm_hw_params_t *hwparams;
    snd_pcm_sw_params_t *swparams;

    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_sw_params_alloca(&swparams);

    snd_pcm_stream_t stream = purpose_to_stream(device->purpose);

    if ((err = snd_pcm_open(&handle, device->name, stream, 0)) < 0) {
        handle_channel_maps(device, maps);
        return SoundIoErrorOpeningDevice;
    }

    if ((err = snd_pcm_hw_params_any(handle, hwparams)) < 0) {
        snd_pcm_close(handle);
        return SoundIoErrorOpeningDevice;
    }

    // disable hardware resampling because we're trying to probe
    if ((err = snd_pcm_hw_params_set_rate_resample(handle, hwparams, 0)) < 0) {
        snd_pcm_close(handle);
        return SoundIoErrorOpeningDevice;
    }

    if ((err = snd_pcm_hw_params_set_access(handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        snd_pcm_close(handle);
        return SoundIoErrorOpeningDevice;
    }

    unsigned int channel_count;
    if ((err = snd_pcm_hw_params_set_channels_last(handle, hwparams, &channel_count)) < 0) {
        snd_pcm_close(handle);
        return SoundIoErrorOpeningDevice;
    }

    unsigned int min_sample_rate;
    unsigned int max_sample_rate;
    int min_dir;
    int max_dir;

    if ((err = snd_pcm_hw_params_get_rate_max(hwparams, &max_sample_rate, &max_dir)) < 0) {
        snd_pcm_close(handle);
        return SoundIoErrorOpeningDevice;
    }
    if (max_dir < 0)
        max_sample_rate -= 1;

    if ((err = snd_pcm_hw_params_get_rate_min(hwparams, &min_sample_rate, &min_dir)) < 0) {
        snd_pcm_close(handle);
        return SoundIoErrorOpeningDevice;
    }
    if (min_dir > 0)
        min_sample_rate += 1;



    snd_pcm_chmap_t *chmap = snd_pcm_get_chmap(handle);
    if (chmap) {
        get_channel_layout(device, chmap);
        free(chmap);
    } else if (!maps) {
        maps = snd_pcm_query_chmaps(handle);
    }
    handle_channel_maps(device, maps);


    device->sample_rate_min = min_sample_rate;
    device->sample_rate_min = max_sample_rate;
    device->sample_rate_default =
            (min_sample_rate <= MAX_SAMPLE_RATE &&
            MAX_SAMPLE_RATE <= max_sample_rate) ? MAX_SAMPLE_RATE : max_sample_rate;


    snd_pcm_close(handle);
    return 0;

            // TODO: device->default_sample_format
            // TODO: device->default_latency
}

static inline bool str_has_prefix(const char *big_str, const char *prefix) {
    return strncmp(big_str, prefix, strlen(prefix)) == 0;
}

static int refresh_devices(SoundIo *soundio) {
    SoundIoAlsa *sia = (SoundIoAlsa *)soundio->backend_data;

    SoundIoDevicesInfo *devices_info = create<SoundIoDevicesInfo>();
    if (!devices_info)
        return SoundIoErrorNoMem;

    void **hints;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        destroy(devices_info);
        return SoundIoErrorNoMem;
    }

    for (void **hint_ptr = hints; *hint_ptr; hint_ptr += 1) {
        char *name = snd_device_name_get_hint(*hint_ptr, "NAME");
        // null - libsoundio has its own dummy backend. API clients should use
        // that instead of alsa null device.
        if (strcmp(name, "null") == 0 ||
            // sysdefault is confusing - the name and description is identical
            // to default, and my best guess for what it does is ignore ~/.asoundrc
            // which is just an accident waiting to happen.
            str_has_prefix(name, "sysdefault:") ||
            // all these surround devices are clutter
            str_has_prefix(name, "front:") ||
            str_has_prefix(name, "surround21:") ||
            str_has_prefix(name, "surround40:") ||
            str_has_prefix(name, "surround41:") ||
            str_has_prefix(name, "surround50:") ||
            str_has_prefix(name, "surround51:") ||
            str_has_prefix(name, "surround71:"))
        {
            free(name);
            continue;
        }

        char *descr = snd_device_name_get_hint(*hint_ptr, "DESC");
        char *descr1 = str_partition_on_char(descr, '\n');

        char *io = snd_device_name_get_hint(*hint_ptr, "IOID");
        bool is_playback;
        bool is_capture;
        if (io) {
            if (strcmp(io, "Input") == 0) {
                is_playback = false;
                is_capture = true;
            } else if (strcmp(io, "Output") == 0) {
                is_playback = true;
                is_capture = false;
            } else {
                soundio_panic("invalid io hint value");
            }
            free(io);
        } else {
            is_playback = true;
            is_capture = true;
        }

        for (int stream_type_i = 0; stream_type_i < array_length(stream_types); stream_type_i += 1) {
            snd_pcm_stream_t stream = stream_types[stream_type_i];
            if (stream == SND_PCM_STREAM_PLAYBACK && !is_playback) continue;
            if (stream == SND_PCM_STREAM_CAPTURE && !is_capture) continue;
            if (stream == SND_PCM_STREAM_CAPTURE && descr1 &&
                (strstr(descr1, "Output") || strstr(descr1, "output")))
            {
                continue;
            }


            SoundIoDevice *device = create<SoundIoDevice>();
            if (!device) {
                free(name);
                free(descr);
                destroy(devices_info);
                snd_device_name_free_hint(hints);
                return SoundIoErrorNoMem;
            }
            device->ref_count = 1;
            device->soundio = soundio;
            device->name = strdup(name);
            device->description = descr1 ?
                soundio_alloc_sprintf(nullptr, "%s: %s", descr, descr1) : strdup(descr);
            device->is_raw = false;

            if (!device->name || !device->description) {
                soundio_device_unref(device);
                free(name);
                free(descr);
                destroy(devices_info);
                snd_device_name_free_hint(hints);
                return SoundIoErrorNoMem;
            }

            SoundIoList<SoundIoDevice *> *device_list;
            if (stream == SND_PCM_STREAM_PLAYBACK) {
                device->purpose = SoundIoDevicePurposeOutput;
                device_list = &devices_info->output_devices;
                if (str_has_prefix(name, "default:"))
                    devices_info->default_output_index = device_list->length;
            } else {
                assert(stream == SND_PCM_STREAM_CAPTURE);
                device->purpose = SoundIoDevicePurposeInput;
                device_list = &devices_info->input_devices;
                if (str_has_prefix(name, "default:"))
                    devices_info->default_input_index = device_list->length;
            }

            probe_device(device, nullptr);

            if (device_list->append(device)) {
                soundio_device_unref(device);
                free(name);
                free(descr);
                destroy(devices_info);
                snd_device_name_free_hint(hints);
                return SoundIoErrorNoMem;
            }
        }

        free(name);
        free(descr);
    }

    snd_device_name_free_hint(hints);

    int card_index = -1;

    if (snd_card_next(&card_index) < 0)
        return SoundIoErrorSystemResources;

    snd_ctl_card_info_t *card_info;
    snd_ctl_card_info_alloca(&card_info);

    snd_pcm_info_t *pcm_info;
    snd_pcm_info_alloca(&pcm_info);

    while (card_index >= 0) {
        int err;
        snd_ctl_t *handle;
        char name[32];
        sprintf(name, "hw:%d", card_index);
        if ((err = snd_ctl_open(&handle, name, 0)) < 0) {
            if (err == -ENOENT) {
                break;
            } else {
                destroy(devices_info);
                return SoundIoErrorOpeningDevice;
            }
        }

        if ((err = snd_ctl_card_info(handle, card_info)) < 0) {
            snd_ctl_close(handle);
            destroy(devices_info);
            return SoundIoErrorSystemResources;
        }
        const char *card_name = snd_ctl_card_info_get_name(card_info);

        int device_index = -1;
        for (;;) {
            if (snd_ctl_pcm_next_device(handle, &device_index) < 0) {
                snd_ctl_close(handle);
                destroy(devices_info);
                return SoundIoErrorSystemResources;
            }
            if (device_index < 0)
                break;

            snd_pcm_info_set_device(pcm_info, device_index);
            snd_pcm_info_set_subdevice(pcm_info, 0);

            for (int stream_type_i = 0; stream_type_i < array_length(stream_types); stream_type_i += 1) {
                snd_pcm_stream_t stream = stream_types[stream_type_i];
                snd_pcm_info_set_stream(pcm_info, stream);

                if ((err = snd_ctl_pcm_info(handle, pcm_info)) < 0) {
                    if (err == -ENOENT) {
                        continue;
                    } else {
                        snd_ctl_close(handle);
                        destroy(devices_info);
                        return SoundIoErrorSystemResources;
                    }
                }

                const char *device_name = snd_pcm_info_get_name(pcm_info);

                SoundIoDevice *device = create<SoundIoDevice>();
                if (!device) {
                    snd_ctl_close(handle);
                    destroy(devices_info);
                    return SoundIoErrorNoMem;
                }
                device->ref_count = 1;
                device->soundio = soundio;
                device->name = soundio_alloc_sprintf(nullptr, "hw:%d,%d", card_index, device_index);
                device->description = soundio_alloc_sprintf(nullptr, "%s %s", card_name, device_name);
                device->is_raw = true;

                if (!device->name || !device->description) {
                    soundio_device_unref(device);
                    snd_ctl_close(handle);
                    destroy(devices_info);
                    return SoundIoErrorNoMem;
                }

                SoundIoList<SoundIoDevice *> *device_list;
                if (stream == SND_PCM_STREAM_PLAYBACK) {
                    device->purpose = SoundIoDevicePurposeOutput;
                    device_list = &devices_info->output_devices;
                } else {
                    assert(stream == SND_PCM_STREAM_CAPTURE);
                    device->purpose = SoundIoDevicePurposeInput;
                    device_list = &devices_info->input_devices;
                }

                snd_pcm_chmap_query_t **maps = snd_pcm_query_chmaps_from_hw(card_index, device_index, -1, stream);
                probe_device(device, maps);

                if (device_list->append(device)) {
                    soundio_device_unref(device);
                    destroy(devices_info);
                    return SoundIoErrorNoMem;
                }
            }
        }
        snd_ctl_close(handle);
        if (snd_card_next(&card_index) < 0) {
            destroy(devices_info);
            return SoundIoErrorSystemResources;
        }
    }

    soundio_os_mutex_lock(sia->mutex);
    soundio_destroy_devices_info(sia->ready_devices_info);
    sia->ready_devices_info = devices_info;
    sia->have_devices_flag.store(true);
    soundio_os_cond_signal(sia->cond, sia->mutex);
    soundio_os_mutex_unlock(sia->mutex);
    return 0;
}

static void device_thread_run(void *arg) {
    SoundIo *soundio = (SoundIo *)arg;
    SoundIoAlsa *sia = (SoundIoAlsa *)soundio->backend_data;

    // Some systems cannot read integer variables if they are not
    // properly aligned. On other systems, incorrect alignment may
    // decrease performance. Hence, the buffer used for reading from
    // the inotify file descriptor should have the same alignment as
    // struct inotify_event.
    char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event *event;

    struct pollfd fds[2];
    fds[0].fd = sia->notify_fd;
    fds[0].events = POLLIN;

    fds[1].fd = sia->notify_pipe_fd[0];
    fds[1].events = POLLIN;

    int err;
    for (;;) {
        int poll_num = poll(fds, 2, -1);
        if (!sia->abort_flag.test_and_set())
            break;
        if (poll_num == -1) {
            if (errno == EINTR)
                continue;
            assert(errno != EFAULT);
            assert(errno != EFAULT);
            assert(errno != EINVAL);
            assert(errno == ENOMEM);
            soundio_panic("kernel ran out of polling memory");
        }
        if (poll_num <= 0)
            continue;
        bool got_rescan_event = false;
        if (fds[0].revents & POLLIN) {
            for (;;) {
                ssize_t len = read(sia->notify_fd, buf, sizeof(buf));
                if (len == -1) {
                    assert(errno != EBADF);
                    assert(errno != EFAULT);
                    assert(errno != EINVAL);
                    assert(errno != EIO);
                    assert(errno != EISDIR);
                }

                // catches EINTR and EAGAIN
                if (len <= 0)
                    break;

                // loop over all events in the buffer
                for (char *ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
                    event = (const struct inotify_event *) ptr;

                    if (!((event->mask & IN_CREATE) || (event->mask & IN_DELETE)))
                        continue;
                    if (event->mask & IN_ISDIR)
                        continue;
                    if (!event->len || event->len < 8)
                        continue;
                    if (event->name[0] != 'p' ||
                        event->name[1] != 'c' ||
                        event->name[2] != 'm')
                    {
                        continue;
                    }
                    got_rescan_event = true;
                    break;
                }
            }
        }
        if (fds[1].revents & POLLIN) {
            got_rescan_event = true;
            for (;;) {
                ssize_t len = read(sia->notify_pipe_fd[0], buf, sizeof(buf));
                if (len == -1) {
                    assert(errno != EBADF);
                    assert(errno != EFAULT);
                    assert(errno != EINVAL);
                    assert(errno != EIO);
                    assert(errno != EISDIR);
                }
                if (len <= 0)
                    break;
            }
        }
        if (got_rescan_event) {
            if ((err = refresh_devices(soundio)))
                soundio_panic("error refreshing devices: %s", soundio_error_string(err));
        }
    }
}

static void block_until_have_devices(SoundIoAlsa *sia) {
    if (sia->have_devices_flag.load())
        return;
    soundio_os_mutex_lock(sia->mutex);
    while (!sia->have_devices_flag.load())
        soundio_os_cond_wait(sia->cond, sia->mutex);
    soundio_os_mutex_unlock(sia->mutex);
}

static void flush_events(SoundIo *soundio) {
    SoundIoAlsa *sia = (SoundIoAlsa *)soundio->backend_data;
    block_until_have_devices(sia);

    bool change = false;
    SoundIoDevicesInfo *old_devices_info = nullptr;

    soundio_os_mutex_lock(sia->mutex);

    if (sia->ready_devices_info) {
        old_devices_info = soundio->safe_devices_info;
        soundio->safe_devices_info = sia->ready_devices_info;
        sia->ready_devices_info = nullptr;
        change = true;
    }

    soundio_os_mutex_unlock(sia->mutex);

    if (change)
        soundio->on_devices_change(soundio);

    soundio_destroy_devices_info(old_devices_info);
}

static void wait_events(SoundIo *soundio) {
    SoundIoAlsa *sia = (SoundIoAlsa *)soundio->backend_data;
    flush_events(soundio);
    soundio_os_mutex_lock(sia->mutex);
    soundio_os_cond_wait(sia->cond, sia->mutex);
    soundio_os_mutex_unlock(sia->mutex);
}

static void wakeup(SoundIo *soundio) {
    SoundIoAlsa *sia = (SoundIoAlsa *)soundio->backend_data;
    soundio_os_mutex_lock(sia->mutex);
    soundio_os_cond_signal(sia->cond, sia->mutex);
    soundio_os_mutex_unlock(sia->mutex);
}

static void output_device_destroy_alsa(SoundIo *soundio,
        SoundIoOutputDevice *output_device)
{
    soundio_panic("TODO");
}

static int output_device_init_alsa(SoundIo *soundio,
        SoundIoOutputDevice *output_device)
{
    soundio_panic("TODO");
}

static int output_device_start_alsa(SoundIo *soundio,
        SoundIoOutputDevice *output_device)
{
    soundio_panic("TODO");
}

static int output_device_free_count_alsa(SoundIo *soundio,
        SoundIoOutputDevice *output_device)
{
    soundio_panic("TODO");
}

static void output_device_begin_write_alsa(SoundIo *soundio,
        SoundIoOutputDevice *output_device, char **data, int *frame_count)
{
    soundio_panic("TODO");
}

static void output_device_write_alsa(SoundIo *soundio,
        SoundIoOutputDevice *output_device, char *data, int frame_count)
{
    soundio_panic("TODO");
}

static void output_device_clear_buffer_alsa(SoundIo *soundio,
        SoundIoOutputDevice *output_device)
{
    soundio_panic("TODO");
}

static int input_device_init_alsa(SoundIo *soundio,
        SoundIoInputDevice *input_device)
{
    soundio_panic("TODO");
}

static void input_device_destroy_alsa(SoundIo *soundio,
        SoundIoInputDevice *input_device)
{
    soundio_panic("TODO");
}

static int input_device_start_alsa(SoundIo *soundio,
        SoundIoInputDevice *input_device)
{
    soundio_panic("TODO");
}

static void input_device_peek_alsa(SoundIo *soundio,
        SoundIoInputDevice *input_device, const char **data, int *frame_count)
{
    soundio_panic("TODO");
}

static void input_device_drop_alsa(SoundIo *soundio,
        SoundIoInputDevice *input_device)
{
    soundio_panic("TODO");
}

static void input_device_clear_buffer_alsa(SoundIo *soundio,
        SoundIoInputDevice *input_device)
{
    soundio_panic("TODO");
}

int soundio_alsa_init(SoundIo *soundio) {
    int err;

    assert(!soundio->backend_data);
    SoundIoAlsa *sia = create<SoundIoAlsa>();
    if (!sia) {
        destroy_alsa(soundio);
        return SoundIoErrorNoMem;
    }
    soundio->backend_data = sia;
    sia->notify_fd = -1;
    sia->notify_wd = -1;
    sia->have_devices_flag.store(false);
    sia->abort_flag.test_and_set();

    sia->mutex = soundio_os_mutex_create();
    if (!sia->mutex) {
        destroy_alsa(soundio);
        return SoundIoErrorNoMem;
    }

    sia->cond = soundio_os_cond_create();
    if (!sia->cond) {
        destroy_alsa(soundio);
        return SoundIoErrorNoMem;
    }


    // set up inotify to watch /dev/snd for devices added or removed
    sia->notify_fd = inotify_init1(IN_NONBLOCK);
    if (sia->notify_fd == -1) {
        err = errno;
        assert(err != EINVAL);
        destroy_alsa(soundio);
        if (err == EMFILE || err == ENFILE) {
            return SoundIoErrorSystemResources;
        } else {
            assert(err == ENOMEM);
            return SoundIoErrorNoMem;
        }
    }

    sia->notify_wd = inotify_add_watch(sia->notify_fd, "/dev/snd", IN_CREATE | IN_DELETE);
    if (sia->notify_wd == -1) {
        err = errno;
        assert(err != EACCES);
        assert(err != EBADF);
        assert(err != EFAULT);
        assert(err != EINVAL);
        assert(err != ENAMETOOLONG);
        assert(err != ENOENT);
        destroy_alsa(soundio);
        if (err == ENOSPC) {
            return SoundIoErrorSystemResources;
        } else {
            assert(err == ENOMEM);
            return SoundIoErrorNoMem;
        }
    }

    if (pipe2(sia->notify_pipe_fd, O_NONBLOCK)) {
        assert(errno != EFAULT);
        assert(errno != EINVAL);
        assert(errno == EMFILE || errno == ENFILE);
        return SoundIoErrorSystemResources;
    }

    wakeup_device_poll(sia);

    if ((err = soundio_os_thread_create(device_thread_run, soundio, false, &sia->thread))) {
        destroy_alsa(soundio);
        return err;
    }

    soundio->destroy = destroy_alsa;
    soundio->flush_events = flush_events;
    soundio->wait_events = wait_events;
    soundio->wakeup = wakeup;

    soundio->output_device_init = output_device_init_alsa;
    soundio->output_device_destroy = output_device_destroy_alsa;
    soundio->output_device_start = output_device_start_alsa;
    soundio->output_device_free_count = output_device_free_count_alsa;
    soundio->output_device_begin_write = output_device_begin_write_alsa;
    soundio->output_device_write = output_device_write_alsa;
    soundio->output_device_clear_buffer = output_device_clear_buffer_alsa;

    soundio->input_device_init = input_device_init_alsa;
    soundio->input_device_destroy = input_device_destroy_alsa;
    soundio->input_device_start = input_device_start_alsa;
    soundio->input_device_peek = input_device_peek_alsa;
    soundio->input_device_drop = input_device_drop_alsa;
    soundio->input_device_clear_buffer = input_device_clear_buffer_alsa;

    return 0;
}
