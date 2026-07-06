/* w3ldctl — control client for w3ld.
 *
 * Joins its arguments into one command, sends it to the compositor's control
 * socket, and prints the reply. Silent on "ok"; prints pong / query results /
 * errors. Exits non-zero on an error reply.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main (
	int argc,
	char *argv[]
) {
	if (argc < 2) {
		fprintf(stderr, "usage: w3ldctl <command> [args...]\n");
		return 2;
	}

	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (!runtime || !display) {
		fprintf(stderr, "w3ldctl: XDG_RUNTIME_DIR or WAYLAND_DISPLAY unset\n");
		return 2;
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("w3ldctl: socket");
		return 2;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int written = snprintf(addr.sun_path, sizeof addr.sun_path,
			"%s/w3ld-%s.sock", runtime, display);
	if (written < 0 || (size_t)written >= sizeof addr.sun_path) {
		fprintf(stderr, "w3ldctl: socket path too long\n");
		return 2;
	}
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		perror("w3ldctl: connect");
		return 2;
	}

	char message[1024];
	size_t length = 0;
	for (int i = 1; i < argc; i++) {
		int written = snprintf(message + length, sizeof message - length,
				"%s%s", i > 1 ? " " : "", argv[i]);
		if (written < 0 || (size_t)written >= sizeof message - length) {
			fprintf(stderr, "w3ldctl: command too long\n");
			return 2;
		}
		length += written;
	}
	message[length++] = '\n';
	if (write(fd, message, length) < 0) {
		perror("w3ldctl: write");
		return 2;
	}

	/* subscribe streams until the compositor closes the connection. */
	if (!strcmp(argv[1], "subscribe")) {
		char stream[4096];
		ssize_t got;
		while ((got = read(fd, stream, sizeof stream)) > 0) {
			if (write(STDOUT_FILENO, stream, got) < 0)
				break;
		}
		close(fd);
		return 0;
	}

	char reply[8192];
	ssize_t count = read(fd, reply, sizeof reply - 1);
	close(fd);
	if (count <= 0)
		return 0;
	reply[count] = '\0';

	if (strncmp(reply, "error", 5) == 0) {
		fprintf(stderr, "%s\n", reply);
		return 1;
	}
	if (strcmp(reply, "ok") != 0)
		printf("%s\n", reply);
	return 0;
}
