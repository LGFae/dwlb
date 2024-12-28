#define HEX_COLOR(hex)				\
	{ .red   = ((hex >> 24) & 0xff) * 257,	\
	  .green = ((hex >> 16) & 0xff) * 257,	\
	  .blue  = ((hex >> 8) & 0xff) * 257,	\
	  .alpha = (hex & 0xff) * 257 }

#define DECLARE_COLOR(name, foreground, background, start) \
	static const Color name ## _color = { foreground, background, start }

// set 16-bit colors for bar
// use either pixman_color_t struct or HEX_COLOR macro for 8-bit colors
DECLARE_COLOR(active,     HEX_COLOR(0xeeeeeeff), HEX_COLOR(0x005577ff), NULL);
DECLARE_COLOR(occupied,   HEX_COLOR(0xeeeeeeff), HEX_COLOR(0x005577ff), NULL);
DECLARE_COLOR(inactive,   HEX_COLOR(0xbbbbbbff), HEX_COLOR(0x222222ff), NULL);
DECLARE_COLOR(urgent,     HEX_COLOR(0x222222ff), HEX_COLOR(0xeeeeeeff), NULL);
DECLARE_COLOR(middle,     HEX_COLOR(0xeeeeeeff), HEX_COLOR(0x222222ff), NULL);
DECLARE_COLOR(middle_sel, HEX_COLOR(0xeeeeeeff), HEX_COLOR(0x005577ff), NULL);

// this is what we will read in /sys/class/net to collect network data
#define NET_INTERFACE_NAME "enp10s0"

// font
static const size_t fontcount = 2;
static const char *fontstr[2] = {
	"SourceCodePro:style=Semibold:size=13",
	"Symbols Nerd Font:size=14"
};
static const char * const bar_time_fmt = "%02d:%02d:%02d";
static const char * const bar_state_fmt = "󰛶%4s 󰛴%4s | 󱘾%4s 󱘻%4s | %3d°C | %3d%% | %3d%% | %3d%% %3d%% | %02d-%02d-%04d";
// tag names
static const char * const tags_names[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

// initially hide all bars
static const bool hidden = false;
// initially draw all bars at the bottom
static const bool bottom = false;
// hide vacant tags
static const bool hide_vacant = false;
// vertical pixel padding above and below text
static const uint32_t vertical_padding = 0;
// scale
static const uint32_t buffer_scale = 1;
