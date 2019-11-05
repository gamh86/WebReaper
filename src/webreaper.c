#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h> /* for mkdir() */
#include <unistd.h>
#include "buffer.h"
#include "cache.h"
#include "http.h"
#include "robots.h"
#include "screen_utils.h"
#include "utils_url.h"
#include "webreaper.h"

static sigset_t oldset;
static sigset_t newset;

#define BLOCK_SIGNAL(signal)\
do {\
	sigemptyset(&newset);\
	sigaddset(&newset, (signal));\
	sigprocmask(SIG_BLOCK, &newset, &oldset);\
} while (0)

#define UNBLOCK_SIGNAL(signal) sigprocmask(SIG_SETMASK, &oldset, NULL)

struct cache_ctx cache1;
struct cache_ctx cache2;

int nr_reaped = 0;
int current_depth = 0;
int url_cnt = 0;

char *no_url_files[] =
{
	".jpg",
	".jpeg",
	".png",
	".gif",
	".js",
	".css",
	".pdf",
	".svg",
	".ico",
	NULL
};

void
update_bytes(size_t bytes)
{
	pthread_mutex_lock(&screen_mutex);

	reset_left();
	up(UPDATE_BYTES_UP);
	right(UPDATE_BYTES_RIGHT);
	fprintf(stderr, "%12lu", bytes);
	reset_left();
	down(UPDATE_BYTES_UP);

	pthread_mutex_unlock(&screen_mutex);
}

void
update_cache1_count(int count)
{
	pthread_mutex_lock(&screen_mutex);

	reset_left();
	up(UPDATE_CACHE1_COUNT_UP);
	right(UPDATE_CACHE1_COUNT_RIGHT);
	fprintf(stderr, "%4d", count);
	reset_left();
	down(UPDATE_CACHE1_COUNT_UP);

	pthread_mutex_unlock(&screen_mutex);
	return;
}

void
update_cache2_count(int count)
{
	pthread_mutex_lock(&screen_mutex);

	reset_left();
	up(UPDATE_CACHE2_COUNT_UP);
	right(UPDATE_CACHE2_COUNT_RIGHT);
	fprintf(stderr, "%4d", count);
	reset_left();
	down(UPDATE_CACHE2_COUNT_UP);

	pthread_mutex_unlock(&screen_mutex);
	return;
}

void
update_cache_status(int cache, int status_flag)
{
	pthread_mutex_lock(&screen_mutex);

	reset_left();
	up(UPDATE_CACHE_STATUS_UP);
	right(cache == 1 ? UPDATE_CACHE1_STATUS_RIGHT : UPDATE_CACHE2_STATUS_RIGHT);
	
	switch(status_flag)
	{
		default:
		case FL_CACHE_STATUS_FILLING:
			fprintf(stderr, "%s (filling) %s", COL_DARKGREEN, COL_END);
			break;
		case FL_CACHE_STATUS_DRAINING:
			fprintf(stderr, " %s(draining)%s", COL_LIGHTGREY, COL_END);
			break;
		case FL_CACHE_STATUS_FULL:
			fprintf(stderr, "   %s(full)  %s ", COL_DARKRED, COL_END);
			break;
	}

	reset_left();
	down(UPDATE_CACHE_STATUS_UP);

	pthread_mutex_unlock(&screen_mutex);
	return;
}

void
update_current_url(const char *url)
{
	size_t url_len = strlen(url);
	int too_long = 0;
	int max_len = OUTPUT_TABLE_COLUMNS - 10;

	if (url_len >= (size_t)max_len)
		too_long = 1;

	pthread_mutex_lock(&screen_mutex);

	reset_left();
	up(UPDATE_CURRENT_URL_UP);
	clear_line();
	right(UPDATE_CURRENT_URL_RIGHT);

	fprintf(stderr, " %s%.*s%s",
		ACTION_ING_STR,
		too_long ? max_len : (int)url_len,
		url,
		too_long ? "..." : "");

	reset_left();
	down(UPDATE_CURRENT_URL_UP);

	pthread_mutex_unlock(&screen_mutex);
	return;
}

void
update_current_local(const char *url)
{
	size_t url_len = strlen(url);
	int too_long = 0;
	int max_len = OUTPUT_TABLE_COLUMNS - 18;

	if (url_len >= (size_t)max_len)
		too_long = 1;

	pthread_mutex_lock(&screen_mutex);

	reset_left();
	up(UPDATE_CURRENT_LOCAL_UP);
	clear_line();

	if (!url_len)
		goto out_release_lock;

	right(UPDATE_CURRENT_LOCAL_RIGHT);

	fprintf(stderr, " %sCreated %s%.*s%s%s",
		ACTION_DONE_STR,
		COL_DARKGREY,
		too_long ? max_len : (int)url_len,
		url,
		too_long ? "..." : "",
		COL_END);

	out_release_lock:
	reset_left();
	down(UPDATE_CURRENT_LOCAL_UP);

	pthread_mutex_unlock(&screen_mutex);
	return;
}

void
update_operation_status(const char *status_string, ...)
{
	size_t len;
	int too_long = 0;
	int max_len = OUTPUT_TABLE_COLUMNS - 6;
	va_list args;
	static char tmp[256];

	va_start(args, status_string);
	vsprintf(tmp, status_string, args);
	va_end(args);

	len = strlen(tmp);

	if (len >= (size_t)max_len)
		too_long = 1;

	pthread_mutex_lock(&screen_mutex);

	reset_left();
	up(UPDATE_OP_STATUS_UP);
	clear_line();

	if (!len)
		goto out_release_lock;

	right(UPDATE_OP_STATUS_RIGHT);

	fprintf(stderr, "%s(%.*s%s)%s",
			COL_LIGHTRED,
			too_long ? max_len : (int)len,
			tmp,
			too_long ? "..." : "",
			COL_END);

	out_release_lock:
	reset_left();
	down(UPDATE_OP_STATUS_UP);

	pthread_mutex_unlock(&screen_mutex);
	return;
}

void
update_connection_state(struct http_t *http, int state)
{
	pthread_mutex_lock(&screen_mutex);

	reset_left();
	up(UPDATE_CONN_STATE_UP);
	clear_line();
	right(UPDATE_CONN_STATE_RIGHT);

	switch(state)
	{
		default:
		case FL_CONNECTION_CONNECTED:
			fprintf(stderr, "%sConnected%s to %s%s%s (%s)", COL_DARKGREEN, COL_END, COL_RED, http->host, COL_END, http->conn.host_ipv4);
			break;
		case FL_CONNECTION_DISCONNECTED:
			fprintf(stderr, "%sDisconnected%s", COL_LIGHTGREY, COL_END);
			break;
		case FL_CONNECTION_CONNECTING:
			fprintf(stderr, "Connecting to server %s at %s", http->host, http->conn.host_ipv4);
			break;
	}

	reset_left();
	down(UPDATE_CONN_STATE_UP);

	pthread_mutex_unlock(&screen_mutex);
	return;
}

void
update_status_code(int status_code)
{
	pthread_mutex_lock(&screen_mutex);

	reset_left();
	up(UPDATE_STATUS_CODE_UP);

	right(UPDATE_STATUS_CODE_RIGHT);

	switch(status_code)
	{
		case HTTP_OK:
		case HTTP_ALREADY_EXISTS:
			fprintf(stderr, "%s%3d%s", COL_DARKGREEN, status_code, COL_END);
			break;
		case HTTP_MOVED_PERMANENTLY:
		case HTTP_FOUND:
		case HTTP_SEE_OTHER:
			fprintf(stderr, "%s%3d%s", COL_ORANGE, status_code, COL_END);
			break;
		default:
			fprintf(stderr, "%s%3d%s", COL_RED, status_code, COL_END);
	}

	reset_left();
	down(UPDATE_STATUS_CODE_UP);

	pthread_mutex_unlock(&screen_mutex);
	return;
}

void
put_error_msg(const char *fmt, ...)
{
	va_list args;
	static char tmp[256];
	size_t len;
	int go_right = 1;

	va_start(args, fmt);
	vsprintf(tmp, fmt, args);
	va_end(args);

	len = strlen(tmp);

	if (len < OUTPUT_TABLE_COLUMNS)
		go_right = (OUTPUT_TABLE_COLUMNS - len);

	pthread_mutex_lock(&screen_mutex);

	reset_left();
	up(UPDATE_ERROR_MSG_UP);
	clear_line();
	right(go_right);

	fprintf(stderr, "%s%.*s%s", COL_RED, !go_right ? OUTPUT_TABLE_COLUMNS : (int)len, tmp, COL_END);
	reset_left();
	down(UPDATE_ERROR_MSG_UP);

	pthread_mutex_unlock(&screen_mutex);
	return;
}

void
clear_error_msg(void)
{
	pthread_mutex_lock(&screen_mutex);

	reset_left();
	up(UPDATE_ERROR_MSG_UP);
	clear_line();
	down(UPDATE_ERROR_MSG_UP);

	pthread_mutex_unlock(&screen_mutex);
	return;
}

void
deconstruct_btree(http_link_t *root, wr_cache_t *cache)
{
	if (!root)
	{
#ifdef DEBUG
		fprintf(stderr, "deconstruct_btree: root is NULL\n");
#endif
		return;
	}

	if (((char *)root - (char *)cache->cache) >= cache->cache_size)
	{
#ifdef DEBUG
		fprintf(stderr, "node @ %p is beyond our cache... (cache %p to %p)\n",
		root,
		cache,
		(void *)((char *)cache->cache + cache->cache_size));
#endif

		assert(0);
	}

	if (root->left)
	{
#ifdef DEBUG
		fprintf(stderr, "Going left from %p to %p\n", root, root->left);
#endif
		deconstruct_btree(root->left, cache);
	}

	if (root->right)
	{
#ifdef DEBUG
		fprintf(stderr, "Going right from %p to %p\n", root, root->right);
#endif
		deconstruct_btree(root->right, cache);
	}

#ifdef DEBUG
	fprintf(stderr, "Setting left/right/parent to NULL in node %p\n", root);
#endif
	root->left = NULL;
	root->right = NULL;
	root->parent = NULL;

	return;
}

int
check_local_dirs(struct http_t *http, buf_t *filename)
{
	assert(http);
	assert(filename);

	char *p;
	char *e;
	char *end;
	char *name = filename->buf_head;
	buf_t _tmp;

	buf_init(&_tmp, pathconf("/", _PC_PATH_MAX));

	if (*(filename->buf_tail - 1) == '/')
		buf_snip(filename, 1);

	end = filename->buf_tail;
	p = strstr(name, WEBREAPER_DIR);

	if (!p)
	{
		put_error_msg("check_local_dirs: failed to find webreaper directory in caller's filename\n");
		errno = EPROTO;
		return -1;
	}

	e = ++p;

	e = memchr(p, '/', (end - p));

	if (!e)
	{
		put_error_msg("check_local_dirs: failed to find necessary '/' character in caller's filename\n");
		errno = EPROTO;
		return -1;
	}

	p = ++e;

/*
 * e.g. /home/johndoe/WR_Reaped/favourite-site.com/categories/best-rated
 *                              ^start here, work along to end, checking
 * creating a directory for each part if necessary.
 */

	while (e < end)
	{
		e = memchr(p, '/', (end - p));

		if (!e) /* The rest of the filename is the file itself */
		{
			break;
		}

		buf_append_ex(&_tmp, name, (e - name));
		BUF_NULL_TERMINATE(&_tmp);

		if(access(_tmp.buf_head, F_OK) != 0)
		{
			if (mkdir(_tmp.buf_head, S_IRWXU) < 0)
				put_error_msg("Failed to create directory: %s", strerror(errno));
		}

		p = ++e;
		buf_clear(&_tmp);
	}

	buf_destroy(&_tmp);
	return 0;
}

void
replace_with_local_urls(struct http_t *http, buf_t *buf)
{
	assert(http);
	assert(buf);

	char *tail = buf->buf_tail;
	char *p;
	char *savep;
	char *url_start;
	char *url_end;
	off_t url_start_off;
	off_t url_end_off;
	off_t savep_off;
	off_t poff;
	size_t range;
	buf_t url;
	buf_t path;
	buf_t full;
	int url_type_idx;

	buf_init(&url, HTTP_URL_MAX);
	buf_init(&path, HTTP_URL_MAX);
	buf_init(&full, HTTP_URL_MAX);

#define save_pointers()\
do {\
	savep_off = (savep - buf->buf_head);\
	poff = (savep - buf->buf_head);\
	url_start_off = (url_start - buf->buf_head);\
	url_end_off = (url_end - buf->buf_head);\
} while (0)

#define restore_pointers()\
do {\
	savep = (buf->buf_head + savep_off);\
	p = (buf->buf_head + poff);\
	url_start = (buf->buf_head + url_start_off);\
	url_end = (buf->buf_head + url_end_off);\
} while (0)

	savep = buf->buf_head;
	url_type_idx = 0;

	while (1)
	{
		buf_clear(&url);

		assert(buf->buf_tail <= buf->buf_end);
		assert(buf->buf_head >= buf->data);

		p = strstr(savep, url_types[url_type_idx].string);

		if (!p || p >= tail)
		{
			++url_type_idx;

			if (url_types[url_type_idx].delim == 0)
				break;

			savep = buf->buf_head;
			continue;
		}

		url_start = (p += url_types[url_type_idx].len);
		url_end = memchr(url_start, url_types[url_type_idx].delim, (tail - url_start));

		if (!url_end)
		{
			++url_type_idx;

			if (url_types[url_type_idx].delim == 0)
				break;

			savep = buf->buf_head;
			continue;
		}

		assert(buf->buf_tail <= buf->buf_end);
		assert(url_start < buf->buf_tail);
		assert(url_end < buf->buf_tail);
		assert(p < buf->buf_tail);
		assert(savep < buf->buf_tail);
		assert((tail - buf->buf_head) == (buf->buf_tail - buf->buf_head));

		range = (url_end - url_start);

		if (!range)
		{
			++savep;
			continue;
		}

		if (!strncmp("http://", url_start, range) || !strncmp("https://", url_start, range))
		{
			savep = ++url_end;
			continue;
		}

		if (range >= HTTP_URL_MAX)
		{
			savep = ++url_end;
			continue;
		}

		assert(range < HTTP_URL_MAX);

		buf_append_ex(&url, url_start, range);
		BUF_NULL_TERMINATE(&url);

		if (range)
		{
			//fprintf(stderr, "turning %s into full url\n", url.buf_head);
			make_full_url(http, &url, &full);
			//fprintf(stderr, "made %s\n", full.buf_head);

			if (make_local_url(http, &full, &path) == 0)
			{
				//fprintf(stderr, "made local url %s\n", path.buf_head);
				buf_collapse(buf, (off_t)(url_start - buf->buf_head), range);
				tail = buf->buf_tail;

				save_pointers();

				assert(path.data_len < path_max);
				buf_shift(buf, (off_t)(url_start - buf->buf_head), path.data_len);
				tail = buf->buf_tail;

				restore_pointers();

				assert((url_start - buf->buf_head) == url_start_off);
				assert((url_end - buf->buf_head) == url_end_off);
				assert((p - buf->buf_head) == poff);
				assert((savep - buf->buf_head) == savep_off);

				strncpy(url_start, path.buf_head, path.data_len);
			}
		}

		assert(buf_integrity(&url));
		assert(buf_integrity(&full));
		assert(buf_integrity(&path));

		//++savep;
		savep = ++url_end;

		if (savep >= tail)
			break;
	}
}

static int
__url_parseable(char *url)
{
	int i;

	for (i = 0; no_url_files[i] != NULL; ++i)
	{
		if (strstr(url, no_url_files[i]))
			return 0;
	}

	return 1;
}

int
archive_page(struct http_t *http)
{
	int fd = -1;
	buf_t *buf = &http_rbuf(http);
	buf_t tmp;
	buf_t local_url;
	char *p;
	int rv;

	update_operation_status("Archiving %s", http->full_url);
	p = HTTP_EOH(buf);

	if (p)
		buf_collapse(buf, (off_t)0, (p - buf->buf_head));

	if (__url_parseable(http->full_url))
		replace_with_local_urls(http, buf);

	buf_init(&tmp, HTTP_URL_MAX);
	buf_init(&local_url, 1024);

	buf_append(&tmp, http->full_url);
	make_local_url(http, &tmp, &local_url);

/* Now we have "file:///path/to/file.extension" */
	buf_collapse(&local_url, (off_t)0, strlen("file://"));

	rv = check_local_dirs(http, &local_url);

	if (rv < 0)
		goto fail_free_bufs;

	if (access(local_url.buf_head, F_OK) == 0)
	{
		//update_operation_status("Already archived local copy", 1);
		goto out_free_bufs;
	}

	fd = open(local_url.buf_head, O_RDWR|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR);

	if (fd == -1)
	{
		put_error_msg("Failed to create local copy (%s)", strerror(errno));
		goto fail_free_bufs;
	}

	update_operation_status("Created %s", local_url.buf_head);
	++nr_reaped;

	buf_write_fd(fd, buf);
	close(fd);
	fd = -1;

	out_free_bufs:
	buf_destroy(&tmp);
	buf_destroy(&local_url);

	return 0;

	fail_free_bufs:
	buf_destroy(&tmp);
	buf_destroy(&local_url);

	return -1;
}

static const char *const __disallowed_tokens[] =
{
	"javascript:",
	"data:image",
	".exe",
	".dll",
	"cgi-",
	(char *)NULL
};

static int nr_already = 0;
static int nr_twins = 0;
static int nr_dups = 0;
static int nr_urls_call = 0;

/**
 * __url_acceptable - determine if parsed URL is acceptable by searching for certain tokens
 *				and checking if the URL is already present in the DRAINING cache.
 *
 * @http: our HTTP object with remote host info
 * @fctx: the FILLING cache context with binary tree root
 * @dctx: the DRAINING cache contetx with binary tree root
 * @url: the parsed URL to check
 */
static int
__url_acceptable(struct http_t *http, struct cache_ctx *fctx, struct cache_ctx *dctx, buf_t *url)
{
	assert(http);
	assert(fctx);
	assert(dctx);
	assert(url);

	//static char tmp_page[HTTP_URL_MAX];
	int i;

	if (url->data_len >= 256)
		return 0;

	if (!strncmp("http:", url->buf_head, 5)
	|| !strncmp("https:", url->buf_head, 6))
	{
		if (url->data_len < httplen || url->data_len < httpslen)
			return 0;

#if 0
		if (got_token_graph(wrctx))
		{
			http_parse_page(url->buf_head, tmp_page);
			if (!robots_eval_url(allowed, forbidden, tmp_page))
			{
				return 0;
			}
		}
#endif
	}

	if (local_archive_exists(url->buf_head))
	{
		++nr_already;
		return 0;
	}

	if (memchr(url->buf_head, '#', url->buf_tail - url->buf_head))
		return 0;

	for (i = 0; __disallowed_tokens[i] != NULL; ++i)
	{
		if (strstr(url->buf_head, __disallowed_tokens[i]))
			return 0;
	}

	if (is_xdomain(http, url))
	{
		if (!option_set(OPT_ALLOW_XDOMAIN))
			return 0;
	}


/*
 * Check the current "draining" cache for duplicate URLs
 */
	if (dctx->cache)
	{
		wr_cache_lock(dctx->cache);

		int cmp = 0;
		http_link_t *nptr = dctx->root;

		while (nptr)
		{
			cmp = strcmp(url->buf_head, nptr->url);

			if (url->buf_head[0] && nptr->url[0] && !cmp)
			{
				++nr_twins;
				wr_cache_unlock(dctx->cache);
				return 0;
			}
			else
			if (cmp < 0)
			{
				nptr = nptr->left;
			}
			else
			{
				nptr = nptr->right;
			}

			if (!nptr)
				break;
		}
	}

	wr_cache_unlock(dctx->cache);
	return 1;
}

/**
 * __insert_link - insert a URL into the current "filling" cache
 * @fctx: context holding cache pointer and binary tree root
 * @url: url to add to cache
 */
static int
__insert_link(struct cache_ctx *fctx, buf_t *url)
{
	assert(fctx);
	assert(url);

	if (!(fctx->root))
	{
		wr_cache_lock(fctx->cache);

		http_link_t *r = fctx->root;
		r = (http_link_t *)wr_cache_alloc(fctx->cache, &fctx->root);

		strncpy(r->url, url->buf_head, url->data_len);
		r->url[url->data_len] = 0;

		r->left = NULL;
		r->right = NULL;
		r->parent = NULL;

		wr_cache_unlock(fctx->cache);

		return 0;
	}

/*
 * Cannot use recursion to insert nodes because when the cache
 * is extended, all the addresses that active pointers hold are
 * patched, and any active pointers that reside in the cache
 * itself are also rightly patched. However, the numerous
 * stack frames due to recursion still hold old addresses from
 * the old cache location. We cannot patch them. So we need
 * to iteratively insert nodes into the tree.
 */

	http_link_t *nptr = fctx->root;
	int cmp;
	off_t nptr_offset;
	void *nptr_stack = &nptr;
	http_link_t *new_addr;

	wr_cache_lock(fctx->cache);

	while (1)
	{
		cmp = strcmp(url->buf_head, nptr->url);
		//fprintf(stderr, "comparing %s with %s\n", url->buf_head, nptr->url);

		if (nptr->url[0] && !cmp)
		{
			++nr_dups;
			--nr_urls_call;
			break;
		}
		else
		if (cmp < 0)
		{
			if (!nptr->left)
			{
				nptr_offset = (off_t)((char *)nptr - (char *)fctx->cache->cache);
				new_addr = (http_link_t *)wr_cache_alloc(fctx->cache, &nptr->left);
				*((unsigned long *)nptr_stack) = (unsigned long)((char *)fctx->cache->cache + nptr_offset);

				nptr->left = new_addr;

				assert(((char *)nptr - (char *)fctx->cache->cache) < fctx->cache->cache_size);
				assert(nptr->left);
				strncpy(nptr->left->url, url->buf_head, url->data_len);
				nptr->left->url[url->data_len] = 0;
				nptr->left->parent = nptr;

				break;
			}
			else
			{
				nptr = nptr->left;
				continue;
			}
		}
		else
		{
			if (!nptr->right)
			{
				nptr_offset = (off_t)((char *)nptr - (char *)fctx->cache->cache);
				new_addr = wr_cache_alloc(fctx->cache, &nptr->right);
				*((unsigned long *)nptr_stack) = (unsigned long)((char *)fctx->cache->cache + nptr_offset);
				assert(((char *)nptr - (char *)fctx->cache->cache) < fctx->cache->cache_size);

				nptr->right = new_addr;

				assert(nptr->right);
				strncpy(nptr->right->url, url->buf_head, url->data_len);
				nptr->right->url[url->data_len] = 0;
				nptr->right->parent = nptr;

				//fprintf(stderr, "copied %s to node @ %p (%d)\n", url->buf_head, nptr->right, wr_cache_nr_used(fctx->cache));
				break;
			}
			else
			{
				nptr = nptr->right;
				continue;
			}
		}
	}

	wr_cache_unlock(fctx->cache);

	return 0;
}

/**
 * parse_links - parse links from page and store in URL cache within fctx
 *			checking for duplicate URLs in URL cache within dctx.
 * @http: our HTTP object with remote host info
 * @fctx: context of cache we are filling
 * @dctx: context of cache within which we are checking for duplicates
 */
int
parse_links(struct http_t *http, struct cache_ctx *fctx, struct cache_ctx *dctx)
{
	assert(http);
	assert(fctx);
	assert(dctx);

	char *p = NULL;
	char *savep = NULL;
	char delim;
	int url_type_idx = 0;
	size_t url_len = 0;
	buf_t *buf = &http_rbuf(http);
	buf_t url;
	buf_t full_url;
	buf_t path;

	if (buf_init(&url, HTTP_URL_MAX) < 0)
		goto fail;

	if (buf_init(&full_url, HTTP_URL_MAX) < 0)
		goto fail_destroy_bufs;

	if (buf_init(&path, path_max) < 0)
		goto fail_destroy_bufs;

	savep = buf->buf_head;

	nr_already = 0;
	nr_twins = 0;
	nr_dups = 0;
	nr_urls_call = 0;

	while (1)
	{
		buf_clear(&url);
		buf_clear(&full_url);
		buf_clear(&path);

		p = strstr(savep, url_types[url_type_idx].string);
		delim = url_types[url_type_idx].delim;

		if (!p || p >= buf->buf_tail)
		{
			++url_type_idx;

			if (url_types[url_type_idx].delim == 0)
				break;

			savep = buf->buf_head;
			continue;
		}

		savep = (p += url_types[url_type_idx].len);
		p = memchr(savep, delim, (buf->buf_tail - savep));

		if (!p)
		{
			++url_type_idx;

			if (url_types[url_type_idx].delim == 0)
				break;

			savep = buf->buf_head;
			continue;
		}

		url_len = (p - savep);

		if (!url_len || url_len >= HTTP_URL_MAX)
		{
			savep = ++p;
			continue;
		}

		assert(url_len > 0);
		assert(url_len < HTTP_URL_MAX);

		buf_append_ex(&url, savep, url_len);
		make_full_url(http, &url, &full_url);

		if (!__url_acceptable(http, fctx, dctx, &full_url))
		{
			savep = ++p;
			continue;
		}

		if (__insert_link(fctx, &full_url) < 0)
			goto fail_destroy_bufs;

		savep = ++p;
		++nr_urls_call;
	}

	buf_destroy(&url);
	buf_destroy(&full_url);
	buf_destroy(&path);

	return 0;

	fail_destroy_bufs:
	buf_destroy(&url);
	buf_destroy(&full_url);
	buf_destroy(&path);

	fail:
	return -1;
}

int
do_request(struct http_t *http)
{
	assert(http);

	int status_code = 0;

	/*
	 * Save bandwidth: send HEAD first.
	 */
	if (http_send_request(http, HTTP_HEAD) < 0)
		goto fail;

	if (http_recv_response(http) < 0)
		goto fail;

	status_code = http_status_code_int(&http_rbuf(http));

	update_status_code(status_code);

	if (HTTP_OK != status_code)
		return status_code;

	if (local_archive_exists(http->full_url))
		return HTTP_ALREADY_EXISTS;

	if (http_connection_closed(http))
	{
		//fprintf(stdout, "%s%sRemote peer closed connection%s\n", COL_RED, ACTION_DONE_STR, COL_END);
		//__show_response_header(&http_rbuf(http));
		update_operation_status("Remote peer closed connection");
		http_reconnect(http);
	}

	status_code &= ~status_code;
	http_send_request(http, HTTP_GET);
	http_recv_response(http);

	status_code = http_status_code_int(&http_rbuf(http));

	update_status_code(status_code);

	return status_code;

	fail:
	return -1;
}

/**
 * reap - archive the pages in the link cache,
 *    choose one at random and return that choice. That will be
 *    our next page from which to parse links.
 * @fctx->cache: the cache of parsed links
 * @conn: our struct with connection context
 */
int
reap(struct http_t *http)
{
	assert(http);

	int nr_links = 0;
	int nr_links_sibling;
	int fill = 1;
	int status_code = 0;
	int i;
	size_t len;
	http_link_t *link;
	buf_t *wbuf = &http_wbuf(http);
	buf_t *rbuf = &http_rbuf(http);

	trailing_slash_off(wrctx);
/*
 * As we archive the pages from URLs stored in one cache,
 * we fill the sibling cache with URLs to follow in the next
 * iteration of the while loop. We use the CACHE_SWITCH flag
 * for using one while filling the other. Fill until we pass
 * a threshold number of URLs we wish to have for archiving
 * next. Stop filling when FILL == 0.
 *
 * Base case for the loop is our 'crawl depth' being equal
 * to CRAWL_DEPTH (#iterations of while loop).
 */

	while (1)
	{
		fill = 1;

		if (cache1.state == DRAINING)
		{
			link = (http_link_t *)cache1.cache;
			nr_links = wr_cache_nr_used(cache1.cache);

#ifdef DEBUG
			fprintf(stderr, "Deconstructing binary tree in cache 2\n");
#endif
			deconstruct_btree(cache2.root, cache2.cache);

			wr_cache_clear_all(cache2.cache);
			if (cache2.cache->nr_assigned > 0)
				cache2.cache->nr_assigned = 0;

			assert(wr_cache_nr_used(cache2.cache) == 0);
			cache2.root = NULL;

			update_cache_status(1, FL_CACHE_STATUS_DRAINING);

			if (fill)
				update_cache_status(2, FL_CACHE_STATUS_FILLING);

			update_operation_status("Draining URL cache 1");
		}
		else
		{
			link = (http_link_t *)cache2.cache;
			nr_links = wr_cache_nr_used(cache2.cache);

#ifdef DEBUG
			fprintf(stderr, "Deconstructing binary tree in cache 1\n");
#endif
			deconstruct_btree(cache1.root, cache1.cache);

			wr_cache_clear_all(cache1.cache);
			if (cache1.cache->nr_assigned > 0)
				cache1.cache->nr_assigned = 0;

			assert(wr_cache_nr_used(cache1.cache) == 0);
			cache1.root = NULL;

			update_cache_status(2, FL_CACHE_STATUS_DRAINING);

			if (fill)
				update_cache_status(1, FL_CACHE_STATUS_FILLING);

			update_operation_status("Draining URL cache 2");
		}

		if (!nr_links)
			break;

		url_cnt = nr_links;

		for (i = 0; i < nr_links; ++i)
		{
			buf_clear(wbuf);
			len = strlen(link->url);

			if (!len)
			{
				++link;
				continue;
			}

			assert(len < HTTP_URL_MAX);

			strcpy(http->full_url, link->url);

			if (!http_parse_page(http->full_url, http->page))
				continue;

			BLOCK_SIGNAL(SIGINT);
			sleep(crawl_delay(wrctx));
			UNBLOCK_SIGNAL(SIGINT);

			http_check_host(http);
			update_current_url(http->full_url);
			status_code = do_request(http);

			if (status_code < 0)
				goto fail;

			++(link->nr_requests);

			switch((unsigned int)status_code)
			{
				case HTTP_OK:
				case HTTP_GONE:
				case HTTP_NOT_FOUND: /* don't want to keep requesting the link and getting 404, so just archive it */
					break;
				case HTTP_BAD_REQUEST:

					buf_clear(wbuf);
					buf_clear(rbuf);

					http_reconnect(http);

					goto next;
					break;
				case HTTP_METHOD_NOT_ALLOWED:
				case HTTP_FORBIDDEN:
				case HTTP_INTERNAL_ERROR:
				case HTTP_BAD_GATEWAY:
				case HTTP_SERVICE_UNAV:
				case HTTP_GATEWAY_TIMEOUT:

					buf_clear(wbuf);
					buf_clear(rbuf);

					http_reconnect(http);

					goto next;
					break;
				case HTTP_IS_XDOMAIN:
				case HTTP_ALREADY_EXISTS:
				case FL_HTTP_SKIP_LINK:
					goto next;
				case HTTP_OPERATION_TIMEOUT:

					buf_clear(rbuf);

					if (!http->host[0])
						strcpy(http->host, http->primary_host);

					http_reconnect(http);

					goto next;
					break;
				default:
					put_error_msg("Unknown HTTP status code returned (%d)", status_code);
					goto fail;
			}

			if (fill)
			{
				if (__url_parseable(http->full_url))
				{
					if (cache1.state == DRAINING)
					{
/*
 * parse_links(struct http_t *, struct cache_ctx *FCTX, struct cache_ctx *DCTX)
 */
						parse_links(http, &cache2, &cache1);
						nr_links_sibling = wr_cache_nr_used(cache2.cache);
						update_cache2_count(nr_links_sibling);
					}
					else
					{
						parse_links(http, &cache1, &cache2);
						nr_links_sibling = wr_cache_nr_used(cache1.cache);
						update_cache1_count(nr_links_sibling);
					}

					if (nr_links_sibling >= NR_LINKS_THRESHOLD)
					{
						fill = 0;
/*
 * if cache1 is draining, then it's cache2 that's full, and vice versa.
 */
						update_cache_status(cache1.state == DRAINING ? 2 : 1, FL_CACHE_STATUS_FULL);
					}
				}
			}

			archive_page(http);

			next:
			++link;
			--url_cnt;

			if (cache1.state == FILLING)
				update_cache1_count(url_cnt);
			else
				update_cache2_count(url_cnt);

			clear_error_msg();

			trailing_slash_off(wrctx);
		} /* for (i = 0; i < nr_links; ++i) */

		++current_depth;

		if (cache1.state == FILLING)
			cache1.state = DRAINING;
		else
			cache1.state = FILLING;

		if (cache2.state == FILLING)
			cache2.state = DRAINING;
		else
			cache2.state = FILLING;

		//flip_cache_state(cache1);
		//flip_cache_state(cache2);

		if (current_depth >= crawl_depth(wrctx))
		{
			update_operation_status("Reached maximum crawl depth");
			break;
		}
	} /* while (1) */

	return 0;

	fail:
	return -1;
}
