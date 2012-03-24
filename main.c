#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <linux/parport.h>
#include <linux/ppdev.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>

#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_LISTEN_PORT 7613
#define DEFAULT_SOCK_PATH "./parserver.sock"


static int running = 1;
static int port = DEFAULT_LISTEN_PORT;
static in_addr_t ip = 0;
static int verbose_flag = 0;

static struct option long_options[] = {
	{"port",    required_argument, 0, 'p'},
	{"listen",  optional_argument, 0, 'l'},
	{"path",    required_argument, 0, 's'},
	{"verbose", no_argument,       0, 'v'},
	{"help",    no_argument,       0, 'h'},
	{0,0,0,0} /* sentinel */
};

void show_usage(void){
	printf("(C) 2011 David Sveningsson <ext@sidvind.com>\n");
	printf("usage: parserver [OPTIONS] DEVICE\n");
	printf("  -p, --port=PORT    Listen on port [default: %d]\n"
	       "  -l, --listen=IP    Listen on on ip [default: 127.0.0.1]\n"
	       "  -s, --path=FILE    Path to Unix Domain Socket. [default: %s]\n"
	       "  -v, --verbose      Enable verbose output.\n"
	       "  -h, --help         This text.\n"
	       "\n"
	       "If neither -l or -p is given it listens on unix domain socket.\n"
	       "Device is usually /dev/parport0\n"
	       , DEFAULT_LISTEN_PORT, DEFAULT_SOCK_PATH);
}

void sigint_handler(int sig){
	running = 0;
	putc('\r', stderr);
}

static int port_open(const char* port){
	// Open the parallel port for reading and writing
	int fd = open(port, O_RDWR);

	if ( fd == -1 ){
		perror("Failed to open parallel port");
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

int main(int argc, char* argv[]){
	ip = inet_addr("127.0.0.1");
	const char* sock_path = DEFAULT_SOCK_PATH;
	int exit_code = 1;

	int option_index = 0;
	int op;

	while ( (op = getopt_long(argc, argv, "hvp:l::s:", long_options, &option_index)) != -1 )
		switch (op){
		case 0: /* longopt with flag */
		case '?': /* unknown */
			break;

		case 'p': /* --port */
			port = atoi(optarg);
			break;

		case 'l': /* --listen */
			ip = inet_addr("127.0.0.1");
			if ( optarg ){
				ip = inet_addr(optarg);
			}
			break;

		case 's': /* --path */
			sock_path = optarg;
			break;

		case 'v': /* --verbose */
			verbose_flag = 1;
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

	fprintf(stderr, "Parallel port interface server\n");

	/* handle signals */
	signal(SIGINT, sigint_handler);

	/* open listening socket */
	struct sockaddr_un addr = {0,};
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if ( sock == -1 ){
		perror("Failed to create socket");
		return 1;
	}
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
				return 1;
			}
		}
		break;
	}
	if ( retry == 3 ){ /* failed to bind */
		fprintf(stderr, "failed to bind socket\n");
		exit(1);
	}
	if ( listen(sock, 1) == -1){
		perror("failed to listen");
	}

	/* fix permissions on socket */
	chmod(addr.sun_path, 0666);

	/* enable verbose mode */
	FILE* verbose = fopen(verbose_flag ? "/dev/stderr" : "/dev/null", "w");

	int fd;
	fprintf(stderr, "Opening device %s\n", device);
	if ( (fd=port_open(device)) == -1 ){
		goto socket_cleanup; /* error already shown */
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

		if ( strcmp(cmd, "set") == 0 ){
			char* pin_str = strtok(NULL, " ");
			char* action = strtok(NULL, " ");
			int pin = 1 << (pin_str[1] - '0');

			if ( strcmp(action, "hi") == 0 ){
				dataL |= pin;
			} else if ( strcmp(action, "low") == 0 ){
				dataL &= ~pin;
			} else if ( strcmp(action, "toggle") == 0 ){
				dataL ^= pin;
			}
		} else if ( strcmp(cmd, "strobe") == 0 ){
			char* pin_str = strtok(NULL, " ");
			char* time_str = strtok(NULL, " ");
			int pin = 1 << (pin_str[1] - '0');
			int time = atoi(time_str) * 1000; /* to µs */

			dataL |= pin;
			ioctl(fd, PPWDATA, &dataH);
			ioctl(fd, PPWDATA, &dataL);
			usleep(time);
			dataL &= ~pin;
		} else {
			fprintf(verbose, "unknown command: %s\n", cmd);
			bytes = snprintf(buffer, 128, "0;unknown command: %s", cmd);
			send(client, buffer, (int)bytes, MSG_NOSIGNAL);
		}

		free(cmd);

		shutdown(client, SHUT_RDWR);
	}

	fprintf(stderr, "Shutting down\n");
	exit_code = 0;

	// Release and close the parallel port
	ioctl(fd, PPRELEASE);
	close(fd);

  socket_cleanup:
	unlink(sock_path);

	return exit_code;
}
