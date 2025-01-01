#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <pixman-1/pixman.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-util.h>

#include "commands.h"
#include "utf8.h"
#include "xdg-shell-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "dwl-ipc-unstable-v2-protocol.h"

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define LENGTH(x)	(sizeof (x) / sizeof (x[0]))

#define PROGRAM "dwlb"
#define VERSION "0.2"
static const char * const usage =
	"usage: dwlb\n"
	"Options\n"
	"	-v		get version information\n"
	"	-h		view this help text\n";

typedef struct {
	char str[5]; // three digits, a suffix (b, k, m, g, t) and a 0-byte
} IOPrint;

typedef struct {
	pixman_color_t fg;
	pixman_color_t bg;
} Color;

typedef struct {
	uint32_t x1;
	uint32_t x2;
} DrawArea;

typedef struct {
	struct wl_output *wl_output;
	struct wl_surface *wl_surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct zxdg_output_v1 *xdg_output;
	struct zdwl_ipc_output_v2 *dwl_wm_output;

	struct wl_list link;

	char *xdg_output_name;
	char *layout, *window_title;

	uint32_t registry_name;

	uint32_t width, height;
	uint32_t stride, bufsize;

	uint32_t mtags, ctags, urg, sel;
	uint32_t layout_idx, last_layout_idx;

	int shm_fd;

	bool configured;
	bool hidden, bottom;
	bool redraw_tags, redraw_window, redraw_layout, redraw;
} Bar;

typedef struct {
	struct wl_seat *wl_seat;
	struct wl_pointer *wl_pointer;

	uint32_t registry_name;
	uint32_t pointer_x, pointer_y;
	uint32_t pointer_button;
	uint32_t pointer_enter_serial;
	bool on_clickable;

	Bar *bar;

	struct wl_list link;
} Seat;

typedef struct {
	/* open file descriptors */
	int proc_stat_fd, proc_meminfo_fd, gpu_hwmon_fd;
	int net_rx_bytes_fd,  net_tx_bytes_fd;


	/* cpu */
	uint32_t cpu_prev_total;
	uint32_t cpu_prev_idle;
	uint8_t  cpu_usage;

	/* memory */
	uint8_t mem_usage;

	/* GPU temperature */
	uint8_t gpu_temperature;

	/* disk */
	uint64_t prev_sectors_read;
	uint64_t prev_sectors_written;
	uint64_t cur_sectors_read;
	uint64_t cur_sectors_written;

	/* network */
	uint64_t prev_rx_bytes;
	uint64_t prev_tx_bytes;
	uint64_t cur_rx_bytes;
	uint64_t cur_tx_bytes;

	/* ALSA */
	snd_mixer_t* mixer;
	snd_mixer_elem_t* playback;
	snd_mixer_elem_t* capture;

	/* time and date */
	struct tm tm;
} Stats;

static void alsa_init(void);
static uint8_t alsa_get_pcapture(void);
static uint8_t alsa_get_pplayback(void);
static void bar_free_canvas(Bar const *bar, pixman_image_t *canvas, uint32_t *data);
static void bar_get_canvas(Bar const *bar, pixman_image_t **canvas, uint32_t **data);
static int create_shm_file(void);
static void die(const char *fmt, ...);
static void draw_background(Bar const *bar, pixman_image_t *canvas, uint32_t x1, uint32_t x2, pixman_color_t const *color);
static void draw_foreground(Bar const *bar, pixman_image_t *canvas, char const* text,
		uint32_t x, uint32_t max_x, uint32_t padding, pixman_color_t const *color);
static void draw_alsa(Bar *bar);
static void draw_layout(Bar *bar);
static void draw_stats(Bar *bar);
static void draw_tags(Bar *bar);
static void draw_window_name(Bar *bar);
static void draw_frame(Bar *bar);
static void dwl_wm_layout(void *data, struct zdwl_ipc_manager_v2 *dwl_wm, const char *name);
static void dwl_wm_output_toggle_visibility(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output);
static void dwl_wm_output_active(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output, uint32_t active);
static void dwl_wm_output_tag(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
		uint32_t tag, uint32_t state, uint32_t clients, uint32_t focused);
static void dwl_wm_output_layout(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output, uint32_t layout);
static void dwl_wm_output_title(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output, const char *title);
static void dwl_wm_output_appid(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output, const char *appid);
static void dwl_wm_output_layout_symbol(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output, const char *layout);
static void dwl_wm_output_frame(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output);
static void dwl_wm_output_fullscreen(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output, uint32_t is_fullscreen);
static void dwl_wm_output_floating(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output, uint32_t is_floating);
static void dwl_wm_tags(void *data, struct zdwl_ipc_manager_v2 *dwl_wm, uint32_t amount);
static void event_loop(void);
static void expand_shm_file(Bar* bar, size_t size);
static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void hide_bar(Bar *bar);
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial, uint32_t w, uint32_t h);
static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface);
static void output_description(void *data, struct zxdg_output_v1 *xdg_output, const char *description);
static void output_done(void *data, struct zxdg_output_v1 *xdg_output);
static void output_logical_size(void *data, struct zxdg_output_v1 *xdg_output, int32_t width, int32_t height);
static void output_logical_position(void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y);
static void output_name(void *data, struct zxdg_output_v1 *xdg_output, const char *name);
static uint64_t parse_trusted_uint64_t(char const** const cur);
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value);
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete);
static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source);
static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis);
static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete);
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y);
static void pointer_frame(void *data, struct wl_pointer *pointer);
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface);
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y);
static void pointer_set_image(Seat *seat, struct wl_pointer *pointer);
static IOPrint print_io(uint64_t io_value);
static void read_socket(void);
static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities);
static void seat_name(void *data, struct wl_seat *wl_seat, const char *name);
static void setup_bar(Bar *bar);
static void set_top(Bar *bar);
static void set_bottom(Bar *bar);
static void shell_command(char const* command);
static void show_bar(Bar *bar);
static void sig_handler(int sig);
static void skip_line(char const** const cur);
static void skip_space(char const** const cur);
static void skip_word(char const** const cur);
static void stats_init(void);
static void stats_update(void);
static void stats_update_cpu(void);
static void stats_update_disk(void);
static void stats_update_gpu_temp(void);
static void stats_update_mem(void);
static void stats_update_network(void);
static void teardown_bar(Bar *bar);
static void teardown_seat(Seat *seat);
static uint32_t text_width(char const* text, uint32_t maxwidth, uint32_t padding);
static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer);

static int sock_fd;
static char *socketpath = NULL;
static char sockbuf[256];

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zxdg_output_manager_v1 *output_manager;

static struct zdwl_ipc_manager_v2 *dwl_wm;
static struct wl_cursor_image *cursor_default_image = NULL;
static struct wl_surface *cursor_default_surface = NULL;
static struct wl_cursor_image *cursor_pointer_image = NULL;
static struct wl_surface *cursor_pointer_surface = NULL;

static struct wl_list bar_list, seat_list;

static struct fcft_font *font;
static uint32_t height, textpadding;

static bool run_display;

static Stats stats;

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static const struct zxdg_output_v1_listener output_listener = {
	.name = output_name,
	.logical_position = output_logical_position,
	.logical_size = output_logical_size,
	.done = output_done,
	.description = output_description
};

static const struct wl_pointer_listener pointer_listener = {
	.axis = pointer_axis,
	.axis_discrete = pointer_axis_discrete,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_value120 = pointer_axis_value120,
	.button = pointer_button,
	.enter = pointer_enter,
	.frame = pointer_frame,
	.leave = pointer_leave,
	.motion = pointer_motion,
};

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static const struct zdwl_ipc_manager_v2_listener dwl_wm_listener = {
	.tags = dwl_wm_tags,
	.layout = dwl_wm_layout
};

static const struct zdwl_ipc_output_v2_listener dwl_wm_output_listener = {
	.toggle_visibility = dwl_wm_output_toggle_visibility,
	.active = dwl_wm_output_active,
	.tag = dwl_wm_output_tag,
	.layout = dwl_wm_output_layout,
	.title = dwl_wm_output_title,
	.appid = dwl_wm_output_appid,
	.layout_symbol = dwl_wm_output_layout_symbol,
	.frame = dwl_wm_output_frame,
	.fullscreen = dwl_wm_output_fullscreen,
	.floating = dwl_wm_output_floating
};

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove
};


#include "config.h"
static struct {
	uint32_t mod[m_TOTAL];
	uint32_t layout;
	uint32_t tag;
} draw_widths;

void
alsa_init(void)
{
    snd_mixer_selem_id_t* sid;
    if (snd_mixer_selem_id_malloc(&sid))
		die("failed to alloc mixer_selem_id");
    snd_mixer_selem_id_set_index(sid, 0);

    if ((snd_mixer_open(&stats.mixer, 0)) < 0)
		die("failed to open snd mixer");

    if ((snd_mixer_attach(stats.mixer, "default")) < 0) {
        snd_mixer_close(stats.mixer);
		die("failed to attach snd mixer");
    }

    if ((snd_mixer_selem_register(stats.mixer, NULL, NULL)) < 0) {
        snd_mixer_close(stats.mixer);
		die("failed to register selem mixer");
    }

    if ((snd_mixer_load(stats.mixer)) < 0) {
        snd_mixer_close(stats.mixer);
		die("failed to load mixer");
    }

    snd_mixer_selem_id_set_name(sid, "Master");
    stats.playback = snd_mixer_find_selem(stats.mixer, sid);
    if (!stats.playback) {
		snd_mixer_free(stats.mixer);
		die("failed to find playback selem");
	}

    snd_mixer_selem_id_set_name(sid, "Capture");
    stats.capture = snd_mixer_find_selem(stats.mixer, sid);
    if (!stats.capture) {
		snd_mixer_free(stats.mixer);
		die("failed to find capture selem");
	}

	snd_mixer_selem_id_free(sid);
}

uint8_t
alsa_get_pcapture(void)
{
    long minv, maxv, invol;
    snd_mixer_selem_get_capture_volume_range(stats.capture, &minv, &maxv);
	snd_mixer_selem_get_capture_volume(stats.capture, 0, &invol);
	maxv -= minv;
	invol -= minv;
	return ((invol * 100) + maxv / 2) / maxv;
}

uint8_t
alsa_get_pplayback(void)
{
    long minv, maxv, outvol;
    snd_mixer_selem_get_playback_volume_range(stats.playback, &minv, &maxv);
	snd_mixer_selem_get_playback_volume(stats.playback, 0, &outvol);
	maxv -= minv;
	outvol -= minv;
	return ((outvol * 100) + maxv / 2) / maxv;
}

void
bar_free_canvas(Bar const *bar, pixman_image_t *canvas, uint32_t *data)
{
	pixman_image_unref(canvas);
	munmap(data, bar->bufsize);
}

void
bar_get_canvas(Bar const *bar, pixman_image_t **canvas, uint32_t **data)
{
	*data = mmap(NULL, bar->bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, bar->shm_fd, 0);
	if (data == MAP_FAILED)
		die("shared memory mmap:");
	*canvas = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, *data, bar->width * 4);
}

int
create_shm_file(void)
{
	int ret, fd;

	fd = memfd_create("surface", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd == -1)
		die("memfd_create:");

	// we can ignore errors this time because this is just an optimization
	do {
		ret = fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
	} while (ret == -1 && errno == EINTR);

	return fd;
}

void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}

	if (socketpath)
		unlink(socketpath);

	exit(1);
}

void
draw_background(
	Bar const *bar,
	pixman_image_t *canvas,
	uint32_t x1,
	uint32_t x2,
	pixman_color_t const *color)
{
	if (x1 >= x2 || x1 >= bar->width)
		return;
	x2 = MIN(x2, bar->width);
	pixman_image_fill_boxes(
			PIXMAN_OP_SRC,
			canvas,
			color,
			1,
			&(pixman_box32_t) {
				x1, 0,
				x2, bar->height,
			});
}

void
draw_foreground(
	Bar const *bar,
	pixman_image_t *canvas,
	char const* text,
	uint32_t x,
	uint32_t max_x,
	uint32_t padding,
	pixman_color_t const *color)
{
	uint32_t nx;
	uint32_t y = (bar->height + font->ascent - font->descent) / 2;
	if (!text || !*text || !max_x)
		return;

	if ((nx = x + padding) + padding >= max_x)
		return;
	x = nx;

	pixman_image_t *fg_fill = pixman_image_create_solid_fill(color);
	uint32_t codepoint, state = UTF8_ACCEPT, last_cp = 0;
	for (char const *p = text; *p; p++) {
		/* Returns nonzero if more bytes are needed */
		if (utf8decode(&state, &codepoint, *p))
			continue;

		/* Turn off subpixel rendering, which complicates things when
		 * mixed with alpha channels */
		const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(font, codepoint, FCFT_SUBPIXEL_NONE);
		if (!glyph)
			continue;

		/* Adjust x position based on kerning with previous glyph */
		long kern = 0;
		if (last_cp)
			fcft_kerning(font, last_cp, codepoint, &kern, NULL);
		if ((nx = x + kern + glyph->advance.x) + padding > max_x)
			break;
		last_cp = codepoint;
		x += kern;

		/* Detect and handle pre-rendered glyphs (e.g. emoji) */
		if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
			/* Only the alpha channel of the mask is used, so we can
			 * use fgfill here to blend prerendered glyphs with the
			 * same opacity */
			pixman_image_composite32(
					PIXMAN_OP_OVER, glyph->pix, fg_fill, canvas, 0, 0, 0, 0,
					x + glyph->x, y - glyph->y, glyph->width, glyph->height);
		} else {
			/* Applying the foreground color here would mess up
			 * component alphas for subpixel-rendered text, so we
			 * apply it when blending. */
			pixman_image_composite32(
					PIXMAN_OP_OVER, fg_fill, glyph->pix, canvas, 0, 0, 0, 0,
					x + glyph->x, y - glyph->y, glyph->width, glyph->height);
		}

		/* increment pen position */
		x = nx;
	}

	pixman_image_unref(fg_fill);
}

void
draw_alsa(Bar *bar)
{
	pixman_image_t *canvas;
	uint32_t *data, x1, x2;

	bar_get_canvas(bar, &canvas, &data);

	snprintf(sockbuf, 256, mod_fmt[m_alsa], alsa_get_pplayback(), alsa_get_pcapture());
	x2 = bar->width - draw_widths.mod[m_date];;
	x1 = x2 - draw_widths.mod[m_alsa];
	draw_background(bar, canvas, x1, x2, &inactive_color.bg);
	draw_foreground(bar, canvas, sockbuf, x1, x2, ALSA_PAD, &inactive_color.fg);

	bar_free_canvas(bar, canvas, data);
}

void
draw_layout(Bar *bar)
{
	pixman_image_t *canvas;
	uint32_t *data;
	const uint32_t x1 = draw_widths.tag * TAGCOUNT + draw_widths.mod[m_time];
	const uint32_t x2 = x1 + draw_widths.layout;

	bar_get_canvas(bar, &canvas, &data);

	draw_background(bar, canvas, x1, x2, &inactive_color.bg);
	draw_foreground(bar, canvas, bar->layout, x1, x2, textpadding, &inactive_color.fg);

	bar_free_canvas(bar, canvas, data);
}

void
draw_stats(Bar *bar)
{
	pixman_image_t *canvas;
	uint32_t *data, x1, x2;

	bar_get_canvas(bar, &canvas, &data);

	snprintf(sockbuf, 256, mod_fmt[m_time],
			stats.tm.tm_hour,
			stats.tm.tm_min,
			stats.tm.tm_sec);
	x1 = 0;
	x2 = x1 + draw_widths.mod[m_time];
	draw_background(bar, canvas, x1, x2, &time_color.bg);
	draw_foreground(bar, canvas, sockbuf, x1, x2, TIME_PAD, &time_color.fg);

	snprintf(sockbuf, 256, mod_fmt[m_date],
		stats.tm.tm_mday,
		stats.tm.tm_mon + 1,
		stats.tm.tm_year + 1900);
	x2 = bar->width;
	x1 = x2 - draw_widths.mod[m_date];
	draw_background(bar, canvas, x1, x2, &active_color.bg);
	draw_foreground(bar, canvas, sockbuf, x1, x2, DATE_PAD, &active_color.fg);

	snprintf(sockbuf, 256, mod_fmt[m_ram], stats.mem_usage);
	x2 = x1 - draw_widths.mod[m_alsa];
	x1 = x2 - draw_widths.mod[m_ram];
	draw_background(bar, canvas, x1, x2, &inactive_color.bg);
	draw_foreground(bar, canvas, sockbuf, x1, x2, STATE_PAD, &inactive_color.fg);

	snprintf(sockbuf, 256, mod_fmt[m_cpu], stats.cpu_usage);
	x2 = x1;
	x1 = x2 - draw_widths.mod[m_cpu];
	draw_background(bar, canvas, x1, x2, &inactive_color.bg);
	draw_foreground(bar, canvas, sockbuf, x1, x2, STATE_PAD, &inactive_color.fg);

	snprintf(sockbuf, 256, mod_fmt[m_temp], stats.gpu_temperature);
	x2 = x1;
	x1 = x2 - draw_widths.mod[m_temp];
	draw_background(bar, canvas, x1, x2, &inactive_color.bg);
	draw_foreground(bar, canvas, sockbuf, x1, x2, STATE_PAD, &inactive_color.fg);

	snprintf(sockbuf, 256, mod_fmt[m_disk], 
			print_io((stats.cur_sectors_read - stats.prev_sectors_read) * 512).str,
			print_io((stats.cur_sectors_written - stats.prev_sectors_written) * 512).str);
	x2 = x1;
	x1 = x2 - draw_widths.mod[m_disk];
	draw_background(bar, canvas, x1, x2, &inactive_color.bg);
	draw_foreground(bar, canvas, sockbuf, x1, x2, STATE_PAD, &inactive_color.fg);

	snprintf(sockbuf, 256, mod_fmt[m_net], 
			print_io(stats.cur_tx_bytes - stats.prev_tx_bytes).str,
			print_io(stats.cur_rx_bytes - stats.prev_rx_bytes).str);
	x2 = x1;
	x1 = x2 - draw_widths.mod[m_net];
	draw_background(bar, canvas, x1, x2, &inactive_color.bg);
	draw_foreground(bar, canvas, sockbuf, x1, x2, STATE_PAD, &inactive_color.fg);

	bar_free_canvas(bar, canvas, data);
}

void
draw_tags(Bar *bar)
{
	pixman_image_t *canvas;
	uint32_t *data, x1, x2, boxs, boxw, i;
	bool active, occupied, urgent;
	Color const *color;

	bar_get_canvas(bar, &canvas, &data);

	boxs = font->height / 9;
	boxw = font->height / 6 + 2;
	for (i = 0; i < TAGCOUNT; ++i) {
		active   = bar->mtags & 1 << i;
		occupied = bar->ctags & 1 << i;
		urgent   = bar->urg   & 1 << i;

		if (hide_vacant && !active && !occupied && !urgent)
			continue;

		x1 = draw_widths.mod[m_time] + draw_widths.tag * i;
		x2 = x1 + draw_widths.tag;
		color = urgent ? &urgent_color : (active ? &active_color : (occupied ? &occupied_color : &inactive_color));
		draw_background(bar, canvas, x1, x2, &color->bg);
		draw_foreground(bar, canvas, &tags[i * 2], x1, x2, textpadding, &color->fg);

		if (!hide_vacant && occupied) {
			pixman_image_fill_boxes(PIXMAN_OP_OVER,
					canvas, &color->fg, 1,
					&(pixman_box32_t){
						.x1 = x1 + boxs, .x2 = x1 + boxs + boxw,
						.y1 = boxs, .y2 = boxs + boxw
					});
			if ((!bar->sel || !active) && boxw >= 3) {
				/* Make box hollow */
				pixman_image_fill_boxes(PIXMAN_OP_SRC,
						canvas, &color->bg, 1,
						&(pixman_box32_t){
							.x1 = x1 + boxs + 1, .x2 = x1 + boxs + boxw - 1,
							.y1 = boxs + 1, .y2 = boxs + boxw - 1
						});
			}
		}
	}

	bar_free_canvas(bar, canvas, data);
}

void
draw_window_name(Bar *bar)
{
	pixman_image_t *canvas;
	uint32_t *data, x2, width;
	const uint32_t x1 = draw_widths.mod[m_time] + draw_widths.tag * TAGCOUNT + draw_widths.layout;
	const Color* const color = bar->sel ? &middle_sel_color : &middle_color;

	width = 0;
	for (int i = 1; i < m_TOTAL; ++i)
		width += draw_widths.mod[i];
	x2 = bar->width - width;

	bar_get_canvas(bar, &canvas, &data);

	draw_background(bar, canvas, x1, x2, &color->bg);
	draw_foreground(bar, canvas, bar->window_title, x1, x2, textpadding, &color->fg);

	bar_free_canvas(bar, canvas, data);
}

void
draw_frame(Bar *bar)
{
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, bar->shm_fd, bar->bufsize);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, bar->width, bar->height, bar->stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	wl_shm_pool_destroy(pool);
	wl_surface_set_buffer_scale(bar->wl_surface, buffer_scale);
	wl_surface_attach(bar->wl_surface, buffer, 0, 0);
	wl_surface_damage_buffer(bar->wl_surface, 0, 0, bar->width, bar->height);
	wl_surface_commit(bar->wl_surface);
}

void
dwl_wm_layout(void *data, struct zdwl_ipc_manager_v2 *dwl_wm,
	const char *name)
{
	// these are allocated statically in config.h
}

void
dwl_wm_output_toggle_visibility(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output)
{
	Bar *bar = (Bar *)data;

	if (bar->hidden)
		show_bar(bar);
	else
		hide_bar(bar);
}

void
dwl_wm_output_active(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	uint32_t active)
{
	Bar *bar = (Bar *)data;

	if (active != bar->sel)
		bar->sel = active;
}

void
dwl_wm_output_tag(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	uint32_t tag, uint32_t state, uint32_t clients, uint32_t focused)
{
	Bar *bar = (Bar *)data;

	if (state & ZDWL_IPC_OUTPUT_V2_TAG_STATE_ACTIVE)
		bar->mtags |= 1 << tag;
	else
		bar->mtags &= ~(1 << tag);
	if (clients > 0)
		bar->ctags |= 1 << tag;
	else
		bar->ctags &= ~(1 << tag);
	if (state & ZDWL_IPC_OUTPUT_V2_TAG_STATE_URGENT)
		bar->urg |= 1 << tag;
	else
		bar->urg &= ~(1 << tag);

	bar->redraw_tags = true;
}

void
dwl_wm_output_layout(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	uint32_t layout)
{
	Bar *bar = (Bar *)data;

	bar->last_layout_idx = bar->layout_idx;
	bar->layout_idx = layout;
	bar->redraw_layout = true;
}

void
dwl_wm_output_title(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	const char *title)
{
	Bar *bar;

	bar = (Bar *)data;
	if (bar->window_title)
		free(bar->window_title);
	if (!(bar->window_title = strdup(title)))
		die("strdup:");
	bar->redraw_window = true;
}

void
dwl_wm_output_appid(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	const char *appid)
{
}

void
dwl_wm_output_layout_symbol(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	const char *layout)
{
	Bar *bar = (Bar *)data;
	strcpy(layouts[bar->layout_idx], layout);
	bar->layout = layouts[bar->layout_idx];
}

void
dwl_wm_output_frame(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output)
{
	Bar *bar = (Bar *)data;
	if (bar->redraw_tags)   draw_tags(bar);
	if (bar->redraw_window) draw_window_name(bar);
	if (bar->redraw_layout) draw_layout(bar);
	bar->redraw = bar->redraw_tags | bar->redraw_window | bar->redraw_layout;
}

void
dwl_wm_output_fullscreen(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	uint32_t is_fullscreen)
{
}

void
dwl_wm_output_floating(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	uint32_t is_floating)
{
}

void
dwl_wm_tags(void *data, struct zdwl_ipc_manager_v2 *dwl_wm,
	uint32_t amount)
{
	if (amount != TAGCOUNT)
		die("incorrect tagcount: have %u, expected %u", amount, TAGCOUNT);
}

void
event_loop(void)
{
	const int wl_fd = wl_display_get_fd(display);
	const int tfd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
	const struct itimerspec spec = {
		{ 1, 0 },
		{ 1, 0 },
	};
	timerfd_settime(tfd, 0, &spec, NULL);

	char buf[8];
	while (run_display) {
		wl_display_flush(display);

		int fd_count = snd_mixer_poll_descriptors_count(stats.mixer);
		struct pollfd fds[fd_count + 3];
		fds[0] = (struct pollfd) { .fd = wl_fd,   .events = POLLIN };
        fds[1] = (struct pollfd) { .fd = sock_fd, .events = POLLIN };
        fds[2] = (struct pollfd) { .fd = tfd,     .events = POLLIN };
		snd_mixer_poll_descriptors(stats.mixer, &fds[3], fd_count);

		if (poll(fds, fd_count + 3, -1) == -1) {
			if (errno == EINTR)
				continue;
			else
				die("poll:");
		}

		if (fds[0].revents) {
			if (wl_display_dispatch(display) == -1)
				break;
		}

		if (fds[1].revents)
			read_socket();

		Bar *bar;
		if (fds[2].revents) {
			read(tfd, buf, 8);
			stats_update();
		}

		for (int i = 0; i < fd_count; ++i) {
			if (fds[3 + i].revents & POLLIN) {
				snd_mixer_handle_events(stats.mixer);
				wl_list_for_each(bar, &bar_list, link) {
					draw_alsa(bar);
					bar->redraw = true;
				}
				break;
			} else if (fds[3 + i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
				die("disconnected from alsa");
			}
		}

		wl_list_for_each(bar, &bar_list, link) {
			if (bar->redraw) {
				if (!bar->hidden)
					draw_frame(bar);
				bar->redraw = false;
				bar->redraw_tags = false;
				bar->redraw_layout = false;
				bar->redraw_window = false;
			}
		}
	}
}

void
expand_shm_file(Bar* bar, size_t size)
{
	int ret;
	if (size > bar->bufsize) {
		do {
			ret = ftruncate(bar->shm_fd, size);
		} while (ret == -1 && errno == EINTR);

		if (ret == -1) {
			close(bar->shm_fd);
			bar->shm_fd = -1;
		} else {
			bar->bufsize = size;
		}
	}
}

void
handle_global(void *data, struct wl_registry *registry,
	      uint32_t name, const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
		output_manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 2);
	} else if (!strcmp(interface, zdwl_ipc_manager_v2_interface.name)) {
		dwl_wm = wl_registry_bind(registry, name, &zdwl_ipc_manager_v2_interface, 2);
		zdwl_ipc_manager_v2_add_listener(dwl_wm, &dwl_wm_listener, NULL);
	} else if (!strcmp(interface, wl_output_interface.name)) {
		Bar *bar = calloc(1, sizeof(Bar));
		if (!bar)
			die("calloc:");
		bar->registry_name = name;
		bar->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
		if (run_display)
			setup_bar(bar);
		wl_list_insert(&bar_list, &bar->link);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		Seat *seat = calloc(1, sizeof(Seat));
		if (!seat)
			die("calloc:");
		seat->registry_name = name;
		seat->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
		wl_seat_add_listener(seat->wl_seat, &seat_listener, seat);
		wl_list_insert(&seat_list, &seat->link);
	}
}

void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	Bar *bar;
	Seat *seat;

	wl_list_for_each(bar, &bar_list, link) {
		if (bar->registry_name == name) {
			wl_list_remove(&bar->link);
			teardown_bar(bar);
			return;
		}
	}
	wl_list_for_each(seat, &seat_list, link) {
		if (seat->registry_name == name) {
			wl_list_remove(&seat->link);
			teardown_seat(seat);
			return;
		}
	}
}

void
hide_bar(Bar *bar)
{
	zwlr_layer_surface_v1_destroy(bar->layer_surface);
	wl_surface_destroy(bar->wl_surface);

	bar->configured = false;
	bar->hidden = true;
}

/* Layer-surface setup adapted from layer-shell example in [wlroots] */
void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
			uint32_t serial, uint32_t w, uint32_t h)
{
	Bar *bar;

	w = w * buffer_scale;
	h = h * buffer_scale;

	zwlr_layer_surface_v1_ack_configure(surface, serial);

	bar = (Bar *)data;

	if (bar->configured && w == bar->width && h == bar->height)
		return;

	bar->width = w;
	bar->height = h;
	bar->stride = bar->width * 4;
	expand_shm_file(bar, bar->stride * bar->height);

	bar->configured = true;

	draw_alsa(bar);
	draw_tags(bar);
	draw_layout(bar);
	draw_window_name(bar);
	draw_stats(bar);
	draw_frame(bar);
}

void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
}

void
output_description(void *data, struct zxdg_output_v1 *xdg_output,
		   const char *description)
{
}

void
output_done(void *data, struct zxdg_output_v1 *xdg_output)
{
}

void
output_name(void *data, struct zxdg_output_v1 *xdg_output, const char *name)
{
	Bar *bar = (Bar *)data;

	if (bar->xdg_output_name)
		free(bar->xdg_output_name);
	if (!(bar->xdg_output_name = strdup(name)))
		die("strdup:");
}

void
output_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
		    int32_t width, int32_t height)
{
}

void
output_logical_position(void *data, struct zxdg_output_v1 *xdg_output,
			int32_t x, int32_t y)
{
}

uint64_t
parse_trusted_uint64_t(char const** const cur)
{
	/* we use this to parse information from `/proc/`, `/sys/` and other
	 * such interfaces. We know the input can be trusted because if those
	 * interfaces start outputing garbage, the kernel itself is fucked, and
	 * we have bigger problems to worry about than our silly little bar */
	uint64_t x = 0;
	while (**cur >= '0' && **cur <= '9') {
		x *= 10;
		x += (**cur) - '0';
		++(*cur);
	}
	return x;
}

void
pointer_axis(void *data, struct wl_pointer *pointer,
	     uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

void
pointer_axis_discrete(void *data, struct wl_pointer *pointer,
		      uint32_t axis, int32_t discrete)
{
	uint32_t vol_x1, mic_x1, mic_x2;
	Seat *seat = (Seat *)data;
	if (!seat->bar)
		return;

	mic_x2 = (seat->bar->width - draw_widths.mod[m_date]) / buffer_scale;
	vol_x1 = mic_x2 - draw_widths.mod[m_alsa] / buffer_scale;;
	mic_x1 = (mic_x2 + vol_x1) / 2;
	if (seat->pointer_x >= vol_x1 && seat->pointer_x <= mic_x2) {
		if (seat->pointer_x > mic_x1) {
			if (discrete < 0)
				shell_command(mic_up_cmd);
			else
				shell_command(mic_down_cmd);
		} else {
			if (discrete < 0)
				shell_command(vol_up_cmd);
			else
				shell_command(vol_down_cmd);
		}
	}
}

void
pointer_axis_source(void *data, struct wl_pointer *pointer,
		    uint32_t axis_source)
{
}

void
pointer_axis_stop(void *data, struct wl_pointer *pointer,
		  uint32_t time, uint32_t axis)
{
}

void
pointer_axis_value120(void *data, struct wl_pointer *pointer,
		      uint32_t axis, int32_t discrete)
{
}


void
pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
	       uint32_t time, uint32_t button, uint32_t state)
{
	Seat *seat = (Seat *)data;

	seat->pointer_button = state == WL_POINTER_BUTTON_STATE_PRESSED ? button : 0;
}

void
pointer_enter(void *data, struct wl_pointer *pointer,
	      uint32_t serial, struct wl_surface *surface,
	      wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	Bar *bar;
	Seat *seat = (Seat *)data;

	seat->bar = NULL;
	wl_list_for_each(bar, &bar_list, link) {
		if (bar->wl_surface == surface) {
			seat->bar = bar;
			break;
		}
	}
	seat->pointer_enter_serial = serial;

	if (!cursor_default_image) {
		const char *size_str = getenv("XCURSOR_SIZE");
		int size = size_str ? atoi(size_str) : 0;
		if (size == 0)
			size = 24;
		struct wl_cursor_theme *cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size * buffer_scale, shm);

		cursor_default_image = wl_cursor_theme_get_cursor(cursor_theme, "default")->images[0];
		cursor_default_surface = wl_compositor_create_surface(compositor);
		wl_surface_set_buffer_scale(cursor_default_surface, buffer_scale);
		wl_surface_attach(cursor_default_surface, wl_cursor_image_get_buffer(cursor_default_image), 0, 0);
		wl_surface_commit(cursor_default_surface);

		cursor_pointer_image = wl_cursor_theme_get_cursor(cursor_theme, "pointer")->images[0];
		cursor_pointer_surface = wl_compositor_create_surface(compositor);
		wl_surface_set_buffer_scale(cursor_pointer_surface, buffer_scale);
		wl_surface_attach(cursor_pointer_surface, wl_cursor_image_get_buffer(cursor_pointer_image), 0, 0);
		wl_surface_commit(cursor_pointer_surface);
	}

	wl_pointer_set_cursor(pointer, serial, cursor_default_surface,
			      cursor_default_image->hotspot_x,
			      cursor_default_image->hotspot_y);
}

void
pointer_frame(void *data, struct wl_pointer *pointer)
{
	uint32_t x, i, vol_x1, mic_x1, mic_x2;
	Seat *seat = (Seat *)data;

	if (!seat->pointer_button || !seat->bar)
		return;

	x = draw_widths.mod[m_time] / buffer_scale;
	if (seat->pointer_x <= x)
		return;

	i = 0;
	do {
		if (hide_vacant) {
			const bool active = seat->bar->mtags & 1 << i;
			const bool occupied = seat->bar->ctags & 1 << i;
			const bool urgent = seat->bar->urg & 1 << i;
			if (!active && !occupied && !urgent)
				continue;
		}
		x += draw_widths.tag / buffer_scale;
	} while (seat->pointer_x >= x && ++i < TAGCOUNT);

	if (i < TAGCOUNT) {
		/* Clicked on tags */
		if (seat->pointer_button == BTN_LEFT)
			zdwl_ipc_output_v2_set_tags(seat->bar->dwl_wm_output, 1 << i, 1);
		else if (seat->pointer_button == BTN_MIDDLE)
			zdwl_ipc_output_v2_set_tags(seat->bar->dwl_wm_output, ~0, 1);
		else if (seat->pointer_button == BTN_RIGHT)
			zdwl_ipc_output_v2_set_tags(seat->bar->dwl_wm_output, seat->bar->mtags ^ (1 << i), 0);
	} else if (seat->pointer_x < x + draw_widths.layout) {
		/* Clicked on layout */
		if (seat->pointer_button == BTN_LEFT)
			zdwl_ipc_output_v2_set_layout(seat->bar->dwl_wm_output, seat->bar->last_layout_idx);
		else if (seat->pointer_button == BTN_RIGHT)
			zdwl_ipc_output_v2_set_layout(seat->bar->dwl_wm_output, 2);
	} else {
		mic_x2 = (seat->bar->width - draw_widths.mod[m_date]) / buffer_scale;
		vol_x1 = mic_x2 - draw_widths.mod[m_alsa] / buffer_scale;;
		mic_x1 = (mic_x2 + vol_x1) / 2;

		if (seat->pointer_x >= vol_x1 && seat->pointer_x <= mic_x2) {
			if (seat->pointer_button == BTN_LEFT) {
				if (seat->pointer_x > mic_x1)
					shell_command("amixer -q set Capture toggle");
				else
					shell_command("amixer -q set Master toggle");
			}
		}
	}

	seat->pointer_button = 0;
}

void
pointer_leave(void *data, struct wl_pointer *pointer,
	      uint32_t serial, struct wl_surface *surface)
{
	Seat *seat = (Seat *)data;

	seat->bar = NULL;
	seat->on_clickable = false;
}

void
pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
	       wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	Seat *seat = (Seat *)data;

	seat->pointer_x = wl_fixed_to_int(surface_x);
	seat->pointer_y = wl_fixed_to_int(surface_y);

	pointer_set_image(seat, pointer);
}

void
pointer_set_image(Seat *seat, struct wl_pointer *pointer)
{
	uint32_t alsa_x1, alsa_x2, layout_x2;
	bool on_clickable;

	if (!seat->bar)
		return;

	alsa_x2 = (seat->bar->width - draw_widths.mod[m_date]) / buffer_scale;
	alsa_x1 = alsa_x2 - draw_widths.mod[m_alsa] / buffer_scale;;
	layout_x2 = draw_widths.mod[m_time] + draw_widths.tag * TAGCOUNT + draw_widths.layout;
	on_clickable =
		   (seat->pointer_x > draw_widths.mod[m_time] && seat->pointer_x <= layout_x2)
		|| (seat->pointer_x >= alsa_x1 && seat->pointer_x <= alsa_x2);

	if (on_clickable && !seat->on_clickable) {
		wl_pointer_set_cursor(pointer, seat->pointer_enter_serial, cursor_pointer_surface,
				cursor_pointer_image->hotspot_x,
				cursor_pointer_image->hotspot_y);
	} else if (!on_clickable && seat->on_clickable) {
		wl_pointer_set_cursor(pointer, seat->pointer_enter_serial, cursor_default_surface,
				cursor_default_image->hotspot_x,
				cursor_default_image->hotspot_y);
	}

	seat->on_clickable = on_clickable;
}

static IOPrint print_io(uint64_t io_value) {
	IOPrint ret = { "    \0" };

	if (io_value == 0)
		return ret;
	else if (io_value < 1000)
		snprintf(ret.str, 5, "%3lub", io_value);
	else if (io_value < 1000000)
		snprintf(ret.str, 5, "%3luk", io_value / 1000);
	else if (io_value < 1000000000)
		snprintf(ret.str, 5, "%3lum", io_value / 1000000);
	else if  (io_value < 1000000000000)
		snprintf(ret.str, 5, "%3lug", io_value / 1000000000);
	else
		snprintf(ret.str, 5, "%3lut", io_value / 1000000000000);

	return ret;
}

void
read_socket(void)
{
	int cli_fd;
	if ((cli_fd = accept(sock_fd, NULL, 0)) == -1)
		die("accept:");
	ssize_t len = recv(cli_fd, sockbuf, sizeof sockbuf - 1, 0);
	if (len == -1)
		die("recv:");
	close(cli_fd);
	if (len == 0)
		return;
	sockbuf[len] = '\0';

	enum Command cmd = sockbuf[0];
	char *output = sockbuf + 1;

	Bar *bar = NULL, *it;
	bool all = false;

	if (!strcmp(output, "all")) {
		all = true;
	} else if (!strcmp(output, "selected")) {
		wl_list_for_each(it, &bar_list, link) {
			if (it->sel) {
				bar = it;
				break;
			}
		}
	} else {
		wl_list_for_each(it, &bar_list, link) {
			if (it->xdg_output_name && !strcmp(output, it->xdg_output_name)) {
				bar = it;
				break;
			}
		}
	}

	if (!all && !bar)
		return;

	switch (cmd) {
	case CommandShow: {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (bar->hidden)
					show_bar(bar);
		} else {
			if (bar->hidden)
				show_bar(bar);
		}
		break;
	}
	case CommandHide: {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (!bar->hidden)
					hide_bar(bar);
		} else {
			if (!bar->hidden)
				hide_bar(bar);
		}
		break;
	}
	case CommandToggleVis: {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (bar->hidden)
					show_bar(bar);
				else
					hide_bar(bar);
		} else {
			if (bar->hidden)
				show_bar(bar);
			else
				hide_bar(bar);
		}
		break;
	}
	case CommandSetTop: {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (bar->bottom)
					set_top(bar);
		} else {
			if (bar->bottom)
				set_top(bar);
		}
		break;
	}
	case CommandSetBot: {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (!bar->bottom)
					set_bottom(bar);
		} else {
			if (!bar->bottom)
				set_bottom(bar);
		}
		break;
	}
	case CommandToggleLoc: {
		if (all) {
			wl_list_for_each(bar, &bar_list, link)
				if (bar->bottom)
					set_top(bar);
				else
					set_bottom(bar);
		} else {
			if (bar->bottom)
				set_top(bar);
			else
				set_bottom(bar);
		}
		break;
	}
    }
}

void
seat_capabilities(void *data, struct wl_seat *wl_seat,
		  uint32_t capabilities)
{
	Seat *seat = (Seat *)data;

	uint32_t has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
	if (has_pointer && !seat->wl_pointer) {
		seat->wl_pointer = wl_seat_get_pointer(seat->wl_seat);
		wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);
	} else if (!has_pointer && seat->wl_pointer) {
		wl_pointer_destroy(seat->wl_pointer);
		seat->wl_pointer = NULL;
	}
}

void
seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

void
setup_bar(Bar *bar)
{
	bar->height = height * buffer_scale;
	bar->bottom = bottom;
	bar->hidden = hidden;

	bar->xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, bar->wl_output);
	if (!bar->xdg_output)
		die("Could not create xdg_output");
	zxdg_output_v1_add_listener(bar->xdg_output, &output_listener, bar);

	bar->dwl_wm_output = zdwl_ipc_manager_v2_get_output(dwl_wm, bar->wl_output);
	if (!bar->dwl_wm_output)
		die("Could not create dwl_wm_output");
	zdwl_ipc_output_v2_add_listener(bar->dwl_wm_output, &dwl_wm_output_listener, bar);

	if (!bar->hidden)
		show_bar(bar);

	bar->shm_fd = create_shm_file();
}

void
set_top(Bar *bar)
{
	if (!bar->hidden) {
		zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
						 ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		bar->redraw = true;
	}
	bar->bottom = false;
}

void
set_bottom(Bar *bar)
{
	if (!bar->hidden) {
		zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
						 ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
						 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		bar->redraw = true;
	}
	bar->bottom = true;
}

void
shell_command(char const* command)
{
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", command, NULL);
		exit(EXIT_SUCCESS);
	}
}

void
show_bar(Bar *bar)
{
	bar->wl_surface = wl_compositor_create_surface(compositor);
	if (!bar->wl_surface)
		die("Could not create wl_surface");

	bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, bar->wl_surface, bar->wl_output,
								   ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, PROGRAM);
	if (!bar->layer_surface)
		die("Could not create layer_surface");
	zwlr_layer_surface_v1_add_listener(bar->layer_surface, &layer_surface_listener, bar);

	zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, bar->height / buffer_scale);
	zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
					 (bar->bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
					 | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, bar->height / buffer_scale);
	wl_surface_commit(bar->wl_surface);

	bar->hidden = false;
}

void
sig_handler(int sig)
{
	if (sig == SIGINT || sig == SIGHUP || sig == SIGTERM)
		run_display = false;
}

void
skip_line(char const** const cur)
{
	while (**cur != '\n') ++(*cur);
	++(*cur);
}

void
skip_space(char const** const cur)
{
	while (**cur == ' ') ++(*cur);
}

void
skip_word(char const** const cur)
{
	while (**cur != ' ') ++(*cur);
}

void
stats_init(void)
{
	int fd1, fd2;
	time_t t;
	DIR *dir;
	/* time and date */
	tzset();
	t = time(NULL);
	localtime_r(&t, &stats.tm);

	/* cpu */
	stats.proc_stat_fd = open("/proc/stat", O_RDONLY | O_CLOEXEC, 0);
	if (stats.proc_stat_fd == -1)
		die("failed to open /proc/stat:");
	stats.cpu_usage = 0;
	stats.cpu_prev_idle = 0;
	stats.cpu_prev_total = 0;

	/* memory */
	stats.proc_meminfo_fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC, 0);
	if (stats.proc_meminfo_fd == -1)
		die("failed to open /proc/meminfo:");
	stats.mem_usage = 0;

	/* GPU temperature */
	if (!(dir = opendir("/sys/class/hwmon/")))
		die("Could not open directory /sys/class/hwmon/:");
	struct dirent *de;
	stats.gpu_hwmon_fd = -1;
	while ((de = readdir(dir))) {
		snprintf(sockbuf, 256, "/sys/class/hwmon/%s/name", de->d_name);
		fd1 = open(sockbuf, O_RDONLY, 0);
		if (fd1 == -1)
			continue;
		read(fd1, sockbuf, 256);
		if (!strncmp(sockbuf, "amdgpu", 6)) {
			snprintf(sockbuf, 256, "/sys/class/hwmon/%s/temp1_input", de->d_name);
			fd2 = open(sockbuf, O_RDONLY | O_CLOEXEC, 0);
			if (fd2 != -1) {
				stats.gpu_hwmon_fd = fd2;
				break;
			}
		}
		close(fd1);
	}
	closedir(dir);
	if (stats.gpu_hwmon_fd == -1)
		die("failed to find amdgpu hwmon");
	stats.gpu_temperature = 0;

	/* disk */
	stats.prev_sectors_read = 0;
	stats.prev_sectors_written = 0;
	stats.cur_sectors_read = 0;
	stats.cur_sectors_written = 0;

	/* network */
	stats.net_rx_bytes_fd =
		open("/sys/class/net/" NET_INTERFACE_NAME "/statistics/rx_bytes", O_RDONLY | O_CLOEXEC, 0);
	if (stats.net_rx_bytes_fd == -1)
		die("failed to open" "/sys/class/net/" NET_INTERFACE_NAME "/statistics/rx_bytes:");

	stats.net_tx_bytes_fd =
		open("/sys/class/net/" NET_INTERFACE_NAME "/statistics/tx_bytes", O_RDONLY | O_CLOEXEC, 0);
	if (stats.net_tx_bytes_fd == -1)
		die("failed to open" "/sys/class/net/" NET_INTERFACE_NAME "/statistics/tx_bytes:");

	stats.prev_rx_bytes = 0;
	stats.prev_tx_bytes = 0;
	stats.cur_rx_bytes = 0;
	stats.cur_tx_bytes = 0;

	/* ALSA */
	alsa_init();

	snprintf(sockbuf, 256, mod_fmt[m_time], '0', '0', '0');
	draw_widths.mod[m_time] = text_width(sockbuf, 0xFFFFFFFFu, TIME_PAD);

	snprintf(sockbuf, 256, mod_fmt[m_date], '0', '0', '0');
	draw_widths.mod[m_date] = text_width(sockbuf, 0xFFFFFFFFu, DATE_PAD);;

	snprintf(sockbuf, 256, mod_fmt[m_alsa], '0', '0');
	draw_widths.mod[m_alsa] = text_width(sockbuf, 0xFFFFFFFFu, STATE_PAD);

	snprintf(sockbuf, 256, mod_fmt[m_ram], '0');
	draw_widths.mod[m_ram] = text_width(sockbuf, 0xFFFFFFFFu, STATE_PAD);

	snprintf(sockbuf, 256, mod_fmt[m_cpu], '0');
	draw_widths.mod[m_cpu] = text_width(sockbuf, 0xFFFFFFFFu, STATE_PAD);

	snprintf(sockbuf, 256, mod_fmt[m_temp], '0');
	draw_widths.mod[m_temp] = text_width(sockbuf, 0xFFFFFFFFu, STATE_PAD);

	snprintf(sockbuf, 256, mod_fmt[m_disk], "0", "0");
	draw_widths.mod[m_disk] = text_width(sockbuf, 0xFFFFFFFFu, STATE_PAD);

	snprintf(sockbuf, 256, mod_fmt[m_net], "0", "0");
	draw_widths.mod[m_net] = text_width(sockbuf, 0xFFFFFFFFu, STATE_PAD);

	draw_widths.tag = text_width("0", 0xFFFFFFFFu, textpadding);
	draw_widths.layout = text_width("[]=", 0xFFFFFFFFu, textpadding);
}

void
stats_update(void)
{
	Bar* bar;
	time_t t;

	t = time(NULL);
	localtime_r(&t, &stats.tm);

	stats_update_cpu();
	stats_update_disk();
	stats_update_gpu_temp();
	stats_update_mem();
	stats_update_network();

	wl_list_for_each(bar, &bar_list, link) {
		draw_stats(bar);
		bar->redraw = true;
	}
}

void
stats_update_cpu(void)
{
	lseek(stats.proc_stat_fd, 5, SEEK_SET);
	read(stats.proc_stat_fd, sockbuf, 128);

	ssize_t j = 0;
	uint32_t total = 0;
	uint32_t idle = 0;
	for (ssize_t i = 0; i < 3; ++i) {
		unsigned int x = 0;
		while (sockbuf[j] != ' ') {
			x *= 10;
			x += sockbuf[j] - '0';
			++j;
		}
		total += x;
		++j;
	}

	for (ssize_t i = 0; i < 2; ++i) {
		unsigned int x = 0;
		while (sockbuf[j] != ' ') {
			x *= 10;
			x += sockbuf[j] - '0';
			++j;
		}
		total += x;
		idle += x;
		++j;
	}

	for (ssize_t i = 0; i < 3; ++i) {
		unsigned int x = 0;
		while (sockbuf[j] != ' ') {
			x *= 10;
			x += sockbuf[j] - '0';
			++j;
		}
		total += x;
		++j;
	}

	uint32_t i = idle - stats.cpu_prev_idle;
	uint32_t t = total - stats.cpu_prev_total + 1;

	stats.cpu_usage = (100 * (t - i) + t / 2) / t;
	stats.cpu_prev_idle = idle;
	stats.cpu_prev_total = total;
}

void
stats_update_disk(void)
{
	char const *cur;
	int fd;
	DIR *dir;

	stats.prev_sectors_read = stats.cur_sectors_read;
	stats.prev_sectors_written = stats.cur_sectors_written;
	if (!(dir = opendir("/sys/block")))
		die("Could not open directory /sys/block:");
	struct dirent *de;
	while ((de = readdir(dir))) {
		snprintf(sockbuf, 256, "/sys/block/%s/stat", de->d_name);
		fd = open(sockbuf, O_RDONLY, 0);
		if (fd == -1)
			continue;
		read(fd, sockbuf, 256);
		cur = sockbuf;

		// completed reads
		skip_space(&cur);
		skip_word(&cur);

		// merged reads
		skip_space(&cur);
		skip_word(&cur);

		// sectors reads
		skip_space(&cur);
		stats.cur_sectors_read = parse_trusted_uint64_t(&cur);

		// milliseconds spent reading
		skip_space(&cur);
		skip_word(&cur);

		// completed writes
		skip_space(&cur);
		skip_word(&cur);

		// merged writes
		skip_space(&cur);
		skip_word(&cur);

		// sectors writes
		skip_space(&cur);
		stats.cur_sectors_written = parse_trusted_uint64_t(&cur);

		close(fd);
	}
	closedir(dir);
}

void
stats_update_gpu_temp(void)
{
	char const *cur;
	uint64_t file_read;
	lseek(stats.gpu_hwmon_fd, 0, SEEK_SET);
	read(stats.gpu_hwmon_fd, sockbuf, 128);
	cur = sockbuf;
	file_read = parse_trusted_uint64_t(&cur);
	stats.gpu_temperature = file_read / 1000;
}

void
stats_update_mem(void)
{
	char const *cur;
	uint64_t total, available;

	lseek(stats.proc_meminfo_fd, 10, SEEK_SET);
	read(stats.proc_meminfo_fd, sockbuf, 128);

	cur = sockbuf;

	skip_space(&cur);
	total  = parse_trusted_uint64_t(&cur);

	skip_line(&cur);
	skip_line(&cur);
	skip_word(&cur);
	skip_space(&cur);
	available  = parse_trusted_uint64_t(&cur);

	stats.mem_usage = 100 - ((100 * available + total / 2) / total);
}

void
stats_update_network(void)
{
	char const *cur;
	stats.prev_rx_bytes = stats.cur_rx_bytes;
	stats.prev_tx_bytes = stats.cur_tx_bytes;

	lseek(stats.net_rx_bytes_fd, 0, SEEK_SET);
	read(stats.net_rx_bytes_fd, sockbuf, 128);
	cur = sockbuf;
	stats.cur_rx_bytes = parse_trusted_uint64_t(&cur);

	lseek(stats.net_tx_bytes_fd, 0, SEEK_SET);
	read(stats.net_tx_bytes_fd, sockbuf, 128);
	cur = sockbuf;
	stats.cur_tx_bytes = parse_trusted_uint64_t(&cur);
}

void
teardown_bar(Bar *bar)
{
	if (bar->window_title)
		free(bar->window_title);
	zdwl_ipc_output_v2_destroy(bar->dwl_wm_output);
	if (bar->xdg_output_name)
		free(bar->xdg_output_name);
	if (!bar->hidden) {
		zwlr_layer_surface_v1_destroy(bar->layer_surface);
		wl_surface_destroy(bar->wl_surface);
	}
	if (bar->shm_fd >= 0) {
		close(bar->shm_fd);
	}
	zxdg_output_v1_destroy(bar->xdg_output);
	wl_output_destroy(bar->wl_output);
	free(bar);
}

void
teardown_seat(Seat *seat)
{
	if (seat->wl_pointer)
		wl_pointer_destroy(seat->wl_pointer);
	wl_seat_destroy(seat->wl_seat);
	free(seat);
}

uint32_t
text_width(char const* text, uint32_t max_x, uint32_t padding)
{
	if (!text || !*text || !max_x || ((padding * 2) >= max_x))
		return 0;

	uint32_t codepoint, x = padding, last_cp = 0, state = UTF8_ACCEPT;
	for (char const *p = text; *p; p++) {
		/* Returns nonzero if more bytes are needed */
		if (utf8decode(&state, &codepoint, *p))
			continue;

		const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(font, codepoint, FCFT_SUBPIXEL_NONE);
		if (!glyph)
			continue;

		long kern = 0;
		if (last_cp)
			fcft_kerning(font, last_cp, codepoint, &kern, NULL);
		if ((x + kern + glyph->advance.x + padding) > max_x)
			break;
		x += kern + glyph->advance.x;
		last_cp = codepoint;
	}

	if (!last_cp)
		return 0;

	return x + padding;
}

void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

int
main(int argc, char **argv)
{
	char *xdgruntimedir, socketdir[256];
	struct sockaddr_un sock_address;
	Bar *bar, *bar2;
	Seat *seat, *seat2;

	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			printf(PROGRAM " " VERSION "\n");
			return 0;
		} else if (!strcmp(argv[1], "-h")) {
			printf(usage);
			return 0;
		} else {
			die("Option '%s' not recognized\n%s", argv[1], usage);
		}
	}

	/* Establish socket directory */
	if (!(xdgruntimedir = getenv("XDG_RUNTIME_DIR")))
		die("Could not retrieve XDG_RUNTIME_DIR");
	snprintf(socketdir, sizeof socketdir, "%s/dwlb", xdgruntimedir);
	if (mkdir(socketdir, S_IRWXU) == -1)
		if (errno != EEXIST)
			die("Could not create directory '%s':", socketdir);
	sock_address.sun_family = AF_UNIX;

	/* Set up display and protocols */
	display = wl_display_connect(NULL);
	if (!display)
		die("Failed to create display");

	wl_list_init(&bar_list);
	wl_list_init(&seat_list);

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !layer_shell || !output_manager || (!dwl_wm))
		die("Compositor does not support all needed protocols");

	/* Load selected font */
	fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_ERROR);
	fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3);

	unsigned int dpi = 96 * buffer_scale;
	snprintf(sockbuf, 256, "dpi=%u", dpi);
	if (!(font = fcft_from_name(FONTCOUNT, fontstr, sockbuf)))
		die("Could not load font");
	textpadding = (font->height * 2) / 5;
	height = font->height / buffer_scale + vertical_padding * 2;

	/* Setup bars */
	stats_init();
	wl_list_for_each(bar, &bar_list, link)
		setup_bar(bar);
	wl_display_roundtrip(display);

	/* Set up socket */
	bool found = false;
	for (uint32_t i = 0; i < 50; i++) {
		if ((sock_fd = socket(AF_UNIX, SOCK_STREAM, 1)) == -1)
			die("socket");
		snprintf(sock_address.sun_path, sizeof sock_address.sun_path, "%s/dwlb-%i", socketdir, i);
		if (connect(sock_fd, (struct sockaddr *)&sock_address, sizeof sock_address) == -1) {
			found = true;
			break;
		}
		close(sock_fd);
	}
	if (!found)
		die("Could not secure a socket path");

	socketpath = (char *)&sock_address.sun_path;
	unlink(socketpath);
	if (bind(sock_fd, (struct sockaddr *)&sock_address, sizeof sock_address) == -1)
		die("bind:");
	if (listen(sock_fd, SOMAXCONN) == -1)
		die("listen:");
	fcntl(sock_fd, F_SETFD, FD_CLOEXEC | fcntl(sock_fd, F_GETFD));

	/* Set up signals */
	struct sigaction sa;
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);

	/* Run */
	run_display = true;
	event_loop();

	/* Clean everything up */
	close(sock_fd);
	close(stats.proc_stat_fd);
	close(stats.proc_meminfo_fd);
	close(stats.gpu_hwmon_fd);
	close(stats.net_rx_bytes_fd);
	close(stats.net_tx_bytes_fd);
	snd_mixer_free(stats.mixer);

	unlink(socketpath);

	wl_list_for_each_safe(bar, bar2, &bar_list, link)
		teardown_bar(bar);
	wl_list_for_each_safe(seat, seat2, &seat_list, link)
		teardown_seat(seat);

	zwlr_layer_shell_v1_destroy(layer_shell);
	zxdg_output_manager_v1_destroy(output_manager);
	zdwl_ipc_manager_v2_destroy(dwl_wm);

	fcft_destroy(font);
	fcft_fini();

	wl_shm_destroy(shm);
	wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);

	return 0;
}
