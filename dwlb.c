#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <pixman-1/pixman.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
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

#define ARRAY_EXPAND(arr, len, cap, inc)				\
	do {								\
		uint32_t new_len, new_cap;				\
		new_len = (len) + (inc);				\
		if (new_len > (cap)) {					\
			new_cap = new_len * 2;				\
			if (!((arr) = realloc((arr), sizeof(*(arr)) * new_cap))) \
				die("realloc:");			\
			(cap) = new_cap;				\
		}							\
		(len) = new_len;					\
	} while (0)
#define ARRAY_APPEND(arr, len, cap, ptr)		\
	do {						\
		ARRAY_EXPAND((arr), (len), (cap), 1);	\
		(ptr) = &(arr)[(len) - 1];		\
	} while (0)

#define PROGRAM "dwlb"
#define VERSION "0.2"
static const char * const usage =
	"usage: dwlb\n"
	"Options\n"
	"	-v		get version information\n"
	"	-h		view this help text\n";

#define TEXT_MAX 2048

enum { WheelUp, WheelDown };

typedef struct {
	char str[5]; // three digits, a suffix (b, k, m, g, t) and a 0-byte
} IOPrint;

typedef struct {
	pixman_color_t color;
	bool bg;
	char *start;
} Color;

typedef struct {
	uint32_t btn;
	uint32_t x1;
	uint32_t x2;
	char command[128];
} Button;

typedef struct {
	char text[TEXT_MAX];
	Color *colors;
	uint32_t colors_l, colors_c;
	Button *buttons;
	uint32_t buttons_l, buttons_c;
} CustomText;

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

	CustomText status;

	bool configured;
	bool hidden, bottom;
	bool redraw;
} Bar;

typedef struct {
	struct wl_seat *wl_seat;
	struct wl_pointer *wl_pointer;

	uint32_t registry_name;
	uint32_t pointer_x, pointer_y;
	uint32_t pointer_button;

	Bar *bar;

	struct wl_list link;
} Seat;

static void copy_customtext(CustomText *from, CustomText *to);
static int create_shm_file(void);
static void die(const char *fmt, ...);
static int draw_frame(Bar *bar);
static uint32_t draw_text(char const *text, uint32_t x, uint32_t y,
		pixman_image_t *foreground, pixman_image_t *background,
		pixman_color_t const *fg_color, pixman_color_t const *bg_color,
		uint32_t max_x, uint32_t buf_height, uint32_t padding,
	  	Color *colors, uint32_t colors_l);
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
static int parse_color(const char *str, pixman_color_t *clr);
static void parse_into_customtext(CustomText *ct, char const *text);
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
static IOPrint print_io(uint64_t io_value);
static void read_socket(void);
static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities);
static void seat_name(void *data, struct wl_seat *wl_seat, const char *name);
static void setup_bar(Bar *bar);
static void set_top(Bar *bar);
static void set_bottom(Bar *bar);
static void shell_command(char *command);
static void show_bar(Bar *bar);
static void sig_handler(int sig);
static void stats_init(void);
static void stats_update(void);
static void stats_update_cpu(void);
static void stats_update_disk(void);
static void stats_update_gpu_temp(void);
static void stats_update_mem(void);
static void teardown_bar(Bar *bar);
static void teardown_seat(Seat *seat);
static uint32_t text_width(char const* text, uint32_t maxwidth, uint32_t padding);
static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer);

static int sock_fd;
static char *socketpath = NULL;
static char sockbuf[1024];

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zxdg_output_manager_v1 *output_manager;

static struct zdwl_ipc_manager_v2 *dwl_wm;
static struct wl_cursor_image *cursor_image;
static struct wl_surface *cursor_surface;

static struct wl_list bar_list, seat_list;

static char **tags;
static uint32_t tags_l, tags_c;
static char **layouts;
static uint32_t layouts_l, layouts_c;

static struct fcft_font *font;
static uint32_t height, textpadding;

static bool run_display;

static struct {
	/* precalculated sizes */
	uint32_t time_width;
	uint32_t state_width;

	/* open file descriptors */
	int proc_stat_fd;
	int proc_meminfo_fd;
	int gpu_hwmon_fd;

	/* time and date */
	struct tm tm;

	/* cpu */
	uint32_t cpu_prev_total;
	uint32_t cpu_prev_idle;
	uint8_t cpu_usage;
	/* memory */
	uint8_t mem_usage;

	/* GPU temperature */
	uint8_t gpu_temperature;

	/* disk */
	uint64_t prev_sectors_read;
	uint64_t prev_sectors_written;
	uint64_t cur_sectors_read;
	uint64_t cur_sectors_written;
} bar_stats;

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

void
copy_customtext(CustomText *from, CustomText *to)
{
	snprintf(to->text, sizeof to->text, "%s", from->text);
	to->colors_l = to->buttons_l = 0;
	for (uint32_t i = 0; i < from->colors_l; i++) {
		Color *color;
		ARRAY_APPEND(to->colors, to->colors_l, to->colors_c, color);
		color->color = from->colors[i].color;
		color->bg = from->colors[i].bg;
		color->start = from->colors[i].start - (char *)&from->text + (char *)&to->text;
	}
	for (uint32_t i = 0; i < from->buttons_l; i++) {
		Button *button;
		ARRAY_APPEND(to->buttons, to->buttons_l, to->buttons_c, button);
		*button = from->buttons[i];
	}
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

int
draw_frame(Bar *bar)
{
	uint32_t *data = mmap(NULL, bar->bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, bar->shm_fd, 0);
	if (data == MAP_FAILED)
		die("shared memory mmap:");

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, bar->shm_fd, bar->bufsize);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, bar->width, bar->height, bar->stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	wl_shm_pool_destroy(pool);

	/* Pixman image corresponding to main buffer */
	pixman_image_t *final = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, data, bar->width * 4);

	/* Text background and foreground layers */
	pixman_image_t *foreground = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, NULL, bar->width * 4);
	pixman_image_t *background = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->width, bar->height, NULL, bar->width * 4);

	/* Draw on images */
	uint32_t x = 0;
	uint32_t y = (bar->height + font->ascent - font->descent) / 2;
	uint32_t boxs = font->height / 9;
	uint32_t boxw = font->height / 6 + 2;

	snprintf(sockbuf, 1024, bar_time_fmt,
			bar_stats.tm.tm_hour,
			bar_stats.tm.tm_min,
			bar_stats.tm.tm_sec);
	x = draw_text(sockbuf,
			x, y,
			foreground, background,
			&active_fg_color, &active_bg_color,
			bar->width, bar->height, textpadding,
			NULL, 0);

	for (uint32_t i = 0; i < tags_l; i++) {
		const bool active = bar->mtags & 1 << i;
		const bool occupied = bar->ctags & 1 << i;
		const bool urgent = bar->urg & 1 << i;

		if (hide_vacant && !active && !occupied && !urgent)
			continue;

		pixman_color_t const *fg_color = urgent ? &urgent_fg_color : (active ? &active_fg_color : (occupied ? &occupied_fg_color : &inactive_fg_color));
		pixman_color_t const *bg_color = urgent ? &urgent_bg_color : (active ? &active_bg_color : (occupied ? &occupied_bg_color : &inactive_bg_color));

		if (!hide_vacant && occupied) {
			pixman_image_fill_boxes(PIXMAN_OP_SRC,
					foreground, fg_color, 1,
					&(pixman_box32_t){
						.x1 = x + boxs, .x2 = x + boxs + boxw,
						.y1 = boxs, .y2 = boxs + boxw
					});
			if ((!bar->sel || !active) && boxw >= 3) {
				/* Make box hollow */
				pixman_image_fill_boxes(PIXMAN_OP_SRC,
						foreground, &(pixman_color_t){ 0 }, 1,
						&(pixman_box32_t){
							.x1 = x + boxs + 1, .x2 = x + boxs + boxw - 1,
							.y1 = boxs + 1, .y2 = boxs + boxw - 1
						});
			}
		}

		x = draw_text(tags[i],
				x, y,
				foreground, background,
				fg_color, bg_color,
				bar->width, bar->height, textpadding,
				NULL, 0);
	}

	x = draw_text(bar->layout,
			x, y,
			foreground, background,
			&inactive_fg_color, &inactive_bg_color,
			bar->width, bar->height, textpadding,
			NULL, 0);

	uint32_t status_width = MIN(bar->width - x, bar_stats.state_width);
	snprintf(sockbuf, 1024, bar_state_fmt,
			print_io((bar_stats.cur_sectors_read - bar_stats.prev_sectors_read) * 512).str,
			print_io((bar_stats.cur_sectors_written - bar_stats.prev_sectors_written) * 512).str,
			bar_stats.gpu_temperature,
			bar_stats.cpu_usage,
			bar_stats.mem_usage,
			bar_stats.tm.tm_mday,
			bar_stats.tm.tm_mon + 1,
			bar_stats.tm.tm_year + 1900);
	draw_text(sockbuf,
			bar->width - status_width, y,
			foreground, background,
			&inactive_fg_color, &inactive_bg_color,
			bar->width, bar->height, textpadding,
			bar->status.colors, bar->status.colors_l);

	uint32_t nx;
	nx = MIN(x + textpadding, bar->width - status_width);
	pixman_image_fill_boxes(PIXMAN_OP_SRC, background,
			bar->sel ? &middle_bg_color_selected : &middle_bg_color, 1,
			&(pixman_box32_t){
				.x1 = x, .x2 = nx,
				.y1 = 0, .y2 = bar->height
			});
	x = nx;
	x = draw_text(bar->window_title,
			x, y,
			foreground, background,
			bar->sel ? &active_fg_color : &inactive_fg_color,
			bar->sel ? &active_bg_color : &inactive_bg_color,
			bar->width - status_width, bar->height, 0,
			NULL,
			0);

	pixman_image_fill_boxes(PIXMAN_OP_SRC, background,
			bar->sel ? &middle_bg_color_selected : &middle_bg_color, 1,
			&(pixman_box32_t){
	    		.x1 = x, .x2 = bar->width - status_width,
	    		.y1 = 0, .y2 = bar->height
    		});

	/* Draw background and foreground on bar */
	pixman_image_composite32(PIXMAN_OP_OVER, background, NULL, final, 0, 0, 0, 0, 0, 0, bar->width, bar->height);
	pixman_image_composite32(PIXMAN_OP_OVER, foreground, NULL, final, 0, 0, 0, 0, 0, 0, bar->width, bar->height);

	pixman_image_unref(foreground);
	pixman_image_unref(background);
	pixman_image_unref(final);

	munmap(data, bar->bufsize);

	wl_surface_set_buffer_scale(bar->wl_surface, buffer_scale);
	wl_surface_attach(bar->wl_surface, buffer, 0, 0);
	wl_surface_damage_buffer(bar->wl_surface, 0, 0, bar->width, bar->height);
	wl_surface_commit(bar->wl_surface);

	return 0;
}

uint32_t
draw_text(char const *text,
	  uint32_t x,
	  uint32_t y,
	  pixman_image_t *foreground,
	  pixman_image_t *background,
	  pixman_color_t const *fg_color,
	  pixman_color_t const *bg_color,
	  uint32_t max_x,
	  uint32_t buf_height,
	  uint32_t padding,
	  Color *colors,
	  uint32_t colors_l)
{
	if (!text || !*text || !max_x)
		return x;

	uint32_t ix = x, nx;

	if ((nx = x + padding) + padding >= max_x)
		return x;
	x = nx;

	bool draw_fg = foreground && fg_color;
	bool draw_bg = background && bg_color;

	pixman_image_t *fg_fill;
	pixman_color_t const *cur_bg_color;
	if (draw_fg)
		fg_fill = pixman_image_create_solid_fill(fg_color);
	if (draw_bg)
		cur_bg_color = bg_color;

	uint32_t color_ind = 0, codepoint, state = UTF8_ACCEPT, last_cp = 0;
	for (char const *p = text; *p; p++) {
		/* Check for new colors */
		if (state == UTF8_ACCEPT && colors && (draw_fg || draw_bg)) {
			while (color_ind < colors_l && p == colors[color_ind].start) {
				if (colors[color_ind].bg) {
					if (draw_bg)
						cur_bg_color = &colors[color_ind].color;
				} else if (draw_fg) {
					pixman_image_unref(fg_fill);
					fg_fill = pixman_image_create_solid_fill(&colors[color_ind].color);
				}
				color_ind++;
			}
		}

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

		if (draw_fg) {
			/* Detect and handle pre-rendered glyphs (e.g. emoji) */
			if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
				/* Only the alpha channel of the mask is used, so we can
				 * use fgfill here to blend prerendered glyphs with the
				 * same opacity */
				pixman_image_composite32(
					PIXMAN_OP_OVER, glyph->pix, fg_fill, foreground, 0, 0, 0, 0,
					x + glyph->x, y - glyph->y, glyph->width, glyph->height);
			} else {
				/* Applying the foreground color here would mess up
				 * component alphas for subpixel-rendered text, so we
				 * apply it when blending. */
				pixman_image_composite32(
					PIXMAN_OP_OVER, fg_fill, glyph->pix, foreground, 0, 0, 0, 0,
					x + glyph->x, y - glyph->y, glyph->width, glyph->height);
			}
		}

		if (draw_bg) {
			pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
						cur_bg_color, 1, &(pixman_box32_t){
							.x1 = x, .x2 = nx,
							.y1 = 0, .y2 = buf_height
						});
		}

		/* increment pen position */
		x = nx;
	}

	if (draw_fg)
		pixman_image_unref(fg_fill);
	if (!last_cp)
		return ix;

	nx = x + padding;
	if (draw_bg) {
		/* Fill padding background */
		pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
					bg_color, 1, &(pixman_box32_t){
						.x1 = ix, .x2 = ix + padding,
						.y1 = 0, .y2 = buf_height
					});
		pixman_image_fill_boxes(PIXMAN_OP_OVER, background,
					bg_color, 1, &(pixman_box32_t){
						.x1 = x, .x2 = nx,
						.y1 = 0, .y2 = buf_height
					});
	}

	return nx;
}

void
dwl_wm_layout(void *data, struct zdwl_ipc_manager_v2 *dwl_wm,
	const char *name)
{
	char **ptr;
	ARRAY_APPEND(layouts, layouts_l, layouts_c, ptr);
	if (!(*ptr = strdup(name)))
		die("strdup:");
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
}

void
dwl_wm_output_layout(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	uint32_t layout)
{
	Bar *bar = (Bar *)data;

	bar->last_layout_idx = bar->layout_idx;
	bar->layout_idx = layout;
}

void
dwl_wm_output_title(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output,
	const char *title)
{
	Bar *bar = (Bar *)data;

	if (bar->window_title)
		free(bar->window_title);
	if (!(bar->window_title = strdup(title)))
		die("strdup:");
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

	if (layouts[bar->layout_idx])
		free(layouts[bar->layout_idx]);
	if (!(layouts[bar->layout_idx] = strdup(layout)))
		die("strdup:");
	bar->layout = layouts[bar->layout_idx];
}

void
dwl_wm_output_frame(void *data, struct zdwl_ipc_output_v2 *dwl_wm_output)
{
	Bar *bar = (Bar *)data;
	bar->redraw = true;
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
	if (!tags && !(tags = malloc(amount * sizeof(char *))))
		die("malloc:");
	uint32_t i = tags_l;
	ARRAY_EXPAND(tags, tags_l, tags_c, MAX(0, (int)amount - (int)tags_l));
	for (; i < amount; i++)
		if (!(tags[i] = strdup(tags_names[MIN(i, LENGTH(tags_names)-1)])))
			die("strdup:");
}

void
event_loop(void)
{
	int wl_fd = wl_display_get_fd(display);

	int tfd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
	struct itimerspec spec = {
		{ 1, 0 },
		{ 1, 0 },
	};
	timerfd_settime(tfd, 0, &spec, NULL);

	struct pollfd fds[3] = {
		{ .fd = wl_fd,   .events = POLLIN },
		{ .fd = sock_fd, .events = POLLIN },
		{ .fd = tfd,     .events = POLLIN },
	};
	char buf[8];
	while (run_display) {
		wl_display_flush(display);

		if (poll(fds, 3, -1) == -1) {
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

		wl_list_for_each(bar, &bar_list, link) {
			if (bar->redraw) {
				if (!bar->hidden)
					draw_frame(bar);
				bar->redraw = false;
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
	w = w * buffer_scale;
	h = h * buffer_scale;

	zwlr_layer_surface_v1_ack_configure(surface, serial);

	Bar *bar = (Bar *)data;

	if (bar->configured && w == bar->width && h == bar->height)
		return;

	bar->width = w;
	bar->height = h;
	bar->stride = bar->width * 4;
	expand_shm_file(bar, bar->stride * bar->height);
	bar->configured = true;

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

/* Color parsing logic adapted from [sway] */
int
parse_color(const char *str, pixman_color_t *clr)
{
	if (*str == '#')
		str++;
	int len = strlen(str);

	// Disallows "0x" prefix that strtoul would ignore
	if ((len != 6 && len != 8) || !isxdigit(str[0]) || !isxdigit(str[1]))
		return -1;

	char *ptr;
	uint32_t parsed = strtoul(str, &ptr, 16);
	if (*ptr)
		return -1;

	if (len == 8) {
		clr->alpha = (parsed & 0xff) * 0x101;
		parsed >>= 8;
	} else {
		clr->alpha = 0xffff;
	}
	clr->red =   ((parsed >> 16) & 0xff) * 0x101;
	clr->green = ((parsed >>  8) & 0xff) * 0x101;
	clr->blue =  ((parsed >>  0) & 0xff) * 0x101;
	return 0;
}

void
parse_into_customtext(CustomText *ct, char const *text)
{
	ct->colors_l = ct->buttons_l = 0;

	if (status_commands) {
		uint32_t codepoint;
		uint32_t state = UTF8_ACCEPT;
		uint32_t last_cp = 0;
		uint32_t x = 0;
		size_t str_pos = 0;

		Button *left_button = NULL;
		Button *middle_button = NULL;
		Button *right_button = NULL;
		Button *scrollup_button = NULL;
		Button *scrolldown_button = NULL;

		for (char const *p = text; *p && str_pos < sizeof(ct->text) - 1; p++) {
			if (state == UTF8_ACCEPT && *p == '^') {
				p++;
				if (*p != '^') {
					char *arg, *end;
					if (!(arg = strchr(p, '(')) || !(end = strchr(arg + 1, ')')))
						continue;
					*arg++ = '\0';
					*end = '\0';

					if (!strcmp(p, "bg")) {
						Color *color;
						ARRAY_APPEND(ct->colors, ct->colors_l, ct->colors_c, color);
						if (!*arg)
							color->color = inactive_bg_color;
						else
							parse_color(arg, &color->color);
						color->bg = true;
						color->start = ct->text + str_pos;
					} else if (!strcmp(p, "fg")) {
						Color *color;
						ARRAY_APPEND(ct->colors, ct->colors_l, ct->colors_c, color);
						if (!*arg)
							color->color = inactive_fg_color;
						else
							parse_color(arg, &color->color);
						color->bg = false;
						color->start = ct->text + str_pos;
					} else if (!strcmp(p, "lm")) {
						if (left_button) {
							left_button->x2 = x;
							left_button = NULL;
						} else if (*arg) {
							ARRAY_APPEND(ct->buttons, ct->buttons_l, ct->buttons_c, left_button);
							left_button->btn = BTN_LEFT;
							snprintf(left_button->command, sizeof left_button->command, "%s", arg);
							left_button->x1 = x;
						}
					} else if (!strcmp(p, "mm")) {
						if (middle_button) {
							middle_button->x2 = x;
							middle_button = NULL;
						} else if (*arg) {
							ARRAY_APPEND(ct->buttons, ct->buttons_l, ct->buttons_c, middle_button);
							middle_button->btn = BTN_MIDDLE;
							snprintf(middle_button->command, sizeof middle_button->command, "%s", arg);
							middle_button->x1 = x;
						}
					} else if (!strcmp(p, "rm")) {
						if (right_button) {
							right_button->x2 = x;
							right_button = NULL;
						} else if (*arg) {
							ARRAY_APPEND(ct->buttons, ct->buttons_l, ct->buttons_c, right_button);
							right_button->btn = BTN_RIGHT;
							snprintf(right_button->command, sizeof right_button->command, "%s", arg);
							right_button->x1 = x;
						}
					} else if (!strcmp(p, "us")) {
						if (scrollup_button) {
							scrollup_button->x2 = x;
							scrollup_button = NULL;
						} else if (*arg) {
							ARRAY_APPEND(ct->buttons, ct->buttons_l, ct->buttons_c, scrollup_button);
							scrollup_button->btn = WheelUp;
							snprintf(scrollup_button->command, sizeof scrollup_button->command, "%s", arg);
							scrollup_button->x1 = x;
						}
					} else if (!strcmp(p, "ds")) {
						if (scrolldown_button) {
							scrolldown_button->x2 = x;
							scrolldown_button = NULL;
						} else if (*arg) {
							ARRAY_APPEND(ct->buttons, ct->buttons_l, ct->buttons_c, scrolldown_button);
							scrolldown_button->btn = WheelDown;
							snprintf(scrolldown_button->command, sizeof scrolldown_button->command, "%s", arg);
							scrolldown_button->x1 = x;
						}
					}

					*--arg = '(';
					*end = ')';

					p = end;
					continue;
				}
			}

			ct->text[str_pos++] = *p;

			if (utf8decode(&state, &codepoint, *p))
				continue;

			const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(font, codepoint, FCFT_SUBPIXEL_NONE);
			if (!glyph)
				continue;

			long kern = 0;
			if (last_cp)
				fcft_kerning(font, last_cp, codepoint, &kern, NULL);
			last_cp = codepoint;

			x += kern + glyph->advance.x;
		}

		if (left_button)
			left_button->x2 = x;
		if (middle_button)
			middle_button->x2 = x;
		if (right_button)
			right_button->x2 = x;
		if (scrollup_button)
			scrollup_button->x2 = x;
		if (scrolldown_button)
			scrolldown_button->x2 = x;


		ct->text[str_pos] = '\0';
	} else {
		snprintf(ct->text, sizeof ct->text, "%s", text);
	}
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
	uint32_t i;
	uint32_t btn = discrete < 0 ? WheelUp : WheelDown;
	Seat *seat = (Seat *)data;

	if (!seat->bar)
		return;

	uint32_t status_x = seat->bar->width / buffer_scale - text_width(seat->bar->status.text, seat->bar->width, textpadding) / buffer_scale;
	if (seat->pointer_x > status_x) {
		/* Clicked on status */
		for (i = 0; i < seat->bar->status.buttons_l; i++) {
			if (btn == seat->bar->status.buttons[i].btn
			    && seat->pointer_x >= status_x + textpadding + seat->bar->status.buttons[i].x1 / buffer_scale
			    && seat->pointer_x < status_x + textpadding + seat->bar->status.buttons[i].x2 / buffer_scale) {
				shell_command(seat->bar->status.buttons[i].command);
				break;
			}
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
	Seat *seat = (Seat *)data;

	seat->bar = NULL;
	Bar *bar;
	wl_list_for_each(bar, &bar_list, link) {
		if (bar->wl_surface == surface) {
			seat->bar = bar;
			break;
		}
	}

	if (!cursor_image) {
		const char *size_str = getenv("XCURSOR_SIZE");
		int size = size_str ? atoi(size_str) : 0;
		if (size == 0)
			size = 24;
		struct wl_cursor_theme *cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size * buffer_scale, shm);
		// TODO:
		// cursor_image = wl_cursor_theme_get_cursor(cursor_theme, "pointer")->images[0];
		cursor_image = wl_cursor_theme_get_cursor(cursor_theme, "default")->images[0];
		cursor_surface = wl_compositor_create_surface(compositor);
		wl_surface_set_buffer_scale(cursor_surface, buffer_scale);
		wl_surface_attach(cursor_surface, wl_cursor_image_get_buffer(cursor_image), 0, 0);
		wl_surface_commit(cursor_surface);
	}
	wl_pointer_set_cursor(pointer, serial, cursor_surface,
			      cursor_image->hotspot_x,
			      cursor_image->hotspot_y);
}

void
pointer_frame(void *data, struct wl_pointer *pointer)
{
	Seat *seat = (Seat *)data;

	if (!seat->pointer_button || !seat->bar)
		return;

	uint32_t x = 0, i = 0;
	x += bar_stats.time_width / buffer_scale;
	do {
		if (hide_vacant) {
			const bool active = seat->bar->mtags & 1 << i;
			const bool occupied = seat->bar->ctags & 1 << i;
			const bool urgent = seat->bar->urg & 1 << i;
			if (!active && !occupied && !urgent)
				continue;
		}
		x += text_width(tags[i], seat->bar->width - x, textpadding) / buffer_scale;
	} while (seat->pointer_x >= x && ++i < tags_l);

	if (i < tags_l) {
		/* Clicked on tags */
		if (seat->pointer_button == BTN_LEFT)
			zdwl_ipc_output_v2_set_tags(seat->bar->dwl_wm_output, 1 << i, 1);
		else if (seat->pointer_button == BTN_MIDDLE)
			zdwl_ipc_output_v2_set_tags(seat->bar->dwl_wm_output, ~0, 1);
		else if (seat->pointer_button == BTN_RIGHT)
			zdwl_ipc_output_v2_set_tags(seat->bar->dwl_wm_output, seat->bar->mtags ^ (1 << i), 0);
	} else if (seat->pointer_x < (x += text_width(seat->bar->layout, seat->bar->width - x, textpadding))) {
		/* Clicked on layout */
		if (seat->pointer_button == BTN_LEFT)
			zdwl_ipc_output_v2_set_layout(seat->bar->dwl_wm_output, seat->bar->last_layout_idx);
		else if (seat->pointer_button == BTN_RIGHT)
			zdwl_ipc_output_v2_set_layout(seat->bar->dwl_wm_output, 2);
	} else {
		uint32_t status_x = MAX(x, seat->bar->width - bar_stats.state_width) / buffer_scale;
		if (seat->pointer_x >= status_x) {
			/* Clicked on status */
			for (i = 0; i < seat->bar->status.buttons_l; i++) {
				if (seat->pointer_button == seat->bar->status.buttons[i].btn
				    && seat->pointer_x >= status_x + textpadding + seat->bar->status.buttons[i].x1 / buffer_scale
				    && seat->pointer_x < status_x + textpadding + seat->bar->status.buttons[i].x2 / buffer_scale) {
					shell_command(seat->bar->status.buttons[i].command);
					break;
				}
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
}

void
pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
	       wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	Seat *seat = (Seat *)data;

	seat->pointer_x = wl_fixed_to_int(surface_x);
	seat->pointer_y = wl_fixed_to_int(surface_y);
}

static IOPrint print_io(uint64_t io_value) {
	IOPrint ret = { "    \0" };

	if (io_value == 0)
		return ret;
	else if (io_value < 1000)
		snprintf(ret.str, 5,"%3lub", io_value);
	else if (io_value < 1000000)
		snprintf(ret.str, 5,"%3luk", io_value / 1000);
	else if (io_value < 1000000000)
		snprintf(ret.str, 5,"%3lum", io_value / 1000000);
	else if  (io_value < 1000000000000)
		snprintf(ret.str, 5,"%3lug", io_value / 1000000000);
	else
		snprintf(ret.str, 5,"%3lut", io_value / 1000000000000);

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
	char *data = NULL;

	for (ssize_t i = 0; output[i] != 0; ++i) {
		if (output[i] == ' ') {
			data = output + i + 1;
			break;
		}
	}

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
	case CommandStatus: {
		if (!data)
			return;
		if (all) {
			Bar *first = NULL;
			wl_list_for_each(bar, &bar_list, link) {
				if (first) {
					copy_customtext(&first->status, &bar->status);
				} else {
					parse_into_customtext(&bar->status, data);
					first = bar;
				}
				bar->redraw = true;
			}
		} else {
			parse_into_customtext(&bar->status, data);
			bar->redraw = true;
		}
		break;
	}
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
shell_command(char *command)
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

void stats_init(void)
{
	tzset();
	time_t t = time(NULL);
	localtime_r(&t, &bar_stats.tm);

	bar_stats.proc_stat_fd = open("/proc/stat", O_RDONLY | O_CLOEXEC, 0);
	if (bar_stats.proc_stat_fd == -1)
		die("failed to open /proc/stat:");
	bar_stats.cpu_usage = 0;
	bar_stats.cpu_prev_idle = 0;
	bar_stats.cpu_prev_total = 0;

	bar_stats.proc_meminfo_fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC, 0);
	if (bar_stats.proc_meminfo_fd == -1)
		die("failed to open /proc/meminfo:");
	bar_stats.mem_usage = 0;

	DIR *dir;
	if (!(dir = opendir("/sys/class/hwmon/")))
		die("Could not open directory /sys/class/hwmon/:");
	struct dirent *de;
	bar_stats.gpu_hwmon_fd = -1;
	while ((de = readdir(dir))) {
		snprintf(sockbuf, 1024, "/sys/class/hwmon/%s/name", de->d_name);
		int fd = open(sockbuf, O_RDONLY, 0);
		if (fd == -1)
			continue;
		read(fd, sockbuf, 1024);
		if (!strncmp(sockbuf, "amdgpu", 6)) {
			snprintf(sockbuf, 1024, "/sys/class/hwmon/%s/temp1_input", de->d_name);
			int fd = open(sockbuf, O_RDONLY | O_CLOEXEC, 0);
			if (fd != -1) {
				bar_stats.gpu_hwmon_fd = fd;
				break;
			}
		}
		close(fd);
	}
	closedir(dir);
	if (bar_stats.gpu_hwmon_fd == -1)
		die("failed to find amdgpu hwmon");
	bar_stats.gpu_temperature = 0;

	bar_stats.prev_sectors_read = 0;
	bar_stats.prev_sectors_written = 0;
	bar_stats.cur_sectors_read = 0;
	bar_stats.cur_sectors_written = 0;

	snprintf(sockbuf, 1024, bar_state_fmt, "0", "0", '0', '0', '0', '0', '0', '0');
	bar_stats.state_width = text_width(sockbuf, 0xFFFFFFFFu, textpadding);
	snprintf(sockbuf, 1024, bar_time_fmt, '0', '0', '0');
	bar_stats.time_width = text_width(sockbuf, 0xFFFFFFFFu, textpadding);
}

void
stats_update(void)
{
	Bar* bar;

	time_t t = time(NULL);
	localtime_r(&t, &bar_stats.tm);

	stats_update_cpu();
	stats_update_disk();
	stats_update_gpu_temp();
	stats_update_mem();

	wl_list_for_each(bar, &bar_list, link)
		bar->redraw = true;
}

void
stats_update_cpu(void)
{
	lseek(bar_stats.proc_stat_fd, 5, SEEK_SET);
	read(bar_stats.proc_stat_fd, sockbuf, 128);

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

	uint32_t i = idle - bar_stats.cpu_prev_idle;
	uint32_t t = total - bar_stats.cpu_prev_total + 1;

	bar_stats.cpu_usage = (100 * (t - i) + t / 2) / t;
	bar_stats.cpu_prev_idle = idle;
	bar_stats.cpu_prev_total = total;
}

void stats_update_disk(void) {
	bar_stats.prev_sectors_read = bar_stats.cur_sectors_read;
	bar_stats.prev_sectors_written = bar_stats.cur_sectors_written;
	DIR *dir;
	if (!(dir = opendir("/sys/block")))
		die("Could not open directory /sys/block:");
	struct dirent *de;
	while ((de = readdir(dir))) {
		snprintf(sockbuf, 1024, "/sys/block/%s/stat", de->d_name);
		int fd = open(sockbuf, O_RDONLY, 0);
		if (fd == -1)
			continue;
		read(fd, sockbuf, 1024);
		char *end, *cur = sockbuf;

		// completed reads
		while (*cur == ' ') ++cur;
		while (*cur != ' ') ++cur;

		// merged reads
		while (*cur == ' ') ++cur;
		while (*cur != ' ') ++cur;

		// sectors reads
		while (*cur == ' ') ++cur;
		bar_stats.cur_sectors_read = strtoul(cur, &end, 10);
		cur = end;

		// milliseconds spent reading
		while (*cur == ' ') ++cur;
		while (*cur != ' ') ++cur;

		// completed writes
		while (*cur == ' ') ++cur;
		while (*cur != ' ') ++cur;

		// merged writes
		while (*cur == ' ') ++cur;
		while (*cur != ' ') ++cur;

		// sectors writes
		while (*cur == ' ') ++cur;
		bar_stats.cur_sectors_written = strtoul(cur, &end, 10);

		close(fd);
	}
	closedir(dir);
}

void
stats_update_gpu_temp(void)
{
	lseek(bar_stats.gpu_hwmon_fd, 0, SEEK_SET);
	read(bar_stats.gpu_hwmon_fd, sockbuf, 128);
	uint32_t file_read = strtoul(sockbuf, NULL, 10);
	bar_stats.gpu_temperature = file_read / 1000;
}

void
stats_update_mem(void)
{
	char *word, *cur;

	lseek(bar_stats.proc_meminfo_fd, 10, SEEK_SET);
	read(bar_stats.proc_meminfo_fd, sockbuf, 128);

	cur = sockbuf;
	while (*cur == ' ') ++cur;
	word = cur;
	while (*cur != ' ') ++cur;
	*cur = 0;
	uint32_t total = strtoul(word, NULL, 10);

	while (*cur != '\n') ++cur;
	++cur;
	while (*cur != '\n') ++cur;
	while (*cur != ' ') ++cur;
	while (*cur == ' ') ++cur;
	word = cur;
	while (*cur != ' ') ++cur;
	*cur = 0;
	uint32_t available = strtoul(word, NULL, 10);

	bar_stats.mem_usage = 100 - ((100 * available + total / 2) / total);
}

void
teardown_bar(Bar *bar)
{
	if (bar->status.colors)
		free(bar->status.colors);
	if (bar->status.buttons)
		free(bar->status.buttons);
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

uint32_t text_width(char const* text, uint32_t maxwidth, uint32_t padding) {
	return draw_text(text, 0, 0, NULL, NULL, NULL, NULL, maxwidth, 0, padding, NULL, 0);
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
	snprintf(sockbuf, 1024, "dpi=%u", dpi);
	if (!(font = fcft_from_name(fontcount, fontstr, sockbuf)))
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
	close(bar_stats.proc_stat_fd);
	close(bar_stats.proc_meminfo_fd);
	close(bar_stats.gpu_hwmon_fd);
	unlink(socketpath);

	if (tags) {
		for (uint32_t i = 0; i < tags_l; i++)
			free(tags[i]);
		free(tags);
	}
	if (layouts) {
		for (uint32_t i = 0; i < layouts_l; i++)
			free(layouts[i]);
		free(layouts);
	}

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
