#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

#define MAX_IOV 8
#define MAX_MESSAGE_SIZE (64 * 1024)

static int
socket_type(int socket)
{
	int type;
	socklen_t size = sizeof(type);
	return getsockopt(socket, SOL_SOCKET, SO_TYPE, &type, &size) < 0 ? -1 : type;
}

static bool
message_size(struct iovec *iov, int count, uint32_t *size)
{
	*size = 0;
	if (count < 1 || count > MAX_IOV) {
		errno = EINVAL;
		return false;
	}
	for (int i = 0; i < count; ++i) {
		if (iov[i].iov_len > MAX_MESSAGE_SIZE - *size) {
			errno = EMSGSIZE;
			return false;
		}
		*size += iov[i].iov_len;
	}
	if (!*size) {
		errno = EINVAL;
		return false;
	}
	return true;
}

ssize_t
send_fd(int socket, int fd, struct iovec *iov, int iovlen)
{
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(fd))];
	} control = {0};
	struct iovec vectors[MAX_IOV + 1];
	struct msghdr message = {0};
	uint32_t size;
	int type = socket_type(socket);

	if (type < 0 || !message_size(iov, iovlen, &size))
		return -1;
#ifdef SO_NOSIGPIPE
	int no_sigpipe = 1;
	if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
	               sizeof(no_sigpipe)) < 0)
		return -1;
#endif
	/* Stream sockets need framing. Sequence packets retain their existing ABI. */
	int first = type == SOCK_STREAM ? 1 : 0;
	if (first)
		vectors[0] = (struct iovec){&size, sizeof(size)};
	memcpy(vectors + first, iov, (size_t)iovlen * sizeof(*iov));
	message.msg_iov = vectors;
	message.msg_iovlen = iovlen + first;
	if (fd >= 0) {
		message.msg_control = control.bytes;
		message.msg_controllen = sizeof(control.bytes);
		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
		cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
	}
	size_t remaining = size + (first ? sizeof(size) : 0);
	while (remaining) {
		ssize_t sent;
		do {
			sent = sendmsg(socket, &message, MSG_NOSIGNAL);
		} while (sent < 0 && errno == EINTR);
		if (sent <= 0) {
			if (!sent)
				errno = EPIPE;
			return -1;
		}
		remaining -= (size_t)sent;
		if (type != SOCK_STREAM) {
			if (remaining) {
				errno = EIO;
				return -1;
			}
			break;
		}
		/* SCM_RIGHTS accompanies the first byte, never a continuation. */
		message.msg_control = NULL;
		message.msg_controllen = 0;
		size_t consumed = (size_t)sent;
		while (message.msg_iovlen && consumed >= message.msg_iov[0].iov_len) {
			consumed -= message.msg_iov[0].iov_len;
			++message.msg_iov;
			--message.msg_iovlen;
		}
		if (message.msg_iovlen) {
			message.msg_iov[0].iov_base =
			    (char *)message.msg_iov[0].iov_base + consumed;
			message.msg_iov[0].iov_len -= consumed;
		}
	}
	return size;
}

/* An incomplete stream frame must not wait indefinitely after readability. */
static bool
receive_bytes(int socket, void *data, size_t size)
{
	while (size) {
		struct pollfd pfd = {.fd = socket, .events = POLLIN};
		int ready;
		do {
			ready = poll(&pfd, 1, 5000);
		} while (ready < 0 && errno == EINTR);
		if (ready <= 0) {
			if (!ready)
				errno = ETIMEDOUT;
			return false;
		}
		ssize_t got;
		do {
			got = recv(socket, data, size, 0);
		} while (got < 0 && errno == EINTR);
		if (got <= 0) {
			if (!got)
				errno = EPROTO;
			return false;
		}
		data = (char *)data + got;
		size -= (size_t)got;
	}
	return true;
}

ssize_t
receive_fd(int socket, int *fd, struct iovec *iov, int iovlen)
{
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(int))];
	} control = {0};
	uint32_t capacity, length = 0;
	int type = socket_type(socket), received = -1;
	struct iovec header = {&length, sizeof(length)};
	struct msghdr message = {
		.msg_iov = type == SOCK_STREAM ? &header : iov,
		.msg_iovlen = type == SOCK_STREAM ? 1 : iovlen,
		.msg_control = control.bytes,
		.msg_controllen = sizeof(control.bytes),
	};

	if (fd)
		*fd = -1;
	if (type < 0 || !message_size(iov, iovlen, &capacity))
		return -1;
	ssize_t size;
	do {
		size = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
	} while (size < 0 && errno == EINTR);
	if (size < 0)
		return -1;
	for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message); cmsg;
	     cmsg = CMSG_NXTHDR(&message, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
		    cmsg->cmsg_len < CMSG_LEN(0))
			continue;
		size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (size_t i = 0; i < count; ++i) {
			int candidate;
			memcpy(&candidate, (char *)CMSG_DATA(cmsg) + i * sizeof(int),
			       sizeof(candidate));
			if (fd && received < 0 && count == 1)
				received = candidate;
			else
				close(candidate);
		}
	}
	if (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) {
		errno = EMSGSIZE;
		goto fail;
	}
	if (!size) {
		if (received >= 0)
			close(received);
		return 0;
	}
	if (type == SOCK_STREAM) {
		if ((size_t)size < sizeof(length) &&
		    !receive_bytes(socket, (char *)&length + size,
		                   sizeof(length) - (size_t)size))
			goto fail;
		if (!length || length > capacity) {
			errno = EMSGSIZE;
			goto fail;
		}
		size_t left = length;
		for (int i = 0; i < iovlen && left; ++i) {
			size_t part = iov[i].iov_len < left ? iov[i].iov_len : left;
			if (!receive_bytes(socket, iov[i].iov_base, part))
				goto fail;
			left -= part;
		}
		size = length;
	}
	if (received >= 0 && fcntl(received, F_SETFD, FD_CLOEXEC) < 0)
		goto fail;
	if (fd)
		*fd = received;
	return size;

fail:
	if (received >= 0)
		close(received);
	return -1;
}
