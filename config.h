#define HEX_COLOR(hex)				\
	{ .red   = ((hex >> 24) & 0xff) * 257,	\
	  .green = ((hex >> 16) & 0xff) * 257,	\
	  .blue  = ((hex >> 8) & 0xff) * 257,	\
	  .alpha = (hex & 0xff) * 257 }

// initially hide all bars
static const bool hidden = false;
// initially draw all bars at the bottom
static const bool bottom = false;
// hide vacant tags
static const bool hide_vacant = false;
// vertical pixel padding above and below text
static const uint32_t vertical_padding = 0;
// allow in-line color commands in status text
static const bool status_commands = true;
// scale
static const uint32_t buffer_scale = 1;
// font
static const size_t fontcount = 2;
static const char *fontstr[2] = {
	"SourceCodePro:style=Semibold:size=13",
	"Symbols Nerd Font:size=14"
};
static const char * const bar_time_fmt = "%02d:%02d:%02d";
static const char * const bar_state_fmt = "%3d%% | %02d-%02d-%04d";
// tag names
static const char * const tags_names[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

// set 16-bit colors for bar
// use either pixman_color_t struct or HEX_COLOR macro for 8-bit colors
static const pixman_color_t active_fg_color = HEX_COLOR(0xeeeeeeff);
static const pixman_color_t active_bg_color = HEX_COLOR(0x005577ff);
static const pixman_color_t occupied_fg_color = HEX_COLOR(0xeeeeeeff);
static const pixman_color_t occupied_bg_color = HEX_COLOR(0x005577ff);
static const pixman_color_t inactive_fg_color = HEX_COLOR(0xbbbbbbff);
static const pixman_color_t inactive_bg_color = HEX_COLOR(0x222222ff);
static const pixman_color_t urgent_fg_color = HEX_COLOR(0x222222ff);
static const pixman_color_t urgent_bg_color = HEX_COLOR(0xeeeeeeff);
static const pixman_color_t middle_bg_color = HEX_COLOR(0x222222ff);
static const pixman_color_t middle_bg_color_selected = HEX_COLOR(0x005577ff);
