/*
 * BlueALSA - aplay.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"

struct pcm_worker {
	pthread_t thread;
	/* used BlueALSA PCM device */
	struct ba_pcm ba_pcm;
	/* file descriptor of PCM FIFO */
	int ba_pcm_fd;
	/* file descriptor of PCM control */
	int ba_pcm_ctrl_fd;
	/* opened playback PCM device */
	snd_pcm_t *pcm;
	/* if true, playback is active */
	bool active;
	/* human-readable BT address */
	char addr[18];
};

static unsigned int verbose = 0;
static const char *device = "default";
static bool ba_profile_a2dp = true;
static bool ba_addr_any = false;
static bdaddr_t *ba_addrs = NULL;
static size_t ba_addrs_count = 0;
static unsigned int pcm_buffer_time = 500000;
static unsigned int pcm_period_time = 100000;
static bool pcm_mixer = true;

static struct ba_dbus_ctx dbus_ctx;
static char dbus_ba_service[32] = BLUEALSA_SERVICE;

static struct ba_pcm *ba_pcms = NULL;
static size_t ba_pcms_count = 0;

static pthread_rwlock_t workers_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct pcm_worker *workers = NULL;
static size_t workers_count = 0;
static size_t workers_size = 0;

static bool main_loop_on = true;
static void main_loop_stop(int sig) {
	/* Call to this handler restores the default action, so on the
	 * second call the program will be forcefully terminated. */

	struct sigaction sigact = { .sa_handler = SIG_DFL };
	sigaction(sig, &sigact, NULL);

	main_loop_on = false;
}

static int pcm_set_hw_params(snd_pcm_t *pcm, int channels, int rate,
		unsigned int *buffer_time, unsigned int *period_time, char **msg) {

	const snd_pcm_access_t access = SND_PCM_ACCESS_RW_INTERLEAVED;
	const snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
	snd_pcm_hw_params_t *params;
	char buf[256];
	int dir;
	int err;

	snd_pcm_hw_params_alloca(&params);

	if ((err = snd_pcm_hw_params_any(pcm, params)) < 0) {
		snprintf(buf, sizeof(buf), "Set all possible ranges: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_access(pcm, params, access)) != 0) {
		snprintf(buf, sizeof(buf), "Set assess type: %s: %s", snd_strerror(err), snd_pcm_access_name(access));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_format(pcm, params, format)) != 0) {
		snprintf(buf, sizeof(buf), "Set format: %s: %s", snd_strerror(err), snd_pcm_format_name(format));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_channels(pcm, params, channels)) != 0) {
		snprintf(buf, sizeof(buf), "Set channels: %s: %d", snd_strerror(err), channels);
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_rate(pcm, params, rate, 0)) != 0) {
		snprintf(buf, sizeof(buf), "Set sampling rate: %s: %d", snd_strerror(err), rate);
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_buffer_time_near(pcm, params, buffer_time, &dir)) != 0) {
		snprintf(buf, sizeof(buf), "Set buffer time: %s: %u", snd_strerror(err), *buffer_time);
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_period_time_near(pcm, params, period_time, &dir)) != 0) {
		snprintf(buf, sizeof(buf), "Set period time: %s: %u", snd_strerror(err), *period_time);
		goto fail;
	}
	if ((err = snd_pcm_hw_params(pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "%s", snd_strerror(err));
		goto fail;
	}

	return 0;

fail:
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

static int pcm_set_sw_params(snd_pcm_t *pcm, snd_pcm_uframes_t buffer_size,
		snd_pcm_uframes_t period_size, char **msg) {

	snd_pcm_sw_params_t *params;
	char buf[256];
	int err;

	snd_pcm_sw_params_alloca(&params);

	if ((err = snd_pcm_sw_params_current(pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "Get current params: %s", snd_strerror(err));
		goto fail;
	}

	/* start the transfer when the buffer is full (or almost full) */
	snd_pcm_uframes_t threshold = (buffer_size / period_size) * period_size;
	if ((err = snd_pcm_sw_params_set_start_threshold(pcm, params, threshold)) != 0) {
		snprintf(buf, sizeof(buf), "Set start threshold: %s: %lu", snd_strerror(err), threshold);
		goto fail;
	}

	/* allow the transfer when at least period_size samples can be processed */
	if ((err = snd_pcm_sw_params_set_avail_min(pcm, params, period_size)) != 0) {
		snprintf(buf, sizeof(buf), "Set avail min: %s: %lu", snd_strerror(err), period_size);
		goto fail;
	}

	if ((err = snd_pcm_sw_params(pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "%s", snd_strerror(err));
		goto fail;
	}

	return 0;

fail:
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

static int pcm_open(snd_pcm_t **pcm, int channels, int rate,
		unsigned int *buffer_time, unsigned int *period_time, char **msg) {

	snd_pcm_t *_pcm = NULL;
	char buf[256];
	char *tmp;
	int err;

	if ((err = snd_pcm_open(&_pcm, device, SND_PCM_STREAM_PLAYBACK, 0)) != 0) {
		snprintf(buf, sizeof(buf), "%s", snd_strerror(err));
		goto fail;
	}

	if ((err = pcm_set_hw_params(_pcm, channels, rate, buffer_time, period_time, &tmp)) != 0) {
		snprintf(buf, sizeof(buf), "Set HW params: %s", tmp);
		goto fail;
	}

	snd_pcm_uframes_t buffer_size, period_size;
	if ((err = snd_pcm_get_params(_pcm, &buffer_size, &period_size)) != 0) {
		snprintf(buf, sizeof(buf), "Get params: %s", snd_strerror(err));
		goto fail;
	}

	if ((err = pcm_set_sw_params(_pcm, buffer_size, period_size, &tmp)) != 0) {
		snprintf(buf, sizeof(buf), "Set SW params: %s", tmp);
		goto fail;
	}

	if ((err = snd_pcm_prepare(_pcm)) != 0) {
		snprintf(buf, sizeof(buf), "Prepare: %s", snd_strerror(err));
		goto fail;
	}

	*pcm = _pcm;
	return 0;

fail:
	if (_pcm != NULL)
		snd_pcm_close(_pcm);
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

static struct ba_pcm *get_ba_pcm(const char *path) {

	size_t i;

	for (i = 0; i < ba_pcms_count; i++)
		if (strcmp(ba_pcms[i].pcm_path, path) == 0)
			return &ba_pcms[i];

	return NULL;
}

static struct pcm_worker *get_active_worker(void) {

	struct pcm_worker *w = NULL;
	size_t i;

	pthread_rwlock_rdlock(&workers_lock);

	for (i = 0; i < workers_count; i++)
		if (workers[i].active) {
			w = &workers[i];
			break;
		}

	pthread_rwlock_unlock(&workers_lock);

	return w;
}

static int pause_device_player(const struct ba_pcm *ba_pcm) {

	DBusMessage *msg = NULL, *rep = NULL;
	DBusError err = DBUS_ERROR_INIT;
	char path[160];
	int ret = 0;

	snprintf(path, sizeof(path), "%s/player0", ba_pcm->device_path);
	msg = dbus_message_new_method_call("org.bluez", path, "org.bluez.MediaPlayer1", "Pause");

	if ((rep = dbus_connection_send_with_reply_and_block(dbus_ctx.conn, msg,
					DBUS_TIMEOUT_USE_DEFAULT, &err)) == NULL) {
		warn("Couldn't pause player: %s", err.message);
		goto fail;
	}

	debug("Requested playback pause");
	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);

	return ret;
}

static void pcm_worker_routine_exit(struct pcm_worker *worker) {
	if (worker->ba_pcm_fd != -1) {
		close(worker->ba_pcm_fd);
		worker->ba_pcm_fd = -1;
	}
	if (worker->ba_pcm_ctrl_fd != -1) {
		close(worker->ba_pcm_ctrl_fd);
		worker->ba_pcm_ctrl_fd = -1;
	}
	if (worker->pcm != NULL) {
		snd_pcm_close(worker->pcm);
		worker->pcm = NULL;
	}
	debug("Exiting PCM worker %s", worker->addr);
}

static void *pcm_worker_routine(void *arg) {
	struct pcm_worker *w = (struct pcm_worker *)arg;

	size_t pcm_1s_samples = w->ba_pcm.sampling * w->ba_pcm.channels;
	ffb_int16_t buffer = { 0 };

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	pthread_cleanup_push(PTHREAD_CLEANUP(pcm_worker_routine_exit), w);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_int16_free), &buffer);

	/* create buffer big enough to hold 100 ms of PCM data */
	if (ffb_init(&buffer, pcm_1s_samples / 10) == NULL) {
		error("Couldn't create PCM buffer: %s", strerror(ENOMEM));
		goto fail;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_open(&dbus_ctx, w->ba_pcm.pcm_path, BA_PCM_FLAG_SINK,
				&w->ba_pcm_fd, &w->ba_pcm_ctrl_fd, &err)) {
		error("Couldn't open PCM: %s", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	/* Initialize the max read length to 10 ms. Later, when the PCM device
	 * will be opened, this value will be adjusted to one period size. */
	size_t pcm_max_read_len = pcm_1s_samples / 100;
	size_t pcm_open_retries = 0;

	/* These variables determine how and when the pause command will be send
	 * to the device player. In order not to flood BT connection with AVRCP
	 * packets, we are going to send pause command every 0.5 second. */
	size_t pause_threshold = pcm_1s_samples / 2 * sizeof(int16_t);
	size_t pause_counter = 0;
	size_t pause_bytes = 0;

	struct pollfd pfds[] = {{ w->ba_pcm_fd, POLLIN, 0 }};
	int timeout = -1;

	debug("Starting PCM loop");
	while (main_loop_on) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		ssize_t ret;

		/* Reading from the FIFO won't block unless there is an open connection
		 * on the writing side. However, the server does not open PCM FIFO until
		 * a transport is created. With the A2DP, the transport is created when
		 * some clients (BT device) requests audio transfer. */
		switch (poll(pfds, ARRAYSIZE(pfds), timeout)) {
		case -1:
			if (errno == EINTR)
				continue;
			error("PCM FIFO poll error: %s", strerror(errno));
			goto fail;
		case 0:
			debug("Device marked as inactive: %s", w->addr);
			pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
			pcm_max_read_len = pcm_1s_samples / 100;
			pause_counter = pause_bytes = 0;
			ffb_rewind(&buffer);
			if (w->pcm != NULL) {
				snd_pcm_close(w->pcm);
				w->pcm = NULL;
			}
			w->active = false;
			timeout = -1;
			continue;
		}

		/* FIFO has been terminated on the writing side */
		if (pfds[0].revents & POLLHUP)
			break;

		#define MIN(a,b) a < b ? a : b
		size_t _in = MIN(pcm_max_read_len, ffb_len_in(&buffer));
		if ((ret = read(w->ba_pcm_fd, buffer.tail, _in * sizeof(int16_t))) == -1) {
			if (errno == EINTR)
				continue;
			error("PCM FIFO read error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/* If PCM mixer is disabled, check whether we should play audio. */
		if (!pcm_mixer) {
			struct pcm_worker *worker = get_active_worker();
			if (worker != NULL && worker != w) {
				if (pause_counter < 5 && (pause_bytes += ret) > pause_threshold) {
					if (pause_device_player(&w->ba_pcm) == -1)
						/* pause command does not work, stop further requests */
						pause_counter = 5;
					pause_counter++;
					pause_bytes = 0;
					timeout = 100;
				}
				continue;
			}
		}

		if (w->pcm == NULL) {

			unsigned int buffer_time = pcm_buffer_time;
			unsigned int period_time = pcm_period_time;
			snd_pcm_uframes_t buffer_size;
			snd_pcm_uframes_t period_size;
			char *tmp;

			/* After PCM open failure wait one second before retry. This can not be
			 * done with a single sleep() call, because we have to drain PCM FIFO. */
			if (pcm_open_retries++ % 20 != 0) {
				usleep(50000);
				continue;
			}

			if (pcm_open(&w->pcm, w->ba_pcm.channels, w->ba_pcm.sampling,
						&buffer_time, &period_time, &tmp) != 0) {
				warn("Couldn't open PCM: %s", tmp);
				pcm_max_read_len = buffer.size;
				usleep(50000);
				free(tmp);
				continue;
			}

			snd_pcm_get_params(w->pcm, &buffer_size, &period_size);
			pcm_max_read_len = period_size * w->ba_pcm.channels;
			pcm_open_retries = 0;

			if (verbose >= 2) {
				printf("Used configuration for %s:\n"
						"  PCM buffer time: %u us (%zu bytes)\n"
						"  PCM period time: %u us (%zu bytes)\n"
						"  Sampling rate: %u Hz\n"
						"  Channels: %u\n",
						w->addr,
						buffer_time, snd_pcm_frames_to_bytes(w->pcm, buffer_size),
						period_time, snd_pcm_frames_to_bytes(w->pcm, period_size),
						w->ba_pcm.sampling, w->ba_pcm.channels);
			}

		}

		/* mark device as active and set timeout to 500ms */
		w->active = true;
		timeout = 500;

		/* calculate the overall number of frames in the buffer */
		ffb_seek(&buffer, ret / sizeof(*buffer.data));
		snd_pcm_sframes_t frames = ffb_len_out(&buffer) / w->ba_pcm.channels;

		if ((frames = snd_pcm_writei(w->pcm, buffer.data, frames)) < 0)
			switch (-frames) {
			case EPIPE:
				debug("An underrun has occurred");
				snd_pcm_prepare(w->pcm);
				usleep(50000);
				frames = 0;
				break;
			default:
				error("Couldn't write to PCM: %s", snd_strerror(frames));
				goto fail;
			}

		/* move leftovers to the beginning and reposition tail */
		ffb_shift(&buffer, frames * w->ba_pcm.channels);

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

static int supervise_pcm_worker_start(struct ba_pcm *ba_pcm) {

	size_t i;
	for (i = 0; i < workers_count; i++)
		if (strcmp(workers[i].ba_pcm.pcm_path, ba_pcm->pcm_path) == 0)
			return 0;

	pthread_rwlock_wrlock(&workers_lock);

	workers_count++;
	if (workers_size < workers_count) {
		workers_size += 4;  /* coarse-grained realloc */
		if ((workers = realloc(workers, sizeof(*workers) * workers_size)) == NULL) {
			error("Couldn't (re)allocate memory for PCM workers: %s", strerror(ENOMEM));
			pthread_rwlock_unlock(&workers_lock);
			return -1;
		}
	}

	struct pcm_worker *worker = &workers[workers_count - 1];
	memcpy(&worker->ba_pcm, ba_pcm, sizeof(worker->ba_pcm));
	ba2str(&worker->ba_pcm.addr, worker->addr);
	worker->active = false;
	worker->ba_pcm_fd = -1;
	worker->ba_pcm_ctrl_fd = -1;
	worker->pcm = NULL;

	pthread_rwlock_unlock(&workers_lock);

	debug("Creating PCM worker %s", worker->addr);

	if ((errno = pthread_create(&worker->thread, NULL, pcm_worker_routine, worker)) != 0) {
		error("Couldn't create PCM worker %s: %s", worker->addr, strerror(errno));
		workers_count--;
		return -1;
	}

	return 0;
}

static int supervise_pcm_worker_stop(struct ba_pcm *ba_pcm) {

	size_t i;
	for (i = 0; i < workers_count; i++)
		if (strcmp(workers[i].ba_pcm.pcm_path, ba_pcm->pcm_path) == 0) {
			pthread_rwlock_wrlock(&workers_lock);
			pthread_cancel(workers[i].thread);
			pthread_join(workers[i].thread, NULL);
			memcpy(&workers[i], &workers[--workers_count], sizeof(workers[i]));
			pthread_rwlock_unlock(&workers_lock);
		}

	return 0;
}

static int supervise_pcm_worker(struct ba_pcm *ba_pcm) {

	if (ba_pcm == NULL)
		return -1;

	if (!(ba_pcm->flags & BA_PCM_FLAG_SINK))
		goto stop;

	if ((ba_profile_a2dp && !(ba_pcm->flags & BA_PCM_FLAG_PROFILE_A2DP)) ||
			(!ba_profile_a2dp && !(ba_pcm->flags & BA_PCM_FLAG_PROFILE_SCO)))
		goto stop;

	/* check whether SCO has selected codec */
	if (ba_pcm->flags & BA_PCM_FLAG_PROFILE_SCO &&
			ba_pcm->codec == 0) {
		debug("Skipping SCO with codec not selected");
		goto stop;
	}

	if (ba_addr_any)
		goto start;

	size_t i;
	for (i = 0; i < ba_addrs_count; i++)
		if (bacmp(&ba_addrs[i], &ba_pcm->addr) == 0)
			goto start;

stop:
	return supervise_pcm_worker_stop(ba_pcm);
start:
	return supervise_pcm_worker_start(ba_pcm);
}

static DBusHandlerResult dbus_signal_handler(DBusConnection *conn, DBusMessage *message, void *data) {
	(void)conn;
	(void)data;

	const char *path = dbus_message_get_path(message);
	const char *interface = dbus_message_get_interface(message);
	const char *signal = dbus_message_get_member(message);

	DBusMessageIter iter;
	struct ba_pcm *pcm;

	if (strcmp(interface, BLUEALSA_INTERFACE_MANAGER) == 0) {

		if (strcmp(signal, "PCMAdded") == 0) {
			if ((ba_pcms = realloc(ba_pcms, (ba_pcms_count + 1) * sizeof(*ba_pcms))) == NULL) {
				error("Couldn't add new PCM: %s", strerror(ENOMEM));
				goto fail;
			}
			if (!dbus_message_iter_init(message, &iter) ||
					!bluealsa_dbus_message_iter_get_pcm(&iter, NULL, &ba_pcms[ba_pcms_count])) {
				error("Couldn't add new PCM: %s", "Invalid signal signature");
				goto fail;
			}
			supervise_pcm_worker(&ba_pcms[ba_pcms_count++]);
			return DBUS_HANDLER_RESULT_HANDLED;
		}

		if (strcmp(signal, "PCMRemoved") == 0) {
			if (!dbus_message_iter_init(message, &iter) ||
					dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
				error("Couldn't remove PCM: %s", "Invalid signal signature");
				goto fail;
			}
			dbus_message_iter_get_basic(&iter, &path);
			supervise_pcm_worker_stop(get_ba_pcm(path));
			return DBUS_HANDLER_RESULT_HANDLED;
		}

	}

	if (strcmp(interface, DBUS_INTERFACE_PROPERTIES) == 0) {
		if ((pcm = get_ba_pcm(path)) == NULL)
			goto fail;
		if (!dbus_message_iter_init(message, &iter) ||
				dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
			error("Couldn't update PCM: %s", "Invalid signal signature");
			goto fail;
		}
		dbus_message_iter_get_basic(&iter, &interface);
		dbus_message_iter_next(&iter);
		if (!bluealsa_dbus_message_iter_get_pcm_props(&iter, NULL, pcm))
			goto fail;
		supervise_pcm_worker(pcm);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hVvb:d:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "dbus", required_argument, NULL, 'b' },
		{ "pcm", required_argument, NULL, 'd' },
		{ "pcm-buffer-time", required_argument, NULL, 3 },
		{ "pcm-period-time", required_argument, NULL, 4 },
		{ "profile-a2dp", no_argument, NULL, 1 },
		{ "profile-sco", no_argument, NULL, 2 },
		{ "single-audio", no_argument, NULL, 5 },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
usage:
			printf("Usage:\n"
					"  %s [OPTION]... <BT-ADDR>...\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -V, --version\t\tprint version and exit\n"
					"  -v, --verbose\t\tmake output more verbose\n"
					"  -b, --dbus=NAME\tBlueALSA service name suffix\n"
					"  -d, --pcm=NAME\tPCM device to use\n"
					"  --pcm-buffer-time=INT\tPCM buffer time\n"
					"  --pcm-period-time=INT\tPCM period time\n"
					"  --profile-a2dp\tuse A2DP profile\n"
					"  --profile-sco\t\tuse SCO profile\n"
					"  --single-audio\tsingle audio mode\n"
					"\nNote:\n"
					"If one wants to receive audio from more than one Bluetooth device, it is\n"
					"possible to specify more than one MAC address. By specifying any/empty MAC\n"
					"address (00:00:00:00:00:00), one will allow connections from any Bluetooth\n"
					"device.\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'v' /* --verbose */ :
			verbose++;
			break;

		case 'b' /* --dbus=NAME */ :
			snprintf(dbus_ba_service, sizeof(dbus_ba_service), BLUEALSA_SERVICE ".%s", optarg);
			break;
		case 'd' /* --pcm=NAME */ :
			device = optarg;
			break;

		case 1 /* --profile-a2dp */ :
			ba_profile_a2dp = true;
			break;
		case 2 /* --profile-sco */ :
			ba_profile_a2dp = false;
			break;

		case 3 /* --pcm-buffer-time=INT */ :
			pcm_buffer_time = atoi(optarg);
			break;
		case 4 /* --pcm-period-time=INT */ :
			pcm_period_time = atoi(optarg);
			break;

		case 5 /* --single-audio */ :
			pcm_mixer = false;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if (optind == argc)
		goto usage;

	log_open(argv[0], false, false);

	size_t i;

	ba_addrs_count = argc - optind;
	if ((ba_addrs = malloc(sizeof(*ba_addrs) * ba_addrs_count)) == NULL) {
		error("Couldn't allocate memory for BT addresses");
		return EXIT_FAILURE;
	}
	for (i = 0; i < ba_addrs_count; i++) {
		if (str2ba(argv[i + optind], &ba_addrs[i]) != 0) {
			error("Invalid BT device address: %s", argv[i + optind]);
			return EXIT_FAILURE;
		}
		if (bacmp(&ba_addrs[i], BDADDR_ANY) == 0)
			ba_addr_any = true;
	}

	if (verbose >= 1) {

		char *ba_str = malloc(19 * ba_addrs_count + 1);
		char *tmp = ba_str;
		size_t i;

		for (i = 0; i < ba_addrs_count; i++, tmp += 19)
			ba2str(&ba_addrs[i], stpcpy(tmp, ", "));

		printf("Selected configuration:\n"
				"  BlueALSA service: %s\n"
				"  PCM device: %s\n"
				"  PCM buffer time: %u us\n"
				"  PCM period time: %u us\n"
				"  Bluetooth device(s): %s\n"
				"  Profile: %s\n",
				dbus_ba_service, device, pcm_buffer_time, pcm_period_time,
				ba_addr_any ? "ANY" : &ba_str[2],
				ba_profile_a2dp ? "A2DP" : "SCO");

		free(ba_str);
	}

	dbus_threads_init_default();

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_connection_ctx_init(&dbus_ctx, dbus_ba_service, &err)) {
		error("Couldn't initialize D-Bus context: %s", err.message);
		return EXIT_FAILURE;
	}

	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, BLUEALSA_INTERFACE_MANAGER, "PCMAdded", NULL);
	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, BLUEALSA_INTERFACE_MANAGER, "PCMRemoved", NULL);
	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged",
			"arg0='"BLUEALSA_INTERFACE_PCM"'");

	if (!dbus_connection_add_filter(dbus_ctx.conn, dbus_signal_handler, NULL, NULL)) {
		error("Couldn't add D-Bus filter: %s", err.message);
		return EXIT_FAILURE;
	}

	if (!bluealsa_dbus_get_pcms(&dbus_ctx, &ba_pcms, &ba_pcms_count, &err))
		warn("Couldn't get BlueALSA PCM list: %s", err.message);

	for (i = 0; i < ba_pcms_count; i++)
		supervise_pcm_worker(&ba_pcms[i]);

	struct sigaction sigact = { .sa_handler = main_loop_stop };
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	debug("Starting main loop");
	while (main_loop_on) {

		struct pollfd pfds[10];
		nfds_t pfds_len = ARRAYSIZE(pfds);

		if (!bluealsa_dbus_connection_poll_fds(&dbus_ctx, pfds, &pfds_len)) {
			error("Couldn't get D-Bus connection file descriptors");
			return EXIT_FAILURE;
		}

		if (poll(pfds, pfds_len, -1) == -1 &&
				errno == EINTR)
			continue;

		if (bluealsa_dbus_connection_poll_dispatch(&dbus_ctx, pfds, pfds_len))
			while (dbus_connection_dispatch(dbus_ctx.conn) == DBUS_DISPATCH_DATA_REMAINS)
				continue;

	}

	return EXIT_SUCCESS;
}
