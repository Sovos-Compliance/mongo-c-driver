/*
 * Copyright 2014 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <errno.h>
#include <string.h>

#include "mongoc-counters-private.h"
#include "mongoc-socket.h"
#include "mongoc-trace.h"


#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "socket"


struct _mongoc_socket_t
{
#ifdef _WIN32
   SOCKET sd;
#else
   int sd;
#endif
};


#define OPERATION_EXPIRED(expire_at) \
   ((expire_at >= 0) && (expire_at < (bson_get_monotonic_time())))


/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_socket_setnonblock --
 *
 *       A helper to set a socket in nonblocking mode.
 *
 * Returns:
 *       true if successful; otherwise false.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static bool
#ifdef _WIN32
_mongoc_socket_setnonblock (SOCKET sd)
#else
_mongoc_socket_setnonblock (int sd)
#endif
{
#ifdef _WIN32
   u_long io_mode = 1;
   return (NO_ERROR == ioctlsocket (sd, FIONBIO, &io_mode));
#else
   int flags;

   flags = fcntl (sd, F_GETFL, sd);
   return (-1 != fcntl (sd, F_SETFL, (flags | O_NONBLOCK)));
#endif
}


/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_socket_wait --
 *
 *       A single socket poll helper.
 *
 *       @events: in most cases should be POLLIN or POLLOUT.
 *
 *       @expire_at should be an absolute time at which to expire using
 *       the monotonic clock (bson_get_monotonic_time(), which is in
 *       microseconds). Or zero to not block at all. Or -1 to block
 *       forever.
 *
 * Returns:
 *       true if an event matched. otherwise false.
 *       a timeout will return false.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

bool
#ifdef _WIN32
_mongoc_socket_wait (SOCKET   sd,           /* IN */
#else
_mongoc_socket_wait (int      sd,           /* IN */
#endif
                     int      events,       /* IN */
                     int64_t  expire_at)    /* IN */
{
#ifdef _WIN32
   WSAPOLLFD pfd;
#else
   struct pollfd pfd;
#endif
   int ret;
   int timeout;

   ENTRY;

   bson_return_val_if_fail (events, false);

   if (expire_at < 0) {
      timeout = -1;
   } else if (expire_at == 0) {
      timeout = 0;
   } else {
      timeout = (expire_at - bson_get_monotonic_time ()) / 1000L;
      if (timeout < 0) {
         timeout = 0;
      }
   }

   pfd.fd = sd;
#ifdef _WIN32
   pfd.events = events;
#else
   pfd.events = events | POLLERR | POLLHUP;
#endif
   pfd.revents = 0;

#ifdef _WIN32
   ret = WSAPoll (&pfd, 1, timeout);
   if (ret == SOCKET_ERROR) {
      MONGOC_WARNING ("WSAGetLastError(): %d", WSAGetLastError ());
      ret = -1;
   }
#else
   ret = poll (&pfd, 1, timeout);
#endif

   if (ret > 0) {
      RETURN (0 != (pfd.revents & events));
   }

   RETURN (false);
}


bool
#ifdef _WIN32
_mongoc_socket_setnodelay (SOCKET sd) /* IN */
#else
_mongoc_socket_setnodelay (int sd)    /* IN */
#endif
{
#ifdef _WIN32
   BOOL optval = 1;
#else
   int optval = 1;
#endif
   int ret;

   ENTRY;

   ret = setsockopt (sd, IPPROTO_TCP, TCP_NODELAY,
                     (char *)&optval, sizeof optval);

#ifdef _WIN32
   if (ret == SOCKET_ERROR) {
      MONGOC_WARNING ("WSAGetLastError(): %d", (int)WSAGetLastError ());
   }
#endif

   RETURN (ret == 0);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_socket_accept --
 *
 *       Wrapper for BSD socket accept(). Handles portability between
 *       BSD sockets and WinSock2 on Windows Vista and newer.
 *
 * Returns:
 *       NULL upon failure to accept or timeout.
 *       A newly allocated mongoc_socket_t on success.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mongoc_socket_t *
mongoc_socket_accept (mongoc_socket_t *sock,      /* IN */
                      int64_t          expire_at) /* IN */
{
   mongoc_socket_t *client;
   struct sockaddr addr;
   socklen_t addrlen = sizeof addr;
   bool try_again = false;
   bool failed = false;
#ifdef _WIN32
   SOCKET sd;
#else
   int sd;
#endif

   ENTRY;

   bson_return_val_if_fail (sock, NULL);

again:
   sd = accept (sock->sd, &addr, &addrlen);

#ifdef _WIN32
   failed = (sd == INVALID_SOCKET);
   try_again = (failed && (WSAGetLastError () == WSAEWOULDBLOCK));
#else
   failed = (sd == -1);
   try_again = (failed && ((errno == EWOULDBLOCK) || (errno == EAGAIN)));
#endif

   if (failed && try_again) {
      if (_mongoc_socket_wait (sock->sd, POLLIN, expire_at)) {
         GOTO (again);
      }
      RETURN (NULL);
   } else if (failed) {
      RETURN (NULL);
   } else if (!_mongoc_socket_setnonblock (sd)) {
#ifdef _WIN32
      closesocket (sd);
#else
      close (sd);
#endif
      RETURN (NULL);
   }

   client = bson_malloc0 (sizeof *client);
   client->sd = sd;

   if (!_mongoc_socket_setnodelay (client->sd)) {
      MONGOC_WARNING ("Failed to enable TCP_NODELAY.");
   }

   RETURN (client);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongo_socket_bind --
 *
 *       A wrapper around bind().
 *
 * Returns:
 *       true if successful. false if not.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

int
mongoc_socket_bind (mongoc_socket_t       *sock,    /* IN */
                    const struct sockaddr *addr,    /* IN */
                    socklen_t              addrlen) /* IN */
{
   int ret;

   ENTRY;

   bson_return_val_if_fail (sock, false);
   bson_return_val_if_fail (addr, false);
   bson_return_val_if_fail (addrlen, false);

   ret = bind (sock->sd, addr, addrlen);

   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_socket_close --
 *
 *       Closes the underlying socket.
 *
 *       In general, you probably don't want to handle the result from
 *       this. That could cause race conditions in the case of preemtion
 *       during system call (EINTR).
 *
 * Returns:
 *       0 on success, -1 on failure.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

int
mongoc_socket_close (mongoc_socket_t *sock) /* IN */
{
   int ret = 0;

   ENTRY;

   bson_return_val_if_fail (sock, false);

#ifdef _WIN32
   if (sock->sd != INVALID_SOCKET) {
      ret = closesocket (sock->sd);
   }
#else
   if (sock->sd != -1) {
      shutdown (sock->sd, SHUT_WR);
      ret = close (sock->sd);
   }
#endif

   if (ret == 0) {
#ifdef _WIN32
      sock->sd = INVALID_SOCKET;
#else
      sock->sd = -1;
#endif
      RETURN (0);
   }

   RETURN (-1);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_socket_connect --
 *
 *       Performs a socket connection but will fail if @expire_at is
 *       reached by the monotonic clock.
 *
 * Returns:
 *       true if successful, false if not.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

int
mongoc_socket_connect (mongoc_socket_t       *sock,      /* IN */
                       const struct sockaddr *addr,      /* IN */
                       socklen_t              addrlen,   /* IN */
                       int64_t                expire_at) /* IN */
{
   bool try_again = false;
   bool failed = false;
   int ret;
   int optval = 0;
   socklen_t optlen = sizeof optval;

   ENTRY;

   bson_return_val_if_fail (sock, false);
   bson_return_val_if_fail (addr, false);
   bson_return_val_if_fail (addrlen, false);

   ret = connect (sock->sd, addr, addrlen);

again:
#ifdef _WIN32
   if (ret == SOCKET_ERROR) {
      failed = true;
      try_again = (WSAGetLastError () == WSAEWOULDBLOCK);
#else
   if (ret == -1) {
      failed = true;
      try_again = ((errno == EAGAIN) || (errno == EINPROGRESS));
#endif
      if (try_again) {
         ret = getsockopt (sock->sd, SOL_SOCKET, SO_ERROR,
                           (char *)&optval, &optlen);
         failed = ((ret == -1) || (optval != 0));
         if (failed) {
            errno = optval;
         }
      }
   }

   if (failed && try_again) {
      if (_mongoc_socket_wait (sock->sd, POLLOUT, expire_at)) {
         GOTO (again);
      }
      RETURN (-1);
   } else if (failed) {
      RETURN (-1);
   } else {
      RETURN (0);
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_socket_destroy --
 *
 *       Cleanup after a mongoc_socket_t structure, possibly closing
 *       underlying sockets.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       @sock is freed and should be considered invalid.
 *
 *--------------------------------------------------------------------------
 */

void
mongoc_socket_destroy (mongoc_socket_t *sock) /* IN */
{
   if (sock) {
      mongoc_socket_close (sock);
      bson_free (sock);
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_socket_listen --
 *
 *       Listen for incoming requests with a backlog up to @backlog.
 *
 *       If @backlog is zero, a sensible default will be chosen.
 *
 * Returns:
 *       true if successful; otherwise false.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

int
mongoc_socket_listen (mongoc_socket_t *sock,    /* IN */
                      unsigned int     backlog) /* IN */
{
   int ret;

   ENTRY;

   bson_return_val_if_fail (sock, false);

   if (backlog == 0) {
      backlog = 10;
   }

   ret = listen (sock->sd, backlog);

   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_socket_new --
 *
 *       Create a new socket.
 *
 *       Free the result mongoc_socket_destroy().
 *
 * Returns:
 *       A newly allocated socket.
 *       NULL on failure.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mongoc_socket_t *
mongoc_socket_new (int domain,   /* IN */
                   int type,     /* IN */
                   int protocol) /* IN */
{
   mongoc_socket_t *sock;
#ifdef _WIN32
   SOCKET sd;
#else
   int sd;
#endif

   ENTRY;

   sd = socket (domain, type, protocol);

#ifdef _WIN32
   if (sd == INVALID_SOCKET) {
#else
   if (sd == -1) {
#endif
      RETURN (NULL);
   }

   if (!_mongoc_socket_setnonblock (sd)) {
      GOTO (fail);
   }

   if (!_mongoc_socket_setnodelay (sd)) {
      MONGOC_WARNING ("Failed to enable TCP_NODELAY.");
   }

   sock = bson_malloc0 (sizeof *sock);
   sock->sd = sd;

   RETURN (sock);

fail:
#ifdef _WIN32
   closesocket (sd);
#else
   close (sd);
#endif

   RETURN (NULL);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_socket_recv --
 *
 *       A portable wrapper around recv() that also respects an absolute
 *       timeout.
 *
 *       @expire_at is 0 for no blocking, -1 for infinite blocking,
 *       or a time using the monotonic clock to expire. Calculate this
 *       using bson_get_monotonic_time() + N_MICROSECONDS.
 *
 * Returns:
 *       The number of bytes received on success.
 *       0 on end of stream.
 *       -1 on failure.
 *
 * Side effects:
 *       @buf will be read into.
 *
 *--------------------------------------------------------------------------
 */

ssize_t
mongoc_socket_recv (mongoc_socket_t *sock,      /* IN */
                    void            *buf,       /* OUT */
                    size_t           buflen,    /* IN */
                    int              flags,     /* IN */
                    int64_t          expire_at) /* IN */
{
   ssize_t ret = 0;
   bool failed = false;
   bool try_again = false;

   ENTRY;

   bson_return_val_if_fail (sock, -1);
   bson_return_val_if_fail (buf, -1);
   bson_return_val_if_fail (buflen, -1);

again:
#ifdef _WIN32
   ret = recv (sock->sd, (char *)buf, (int)buflen, flags);
   failed = (ret == SOCKET_ERROR);
   try_again = (failed && (WSAGetLastError () == WSAEWOULDBLOCK));
#else
   ret = recv (sock->sd, buf, buflen, flags);
   failed = (ret == -1);
   try_again = (failed && ((errno == EAGAIN) || (errno == EWOULDBLOCK)));
#endif

   if (failed && try_again) {
      if (_mongoc_socket_wait (sock->sd, POLLIN, expire_at)) {
         GOTO (again);
      }
   }

   if (failed) {
      RETURN (-1);
   }

   DUMP_BYTES (recvbuf, (uint8_t *)buf, ret);

   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_socket_setsockopt --
 *
 *       A wrapper around setsockopt().
 *
 * Returns:
 *       0 on success, -1 on failure.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

int
mongoc_socket_setsockopt (mongoc_socket_t *sock,    /* IN */
                          int              level,   /* IN */
                          int              optname, /* IN */
                          const void      *optval,  /* IN */
                          socklen_t        optlen)  /* IN */
{
   bson_return_val_if_fail (sock, false);

   return setsockopt (sock->sd, level, optname, optval, optlen);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_socket_send --
 *
 *       A simplified wrapper around mongoc_socket_sendv().
 *
 *       @expire_at is 0 for no blocking, -1 for infinite blocking,
 *       or a time using the monotonic clock to expire. Calculate this
 *       using bson_get_monotonic_time() + N_MICROSECONDS.
 *
 * Returns:
 *       -1 on failure. number of bytes written on success.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

ssize_t
mongoc_socket_send (mongoc_socket_t *sock,      /* IN */
                    const void      *buf,       /* IN */
                    size_t           buflen,    /* IN */
                    int64_t          expire_at) /* IN */
{
   mongoc_iovec_t iov;

   bson_return_val_if_fail (sock, -1);
   bson_return_val_if_fail (buf, -1);
   bson_return_val_if_fail (buflen, -1);

   iov.iov_base = (void *)buf;
   iov.iov_len = buflen;

   return mongoc_socket_sendv (sock, &iov, 1, expire_at);
}


/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_socket_try_sendv --
 *
 *       Helper used by mongoc_socket_sendv() to try to write as many
 *       bytes to the underlying socket until the socket buffer is full.
 *
 *       This is performed in a non-blocking fashion.
 *
 * Returns:
 *       -1 on failure. the number of bytes written on success.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

ssize_t
_mongoc_socket_try_sendv (mongoc_socket_t *sock,   /* IN */
                          mongoc_iovec_t  *iov,    /* IN */
                          size_t           iovcnt) /* IN */
{
#ifdef _WIN32
   WSAMSG msg;
   DWORD dwNumberOfBytesSent = 0;
#else
   struct msghdr msg;
#endif
   ssize_t ret = -1;

   ENTRY;

   BSON_ASSERT (sock);
   BSON_ASSERT (iov);
   BSON_ASSERT (iovcnt);

   memset (&msg, 0, sizeof msg);

   DUMP_IOVEC (sendbuf, iov, iovcnt);

#ifdef _WIN32
   msg.lpBuffers = (LPWSABUF)iov;
   msg.dwBufferCount = (DWORD)iovcnt;
   ret = WSASendMsg (sock->sd, &msg, 0, &dwNumberOfBytesSent, NULL, NULL);
   if (ret == 0) {
      ret = dwNumberOfBytesSent;
   } else {
      ret = -1;
   }
   if (WSAGetLastError () == WSAEWOULDBLOCK) {
      errno = EWOULDBLOCK;
   }
#else
   msg.msg_iov = iov;
   msg.msg_iovlen = iovcnt;
   ret = sendmsg (sock->sd, &msg, 0);
#endif

   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_socket_sendv --
 *
 *       A wrapper around using sendmsg() to send an iovec.
 *       This also deals with the structure differences between
 *       WSABUF and struct iovec.
 *
 *       @expire_at is 0 for no blocking, -1 for infinite blocking,
 *       or a time using the monotonic clock to expire. Calculate this
 *       using bson_get_monotonic_time() + N_MICROSECONDS.
 *
 * Returns:
 *       -1 on failure.
 *       the number of bytes written on success.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

ssize_t
mongoc_socket_sendv (mongoc_socket_t  *sock,      /* IN */
                     mongoc_iovec_t   *iov,       /* IN */
                     size_t            iovcnt,    /* IN */
                     int64_t           expire_at) /* IN */
{
   ssize_t ret = 0;
   ssize_t sent;
   size_t cur = 0;

   ENTRY;

   bson_return_val_if_fail (sock, -1);
   bson_return_val_if_fail (iov, -1);
   bson_return_val_if_fail (iovcnt, -1);

   for (;;) {
      sent = _mongoc_socket_try_sendv (sock, &iov [cur], iovcnt - cur);

      /*
       * If we failed with anything other than EAGAIN or EWOULDBLOCK,
       * we should fail immediately as there is another issue with the
       * underlying socket.
       */
      if (sent == -1) {
         if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
            RETURN (ret ? ret : -1);
         }
      }

      /*
       * Update internal stream counters.
       */
      if (sent > 0) {
         ret += sent;
         mongoc_counter_streams_egress_add (sent);
      } else if (OPERATION_EXPIRED (expire_at)) {
         errno = ETIMEDOUT;
         RETURN (ret ? ret : -1);
      }

      /*
       * Subtract the sent amount from what we still need to send.
       */
      while ((cur < iovcnt) && (sent >= (ssize_t)iov [cur].iov_len)) {
         sent -= iov [cur++].iov_len;
      }

      /*
       * Check if that made us finish all of the iovecs. If so, we are done
       * sending data over the socket.
       */
      if (cur == iovcnt) {
         break;
      }

      /*
       * Increment the current iovec buffer to its proper offset and adjust
       * the number of bytes to write.
       */
      iov [cur].iov_base = ((char *)iov [cur].iov_base) + sent;
      iov [cur].iov_len -= sent;

      BSON_ASSERT (iovcnt - cur);
      BSON_ASSERT (iov [cur].iov_len);

      /*
       * Block on poll() until our desired condition is met.
       */
      if (!_mongoc_socket_wait (sock->sd, POLLOUT, expire_at)) {
         if (ret == 0){
            errno = ETIMEDOUT;
         }
         RETURN (ret  ? ret : -1);
      }
   }

   RETURN (ret);
}


int
mongoc_socket_getsockname (mongoc_socket_t *sock,    /* IN */
                           struct sockaddr *addr,    /* OUT */
                           socklen_t       *addrlen) /* INOUT */
{
   bson_return_val_if_fail (sock, -1);

   return getsockname (sock->sd, addr, addrlen);
}
