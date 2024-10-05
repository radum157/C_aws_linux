// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>
#include <time.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

#define CRLF "\r\n"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

/**
 * Number of digits
 */
__attribute__((visibility("internal")))
int _l_get_digits(long number)
{
	int digits = 0;

	do {
		digits++;
		number /= 10;
	} while (number);

	return digits;
}

/**
 * Long to string. Result should be freed
 */
char *ltoa(long number)
{
	int len = _l_get_digits(number);
	char *str = (char *)malloc(len + 1);

	DIE(!str, "malloc failed");
	str[len] = '\0';

	do {
		str[--len] = number % 10 + '0';
		number /= 10;
	} while (number);

	return str;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	struct stat fstats;
	int rc = stat(conn->filename, &fstats);

	DIE(rc < 0, "stat failed");

	/* Build header */
	strcat(conn->send_buffer, ("HTTP/1.1 200 OK" CRLF));
	strcat(conn->send_buffer, ("Accept-Ranges: bytes" CRLF));

	char *content_length = ltoa(fstats.st_size);

	strcat(conn->send_buffer, "Content-Length: ");
	strcat(conn->send_buffer, content_length);
	free(content_length);

	strcat(conn->send_buffer, (CRLF "Connection: close" CRLF));

	/* End of header */
	strcat(conn->send_buffer, CRLF);

	conn->send_len = strlen(conn->send_buffer);
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* Build header */
	strcat(conn->send_buffer, ("HTTP/1.1 404 Not Found" CRLF));
	strcat(conn->send_buffer, ("Content-Length: 0" CRLF));
	strcat(conn->send_buffer, ("Connection: close" CRLF));

	/* End of header */
	strcat(conn->send_buffer, CRLF);

	conn->send_len = strlen(conn->send_buffer);
	conn->fd = -1;
}

/**
 * Safe strcpy for BUFSIZ arrays
 */
size_t strscpy(char dest[BUFSIZ], const char *restrict src, size_t size)
{
	if (size <= 0)
		return -EXIT_FAILURE;

	size_t dest_size = BUFSIZ;
	size_t len = strlen(src);

	if (len > size)
		len = size;

	if (len >= dest_size)
		len = dest_size - 1;

	memcpy(dest, src, len);
	dest[len] = '\0';

	return len;
}

/**
 * Also sets filename and checks it exists
 */
static enum resource_type connection_get_resource_type(struct connection *conn)
{
	strscpy(conn->filename, AWS_DOCUMENT_ROOT, BUFSIZ);

	char *found = strstr(conn->request_path, AWS_REL_STATIC_FOLDER);

	if (found)
		strscpy(conn->filename + 2, found, BUFSIZ);

	found = strstr(conn->request_path, AWS_REL_DYNAMIC_FOLDER);

	if (found)
		strscpy(conn->filename + 2, found, BUFSIZ);

	if (strcmp(conn->filename, "./") == 0 || access(conn->filename, F_OK) < 0)
		return RESOURCE_TYPE_NONE;

	/* found was last used for dynamic */
	return (found) ? RESOURCE_TYPE_DYNAMIC : RESOURCE_TYPE_STATIC;
}

/**
 * Also initialises the fields
 */
struct connection *connection_create(int sockfd)
{
	struct connection *conn =
		(struct connection *)malloc(sizeof(struct connection));

	DIE(conn == NULL, "malloc failed");
	memset(conn, 0, sizeof(struct connection));

	conn->ctx = ctx;
	conn->sockfd = sockfd;
	conn->state = STATE_INITIAL;
	conn->piocb[0] = &conn->iocb;

	return conn;
}

/**
 * Starts reading from fd
 */
void connection_start_async_io(struct connection *conn)
{
	io_prep_pread(
		&conn->iocb,
		conn->fd,
		conn->recv_buffer,
		BUFSIZ,
		conn->recv_len
	);

	int rc = io_submit(conn->ctx, 1, conn->piocb);

	DIE(rc < 0, "io submit failed");
}

/**
 * Also frees
 */
void connection_remove(struct connection *conn)
{
	conn->state = STATE_NO_STATE;

	close(conn->sockfd);
	w_epoll_remove_ptr(epollfd, conn->sockfd, conn);

	free(conn);
}

void handle_new_connection(void)
{
	/* Accept new connection */
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	int sockfd = accept(listenfd, (SSA *)&addr, &addrlen);

	DIE(sockfd < 0, "connection could not be accepted");

	/* Non-blocking */
	int flags = fcntl(sockfd, F_GETFL);

	fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

	/* Add socket */
	struct connection *conn = connection_create(sockfd);
	int rc = w_epoll_add_ptr_inout(epollfd, sockfd, conn);

	DIE(rc < 0, "epoll addition failed");

	/* Initialise parser */
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;
}

void receive_data(struct connection *conn)
{
	ssize_t read_size = recv(
		conn->sockfd,
		conn->recv_buffer + conn->recv_len,
		BUFSIZ - conn->recv_len,
		0
	);

	/* Nothing left to read */
	if (read_size == 0) {
		conn->state = STATE_REQUEST_RECEIVED;
		return;
	}

	DIE(read_size < 0, "recv failed");
	conn->recv_len += read_size;
}

/**
 * Opens the file specified in the filename field for reading
 *
 * @return The fd
 */
int connection_open_file(struct connection *conn)
{
	conn->fd = open(conn->filename, O_RDONLY);
	if (conn->fd < 0)
		return -1;

	struct stat fstats;

	fstat(conn->fd, &fstats);
	conn->file_size = fstats.st_size;

	return conn->fd;
}

/**
 * Completes async by writing to sockfd
 */
void connection_complete_async_io(struct connection *conn)
{
	io_prep_pwrite(
		&conn->iocb,
		conn->sockfd,
		conn->send_buffer,
		conn->send_len,
		0
	);

	int rc = io_submit(conn->ctx, 1, conn->piocb);

	DIE(rc < 0, "io submit failed");
}

int parse_header(struct connection *conn)
{
	/* Use mostly null settings except for on_path callback. */
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};

	ssize_t parse_size = http_parser_execute(
		&conn->request_parser,
		&settings_on_path,
		conn->recv_buffer,
		conn->recv_len
	);

	return (parse_size < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}

enum connection_state connection_send_static(struct connection *conn)
{
	off_t offset = conn->file_pos;
	ssize_t sent_bytes = sendfile(
		conn->sockfd,
		conn->fd,
		&offset,
		conn->file_size
	);

	conn->file_pos += sent_bytes;

	/* All was sent */
	if (conn->file_pos == conn->file_size) {
		close(conn->fd);
		return STATE_DATA_SENT;
	}

	return STATE_SENDING_DATA;
}

/**
 * Helper. Sends synchronously! Used for headers
 */
int connection_send_data(struct connection *conn)
{
	ssize_t sent_bytes = send(
		conn->sockfd,
		conn->send_buffer + conn->send_pos,
		conn->send_len,
		0
	);

	DIE(sent_bytes < 0, "send failed");

	if (sent_bytes < conn->send_len) {
		conn->send_len -= sent_bytes;
		conn->send_pos += sent_bytes;

		return EXIT_FAILURE;
	}

	/* All was sent */
	conn->send_pos = 0;
	return EXIT_SUCCESS;
}

/**
 * Sends dynamic files asynchronously
 */
int connection_send_dynamic(struct connection *conn)
{
	static struct timespec timeout = { 0, 0 };
	struct io_event ev;
	int rc = io_getevents(conn->ctx, 0, 1, &ev, &timeout);

	DIE(rc < 0, "getevents failed");

	/* Keep waiting */
	if (rc == 0)
		return EXIT_FAILURE;

	/* All was read and written */
	if (ev.res == 0) {
		close(conn->eventfd);
		w_epoll_remove_ptr(epollfd, conn->eventfd, conn);

		return EXIT_SUCCESS;
	}

	/* Read finished */
	if (ev.obj->aio_fildes == conn->fd) {
		conn->recv_len += ev.res;
		conn->send_len = ev.res;

		memset(conn->send_buffer, 0, BUFSIZ);
		memcpy(conn->send_buffer, conn->recv_buffer, ev.res);

		connection_complete_async_io(conn);
		return EXIT_FAILURE;
	}

	/* Write finished */
	conn->state = STATE_ASYNC_ONGOING;
	return EXIT_FAILURE;
}

/**
 * Handle receiveing operations
 */
void handle_input(struct connection *conn)
{
	switch (conn->state) {
	case STATE_INITIAL:
		conn->state = STATE_RECEIVING_DATA;
		break;

	case STATE_RECEIVING_DATA:
		receive_data(conn);
		break;

	case STATE_REQUEST_RECEIVED:
		DIE(parse_header(conn) == EXIT_FAILURE, "could not fully parse");

		conn->res_type = connection_get_resource_type(conn);
		conn->state = (conn->res_type == RESOURCE_TYPE_NONE)
			? STATE_SENDING_404 : STATE_SENDING_HEADER;
		break;

	default:
		/* Trickle down */
		handle_output(conn);
		return;
	}
}

/**
 * Open files and set next state after sending header (NOT 404)
 */
static void connection_prepare_send_data(struct connection *conn)
{
	DIE(connection_open_file(conn) < 0, "open failed");
	conn->state = STATE_SENDING_DATA;

	if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
		conn->state = STATE_ASYNC_ONGOING;
		conn->recv_len = 0;

		conn->eventfd = eventfd(0, EFD_NONBLOCK);
		DIE(conn->eventfd < 0, "eventfd failed");
		io_set_eventfd(&conn->iocb, conn->eventfd);
	}
}

/**
 * Handle responding
 */
void handle_output(struct connection *conn)
{
	switch (conn->state) {
		/* Safeguard */
	case STATE_RECEIVING_DATA:
		conn->state = STATE_REQUEST_RECEIVED;
	case STATE_REQUEST_RECEIVED:
		handle_input(conn);
	case STATE_INITIAL:
		break;

		/**
		 * ? The order is:
		 *
		 * sending header / 404: prepare buffer
		 * header / 404 sent: send header
		 * async ongoing: need to start reading
		 * sending data: check for aio operations succeeding and update
		 * data sent = connection closed
		 * connection closed: remove connection
		 */
	case STATE_SENDING_HEADER:
		connection_prepare_send_reply_header(conn);
		conn->state = STATE_HEADER_SENT;
		break;

	case STATE_HEADER_SENT:
		/* Ensure header is fully sent */
		if (connection_send_data(conn) == EXIT_FAILURE)
			break;

		connection_prepare_send_data(conn);
		break;

	case STATE_ASYNC_ONGOING:
		connection_start_async_io(conn);
		conn->state = STATE_SENDING_DATA;
		break;

	case STATE_SENDING_DATA:
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			conn->state = connection_send_static(conn);
		} else if (connection_send_dynamic(conn) == EXIT_SUCCESS) {
			close(conn->fd);
			conn->state = STATE_DATA_SENT;
		}
		break;

	case STATE_SENDING_404:
		connection_prepare_send_404(conn);
		conn->state = STATE_404_SENT;
		break;

	case STATE_404_SENT:
		/* Ensure header is fully sent */
		if (connection_send_data(conn) == EXIT_FAILURE)
			break;

	case STATE_DATA_SENT:
	case STATE_CONNECTION_CLOSED:
		connection_remove(conn);
		break;

	default:
		ERR("Unexpected state\n");
		printf("%d\n", conn->state);
		exit(EXIT_FAILURE);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	if (event & EPOLLIN) {
		handle_input(conn);
		return;
	}

	handle_output(conn);
}

int main(void)
{
	/* Async io */
	int rc = io_setup(10, &ctx);

	DIE(rc < 0, "io setup failed");

	/* Multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "epoll creation failed");

	/* Server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "listener creation failed");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "failed addition to epoll");

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "epoll wait failed");

		/* New connection */
		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN)
				handle_new_connection();
			continue;
		}

		handle_client(rev.events, rev.data.ptr);
	}

	close(listenfd);
	io_destroy(ctx);
	return 0;
}
