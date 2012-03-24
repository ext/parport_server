#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <linux/parport.h>
#include <linux/ppdev.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>

#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_LISTEN_PORT 7613
#define DEFAULT_SOCK_PATH "parserver.sock"
#define DEFAULT_PID_PATH "parserver.pid"
#define VERSION "1.8"

static int running = 1;
static int use_daemon = 0;
static int use_tcp = 0;
static int port = 0;
static in_addr_t ip = 0;
static int quiet_flag = 0;
static int verbose_flag = 0;
static FILE* verbose = NULL;

static struct option long_options[] = {
	{"port",    required_argument, 0, 'p'},
	{"listen",  optional_argument, 0, 'l'},
	{"path",    required_argument, 0, 's'},
	{"pidfile", required_argument, 0, 'n'},
	{"daemon",  no_argument,       0, 'd'},
	{"verbose", no_argument,       0, 'v'},
	{"quiet",   no_argument,       0, 'q'},
	{"help",    no_argument,       0, 'h'},
	{0,0,0,0} /* sentinel */
};

void show_usage(void){
	printf("parserver-" VERSION " Parallel port interface server.\n"
	       "(C) 2011-2012 David Sveningsson <ext@sidvind.com>\n\n");
	printf("usage: parserver [OPTIONS] DEVICE\n");
	printf("  -p, --port=PORT    Port to use for TCP [default: %d]\n"
	       "  -l, --listen[=IP]  Listen on TCP [default ip: 127.0.0.1]\n"
	       "  -s, --path=FILE    Path to Unix Domain Socket. [default: " DEFAULT_SOCK_PATH "]\n"
	       "  -n, --pidfile=FILE Path to pidfile (when using --daemon) [default: " DEFAULT_PID_PATH "\n"
	       "  -d, --daemon       Fork to background.\n"
	       "  -v, --verbose      Enable verbose output.\n"
	       "  -q, --quiet        Enable quiet output.\n"
	       "  -h, --help         This text.\n"
	       "\n"
	       "Unless -l is given it listens on unix domain socket.\n"
	       "Device is usually /dev/parport0\n"
	       , DEFAULT_LISTEN_PORT);
}

void signal_handler(int sig){
	if ( !running ){
		fprintf(stderr, "\rgot signal %d again, aborting\n", sig);
		abort();
	}
	if ( !quiet_flag ){
		fprintf(stderr, "\rgot signal %d\n", sig);
	}
	running = 0;
}

static int port_open(const char* port){
	// Open the parallel port for reading and writing
	int fd = open(port, O_RDWR);

	if ( fd == -1 ){
		fprintf(stderr, "Failed to open parallel port `%s': %s\n", port, strerror(errno));
		return -1;
	}

	// Try to claim port
	if ( ioctl(fd, PPCLAIM, NULL) ){
		perror("Could not claim parallel port");
		close(fd);
		return -1;
	}

	// Set the Mode
	static const int mode = IEEE1284_MODE_BYTE;
	if ( ioctl(fd, PPSETMODE, &mode) ){
		perror("Could not set mode");
		ioctl(fd, PPRELEASE);
		close(fd);
		return -1;
	}

	// Set data pins to output
	static const int dir = 0x00;
	if ( ioctl(fd, PPDATADIR, &dir) ){
		perror("Could not set parallel port direction");
		ioctl(fd, PPRELEASE);
		close(fd);
		return -1;
	}

	return fd;
}

static int open_domainsocket(const char* sock_path){
	struct sockaddr_un addr = {0,};
	/* Open socket */
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if ( sock == -1 ){
		perror("Failed to create socket");
		return -1;
	}

	/* Try to bind socket and if it fails it tries to connect to the socket to
	 * determine if it is running or not. If a dead socket is detected it tries to
	 * remove it and retries. */
	int retry = 0;
	while ( retry++ < 3 ){ /* retry at most 2 times */
		addr.sun_family = AF_UNIX;
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
		if ( bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == -1 ){
			if ( errno == EADDRINUSE ){ /* adress already used, try to connect to the socket */
				if ( connect(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == 0 ){
					fprintf(stderr, "daemon already running\n");
					exit(0);
				} else {
					/* could not connect, assume it is dead */
					fprintf(stderr, "dead daemon deteched, removing socket\n");
					unlink(addr.sun_path);
					continue;
				}
			} else {
				perror("failed to bind socket");
				return -1;
			}
		}
		break;
	}

	if ( retry == 3 ){
		/* Failed to bind */
		fprintf(stderr, "failed to bind socket\n");
		return -1;
	}

	/* Setup listen */
	if ( listen(sock, 1) == -1){
		perror("failed to listen");
		unlink(addr.sun_path);
		return -1;
	}

	/* fix permissions on socket */
	chmod(addr.sun_path, 0666);

	fprintf(verbose, "Listening on socket `%s'.\n", addr.sun_path);
	return sock;
}

static int open_tcp(in_addr_t ip, int port){
	struct sockaddr_in addr;

	/* bind and listen to port */
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if ( sock == -1 ){
		perror("socket");
		return -1;
	}

	int optval = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ip;
	addr.sin_port = port;

	if ( bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) == -1 ){
		perror("bind");
		return -1;
	}

	if ( listen(sock, 3) == -1 ){
		perror("listen");
		return -1;
	}

	fprintf(verbose, "Listening on TCP %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
	return sock;
}

int main(int argc, char* argv[]){
	ip = inet_addr("127.0.0.1");
	port = htons(DEFAULT_LISTEN_PORT);
	const char* sock_path = DEFAULT_SOCK_PATH;
	const char* pid_path = DEFAULT_PID_PATH;
	int exit_code = 1;

	int option_index = 0;
	int op;

	while ( (op = getopt_long(argc, argv, "hvqdn:p:l::s:", long_options, &option_index)) != -1 )
		switch (op){
		case 0: /* longopt with flag */
		case '?': /* unknown */
			break;

		case 'p': /* --port */
			port = htons(atoi(optarg));
			break;

		case 'l': /* --listen */
			use_tcp = 1;
			ip = inet_addr("127.0.0.1");
			if ( optarg ){
				ip = inet_addr(optarg);
			} else if ( optind < argc && isdigit(argv[optind][0]) ){ /* workaround for arguments to optional parameters */
				ip = inet_addr(argv[optind]);
			}
			break;

		case 's': /* --path */
			sock_path = optarg;
			break;

		case 'n': /* --pidfile */
			pid_path = optarg;
			break;

		case 'd': /* --daemon */
			use_daemon = 1;
			break;

		case 'v': /* --verbose */
			verbose_flag = 1;
			break;

		case 'q': /* --quiet */
			quiet_flag = 1;
			break;

		case 'h': /* --help */
			show_usage();
			exit(0);

		default:
			fprintf(stderr, "declared but unhandled argument -%c\n", op);
			break;
		}

	if ( optind == argc ){
		fprintf(stderr, "Missing device, see --help for usage.\n");
		exit(1);
	}

	const char* device = argv[optind];

	/* text */
	if ( !quiet_flag ) fprintf(stderr, "parserver-" VERSION " Parallel port interface server.\n"
	                           "(C) 2011-2012 David Sveningsson <ext@sidvind.com>\n\n");

	/* handle signals */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* enable verbose mode */
	verbose = fopen(verbose_flag ? "/dev/stderr" : "/dev/null", "w");

	/* open listening socket */
	int sock;
	if ( use_tcp == 0 && (sock=open_domainsocket(sock_path)) == -1 ){
		return 1; /* error already shown */
	} else if ( use_tcp == 1 && (sock=open_tcp(ip, port)) == -1 ){
		return 1; /* error already shown */
	}

	int fd;
	fprintf(verbose, "Opening device %s\n", device);
	if ( (fd=port_open(device)) == -1 ){
		goto socket_cleanup; /* error already shown */
	}

	if ( use_daemon ){
		pid_t pid = fork();

		/* store pid in file */
		if ( pid ){ /* parent */
			FILE* fp = fopen(pid_path, "w");
			if ( !fp ){
				fprintf(stderr, "Failed to write pidfile `%s': %s (child is still running as %d)\n", pid_path, strerror(errno), pid);
				return 1;
			}
			fprintf(fp, "%d\n", pid);
			fclose(fp);
			return 0;
		}
	}

	/* all pins low */
	char dataH = 0x00;
	char dataL = 0x00;

	while ( running ){
		fprintf(verbose, "current output: 0x%02x\n", 0xFF & dataL);
		ioctl(fd, PPWDATA, &dataH);
		ioctl(fd, PPWDATA, &dataL);

		/* wait until a client connects */
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(sock, &fds);
		if ( select(sock+1, &fds, NULL, NULL, NULL) == -1 ){
			if ( errno != EINTR ){
				perror("select");
			}
			continue;
		}

		struct sockaddr_un peer;
		socklen_t len = sizeof(struct sockaddr_un);
		int client = accept(sock, (struct sockaddr*)&peer, &len);

		if ( client == -1 ){
			perror("accept error");
			running = 0;
			continue;
		}

		char buffer[128] = {0,};
		ssize_t bytes = recv(client, buffer, sizeof(buffer), 0);
		if ( bytes < 0 ){
			fprintf(stderr, "failed to read client data: %s\n", strerror(errno));
			shutdown(client, SHUT_RDWR);
			continue;
		}

		if ( buffer[bytes-1] == '\n' ){ /* remove trailing newline */
			buffer[--bytes] = 0;
		}

		fprintf(verbose, "client data: %*s\n", (int)bytes, buffer);

		char* cmd = strtok(buffer, " ");
		if ( !cmd ){
			cmd = buffer;
		}
		cmd = strdup(cmd);

		if ( strcasecmp(cmd, "set") == 0 ){
			char* pin_str = strtok(NULL, " ");
			char* action = strtok(NULL, " ");
			int pin = 1 << (pin_str[1] - '0');

			if ( strcasecmp(action, "hi") == 0 ){
				dataL |= pin;
			} else if ( strcasecmp(action, "low") == 0 ){
				dataL &= ~pin;
			} else if ( strcasecmp(action, "toggle") == 0 ){
				dataL ^= pin;
			}
			bytes = snprintf(buffer, 128, "1;ok\n");
			send(client, buffer, (int)bytes, MSG_NOSIGNAL);
		} else if ( strcasecmp(cmd, "strobe") == 0 ){
			char* pin_str = strtok(NULL, " ");
			char* time_str = strtok(NULL, " ");
			int pin = 1 << (pin_str[1] - '0');
			int time = atoi(time_str) * 1000; /* to µs */

			dataL |= pin;
			ioctl(fd, PPWDATA, &dataH);
			ioctl(fd, PPWDATA, &dataL);
			usleep(time);
			dataL &= ~pin;
			bytes = snprintf(buffer, 128, "1;ok\n");
			send(client, buffer, (int)bytes, MSG_NOSIGNAL);
		} else {
			fprintf(verbose, "unknown command: %s\n", cmd);
			bytes = snprintf(buffer, 128, "0;unknown command: %s\n", cmd);
			send(client, buffer, (int)bytes, MSG_NOSIGNAL);
		}

		free(cmd);
		shutdown(client, SHUT_RDWR);
	}

	if ( !quiet_flag ) fprintf(stderr, "Shutting down\n");
	exit_code = 0;

	// Release and close the parallel port
	ioctl(fd, PPRELEASE);
	close(fd);

  socket_cleanup:
	unlink(sock_path);

	return exit_code;
}
