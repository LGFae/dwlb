#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "commands.h"

#define TEXT_MAX 2048
#define PROGRAM "dwlb-ctl"
#define VERSION "0.1"

static char socketdir[256];
static char sockbuf[4096];
static char *socketpath;

static const char * const usage =
	"usage: dwlb-ctl [Command]\n"
	"Commands\n"
	"    -target-socket [SOCKET-NAME]    set the socket to send command to.\n"
	"                                    Sockets can be found in `$XDG_RUNTIME_DIR/dwlb/`\n"
	"    -status        [OUTPUT] [TEXT]  set status text\n"
	"    -status-stdin  [OUTPUT]         set status text from stdin\n"
	"    -show          [OUTPUT]         show bar\n"
	"    -hide          [OUTPUT]         hide bar\n"
	"    -toggle-visibility [OUTPUT]     toggle bar visibility\n"
	"    -set-top       [OUTPUT]         draw bar at the top\n"
	"    -set-bottom    [OUTPUT]         draw bar at the bottom\n"
	"    -toggle-location [OUTPUT]       toggle bar location\n"
	"\n"
	"  For every command, [OUTPUT] 'all' will apply the command on all outputs,\n"
	"  while 'selected' will apply to the current select output.\n"
	"Other\n"
	"    -v                              get version information\n"
	"    -h                              view this help text\n";


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
client_send_command(struct sockaddr_un *sock_address, const char *output,
		    enum Command cmd, const char *data, const char *target_socket)
{
	int sock_fd;
	size_t len;
	DIR *dir;
	if (!(dir = opendir(socketdir)))
		die("Could not open directory '%s':", socketdir);

	if (data)
		len = snprintf(sockbuf, sizeof(sockbuf), "%c%s %s", cmd, output, data);
	else
		len = snprintf(sockbuf, sizeof(sockbuf), "%c%s", cmd, output);

	struct dirent *de;
	bool newfd = true;

	/* Send data to all dwlb instances */
	while ((de = readdir(dir))) {
		if (!strncmp(de->d_name, "dwlb-", 5)) {
			if (!target_socket || !strncmp(de -> d_name, target_socket, 6)){
				if (newfd && (sock_fd = socket(AF_UNIX, SOCK_STREAM, 1)) == -1)
					die("socket:");
				snprintf(sock_address->sun_path, sizeof sock_address->sun_path, "%s/%s", socketdir, de->d_name);
				if (connect(sock_fd, (struct sockaddr *) sock_address, sizeof(*sock_address)) == -1) {
					newfd = false;
					continue;
				}
				if (send(sock_fd, sockbuf, len, 0) == -1)
					fprintf(stderr, "Could not send status data to '%s'\n", sock_address->sun_path);
				close(sock_fd);
				newfd = true;
			}
		}
	}

	closedir(dir);
}

int
main(int argc, char **argv)
{
	if (argc <= 1) {
		die("ERROR: missing command\n%s", usage);
	}
	char *xdgruntimedir;
	struct sockaddr_un sock_address;

	/* Establish socket directory */
	if (!(xdgruntimedir = getenv("XDG_RUNTIME_DIR")))
		die("Could not retrieve XDG_RUNTIME_DIR");
	snprintf(socketdir, sizeof socketdir, "%s/dwlb", xdgruntimedir);

	struct stat sb;
    if (!(stat(socketdir, &sb) == 0 && S_ISDIR(sb.st_mode)))
		die("there is no directory for the dwlb socket in: %s", xdgruntimedir);
    
	sock_address.sun_family = AF_UNIX;

	/* Parse options */
	char *target_socket = NULL;
	int i = 1;
	if (argc > 1 && !strcmp(argv[1], "-target-socket")) {
		if (2 >= argc) {
			die("Option -socket requires an argument");
		}
		target_socket = argv[2];
		i += 2;
	}
	if (!strcmp(argv[i], "-status")) {
		if (++i + 1 >= argc)
			die("Option -status requires two arguments");
		client_send_command(&sock_address, argv[i], CommandStatus, argv[i + 1], target_socket);
	} else if (!strcmp(argv[i], "-status-stdin")) {
		if (++i >= argc)
			die("Option -status-stdin requires an argument");
		char *status = malloc(TEXT_MAX * sizeof(char));
		while (fgets(status, TEXT_MAX-1, stdin)) {
			status[strlen(status)-1] = '\0';
			client_send_command(&sock_address, argv[i], CommandStatus, status, target_socket);
		}
		free(status);
	} else if (!strcmp(argv[i], "-show")) {
		if (++i >= argc)
			die("Option -show requires an argument");
		client_send_command(&sock_address, argv[i], CommandShow, NULL, target_socket);
	} else if (!strcmp(argv[i], "-hide")) {
		if (++i >= argc)
			die("Option -hide requires an argument");
		client_send_command(&sock_address, argv[i], CommandHide, NULL, target_socket);
	} else if (!strcmp(argv[i], "-toggle-visibility")) {
		if (++i >= argc)
			die("Option -toggle requires an argument");
		client_send_command(&sock_address, argv[i], CommandToggleVis, NULL, target_socket);
	} else if (!strcmp(argv[i], "-set-top")) {
		if (++i >= argc)
			die("Option -set-top requires an argument");
		client_send_command(&sock_address, argv[i], CommandSetTop, NULL, target_socket);
	} else if (!strcmp(argv[i], "-set-bottom")) {
		if (++i >= argc)
			die("Option -set-bottom requires an argument");
		client_send_command(&sock_address, argv[i], CommandSetBot, NULL, target_socket);
	} else if (!strcmp(argv[i], "-toggle-location")) {
		if (++i >= argc)
			die("Option -toggle-location requires an argument");
		client_send_command(&sock_address, argv[i], CommandToggleLoc, NULL, target_socket);
	} else if (!strcmp(argv[i], "-v")) {
		printf(PROGRAM " " VERSION "\n");
	} else if (!strcmp(argv[i], "-h")) {
		printf(usage);
	} else {
		die("Option '%s' not recognized\n%s", argv[i], usage);
	}

	return 0;
}
