#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "buffer.h"
#include "cache.h"
#include "http.h"
#include "netwasabi.h"

/* TODO
 *
 * Create global HTTP logging function and remove
 * the put_error_msg() function calls because they
 * do _not_ belong in this module.
 */

static pthread_once_t __ossl_init_once = PTHREAD_ONCE_INIT;

#if 0
static sigjmp_buf __env__;
static sigset_t __new;
static sigset_t __old;

#define __block_signal(s)\
do {\
	sigemptyset(&__new);\
	sigaddset(&__new, (s));\
	sigprocmask(SIG_BLOCK, &__new, &__old);\
} while (0)

#define __unblock_signal(s) sigprocmask(SIG_SETMASK, &__old, NULL);

static void
__catch_signal(int signo)
{
	siglongjmp(__env__, 1);
}
#endif

/**
 * __init_openssl - initialise the openssl library
 */
static inline void
__init_openssl(void)
{
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
	//OPENSSL_config(NULL);
	ERR_load_crypto_strings();
}

/**
 * http_connect - set up a connection with the target site
 * @http: HTTP object with remote host information
 */
int
http_connect(struct http_t *http)
{
	assert(http);

	struct sockaddr_in sock4;
	struct addrinfo *ainf = NULL;
	struct addrinfo *aip = NULL;

	clear_struct(&sock4);

	if (getaddrinfo(http->host, NULL, NULL, &ainf) < 0)
	{
		put_error_msg("open_connection: getaddrinfo error (%s)", gai_strerror(errno));
		goto fail;
	}

	for (aip = ainf; aip; aip = aip->ai_next)
	{
		if (aip->ai_family == AF_INET && aip->ai_socktype == SOCK_STREAM)
		{
			memcpy(&sock4, aip->ai_addr, aip->ai_addrlen);
			break;
		}
	}

	if (!aip)
		goto fail;

	assert(http->conn.host_ipv4);
	sprintf(http->conn.host_ipv4, "%s", inet_ntoa(sock4.sin_addr));
	update_connection_state(http, FL_CONNECTION_CONNECTING);

	if (http->usingSecure)
		sock4.sin_port = htons(HTTPS_PORT);
	else
		sock4.sin_port = htons(HTTP_PORT);

	if ((http_socket(http) = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		put_error_msg("http_connect: failed to open socket (%s)", strerror(errno));
		goto fail_release_ainf;
	}

	assert(http_socket(http) > 2);

	if (connect(http_socket(http), (struct sockaddr *)&sock4, (socklen_t)sizeof(sock4)) != 0)
	{
		put_error_msg("http_connect: connect error (%s)", strerror(errno));
		goto fail_release_ainf;
	}

	if (http->usingSecure)	
	{
/*
 * Calling __init_openssl() more than once (multithreaded)
 * has in some instances caused segfaults. Thus, use
 * pthread_once() to do it once only.
 */
		pthread_once(&__ossl_init_once, __init_openssl);
		http->conn.ssl_ctx = SSL_CTX_new(TLS_client_method());
		http_tls(http) = SSL_new(http->conn.ssl_ctx);

		SSL_set_fd(http_tls(http), http_socket(http)); /* Set the socket for reading/writing */
		SSL_set_connect_state(http_tls(http)); /* Set as client */
	}

	http->conn.sock_nonblocking = 0;
	http->conn.ssl_nonblocking = 0;

	update_connection_state(http, FL_CONNECTION_CONNECTED);
	freeaddrinfo(ainf);
	return 0;

	fail_release_ainf:
	update_connection_state(http, FL_CONNECTION_DISCONNECTED);
	put_error_msg("HTTP: failed to connect to remote host");
	freeaddrinfo(ainf);

	fail:
	return -1;
}

void
http_disconnect(struct http_t *http)
{
	assert(http);

	shutdown(http_socket(http), SHUT_RDWR);
	close(http_socket(http));
	http_socket(http) = -1;

	update_connection_state(http, FL_CONNECTION_DISCONNECTED);

	if (http->usingSecure)
	{
		SSL_CTX_free(http->conn.ssl_ctx);
		SSL_free(http_tls(http));
		http->conn.ssl_ctx = NULL;
		http_tls(http) = NULL;
	}

	return;
}

int
http_reconnect(struct http_t *http)
{
	struct sockaddr_in sock4;
	struct addrinfo *ainf = NULL;
	struct addrinfo *aip = NULL;

	shutdown(http_socket(http), SHUT_RDWR);
	close(http_socket(http));
	http_socket(http) = -1;

	if (http->usingSecure)
	{
		SSL_CTX_free(http->conn.ssl_ctx);
		SSL_free(http_tls(http));
		http->conn.ssl_ctx = NULL;
		http_tls(http) = NULL;
	}

	clear_struct(&sock4);

	if (getaddrinfo(http->host, NULL, NULL, &ainf) < 0)
	{
		put_error_msg("open_connection: getaddrinfo error (%s)", gai_strerror(errno));
		goto fail;
	}

	for (aip = ainf; aip; aip = aip->ai_next)
	{
		if (aip->ai_family == AF_INET && aip->ai_socktype == SOCK_STREAM)
		{
			memcpy(&sock4, aip->ai_addr, aip->ai_addrlen);
			break;
		}
	}

	if (!aip)
		goto fail;

	sprintf(http->conn.host_ipv4, "%s", inet_ntoa(sock4.sin_addr));

	update_connection_state(http, FL_CONNECTION_CONNECTING);

	if (http->usingSecure)
		sock4.sin_port = htons(HTTPS_PORT);
	else
		sock4.sin_port = htons(HTTP_PORT);

	if ((http_socket(http) = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		put_error_msg("open_connection: connect error (%s)", strerror(errno));
		goto fail_release_ainf;
	}

	if (connect(http_socket(http), (struct sockaddr *)&sock4, (socklen_t)sizeof(sock4)) != 0)
	{
		put_error_msg("open_connection: connect error (%s)", strerror(errno));
		goto fail_release_ainf;
	}

	if (http->usingSecure)
	{
		http->conn.ssl_ctx = SSL_CTX_new(TLS_client_method());
		http_tls(http) = SSL_new(http->conn.ssl_ctx);

		SSL_set_fd(http_tls(http), http_socket(http)); /* Set the socket for reading/writing */
		SSL_set_connect_state(http_tls(http)); /* Set as client */
	}

	http->conn.sock_nonblocking = 0;
	http->conn.ssl_nonblocking = 0;

	update_connection_state(http, FL_CONNECTION_CONNECTED);
	freeaddrinfo(ainf);
	return 0;

	fail_release_ainf:
	freeaddrinfo(ainf);

	update_connection_state(http, FL_CONNECTION_DISCONNECTED);

	fail:
	return -1;
}

int
HTTP_upgrade_to_TLS(struct http_t *http)
{
	assert(http);

	http_disconnect(http);

	http->usingSecure = 1;

	if (http_connect(http) < 0)
		goto fail;

	return 0;

	fail:
	return -1;
}
