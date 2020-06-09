// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Server process functions.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc_server
 */

#include "ipc_server.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_config_have.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_debug.h"

#include "ipc_server_utils.h"

#include "main/comp_compositor.h"
#include "main/comp_renderer.h"

#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#ifdef XRT_HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif


/*
 *
 * Defines and helpers.
 *
 */

#define IPC_MAX_CLIENTS 8

DEBUG_GET_ONCE_BOOL_OPTION(exit_on_disconnect, "IPC_EXIT_ON_DISCONNECT", false)


/*
 *
 * Static functions.
 *
 */

static void
teardown_all(struct ipc_server *s)
{
	u_var_remove_root(s);

	ipc_server_wait_free(&s->iw);

	xrt_comp_destroy(&s->xc);

	for (size_t i = 0; i < IPC_SERVER_NUM_XDEVS; i++) {
		xrt_device_destroy(&s->xdevs[i]);
	}

	xrt_instance_destroy(&s->xinst);

	if (s->listen_socket > 0) {
		// Close socket on exit
		close(s->listen_socket);
		s->listen_socket = -1;
		if (!s->launched_by_socket && s->socket_filename) {
			// Unlink it too, but only if we bound it.
			unlink(s->socket_filename);
			free(s->socket_filename);
			s->socket_filename = NULL;
		}
	}
}

static int
init_tracking_origins(struct ipc_server *s)
{
	for (size_t i = 0; i < IPC_SERVER_NUM_XDEVS; i++) {
		if (s->xdevs[i] == NULL) {
			continue;
		}

		struct xrt_device *xdev = s->xdevs[i];
		struct xrt_tracking_origin *xtrack = xdev->tracking_origin;
		assert(xtrack != NULL);
		size_t index = 0;

		for (; index < IPC_SERVER_NUM_XDEVS; index++) {
			if (s->xtracks[index] == NULL) {
				s->xtracks[index] = xtrack;
				break;
			} else if (s->xtracks[index] == xtrack) {
				break;
			}
		}
	}

	return 0;
}

static int
init_shm(struct ipc_server *s)
{
	const size_t size = sizeof(struct ipc_shared_memory);

	int fd = shm_open("/monado_shm", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		return -1;
	}

	if (ftruncate(fd, size) < 0) {
		close(fd);
		return -1;
	}

	const int access = PROT_READ | PROT_WRITE;
	const int flags = MAP_SHARED;
	s->ism = mmap(NULL, size, access, flags, fd, 0);
	if (s->ism == NULL) {
		close(fd);
		return -1;
	}

	// we have a filehandle, we will pass this to
	// our client rather than access via filesystem
	shm_unlink("/monado_shm");

	s->ism_fd = fd;


	/*
	 *
	 * Setup the shared memory state.
	 *
	 */

	uint32_t count = 0;
	struct ipc_shared_memory *ism = s->ism;

	count = 0;
	for (size_t i = 0; i < IPC_SERVER_NUM_XDEVS; i++) {
		struct xrt_tracking_origin *xtrack = s->xtracks[i];
		if (xtrack == NULL) {
			continue;
		}

		// The position of the tracking origin matches that in the
		// servers memory.
		assert(i < IPC_SHARED_MAX_DEVICES);

		struct ipc_shared_tracking_origin *itrack =
		    &ism->itracks[count++];
		memcpy(itrack->name, xtrack->name, sizeof(itrack->name));
		itrack->type = xtrack->type;
		itrack->offset = xtrack->offset;
	}

	ism->num_itracks = count;

	count = 0;
	uint32_t input_index = 0;
	uint32_t output_index = 0;
	for (size_t i = 0; i < IPC_SERVER_NUM_XDEVS; i++) {
		struct xrt_device *xdev = s->xdevs[i];
		if (xdev == NULL) {
			continue;
		}

		struct ipc_shared_device *idev = &ism->idevs[count++];

		idev->name = xdev->name;
		memcpy(idev->str, xdev->str, sizeof(idev->str));

		// Is this a HMD?
		if (xdev->hmd != NULL) {
			ism->hmd.views[0].display.w_pixels =
			    xdev->hmd->views[0].display.w_pixels;
			ism->hmd.views[0].display.h_pixels =
			    xdev->hmd->views[0].display.h_pixels;
			ism->hmd.views[0].fov = xdev->hmd->views[0].fov;
			ism->hmd.views[1].display.w_pixels =
			    xdev->hmd->views[1].display.w_pixels;
			ism->hmd.views[1].display.h_pixels =
			    xdev->hmd->views[1].display.h_pixels;
			ism->hmd.views[1].fov = xdev->hmd->views[1].fov;
		}

		// Setup the tracking origin.
		idev->tracking_origin_index = (uint32_t)-1;
		for (size_t k = 0; k < IPC_SERVER_NUM_XDEVS; k++) {
			if (xdev->tracking_origin != s->xtracks[k]) {
				continue;
			}

			idev->tracking_origin_index = k;
			break;
		}

		assert(idev->tracking_origin_index != (uint32_t)-1);

		// Initial update.
		xrt_device_update_inputs(xdev);

		// Copy the initial state and also count the number in inputs.
		size_t input_start = input_index;
		for (size_t k = 0; k < xdev->num_inputs; k++) {
			ism->inputs[input_index++] = xdev->inputs[k];
		}

		// Setup the 'offsets' and number of inputs.
		if (input_start != input_index) {
			idev->num_inputs = input_index - input_start;
			idev->first_input_index = input_start;
		}

		// Copy the initial state and also count the number in outputs.
		size_t output_start = output_index;
		for (size_t k = 0; k < xdev->num_outputs; k++) {
			ism->outputs[output_index++] = xdev->outputs[k];
		}

		// Setup the 'offsets' and number of outputs.
		if (output_start != output_index) {
			idev->num_outputs = output_index - output_start;
			idev->first_output_index = output_start;
		}
	}

	// Finally tell the client how many devices we have.
	s->ism->num_idevs = count;

	sem_init(&s->ism->wait_frame.sem, true, 0);

	return 0;
}

static int
get_systemd_socket(struct ipc_server *s, int *out_fd)
{
#ifdef XRT_HAVE_SYSTEMD
	// We may have been launched with socket activation
	int num_fds = sd_listen_fds(0);
	if (num_fds > 1) {
		fprintf(stderr,
		        "Too many file descriptors passed by systemd.\n");
		return -1;
	}
	if (num_fds == 1) {
		*out_fd = SD_LISTEN_FDS_START + 0;
		s->launched_by_socket = true;
		printf("Got existing socket from systemd.\n");
	}
#endif
	return 0;
}

static int
create_listen_socket(struct ipc_server *s, int *out_fd)
{
	// no fd provided
	struct sockaddr_un addr;
	int fd, ret;

	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "Message Socket Create Error!\n");
		return fd;
	}

	memset(&addr, 0, sizeof(addr));

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, IPC_MSG_SOCK_FILE);

	ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		fprintf(stderr,
		        "ERROR: Could not bind socket to path %s: is the "
		        "service running already?\n",
		        IPC_MSG_SOCK_FILE);
#ifdef XRT_HAVE_SYSTEMD
		fprintf(stderr,
		        "Or, is the systemd unit monado.socket or "
		        "monado-dev.socket active?\n");
#endif
		close(fd);
		return ret;
	}
	// Save for later
	s->socket_filename = strdup(IPC_MSG_SOCK_FILE);

	ret = listen(fd, IPC_MAX_CLIENTS);
	if (ret < 0) {
		close(fd);
		return ret;
	}
	printf("Created listening socket.\n");
	*out_fd = fd;
	return 0;
}

static int
init_listen_socket(struct ipc_server *s)
{
	int fd = -1, ret;
	s->listen_socket = -1;

	ret = get_systemd_socket(s, &fd);
	if (ret < 0) {
		return ret;
	}

	if (fd == -1) {
		ret = create_listen_socket(s, &fd);
		if (ret < 0) {
			return ret;
		}
	}
	// All ok!
	s->listen_socket = fd;
	printf("Listening socket is fd %d\n", s->listen_socket);

	return fd;
}

static int
init_epoll(struct ipc_server *s)
{
	int ret = epoll_create1(EPOLL_CLOEXEC);
	if (ret < 0) {
		return ret;
	}

	s->epoll_fd = ret;

	struct epoll_event ev = {0};

	if (!s->launched_by_socket) {
		// Can't do this when launched by systemd socket activation by
		// default
		ev.events = EPOLLIN;
		ev.data.fd = 0; // stdin
		ret = epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, 0, &ev);
		if (ret < 0) {
			fprintf(stderr, "ERROR: epoll_ctl(stdin) failed '%i'\n",
			        ret);
			return ret;
		}
	}

	ev.events = EPOLLIN;
	ev.data.fd = s->listen_socket;
	ret = epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->listen_socket, &ev);
	if (ret < 0) {
		fprintf(stderr, "ERROR: epoll_ctl(listen_socket) failed '%i'\n",
		        ret);
		return ret;
	}

	return 0;
}

static int
init_all(struct ipc_server *s)
{
	// Yes we should be running.
	s->running = true;
	s->exit_on_disconnect = debug_get_bool_option_exit_on_disconnect();

	int ret = xrt_instance_create(&s->xinst);
	if (ret < 0) {
		teardown_all(s);
		return ret;
	}

	ret = xrt_instance_select(s->xinst, s->xdevs, IPC_SERVER_NUM_XDEVS);
	if (ret < 0) {
		teardown_all(s);
		return ret;
	}

	if (s->xdevs[0] == NULL) {
		teardown_all(s);
		return -1;
	}

	ret = init_tracking_origins(s);
	if (ret < 0) {
		teardown_all(s);
		return -1;
	}

	ret = xrt_instance_create_fd_compositor(s->xinst, s->xdevs[0], false,
	                                        &s->xcfd);
	if (ret < 0) {
		teardown_all(s);
		return ret;
	}

	ret = init_shm(s);
	if (ret < 0) {
		teardown_all(s);
		return ret;
	}

	ret = init_listen_socket(s);
	if (ret < 0) {
		teardown_all(s);
		return ret;
	}

	ret = init_epoll(s);
	if (ret < 0) {
		teardown_all(s);
		return ret;
	}

	ret = ipc_server_wait_alloc(s, &s->iw);
	if (ret < 0) {
		teardown_all(s);
		return ret;
	}

	// Easier to use.
	s->xc = &s->xcfd->base;

	u_var_add_root(s, "IPC Server", false);
	u_var_add_bool(s, &s->print_debug, "print.debug");
	u_var_add_bool(s, &s->print_spew, "print.spew");
	u_var_add_bool(s, &s->exit_on_disconnect, "exit_on_disconnect");
	u_var_add_bool(s, (void *)&s->running, "running");

	return 0;
}

static void
handle_listen(struct ipc_server *vs)
{
	int ret = accept(vs->listen_socket, NULL, NULL);
	if (ret < 0) {
		fprintf(stderr, "ERROR: accept '%i'\n", ret);
		vs->running = false;
	}

	volatile struct ipc_client_state *cs = &vs->thread_state;

	// The return is the new fd;
	int fd = ret;

	if (vs->thread_started && !vs->thread_stopping) {
		fprintf(stderr, "ERROR: Client already connected!\n");
		close(fd);
		return;
	}

	if (vs->thread_stopping) {
		os_thread_join((struct os_thread *)&vs->thread);
		os_thread_destroy((struct os_thread *)&vs->thread);
		vs->thread_stopping = false;
	}

	vs->thread_started = true;
	cs->ipc_socket_fd = fd;
	os_thread_start((struct os_thread *)&vs->thread,
	                ipc_server_client_thread, (void *)cs);
}

#define NUM_POLL_EVENTS 8
#define NO_SLEEP 0

static void
check_epoll(struct ipc_server *vs)
{
	int epoll_fd = vs->epoll_fd;

	struct epoll_event events[NUM_POLL_EVENTS] = {0};

	// No sleeping, returns immediately.
	int ret = epoll_wait(epoll_fd, events, NUM_POLL_EVENTS, NO_SLEEP);
	if (ret < 0) {
		fprintf(stderr, "EPOLL ERROR! \"%i\"\n", ret);
		vs->running = false;
		return;
	}

	for (int i = 0; i < ret; i++) {
		// If we get data on stdin, stop.
		if (events[i].data.fd == 0) {
			vs->running = false;
			return;
		}

		// Somebody new at the door.
		if (events[i].data.fd == vs->listen_socket) {
			handle_listen(vs);
		}
	}
}

static bool
_update_projection_layer(struct comp_compositor *c,
                         volatile struct ipc_client_state *active_client,
                         volatile struct ipc_layer_render_state *layer,
                         uint32_t i)
{
	// left
	uint32_t lsi = layer->swapchain_ids[0];
	// right
	uint32_t rsi = layer->swapchain_ids[1];

	if (active_client->xscs[lsi] == NULL ||
	    active_client->xscs[rsi] == NULL) {
		fprintf(stderr,
		        "ERROR: Invalid swap chain for projection layer.\n");
		return false;
	}

	struct comp_swapchain *cl = comp_swapchain(active_client->xscs[lsi]);
	struct comp_swapchain *cr = comp_swapchain(active_client->xscs[rsi]);

	struct comp_swapchain_image *l = NULL;
	struct comp_swapchain_image *r = NULL;
	l = &cl->images[layer->data.stereo.l.sub.image_index];
	r = &cr->images[layer->data.stereo.r.sub.image_index];

	//! @todo we are ignoring subrect here!
	// (and perhaps can simplify by re-using some structs?)
	comp_renderer_set_projection_layer(
	    c->r, l, r, layer->data.flip_y, i,
	    layer->data.stereo.l.sub.array_index,
	    layer->data.stereo.r.sub.array_index);

	return true;
}

static bool
_update_quad_layer(struct comp_compositor *c,
                   volatile struct ipc_client_state *active_client,
                   volatile struct ipc_layer_render_state *layer,
                   uint32_t i)
{
	uint32_t sci = layer->swapchain_ids[0];

	if (active_client->xscs[sci] == NULL) {
		fprintf(stderr, "ERROR: Invalid swap chain for quad layer.\n");
		return false;
	}

	struct comp_swapchain *sc = comp_swapchain(active_client->xscs[sci]);
	struct comp_swapchain_image *image = NULL;
	image = &sc->images[layer->data.quad.sub.image_index];

	struct xrt_pose pose = layer->data.quad.pose;
	struct xrt_vec2 size = layer->data.quad.size;

	//! @todo we are ignoring subrect here!
	// (and perhaps can simplify by re-using some structs?)
	comp_renderer_set_quad_layer(c->r, image, &pose, &size,
	                             layer->data.flip_y, i,
	                             layer->data.quad.sub.array_index);

	return true;
}

static bool
_update_layers(struct comp_compositor *c,
               volatile struct ipc_client_state *active_client,
               uint32_t *num_layers)
{
	volatile struct ipc_render_state *render_state =
	    &active_client->render_state;

	if (*num_layers != render_state->num_layers) {
		//! @todo Resizing here would be faster
		*num_layers = render_state->num_layers;
		comp_renderer_destroy_layers(c->r);
		comp_renderer_allocate_layers(c->r, render_state->num_layers);
	}

	for (uint32_t i = 0; i < render_state->num_layers; i++) {
		volatile struct ipc_layer_render_state *layer =
		    &render_state->layers[i];
		switch (layer->data.type) {
		case XRT_LAYER_STEREO_PROJECTION: {
			if (!_update_projection_layer(c, active_client, layer,
			                              i))
				return false;
			break;
		}
		case XRT_LAYER_QUAD: {
			if (!_update_quad_layer(c, active_client, layer, i))
				return false;
			break;
		}
		}
	}
	return true;
}

static int
main_loop(struct ipc_server *vs)
{
	struct xrt_compositor *xc = vs->xc;
	struct comp_compositor *c = comp_compositor(xc);

	// make sure all our client connections have a handle to the compositor
	// and consistent initial state
	vs->thread_state.server = vs;
	vs->thread_state.xc = xc;

	uint32_t num_layers = 0;

	while (vs->running) {

		/*
		 * Check polling.
		 */
		check_epoll(vs);

		/*
		 * Update active client.
		 */

		volatile struct ipc_client_state *active_client = NULL;
		if (vs->thread_state.active) {
			active_client = &vs->thread_state;
		}

		/*
		 * Render the swapchains.
		 */

		if (active_client == NULL || !active_client->active ||
		    active_client->num_swapchains == 0) {
			if (num_layers != 0) {
				COMP_DEBUG(c, "Destroying layers.");
				comp_renderer_destroy_layers(c->r);
				num_layers = 0;
			}
		} else {
			// our ipc server thread will fill in l & r
			// swapchain indices and toggle wait to false
			// when the client calls end_frame, signalling
			// us to render.
			volatile struct ipc_render_state *render_state =
			    &active_client->render_state;

			if (render_state->rendering) {
				if (!_update_layers(c, active_client,
				                    &num_layers))
					continue;

				// set our client state back to waiting.
				render_state->rendering = false;
			}
		}

		comp_renderer_draw(c->r);

		// Now is a good time to destroy objects.
		comp_compositor_garbage_collect(c);
	}

	return 0;
}


/*
 *
 * Exported functions.
 *
 */

int
ipc_server_main(int argc, char **argv)
{
	struct ipc_server *s = U_TYPED_CALLOC(struct ipc_server);
	int ret = init_all(s);
	if (ret < 0) {
		free(s);
		return ret;
	}

	ret = main_loop(s);

	teardown_all(s);
	free(s);

	fprintf(stderr, "SERVER: Exiting! '%i'\n", ret);

	return ret;
}
