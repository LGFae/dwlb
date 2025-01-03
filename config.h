#define HEX_COLOR(hex)				\
	{ .red   = ((hex >> 24) & 0xff) * 257,	\
	  .green = ((hex >> 16) & 0xff) * 257,	\
	  .blue  = ((hex >> 8) & 0xff) * 257,	\
	  .alpha = (hex & 0xff) * 257 }

#define DECLARE_COLOR(name, foreground, background) \
	static const Color name ## _color = { foreground, background }

// set 16-bit colors for bar
// use either pixman_color_t struct or HEX_COLOR macro for 8-bit colors
DECLARE_COLOR(time,       HEX_COLOR(0x080410ff), HEX_COLOR(0x936dd3ee));
DECLARE_COLOR(active,     HEX_COLOR(0x040810ff), HEX_COLOR(0x417ebaee));
DECLARE_COLOR(occupied,   HEX_COLOR(0x040810ff), HEX_COLOR(0x417ebaee));
DECLARE_COLOR(inactive,   HEX_COLOR(0xbbbbbbff), HEX_COLOR(0x222222ff));
DECLARE_COLOR(urgent,     HEX_COLOR(0x222222ff), HEX_COLOR(0xeeeeeeff));
DECLARE_COLOR(middle,     HEX_COLOR(0x9b9b9bff), HEX_COLOR(0x2f4e6aee));
DECLARE_COLOR(middle_sel, HEX_COLOR(0x040810ff), HEX_COLOR(0x417ebaee));

// this is what we will read in /sys/class/net to collect network data
#define NET_INTERFACE_NAME "enp10s0"

// font
#define FONTCOUNT (2)
static const char *fontstr[FONTCOUNT] = {
	"SourceCodePro:style=Semibold:size=13",
	"Symbols Nerd Font:size=14"
};
static const char * const bar_alsa_fmt = "%3d%% %3d%% ";
static const char * const bar_time_fmt = "%02d:%02d:%02d";
static const char * const bar_state_fmt = "󰛶%4s 󰛴%4s | 󱘾%4s 󱘻%4s | %3d°C | %3d%% | %3d%% |";
static const char * const bar_date_fmt = "%02d-%02d-%04d";

static const char* const vol_up_cmd =   "amixer -q set Master 1%+";
static const char* const vol_down_cmd = "amixer -q set Master 1%-";
static const char* const mic_up_cmd =   "amixer -q set Capture 1%+";
static const char* const mic_down_cmd = "amixer -q set Capture 1%-" ;

// tags
#define TAGCOUNT (9)
static const char tags[TAGCOUNT * 2] = { "1\0002\0003\0004\0005\0006\0007\0008\0009\000" };

// layouts
#define LAYOUTCOUNT (3)
static char layouts[LAYOUTCOUNT][4];

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
