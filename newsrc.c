/**
 * Copyright (C) 1998 Brandon Long <blong@fiction.net>
 * Copyright (C) 1999 Andrej Gritsenko <andrej@lucky.net>
 * Copyright (C) 2000-2012 Vsevolod Volkov <vvv@mutt.org.ua>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "mutt.h"
#include "account.h"
#include "bcache.h"
#include "context.h"
#include "format_flags.h"
#include "globals.h"
#include "hash.h"
#include "header.h"
#include "lib.h"
#include "mutt_curses.h"
#include "mutt_socket.h"
#include "mx.h"
#include "nntp.h"
#include "options.h"
#include "protos.h"
#include "sort.h"
#include "url.h"
#ifdef USE_HCACHE
#include "hcache/hcache.h"
#endif

struct BodyCache;

/* Find NntpData for given newsgroup or add it */
static struct NntpData *nntp_data_find(struct NntpServer *nserv, const char *group)
{
  struct NntpData *nntp_data = hash_find(nserv->groups_hash, group);

  if (!nntp_data)
  {
    int len = strlen(group) + 1;
    /* create NntpData structure and add it to hash */
    nntp_data = safe_calloc(1, sizeof(struct NntpData) + len);
    nntp_data->group = (char *) nntp_data + sizeof(struct NntpData);
    strfcpy(nntp_data->group, group, len);
    nntp_data->nserv = nserv;
    nntp_data->deleted = true;
    if (nserv->groups_hash->nelem < nserv->groups_hash->curnelem * 2)
      nserv->groups_hash =
          hash_resize(nserv->groups_hash, nserv->groups_hash->nelem * 2, 0);
    hash_insert(nserv->groups_hash, nntp_data->group, nntp_data);

    /* add NntpData to list */
    if (nserv->groups_num >= nserv->groups_max)
    {
      nserv->groups_max *= 2;
      safe_realloc(&nserv->groups_list, nserv->groups_max * sizeof(nntp_data));
    }
    nserv->groups_list[nserv->groups_num++] = nntp_data;
  }
  return nntp_data;
}

/* Remove all temporarily cache files */
void nntp_acache_free(struct NntpData *nntp_data)
{
  for (int i = 0; i < NNTP_ACACHE_LEN; i++)
  {
    if (nntp_data->acache[i].path)
    {
      unlink(nntp_data->acache[i].path);
      FREE(&nntp_data->acache[i].path);
    }
  }
}

/* Free NntpData, used to destroy hash elements */
void nntp_data_free(void *data)
{
  struct NntpData *nntp_data = data;

  if (!nntp_data)
    return;
  nntp_acache_free(nntp_data);
  mutt_bcache_close(&nntp_data->bcache);
  FREE(&nntp_data->newsrc_ent);
  FREE(&nntp_data->desc);
  FREE(&data);
}

/* Unlock and close .newsrc file */
void nntp_newsrc_close(struct NntpServer *nserv)
{
  if (!nserv->newsrc_fp)
    return;

  mutt_debug(1, "Unlocking %s\n", nserv->newsrc_file);
  mx_unlock_file(nserv->newsrc_file, fileno(nserv->newsrc_fp), 0);
  safe_fclose(&nserv->newsrc_fp);
}

/* calculate number of unread articles using .newsrc data */
void nntp_group_unread_stat(struct NntpData *nntp_data)
{
  anum_t first, last;

  nntp_data->unread = 0;
  if (nntp_data->lastMessage == 0 || nntp_data->firstMessage > nntp_data->lastMessage)
    return;

  nntp_data->unread = nntp_data->lastMessage - nntp_data->firstMessage + 1;
  for (unsigned int i = 0; i < nntp_data->newsrc_len; i++)
  {
    first = nntp_data->newsrc_ent[i].first;
    if (first < nntp_data->firstMessage)
      first = nntp_data->firstMessage;
    last = nntp_data->newsrc_ent[i].last;
    if (last > nntp_data->lastMessage)
      last = nntp_data->lastMessage;
    if (first <= last)
      nntp_data->unread -= last - first + 1;
  }
}

/* Parse .newsrc file:
 *  0 - not changed
 *  1 - parsed
 * -1 - error */
int nntp_newsrc_parse(struct NntpServer *nserv)
{
  char *line = NULL;
  struct stat sb;

  if (nserv->newsrc_fp)
  {
    /* if we already have a handle, close it and reopen */
    safe_fclose(&nserv->newsrc_fp);
  }
  else
  {
    /* if file doesn't exist, create it */
    nserv->newsrc_fp = safe_fopen(nserv->newsrc_file, "a");
    safe_fclose(&nserv->newsrc_fp);
  }

  /* open .newsrc */
  nserv->newsrc_fp = safe_fopen(nserv->newsrc_file, "r");
  if (!nserv->newsrc_fp)
  {
    mutt_perror(nserv->newsrc_file);
    mutt_sleep(2);
    return -1;
  }

  /* lock it */
  mutt_debug(1, "Locking %s\n", nserv->newsrc_file);
  if (mx_lock_file(nserv->newsrc_file, fileno(nserv->newsrc_fp), 0, 0, 1))
  {
    safe_fclose(&nserv->newsrc_fp);
    return -1;
  }

  if (stat(nserv->newsrc_file, &sb))
  {
    mutt_perror(nserv->newsrc_file);
    nntp_newsrc_close(nserv);
    mutt_sleep(2);
    return -1;
  }

  if (nserv->size == sb.st_size && nserv->mtime == sb.st_mtime)
    return 0;

  nserv->size = sb.st_size;
  nserv->mtime = sb.st_mtime;
  nserv->newsrc_modified = true;
  mutt_debug(1, "Parsing %s\n", nserv->newsrc_file);

  /* .newsrc has been externally modified or hasn't been loaded yet */
  for (unsigned int i = 0; i < nserv->groups_num; i++)
  {
    struct NntpData *nntp_data = nserv->groups_list[i];

    if (!nntp_data)
      continue;

    nntp_data->subscribed = false;
    nntp_data->newsrc_len = 0;
    FREE(&nntp_data->newsrc_ent);
  }

  line = safe_malloc(sb.st_size + 1);
  while (sb.st_size && fgets(line, sb.st_size + 1, nserv->newsrc_fp))
  {
    char *b = NULL, *h = NULL, *p = NULL;
    unsigned int j = 1;
    bool subs = false;
    struct NntpData *nntp_data = NULL;

    /* find end of newsgroup name */
    p = strpbrk(line, ":!");
    if (!p)
      continue;

    /* ":" - subscribed, "!" - unsubscribed */
    if (*p == ':')
      subs = true;
    *p++ = '\0';

    /* get newsgroup data */
    nntp_data = nntp_data_find(nserv, line);
    FREE(&nntp_data->newsrc_ent);

    /* count number of entries */
    b = p;
    while (*b)
      if (*b++ == ',')
        j++;
    nntp_data->newsrc_ent = safe_calloc(j, sizeof(struct NewsrcEntry));
    nntp_data->subscribed = subs;

    /* parse entries */
    j = 0;
    while (p)
    {
      b = p;

      /* find end of entry */
      p = strchr(p, ',');
      if (p)
        *p++ = '\0';

      /* first-last or single number */
      h = strchr(b, '-');
      if (h)
        *h++ = '\0';
      else
        h = b;

      if (sscanf(b, ANUM, &nntp_data->newsrc_ent[j].first) == 1 &&
          sscanf(h, ANUM, &nntp_data->newsrc_ent[j].last) == 1)
        j++;
    }
    if (j == 0)
    {
      nntp_data->newsrc_ent[j].first = 1;
      nntp_data->newsrc_ent[j].last = 0;
      j++;
    }
    if (nntp_data->lastMessage == 0)
      nntp_data->lastMessage = nntp_data->newsrc_ent[j - 1].last;
    nntp_data->newsrc_len = j;
    safe_realloc(&nntp_data->newsrc_ent, j * sizeof(struct NewsrcEntry));
    nntp_group_unread_stat(nntp_data);
    mutt_debug(2, "nntp_newsrc_parse: %s\n", nntp_data->group);
  }
  FREE(&line);
  return 1;
}

/* Generate array of .newsrc entries */
void nntp_newsrc_gen_entries(struct Context *ctx)
{
  struct NntpData *nntp_data = ctx->data;
  anum_t last = 0, first = 1;
  int series;
  int save_sort = SORT_ORDER;
  unsigned int entries;

  if (Sort != SORT_ORDER)
  {
    save_sort = Sort;
    Sort = SORT_ORDER;
    mutt_sort_headers(ctx, 0);
  }

  entries = nntp_data->newsrc_len;
  if (!entries)
  {
    entries = 5;
    nntp_data->newsrc_ent = safe_calloc(entries, sizeof(struct NewsrcEntry));
  }

  /* Set up to fake initial sequence from 1 to the article before the
   * first article in our list */
  nntp_data->newsrc_len = 0;
  series = 1;
  for (int i = 0; i < ctx->msgcount; i++)
  {
    /* search for first unread */
    if (series)
    {
      /* We don't actually check sequential order, since we mark
       * "missing" entries as read/deleted */
      last = NHDR(ctx->hdrs[i])->article_num;
      if (last >= nntp_data->firstMessage && !ctx->hdrs[i]->deleted &&
          !ctx->hdrs[i]->read)
      {
        if (nntp_data->newsrc_len >= entries)
        {
          entries *= 2;
          safe_realloc(&nntp_data->newsrc_ent, entries * sizeof(struct NewsrcEntry));
        }
        nntp_data->newsrc_ent[nntp_data->newsrc_len].first = first;
        nntp_data->newsrc_ent[nntp_data->newsrc_len].last = last - 1;
        nntp_data->newsrc_len++;
        series = 0;
      }
    }

    /* search for first read */
    else
    {
      if (ctx->hdrs[i]->deleted || ctx->hdrs[i]->read)
      {
        first = last + 1;
        series = 1;
      }
      last = NHDR(ctx->hdrs[i])->article_num;
    }
  }

  if (series && first <= nntp_data->lastLoaded)
  {
    if (nntp_data->newsrc_len >= entries)
    {
      entries++;
      safe_realloc(&nntp_data->newsrc_ent, entries * sizeof(struct NewsrcEntry));
    }
    nntp_data->newsrc_ent[nntp_data->newsrc_len].first = first;
    nntp_data->newsrc_ent[nntp_data->newsrc_len].last = nntp_data->lastLoaded;
    nntp_data->newsrc_len++;
  }
  safe_realloc(&nntp_data->newsrc_ent, nntp_data->newsrc_len * sizeof(struct NewsrcEntry));

  if (save_sort != Sort)
  {
    Sort = save_sort;
    mutt_sort_headers(ctx, 0);
  }
}

/* Update file with new contents */
static int update_file(char *filename, char *buf)
{
  FILE *fp = NULL;
  char tmpfile[_POSIX_PATH_MAX];
  int rc = -1;

  while (1)
  {
    snprintf(tmpfile, sizeof(tmpfile), "%s.tmp", filename);
    fp = safe_fopen(tmpfile, "w");
    if (!fp)
    {
      mutt_perror(tmpfile);
      *tmpfile = '\0';
      break;
    }
    if (fputs(buf, fp) == EOF)
    {
      mutt_perror(tmpfile);
      break;
    }
    if (safe_fclose(&fp) == EOF)
    {
      mutt_perror(tmpfile);
      fp = NULL;
      break;
    }
    fp = NULL;
    if (rename(tmpfile, filename) < 0)
    {
      mutt_perror(filename);
      break;
    }
    *tmpfile = '\0';
    rc = 0;
    break;
  }
  if (fp)
    safe_fclose(&fp);
  if (*tmpfile)
    unlink(tmpfile);
  if (rc)
    mutt_sleep(2);
  return rc;
}

/* Update .newsrc file */
int nntp_newsrc_update(struct NntpServer *nserv)
{
  char *buf = NULL;
  size_t buflen, off;
  int rc = -1;

  if (!nserv)
    return -1;

  buflen = 10 * LONG_STRING;
  buf = safe_calloc(1, buflen);
  off = 0;

  /* we will generate full newsrc here */
  for (unsigned int i = 0; i < nserv->groups_num; i++)
  {
    struct NntpData *nntp_data = nserv->groups_list[i];
    unsigned int n;

    if (!nntp_data || !nntp_data->newsrc_ent)
      continue;

    /* write newsgroup name */
    if (off + strlen(nntp_data->group) + 3 > buflen)
    {
      buflen *= 2;
      safe_realloc(&buf, buflen);
    }
    snprintf(buf + off, buflen - off, "%s%c ", nntp_data->group,
             nntp_data->subscribed ? ':' : '!');
    off += strlen(buf + off);

    /* write entries */
    for (n = 0; n < nntp_data->newsrc_len; n++)
    {
      if (off + LONG_STRING > buflen)
      {
        buflen *= 2;
        safe_realloc(&buf, buflen);
      }
      if (n)
        buf[off++] = ',';
      if (nntp_data->newsrc_ent[n].first == nntp_data->newsrc_ent[n].last)
        snprintf(buf + off, buflen - off, "%d", nntp_data->newsrc_ent[n].first);
      else if (nntp_data->newsrc_ent[n].first < nntp_data->newsrc_ent[n].last)
        snprintf(buf + off, buflen - off, "%d-%d",
                 nntp_data->newsrc_ent[n].first, nntp_data->newsrc_ent[n].last);
      off += strlen(buf + off);
    }
    buf[off++] = '\n';
  }
  buf[off] = '\0';

  /* newrc being fully rewritten */
  mutt_debug(1, "Updating %s\n", nserv->newsrc_file);
  if (nserv->newsrc_file && update_file(nserv->newsrc_file, buf) == 0)
  {
    struct stat sb;

    rc = stat(nserv->newsrc_file, &sb);
    if (rc == 0)
    {
      nserv->size = sb.st_size;
      nserv->mtime = sb.st_mtime;
    }
    else
    {
      mutt_perror(nserv->newsrc_file);
      mutt_sleep(2);
    }
  }
  FREE(&buf);
  return rc;
}

/* Make fully qualified cache file name */
static void cache_expand(char *dst, size_t dstlen, struct Account *acct, char *src)
{
  char *c = NULL;
  char file[_POSIX_PATH_MAX];

  /* server subdirectory */
  if (acct)
  {
    struct CissUrl url;

    mutt_account_tourl(acct, &url);
    url.path = src;
    url_ciss_tostring(&url, file, sizeof(file), U_PATH);
  }
  else
    strfcpy(file, src ? src : "", sizeof(file));

  snprintf(dst, dstlen, "%s/%s", NewsCacheDir, file);

  /* remove trailing slash */
  c = dst + strlen(dst) - 1;
  if (*c == '/')
    *c = '\0';
  mutt_expand_path(dst, dstlen);
}

/* Make fully qualified url from newsgroup name */
void nntp_expand_path(char *line, size_t len, struct Account *acct)
{
  struct CissUrl url;

  mutt_account_tourl(acct, &url);
  url.path = safe_strdup(line);
  url_ciss_tostring(&url, line, len, 0);
  FREE(&url.path);
}

/* Parse newsgroup */
int nntp_add_group(char *line, void *data)
{
  struct NntpServer *nserv = data;
  struct NntpData *nntp_data = NULL;
  char group[LONG_STRING];
  char desc[HUGE_STRING] = "";
  char mod;
  anum_t first, last;

  if (!nserv || !line)
    return 0;

  if (sscanf(line, "%s " ANUM " " ANUM " %c %[^\n]", group, &last, &first, &mod, desc) < 4)
    return 0;

  nntp_data = nntp_data_find(nserv, group);
  nntp_data->deleted = false;
  nntp_data->firstMessage = first;
  nntp_data->lastMessage = last;
  nntp_data->allowed = (mod == 'y') || (mod == 'm');
  mutt_str_replace(&nntp_data->desc, desc);
  if (nntp_data->newsrc_ent || nntp_data->lastCached)
    nntp_group_unread_stat(nntp_data);
  else if (nntp_data->lastMessage && nntp_data->firstMessage <= nntp_data->lastMessage)
    nntp_data->unread = nntp_data->lastMessage - nntp_data->firstMessage + 1;
  else
    nntp_data->unread = 0;
  return 0;
}

/* Load list of all newsgroups from cache */
static int active_get_cache(struct NntpServer *nserv)
{
  char buf[HUGE_STRING];
  char file[_POSIX_PATH_MAX];
  time_t t;
  FILE *fp = NULL;

  cache_expand(file, sizeof(file), &nserv->conn->account, ".active");
  mutt_debug(1, "Parsing %s\n", file);
  fp = safe_fopen(file, "r");
  if (!fp)
    return -1;

  if (fgets(buf, sizeof(buf), fp) == NULL || sscanf(buf, "%ld%s", &t, file) != 1 || t == 0)
  {
    safe_fclose(&fp);
    return -1;
  }
  nserv->newgroups_time = t;

  mutt_message(_("Loading list of groups from cache..."));
  while (fgets(buf, sizeof(buf), fp))
    nntp_add_group(buf, nserv);
  nntp_add_group(NULL, NULL);
  safe_fclose(&fp);
  mutt_clear_error();
  return 0;
}

/* Save list of all newsgroups to cache */
int nntp_active_save_cache(struct NntpServer *nserv)
{
  char file[_POSIX_PATH_MAX];
  char *buf = NULL;
  size_t buflen, off;
  int rc;

  if (!nserv->cacheable)
    return 0;

  buflen = 10 * LONG_STRING;
  buf = safe_calloc(1, buflen);
  snprintf(buf, buflen, "%lu\n", (unsigned long) nserv->newgroups_time);
  off = strlen(buf);

  for (unsigned int i = 0; i < nserv->groups_num; i++)
  {
    struct NntpData *nntp_data = nserv->groups_list[i];

    if (!nntp_data || nntp_data->deleted)
      continue;

    if (off + strlen(nntp_data->group) + (nntp_data->desc ? strlen(nntp_data->desc) : 0) + 50 > buflen)
    {
      buflen *= 2;
      safe_realloc(&buf, buflen);
    }
    snprintf(buf + off, buflen - off, "%s %d %d %c%s%s\n", nntp_data->group,
             nntp_data->lastMessage, nntp_data->firstMessage,
             nntp_data->allowed ? 'y' : 'n', nntp_data->desc ? " " : "",
             nntp_data->desc ? nntp_data->desc : "");
    off += strlen(buf + off);
  }

  cache_expand(file, sizeof(file), &nserv->conn->account, ".active");
  mutt_debug(1, "Updating %s\n", file);
  rc = update_file(file, buf);
  FREE(&buf);
  return rc;
}

#ifdef USE_HCACHE
/* Used by mutt_hcache_open() to compose hcache file name */
static int nntp_hcache_namer(const char *path, char *dest, size_t destlen)
{
  return snprintf(dest, destlen, "%s.hcache", path);
}

/* Open newsgroup hcache */
header_cache_t *nntp_hcache_open(struct NntpData *nntp_data)
{
  struct CissUrl url;
  char file[_POSIX_PATH_MAX];

  if (!nntp_data->nserv || !nntp_data->nserv->cacheable ||
      !nntp_data->nserv->conn || !nntp_data->group ||
      !(nntp_data->newsrc_ent || nntp_data->subscribed || option(OPTSAVEUNSUB)))
    return NULL;

  mutt_account_tourl(&nntp_data->nserv->conn->account, &url);
  url.path = nntp_data->group;
  url_ciss_tostring(&url, file, sizeof(file), U_PATH);
  return mutt_hcache_open(NewsCacheDir, file, nntp_hcache_namer);
}

/* Remove stale cached headers */
void nntp_hcache_update(struct NntpData *nntp_data, header_cache_t *hc)
{
  char buf[16];
  int old = 0;
  void *hdata = NULL;
  anum_t first, last, current;

  if (!hc)
    return;

  /* fetch previous values of first and last */
  hdata = mutt_hcache_fetch_raw(hc, "index", 5);
  if (hdata)
  {
    mutt_debug(2, "nntp_hcache_update: mutt_hcache_fetch index: %s\n", (char *) hdata);
    if (sscanf(hdata, ANUM " " ANUM, &first, &last) == 2)
    {
      old = 1;
      nntp_data->lastCached = last;

      /* clean removed headers from cache */
      for (current = first; current <= last; current++)
      {
        if (current >= nntp_data->firstMessage && current <= nntp_data->lastMessage)
          continue;

        snprintf(buf, sizeof(buf), "%d", current);
        mutt_debug(2, "nntp_hcache_update: mutt_hcache_delete %s\n", buf);
        mutt_hcache_delete(hc, buf, strlen(buf));
      }
    }
    mutt_hcache_free(hc, &hdata);
  }

  /* store current values of first and last */
  if (!old || nntp_data->firstMessage != first || nntp_data->lastMessage != last)
  {
    snprintf(buf, sizeof(buf), "%u %u", nntp_data->firstMessage, nntp_data->lastMessage);
    mutt_debug(2, "nntp_hcache_update: mutt_hcache_store index: %s\n", buf);
    mutt_hcache_store_raw(hc, "index", 5, buf, strlen(buf));
  }
}
#endif

/* Remove bcache file */
static int nntp_bcache_delete(const char *id, struct BodyCache *bcache, void *data)
{
  struct NntpData *nntp_data = data;
  anum_t anum;
  char c;

  if (!nntp_data || sscanf(id, ANUM "%c", &anum, &c) != 1 ||
      anum < nntp_data->firstMessage || anum > nntp_data->lastMessage)
  {
    if (nntp_data)
      mutt_debug(2, "nntp_bcache_delete: mutt_bcache_del %s\n", id);
    mutt_bcache_del(bcache, id);
  }
  return 0;
}

/* Remove stale cached messages */
void nntp_bcache_update(struct NntpData *nntp_data)
{
  mutt_bcache_list(nntp_data->bcache, nntp_bcache_delete, nntp_data);
}

/* Remove hcache and bcache of newsgroup */
void nntp_delete_group_cache(struct NntpData *nntp_data)
{
  if (!nntp_data || !nntp_data->nserv || !nntp_data->nserv->cacheable)
    return;

#ifdef USE_HCACHE
  char file[_POSIX_PATH_MAX];
  nntp_hcache_namer(nntp_data->group, file, sizeof(file));
  cache_expand(file, sizeof(file), &nntp_data->nserv->conn->account, file);
  unlink(file);
  nntp_data->lastCached = 0;
  mutt_debug(2, "nntp_delete_group_cache: %s\n", file);
#endif

  if (!nntp_data->bcache)
    nntp_data->bcache =
        mutt_bcache_open(&nntp_data->nserv->conn->account, nntp_data->group);
  if (nntp_data->bcache)
  {
    mutt_debug(2, "nntp_delete_group_cache: %s/*\n", nntp_data->group);
    mutt_bcache_list(nntp_data->bcache, nntp_bcache_delete, NULL);
    mutt_bcache_close(&nntp_data->bcache);
  }
}

/* Remove hcache and bcache of all unexistent and unsubscribed newsgroups */
void nntp_clear_cache(struct NntpServer *nserv)
{
  char file[_POSIX_PATH_MAX];
  char *fp = NULL;
  struct dirent *entry = NULL;
  DIR *dp = NULL;

  if (!nserv || !nserv->cacheable)
    return;

  cache_expand(file, sizeof(file), &nserv->conn->account, NULL);
  dp = opendir(file);
  if (dp)
  {
    safe_strncat(file, sizeof(file), "/", 1);
    fp = file + strlen(file);
    while ((entry = readdir(dp)))
    {
      char *group = entry->d_name;
      struct stat sb;
      struct NntpData *nntp_data = NULL;
      struct NntpData nntp_tmp;

      if ((mutt_strcmp(group, ".") == 0) || (mutt_strcmp(group, "..") == 0))
        continue;
      *fp = '\0';
      safe_strncat(file, sizeof(file), group, strlen(group));
      if (stat(file, &sb))
        continue;

#ifdef USE_HCACHE
      if (S_ISREG(sb.st_mode))
      {
        char *ext = group + strlen(group) - 7;
        if (strlen(group) < 8 || (mutt_strcmp(ext, ".hcache") != 0))
          continue;
        *ext = '\0';
      }
      else
#endif
          if (!S_ISDIR(sb.st_mode))
        continue;

      nntp_data = hash_find(nserv->groups_hash, group);
      if (!nntp_data)
      {
        nntp_data = &nntp_tmp;
        nntp_data->nserv = nserv;
        nntp_data->group = group;
        nntp_data->bcache = NULL;
      }
      else if (nntp_data->newsrc_ent || nntp_data->subscribed || option(OPTSAVEUNSUB))
        continue;

      nntp_delete_group_cache(nntp_data);
      if (S_ISDIR(sb.st_mode))
      {
        rmdir(file);
        mutt_debug(2, "nntp_clear_cache: %s\n", file);
      }
    }
    closedir(dp);
  }
  return;
}

/* %a = account url
 * %p = port
 * %P = port if specified
 * %s = news server name
 * %S = url schema
 * %u = username */
const char *nntp_format_str(char *dest, size_t destlen, size_t col, int cols, char op,
                            const char *src, const char *fmt, const char *ifstring,
                            const char *elsestring, unsigned long data, format_flag flags)
{
  struct NntpServer *nserv = (struct NntpServer *) data;
  struct Account *acct = &nserv->conn->account;
  struct CissUrl url;
  char fn[SHORT_STRING], tmp[SHORT_STRING], *p = NULL;

  switch (op)
  {
    case 'a':
      mutt_account_tourl(acct, &url);
      url_ciss_tostring(&url, fn, sizeof(fn), U_PATH);
      p = strchr(fn, '/');
      if (p)
        *p = '\0';
      snprintf(tmp, sizeof(tmp), "%%%ss", fmt);
      snprintf(dest, destlen, tmp, fn);
      break;
    case 'p':
      snprintf(tmp, sizeof(tmp), "%%%su", fmt);
      snprintf(dest, destlen, tmp, acct->port);
      break;
    case 'P':
      *dest = '\0';
      if (acct->flags & MUTT_ACCT_PORT)
      {
        snprintf(tmp, sizeof(tmp), "%%%su", fmt);
        snprintf(dest, destlen, tmp, acct->port);
      }
      break;
    case 's':
      strncpy(fn, acct->host, sizeof(fn) - 1);
      mutt_strlower(fn);
      snprintf(tmp, sizeof(tmp), "%%%ss", fmt);
      snprintf(dest, destlen, tmp, fn);
      break;
    case 'S':
      mutt_account_tourl(acct, &url);
      url_ciss_tostring(&url, fn, sizeof(fn), U_PATH);
      p = strchr(fn, ':');
      if (p)
        *p = '\0';
      snprintf(tmp, sizeof(tmp), "%%%ss", fmt);
      snprintf(dest, destlen, tmp, fn);
      break;
    case 'u':
      snprintf(tmp, sizeof(tmp), "%%%ss", fmt);
      snprintf(dest, destlen, tmp, acct->user);
      break;
  }
  return src;
}

/* Automatically loads a newsrc into memory, if necessary.
 * Checks the size/mtime of a newsrc file, if it doesn't match, load
 * again.  Hmm, if a system has broken mtimes, this might mean the file
 * is reloaded every time, which we'd have to fix. */
struct NntpServer *nntp_select_server(char *server, int leave_lock)
{
  char file[_POSIX_PATH_MAX];
#ifdef USE_HCACHE
  char *p = NULL;
#endif
  int rc;
  struct Account acct;
  struct NntpServer *nserv = NULL;
  struct NntpData *nntp_data = NULL;
  struct Connection *conn = NULL;
  struct CissUrl url;

  if (!server || !*server)
  {
    mutt_error(_("No news server defined!"));
    mutt_sleep(2);
    return NULL;
  }

  /* create account from news server url */
  acct.flags = 0;
  acct.port = NNTP_PORT;
  acct.type = MUTT_ACCT_TYPE_NNTP;
  snprintf(file, sizeof(file), "%s%s", strstr(server, "://") ? "" : "news://", server);
  if (url_parse_ciss(&url, file) < 0 || (url.path && *url.path) ||
      !(url.scheme == U_NNTP || url.scheme == U_NNTPS) ||
      mutt_account_fromurl(&acct, &url) < 0)
  {
    mutt_error(_("%s is an invalid news server specification!"), server);
    mutt_sleep(2);
    return NULL;
  }
  if (url.scheme == U_NNTPS)
  {
    acct.flags |= MUTT_ACCT_SSL;
    acct.port = NNTP_SSL_PORT;
  }

  /* find connection by account */
  conn = mutt_conn_find(NULL, &acct);
  if (!conn)
    return NULL;
  if (!(conn->account.flags & MUTT_ACCT_USER) && acct.flags & MUTT_ACCT_USER)
  {
    conn->account.flags |= MUTT_ACCT_USER;
    conn->account.user[0] = '\0';
  }

  /* news server already exists */
  nserv = conn->data;
  if (nserv)
  {
    if (nserv->status == NNTP_BYE)
      nserv->status = NNTP_NONE;
    if (nntp_open_connection(nserv) < 0)
      return NULL;

    rc = nntp_newsrc_parse(nserv);
    if (rc < 0)
      return NULL;

    /* check for new newsgroups */
    if (!leave_lock && nntp_check_new_groups(nserv) < 0)
      rc = -1;

    /* .newsrc has been externally modified */
    if (rc > 0)
      nntp_clear_cache(nserv);
    if (rc < 0 || !leave_lock)
      nntp_newsrc_close(nserv);
    return rc < 0 ? NULL : nserv;
  }

  /* new news server */
  nserv = safe_calloc(1, sizeof(struct NntpServer));
  nserv->conn = conn;
  nserv->groups_hash = hash_create(1009, 0);
  nserv->groups_max = 16;
  nserv->groups_list = safe_malloc(nserv->groups_max * sizeof(nntp_data));

  rc = nntp_open_connection(nserv);

  /* try to create cache directory and enable caching */
  nserv->cacheable = false;
  if (rc >= 0 && NewsCacheDir && *NewsCacheDir)
  {
    cache_expand(file, sizeof(file), &conn->account, NULL);
    if (mutt_mkdir(file, S_IRWXU) < 0)
    {
      mutt_error(_("Can't create %s: %s."), file, strerror(errno));
      mutt_sleep(2);
    }
    nserv->cacheable = true;
  }

  /* load .newsrc */
  if (rc >= 0)
  {
    mutt_FormatString(file, sizeof(file), 0, MuttIndexWindow->cols,
                      NONULL(NewsRc), nntp_format_str, (unsigned long) nserv, 0);
    mutt_expand_path(file, sizeof(file));
    nserv->newsrc_file = safe_strdup(file);
    rc = nntp_newsrc_parse(nserv);
  }
  if (rc >= 0)
  {
    /* try to load list of newsgroups from cache */
    if (nserv->cacheable && active_get_cache(nserv) == 0)
      rc = nntp_check_new_groups(nserv);

    /* load list of newsgroups from server */
    else
      rc = nntp_active_fetch(nserv);
  }

  if (rc >= 0)
    nntp_clear_cache(nserv);

#ifdef USE_HCACHE
  /* check cache files */
  if (rc >= 0 && nserv->cacheable)
  {
    struct dirent *entry = NULL;
    DIR *dp = opendir(file);

    if (dp)
    {
      while ((entry = readdir(dp)))
      {
        header_cache_t *hc = NULL;
        void *hdata = NULL;
        char *group = entry->d_name;

        p = group + strlen(group) - 7;
        if (strlen(group) < 8 || (strcmp(p, ".hcache") != 0))
          continue;
        *p = '\0';
        nntp_data = hash_find(nserv->groups_hash, group);
        if (!nntp_data)
          continue;

        hc = nntp_hcache_open(nntp_data);
        if (!hc)
          continue;

        /* fetch previous values of first and last */
        hdata = mutt_hcache_fetch_raw(hc, "index", 5);
        if (hdata)
        {
          anum_t first, last;

          if (sscanf(hdata, ANUM " " ANUM, &first, &last) == 2)
          {
            if (nntp_data->deleted)
            {
              nntp_data->firstMessage = first;
              nntp_data->lastMessage = last;
            }
            if (last >= nntp_data->firstMessage && last <= nntp_data->lastMessage)
            {
              nntp_data->lastCached = last;
              mutt_debug(2, "nntp_select_server: %s lastCached=%u\n", nntp_data->group, last);
            }
          }
          mutt_hcache_free(hc, &hdata);
        }
        mutt_hcache_close(hc);
      }
      closedir(dp);
    }
  }
#endif

  if (rc < 0 || !leave_lock)
    nntp_newsrc_close(nserv);

  if (rc < 0)
  {
    hash_destroy(&nserv->groups_hash, nntp_data_free);
    FREE(&nserv->groups_list);
    FREE(&nserv->newsrc_file);
    FREE(&nserv->authenticators);
    FREE(&nserv);
    mutt_socket_close(conn);
    mutt_socket_free(conn);
    return NULL;
  }

  conn->data = nserv;
  return nserv;
}

/* Full status flags are not supported by nntp, but we can fake some of them:
 * Read = a read message number is in the .newsrc
 * New = not read and not cached
 * Old = not read but cached */
void nntp_article_status(struct Context *ctx, struct Header *hdr, char *group, anum_t anum)
{
  struct NntpData *nntp_data = ctx->data;

  if (group)
    nntp_data = hash_find(nntp_data->nserv->groups_hash, group);

  if (!nntp_data)
    return;

  for (unsigned int i = 0; i < nntp_data->newsrc_len; i++)
  {
    if ((anum >= nntp_data->newsrc_ent[i].first) &&
        (anum <= nntp_data->newsrc_ent[i].last))
    {
      /* can't use mutt_set_flag() because mx_update_context()
         didn't called yet */
      hdr->read = true;
      return;
    }
  }

  /* article was not cached yet, it's new */
  if (anum > nntp_data->lastCached)
    return;

  /* article isn't read but cached, it's old */
  if (option(OPTMARKOLD))
    hdr->old = true;
}

/* Subscribe newsgroup */
struct NntpData *mutt_newsgroup_subscribe(struct NntpServer *nserv, char *group)
{
  struct NntpData *nntp_data = NULL;

  if (!nserv || !nserv->groups_hash || !group || !*group)
    return NULL;

  nntp_data = nntp_data_find(nserv, group);
  nntp_data->subscribed = true;
  if (!nntp_data->newsrc_ent)
  {
    nntp_data->newsrc_ent = safe_calloc(1, sizeof(struct NewsrcEntry));
    nntp_data->newsrc_len = 1;
    nntp_data->newsrc_ent[0].first = 1;
    nntp_data->newsrc_ent[0].last = 0;
  }
  return nntp_data;
}

/* Unsubscribe newsgroup */
struct NntpData *mutt_newsgroup_unsubscribe(struct NntpServer *nserv, char *group)
{
  struct NntpData *nntp_data = NULL;

  if (!nserv || !nserv->groups_hash || !group || !*group)
    return NULL;

  nntp_data = hash_find(nserv->groups_hash, group);
  if (!nntp_data)
    return NULL;

  nntp_data->subscribed = false;
  if (!option(OPTSAVEUNSUB))
  {
    nntp_data->newsrc_len = 0;
    FREE(&nntp_data->newsrc_ent);
  }
  return nntp_data;
}

/* Catchup newsgroup */
struct NntpData *mutt_newsgroup_catchup(struct NntpServer *nserv, char *group)
{
  struct NntpData *nntp_data = NULL;

  if (!nserv || !nserv->groups_hash || !group || !*group)
    return NULL;

  nntp_data = hash_find(nserv->groups_hash, group);
  if (!nntp_data)
    return NULL;

  if (nntp_data->newsrc_ent)
  {
    safe_realloc(&nntp_data->newsrc_ent, sizeof(struct NewsrcEntry));
    nntp_data->newsrc_len = 1;
    nntp_data->newsrc_ent[0].first = 1;
    nntp_data->newsrc_ent[0].last = nntp_data->lastMessage;
  }
  nntp_data->unread = 0;
  if (Context && Context->data == nntp_data)
  {
    for (unsigned int i = 0; i < Context->msgcount; i++)
      mutt_set_flag(Context, Context->hdrs[i], MUTT_READ, 1);
  }
  return nntp_data;
}

/* Uncatchup newsgroup */
struct NntpData *mutt_newsgroup_uncatchup(struct NntpServer *nserv, char *group)
{
  struct NntpData *nntp_data = NULL;

  if (!nserv || !nserv->groups_hash || !group || !*group)
    return NULL;

  nntp_data = hash_find(nserv->groups_hash, group);
  if (!nntp_data)
    return NULL;

  if (nntp_data->newsrc_ent)
  {
    safe_realloc(&nntp_data->newsrc_ent, sizeof(struct NewsrcEntry));
    nntp_data->newsrc_len = 1;
    nntp_data->newsrc_ent[0].first = 1;
    nntp_data->newsrc_ent[0].last = nntp_data->firstMessage - 1;
  }
  if (Context && Context->data == nntp_data)
  {
    nntp_data->unread = Context->msgcount;
    for (unsigned int i = 0; i < Context->msgcount; i++)
      mutt_set_flag(Context, Context->hdrs[i], MUTT_READ, 0);
  }
  else
  {
    nntp_data->unread = nntp_data->lastMessage;
    if (nntp_data->newsrc_ent)
      nntp_data->unread -= nntp_data->newsrc_ent[0].last;
  }
  return nntp_data;
}

/* Get first newsgroup with new messages */
void nntp_buffy(char *buf, size_t len)
{
  for (unsigned int i = 0; i < CurrentNewsSrv->groups_num; i++)
  {
    struct NntpData *nntp_data = CurrentNewsSrv->groups_list[i];

    if (!nntp_data || !nntp_data->subscribed || !nntp_data->unread)
      continue;

    if (Context && Context->magic == MUTT_NNTP &&
        (mutt_strcmp(nntp_data->group, ((struct NntpData *) Context->data)->group) == 0))
    {
      unsigned int j, unread = 0;

      for (j = 0; j < Context->msgcount; j++)
        if (!Context->hdrs[j]->read && !Context->hdrs[j]->deleted)
          unread++;
      if (!unread)
        continue;
    }
    strfcpy(buf, nntp_data->group, len);
    break;
  }
}
