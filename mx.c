/**
 * Copyright (C) 1996-2002,2010,2013 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 1999-2003 Thomas Roessler <roessler@does-not-exist.org>
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
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include "mutt.h"
#include "mx.h"
#include "address.h"
#include "ascii.h"
#include "body.h"
#include "buffy.h"
#include "context.h"
#include "copy.h"
#include "envelope.h"
#include "globals.h"
#include "hash.h"
#include "header.h"
#include "keymap.h"
#include "keymap_defs.h"
#include "lib.h"
#include "mailbox.h"
#include "ncrypt/ncrypt.h"
#include "options.h"
#include "pattern.h"
#include "protos.h"
#include "sort.h"
#include "thread.h"
#include "url.h"
#ifdef USE_SIDEBAR
#include "sidebar.h"
#endif
#ifdef USE_COMPRESSED
#include "compress.h"
#endif
#ifdef USE_IMAP
#include "imap/imap.h"
#endif
#ifdef USE_POP
#include "pop.h"
#endif
#ifdef USE_NNTP
#include "nntp.h"
#endif
#ifdef USE_NOTMUCH
#include "mutt_notmuch.h"
#endif
#ifdef USE_DOTLOCK
#include "dotlock.h"
#endif

struct MxOps *mx_get_ops(int magic)
{
  switch (magic)
  {
#ifdef USE_IMAP
    case MUTT_IMAP:
      return &mx_imap_ops;
#endif
    case MUTT_MAILDIR:
      return &mx_maildir_ops;
    case MUTT_MBOX:
      return &mx_mbox_ops;
    case MUTT_MH:
      return &mx_mh_ops;
    case MUTT_MMDF:
      return &mx_mmdf_ops;
#ifdef USE_POP
    case MUTT_POP:
      return &mx_pop_ops;
#endif
#ifdef USE_COMPRESSED
    case MUTT_COMPRESSED:
      return &mx_comp_ops;
#endif
#ifdef USE_NNTP
    case MUTT_NNTP:
      return &mx_nntp_ops;
#endif
#ifdef USE_NOTMUCH
    case MUTT_NOTMUCH:
      return &mx_notmuch_ops;
#endif
    default:
      return NULL;
  }
}

#define mutt_is_spool(s) (mutt_strcmp(Spoolfile, s) == 0)

#ifdef USE_DOTLOCK
/* parameters:
 * path - file to lock
 * retry - should retry if unable to lock?
 */
static int invoke_dotlock(const char *path, int dummy, int flags, int retry)
{
  char cmd[LONG_STRING + _POSIX_PATH_MAX];
  char f[SHORT_STRING + _POSIX_PATH_MAX];
  char r[SHORT_STRING];

  if (flags & DL_FL_RETRY)
    snprintf(r, sizeof(r), "-r %d ", retry ? MAXLOCKATTEMPT : 0);

  mutt_quote_filename(f, sizeof(f), path);

  snprintf(cmd, sizeof(cmd), "%s %s%s%s%s%s%s%s", NONULL(MuttDotlock),
           flags & DL_FL_TRY ? "-t " : "", flags & DL_FL_UNLOCK ? "-u " : "",
           flags & DL_FL_USEPRIV ? "-p " : "", flags & DL_FL_FORCE ? "-f " : "",
           flags & DL_FL_UNLINK ? "-d " : "", flags & DL_FL_RETRY ? r : "", f);

  return mutt_system(cmd);
}

static int dotlock_file(const char *path, int fd, int retry)
{
  int r;
  int flags = DL_FL_USEPRIV | DL_FL_RETRY;

  if (retry)
    retry = 1;

retry_lock:
  if ((r = invoke_dotlock(path, fd, flags, retry)) == DL_EX_EXIST)
  {
    if (!option(OPTNOCURSES))
    {
      char msg[LONG_STRING];

      snprintf(msg, sizeof(msg), _("Lock count exceeded, remove lock for %s?"), path);
      if (retry && mutt_yesorno(msg, MUTT_YES) == MUTT_YES)
      {
        flags |= DL_FL_FORCE;
        retry--;
        mutt_clear_error();
        goto retry_lock;
      }
    }
    else
    {
      mutt_error(_("Can't dotlock %s.\n"), path);
    }
  }
  return (r == DL_EX_OK ? 0 : -1);
}

static int undotlock_file(const char *path, int fd)
{
  return (invoke_dotlock(path, fd, DL_FL_USEPRIV | DL_FL_UNLOCK, 0) == DL_EX_OK ? 0 : -1);
}

#endif /* USE_DOTLOCK */

/* Args:
 *      excl            if excl != 0, request an exclusive lock
 *      dot             if dot != 0, try to dotlock the file
 *      timeout         should retry locking?
 */
int mx_lock_file(const char *path, int fd, int excl, int dot, int timeout)
{
#if defined(USE_FCNTL) || defined(USE_FLOCK)
  int count;
  int attempt;
  struct stat sb = { 0 }, prev_sb = { 0 }; /* silence gcc warnings */
#endif
  int r = 0;

#ifdef USE_FCNTL
  struct flock lck;

  memset(&lck, 0, sizeof(struct flock));
  lck.l_type = excl ? F_WRLCK : F_RDLCK;
  lck.l_whence = SEEK_SET;

  count = 0;
  attempt = 0;
  while (fcntl(fd, F_SETLK, &lck) == -1)
  {
    mutt_debug(1, "mx_lock_file(): fcntl errno %d.\n", errno);
    if (errno != EAGAIN && errno != EACCES)
    {
      mutt_perror("fcntl");
      return -1;
    }

    if (fstat(fd, &sb) != 0)
      sb.st_size = 0;

    if (count == 0)
      prev_sb = sb;

    /* only unlock file if it is unchanged */
    if (prev_sb.st_size == sb.st_size && ++count >= (timeout ? MAXLOCKATTEMPT : 0))
    {
      if (timeout)
        mutt_error(_("Timeout exceeded while attempting fcntl lock!"));
      return -1;
    }

    prev_sb = sb;

    mutt_message(_("Waiting for fcntl lock... %d"), ++attempt);
    sleep(1);
  }
#endif /* USE_FCNTL */

#ifdef USE_FLOCK
  count = 0;
  attempt = 0;
  while (flock(fd, (excl ? LOCK_EX : LOCK_SH) | LOCK_NB) == -1)
  {
    if (errno != EWOULDBLOCK)
    {
      mutt_perror("flock");
      r = -1;
      break;
    }

    if (fstat(fd, &sb) != 0)
      sb.st_size = 0;

    if (count == 0)
      prev_sb = sb;

    /* only unlock file if it is unchanged */
    if (prev_sb.st_size == sb.st_size && ++count >= (timeout ? MAXLOCKATTEMPT : 0))
    {
      if (timeout)
        mutt_error(_("Timeout exceeded while attempting flock lock!"));
      r = -1;
      break;
    }

    prev_sb = sb;

    mutt_message(_("Waiting for flock attempt... %d"), ++attempt);
    sleep(1);
  }
#endif /* USE_FLOCK */

#ifdef USE_DOTLOCK
  if (r == 0 && dot)
    r = dotlock_file(path, fd, timeout);
#endif /* USE_DOTLOCK */

  if (r != 0)
  {
/* release any other locks obtained in this routine */

#ifdef USE_FCNTL
    lck.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &lck);
#endif /* USE_FCNTL */

#ifdef USE_FLOCK
    flock(fd, LOCK_UN);
#endif /* USE_FLOCK */
  }

  return r;
}

int mx_unlock_file(const char *path, int fd, int dot)
{
#ifdef USE_FCNTL
  struct flock unlockit = { F_UNLCK, 0, 0, 0, 0 };

  memset(&unlockit, 0, sizeof(struct flock));
  unlockit.l_type = F_UNLCK;
  unlockit.l_whence = SEEK_SET;
  fcntl(fd, F_SETLK, &unlockit);
#endif

#ifdef USE_FLOCK
  flock(fd, LOCK_UN);
#endif

#ifdef USE_DOTLOCK
  if (dot)
    undotlock_file(path, fd);
#endif

  return 0;
}

static void mx_unlink_empty(const char *path)
{
  int fd;
#ifndef USE_DOTLOCK
  struct stat sb;
#endif

  if ((fd = open(path, O_RDWR)) == -1)
    return;

  if (mx_lock_file(path, fd, 1, 0, 1) == -1)
  {
    close(fd);
    return;
  }

#ifdef USE_DOTLOCK
  invoke_dotlock(path, fd, DL_FL_UNLINK, 1);
#else
  if (fstat(fd, &sb) == 0 && sb.st_size == 0)
    unlink(path);
#endif

  mx_unlock_file(path, fd, 0);
  close(fd);
}

/* try to figure out what type of mailbox ``path'' is
 *
 * return values:
 *      MUTT_*  mailbox type
 *      0       not a mailbox
 *      -1      error
 */

#ifdef USE_IMAP

bool mx_is_imap(const char *p)
{
  url_scheme_t scheme;

  if (!p)
    return false;

  if (*p == '{')
    return true;

  scheme = url_check_scheme(p);
  if (scheme == U_IMAP || scheme == U_IMAPS)
    return true;

  return false;
}

#endif

#ifdef USE_POP
bool mx_is_pop(const char *p)
{
  url_scheme_t scheme;

  if (!p)
    return false;

  scheme = url_check_scheme(p);
  if (scheme == U_POP || scheme == U_POPS)
    return true;

  return false;
}
#endif

#ifdef USE_NNTP
bool mx_is_nntp(const char *p)
{
  url_scheme_t scheme;

  if (!p)
    return false;

  scheme = url_check_scheme(p);
  if (scheme == U_NNTP || scheme == U_NNTPS)
    return true;

  return false;
}
#endif

#ifdef USE_NOTMUCH
bool mx_is_notmuch(const char *p)
{
  url_scheme_t scheme;

  if (!p)
    return false;

  scheme = url_check_scheme(p);
  if (scheme == U_NOTMUCH)
    return true;

  return false;
}
#endif

int mx_get_magic(const char *path)
{
  struct stat st;
  int magic = 0;
  char tmp[_POSIX_PATH_MAX];
  FILE *f = NULL;

#ifdef USE_IMAP
  if (mx_is_imap(path))
    return MUTT_IMAP;
#endif /* USE_IMAP */

#ifdef USE_POP
  if (mx_is_pop(path))
    return MUTT_POP;
#endif /* USE_POP */

#ifdef USE_NNTP
  if (mx_is_nntp(path))
    return MUTT_NNTP;
#endif /* USE_NNTP */

#ifdef USE_NOTMUCH
  if (mx_is_notmuch(path))
    return MUTT_NOTMUCH;
#endif

  if (stat(path, &st) == -1)
  {
    mutt_debug(1, "mx_get_magic(): unable to stat %s: %s (errno %d).\n", path,
               strerror(errno), errno);
    return -1;
  }

  if (S_ISDIR(st.st_mode))
  {
    /* check for maildir-style mailbox */
    if (mx_is_maildir(path))
      return MUTT_MAILDIR;

    /* check for mh-style mailbox */
    if (mx_is_mh(path))
      return MUTT_MH;
  }
  else if (st.st_size == 0)
  {
    /* hard to tell what zero-length files are, so assume the default magic */
    if (DefaultMagic == MUTT_MBOX || DefaultMagic == MUTT_MMDF)
      return DefaultMagic;
    else
      return MUTT_MBOX;
  }
  else if ((f = fopen(path, "r")) != NULL)
  {
    struct utimbuf times;
    int ch;

    /* Some mailbox creation tools erroneously append a blank line to
     * a file before appending a mail message.  This allows mutt to
     * detect magic for and thus open those files. */
    while ((ch = fgetc(f)) != EOF)
    {
      if (ch != '\n' && ch != '\r')
      {
        ungetc(ch, f);
        break;
      }
    }

    if (fgets(tmp, sizeof(tmp), f))
    {
      if (mutt_strncmp("From ", tmp, 5) == 0)
        magic = MUTT_MBOX;
      else if (mutt_strcmp(MMDF_SEP, tmp) == 0)
        magic = MUTT_MMDF;
    }
    safe_fclose(&f);

    if (!option(OPTCHECKMBOXSIZE))
    {
      /* need to restore the times here, the file was not really accessed,
       * only the type was accessed.  This is important, because detection
       * of "new mail" depends on those times set correctly.
       */
      times.actime = st.st_atime;
      times.modtime = st.st_mtime;
      utime(path, &times);
    }
  }
  else
  {
    mutt_debug(1, "mx_get_magic(): unable to open file %s for reading.\n", path);
    return -1;
  }

#ifdef USE_COMPRESSED
  /* If there are no other matches, see if there are any
   * compress hooks that match */
  if ((magic == 0) && mutt_comp_can_read(path))
    return MUTT_COMPRESSED;
#endif
  return magic;
}

/*
 * set DefaultMagic to the given value
 */
int mx_set_magic(const char *s)
{
  if (ascii_strcasecmp(s, "mbox") == 0)
    DefaultMagic = MUTT_MBOX;
  else if (ascii_strcasecmp(s, "mmdf") == 0)
    DefaultMagic = MUTT_MMDF;
  else if (ascii_strcasecmp(s, "mh") == 0)
    DefaultMagic = MUTT_MH;
  else if (ascii_strcasecmp(s, "maildir") == 0)
    DefaultMagic = MUTT_MAILDIR;
  else
    return -1;

  return 0;
}

/* mx_access: Wrapper for access, checks permissions on a given mailbox.
 *   We may be interested in using ACL-style flags at some point, currently
 *   we use the normal access() flags. */
int mx_access(const char *path, int flags)
{
#ifdef USE_IMAP
  if (mx_is_imap(path))
    return imap_access(path, flags);
#endif

  return access(path, flags);
}

static int mx_open_mailbox_append(struct Context *ctx, int flags)
{
  struct stat sb;

  ctx->append = true;
  ctx->magic = mx_get_magic(ctx->path);
  if (ctx->magic == 0)
  {
    mutt_error(_("%s is not a mailbox."), ctx->path);
    return -1;
  }

  if (ctx->magic < 0)
  {
    if (stat(ctx->path, &sb) == -1)
    {
      if (errno == ENOENT)
      {
#ifdef USE_COMPRESSED
        if (mutt_comp_can_append(ctx))
          ctx->magic = MUTT_COMPRESSED;
        else
#endif
          ctx->magic = DefaultMagic;
        flags |= MUTT_APPENDNEW;
      }
      else
      {
        mutt_perror(ctx->path);
        return -1;
      }
    }
    else
      return -1;
  }

  ctx->mx_ops = mx_get_ops(ctx->magic);
  if (!ctx->mx_ops || !ctx->mx_ops->open_append)
    return -1;

  return ctx->mx_ops->open_append(ctx, flags);
}

/*
 * open a mailbox and parse it
 *
 * Args:
 *      flags   MUTT_NOSORT     do not sort mailbox
 *              MUTT_APPEND     open mailbox for appending
 *              MUTT_READONLY   open mailbox in read-only mode
 *              MUTT_QUIET              only print error messages
 *              MUTT_PEEK               revert atime where applicable
 *      ctx     if non-null, context struct to use
 */
struct Context *mx_open_mailbox(const char *path, int flags, struct Context *pctx)
{
  struct Context *ctx = pctx;
  int rc;

  if (!path || !path[0])
    return NULL;

  if (!ctx)
    ctx = safe_malloc(sizeof(struct Context));
  memset(ctx, 0, sizeof(struct Context));

  ctx->path = safe_strdup(path);
  if (!ctx->path)
  {
    if (!pctx)
      FREE(&ctx);
    return NULL;
  }
  if (!(ctx->realpath = realpath(ctx->path, NULL)))
    ctx->realpath = safe_strdup(ctx->path);

  ctx->msgnotreadyet = -1;
  ctx->collapsed = false;

  for (rc = 0; rc < RIGHTSMAX; rc++)
    mutt_bit_set(ctx->rights, rc);

  if (flags & MUTT_QUIET)
    ctx->quiet = true;
  if (flags & MUTT_READONLY)
    ctx->readonly = true;
  if (flags & MUTT_PEEK)
    ctx->peekonly = true;

  if (flags & (MUTT_APPEND | MUTT_NEWFOLDER))
  {
    if (mx_open_mailbox_append(ctx, flags) != 0)
    {
      mx_fastclose_mailbox(ctx);
      if (!pctx)
        FREE(&ctx);
      return NULL;
    }
    return ctx;
  }

  ctx->magic = mx_get_magic(path);
  ctx->mx_ops = mx_get_ops(ctx->magic);

  if (ctx->magic <= 0 || !ctx->mx_ops)
  {
    if (ctx->magic == -1)
      mutt_perror(path);
    else if (ctx->magic == 0 || !ctx->mx_ops)
      mutt_error(_("%s is not a mailbox."), path);

    mx_fastclose_mailbox(ctx);
    if (!pctx)
      FREE(&ctx);
    return NULL;
  }

  mutt_make_label_hash(ctx);

  /* if the user has a `push' command in their .muttrc, or in a folder-hook,
   * it will cause the progress messages not to be displayed because
   * mutt_refresh() will think we are in the middle of a macro.  so set a
   * flag to indicate that we should really refresh the screen.
   */
  set_option(OPTFORCEREFRESH);

  if (!ctx->quiet)
    mutt_message(_("Reading %s..."), ctx->path);

  rc = ctx->mx_ops->open(ctx);

  if ((rc == 0) || (rc == -2))
  {
    if ((flags & MUTT_NOSORT) == 0)
    {
      /* avoid unnecessary work since the mailbox is completely unthreaded
         to begin with */
      unset_option(OPTSORTSUBTHREADS);
      unset_option(OPTNEEDRESCORE);
      mutt_sort_headers(ctx, 1);
    }
    if (!ctx->quiet)
      mutt_clear_error();
    if (rc == -2)
      mutt_error(_("Reading from %s interrupted..."), ctx->path);
  }
  else
  {
    mx_fastclose_mailbox(ctx);
    if (!pctx)
      FREE(&ctx);
  }

  unset_option(OPTFORCEREFRESH);
  return ctx;
}

/* free up memory associated with the mailbox context */
void mx_fastclose_mailbox(struct Context *ctx)
{
  struct utimbuf ut;

  if (!ctx)
    return;

  /* fix up the times so buffy won't get confused */
  if (ctx->peekonly && ctx->path && (ctx->mtime > ctx->atime))
  {
    ut.actime = ctx->atime;
    ut.modtime = ctx->mtime;
    utime(ctx->path, &ut);
  }

  /* never announce that a mailbox we've just left has new mail. #3290
   * XXX: really belongs in mx_close_mailbox, but this is a nice hook point */
  if (!ctx->peekonly)
    mutt_buffy_setnotified(ctx->path);

  if (ctx->mx_ops)
    ctx->mx_ops->close(ctx);

  if (ctx->subj_hash)
    hash_destroy(&ctx->subj_hash, NULL);
  if (ctx->id_hash)
    hash_destroy(&ctx->id_hash, NULL);
  hash_destroy(&ctx->label_hash, NULL);
  mutt_clear_threads(ctx);
  for (int i = 0; i < ctx->msgcount; i++)
    mutt_free_header(&ctx->hdrs[i]);
  FREE(&ctx->hdrs);
  FREE(&ctx->v2r);
  FREE(&ctx->path);
  FREE(&ctx->realpath);
  FREE(&ctx->pattern);
  if (ctx->limit_pattern)
    mutt_pattern_free(&ctx->limit_pattern);
  safe_fclose(&ctx->fp);
  memset(ctx, 0, sizeof(struct Context));
}

/* save changes to disk */
static int sync_mailbox(struct Context *ctx, int *index_hint)
{
  if (!ctx->mx_ops || !ctx->mx_ops->sync)
    return -1;

  if (!ctx->quiet)
    mutt_message(_("Writing %s..."), ctx->path);

  return ctx->mx_ops->sync(ctx, index_hint);
}

/* move deleted mails to the trash folder */
static int trash_append(struct Context *ctx)
{
  struct Context ctx_trash;
  int i;
  struct stat st, stc;
  int opt_confappend, rc;

  if (!TrashPath || !ctx->deleted || (ctx->magic == MUTT_MAILDIR && option(OPTMAILDIRTRASH)))
    return 0;

  for (i = 0; i < ctx->msgcount; i++)
    if (ctx->hdrs[i]->deleted && (!ctx->hdrs[i]->purge))
      break;
  if (i == ctx->msgcount)
    return 0; /* nothing to be done */

  /* avoid the "append messages" prompt */
  opt_confappend = option(OPTCONFIRMAPPEND);
  if (opt_confappend)
    unset_option(OPTCONFIRMAPPEND);
  rc = mutt_save_confirm(TrashPath, &st);
  if (opt_confappend)
    set_option(OPTCONFIRMAPPEND);
  if (rc != 0)
  {
    mutt_error(_("message(s) not deleted"));
    return -1;
  }

  if (lstat(ctx->path, &stc) == 0 && stc.st_ino == st.st_ino &&
      stc.st_dev == st.st_dev && stc.st_rdev == st.st_rdev)
    return 0; /* we are in the trash folder: simple sync */

#ifdef USE_IMAP
  if (Context->magic == MUTT_IMAP && mx_is_imap(TrashPath))
  {
    if (!imap_fast_trash(Context, TrashPath))
      return 0;
  }
#endif

  if (mx_open_mailbox(TrashPath, MUTT_APPEND, &ctx_trash) != NULL)
  {
    /* continue from initial scan above */
    for (; i < ctx->msgcount; i++)
      if (ctx->hdrs[i]->deleted && (!ctx->hdrs[i]->purge))
      {
        if (mutt_append_message(&ctx_trash, ctx, ctx->hdrs[i], 0, 0) == -1)
        {
          mx_close_mailbox(&ctx_trash, NULL);
          return -1;
        }
      }

    mx_close_mailbox(&ctx_trash, NULL);
  }
  else
  {
    mutt_error(_("Can't open trash folder"));
    return -1;
  }

  return 0;
}

/* save changes and close mailbox */
int mx_close_mailbox(struct Context *ctx, int *index_hint)
{
  int i, move_messages = 0, purge = 1, read_msgs = 0;
  int check;
  int isSpool = 0;
  struct Context f;
  char mbox[_POSIX_PATH_MAX];
  char buf[SHORT_STRING];

  if (!ctx)
    return 0;

  ctx->closing = true;

  if (ctx->readonly || ctx->dontwrite || ctx->append)
  {
    mx_fastclose_mailbox(ctx);
    return 0;
  }

#ifdef USE_NNTP
  if (ctx->unread && ctx->magic == MUTT_NNTP)
  {
    struct NntpData *nntp_data = ctx->data;

    if (nntp_data && nntp_data->nserv && nntp_data->group)
    {
      int rc = query_quadoption(OPT_CATCHUP, _("Mark all articles read?"));
      if (rc == MUTT_ABORT)
      {
        ctx->closing = false;
        return -1;
      }
      else if (rc == MUTT_YES)
        mutt_newsgroup_catchup(nntp_data->nserv, nntp_data->group);
    }
  }
#endif

  for (i = 0; i < ctx->msgcount; i++)
  {
    if (!ctx->hdrs[i]->deleted && ctx->hdrs[i]->read &&
        !(ctx->hdrs[i]->flagged && option(OPTKEEPFLAGGED)))
      read_msgs++;
  }

  if (read_msgs && quadoption(OPT_MOVE) != MUTT_NO && ctx->magic != MUTT_NNTP)
  {
    char *p = NULL;

    if ((p = mutt_find_hook(MUTT_MBOXHOOK, ctx->path)))
    {
      isSpool = 1;
      strfcpy(mbox, p, sizeof(mbox));
    }
    else
    {
      strfcpy(mbox, NONULL(Inbox), sizeof(mbox));
      isSpool = mutt_is_spool(ctx->path) && !mutt_is_spool(mbox);
    }

    if (isSpool && *mbox)
    {
      mutt_expand_path(mbox, sizeof(mbox));
      snprintf(buf, sizeof(buf), _("Move read messages to %s?"), mbox);
      if ((move_messages = query_quadoption(OPT_MOVE, buf)) == MUTT_ABORT)
      {
        ctx->closing = false;
        return -1;
      }
    }
  }

  /*
   * There is no point in asking whether or not to purge if we are
   * just marking messages as "trash".
   */
  if (ctx->deleted && !(ctx->magic == MUTT_MAILDIR && option(OPTMAILDIRTRASH)))
  {
    snprintf(buf, sizeof(buf),
             ctx->deleted == 1 ? _("Purge %d deleted message?") :
                                 _("Purge %d deleted messages?"),
             ctx->deleted);
    if ((purge = query_quadoption(OPT_DELETE, buf)) == MUTT_ABORT)
    {
      ctx->closing = false;
      return -1;
    }
  }

  if (option(OPTMARKOLD))
  {
    for (i = 0; i < ctx->msgcount; i++)
    {
      if (!ctx->hdrs[i]->deleted && !ctx->hdrs[i]->old && !ctx->hdrs[i]->read)
        mutt_set_flag(ctx, ctx->hdrs[i], MUTT_OLD, 1);
    }
  }

  if (move_messages)
  {
    if (!ctx->quiet)
      mutt_message(_("Moving read messages to %s..."), mbox);

#ifdef USE_IMAP
    /* try to use server-side copy first */
    i = 1;

    if (ctx->magic == MUTT_IMAP && mx_is_imap(mbox))
    {
      /* tag messages for moving, and clear old tags, if any */
      for (i = 0; i < ctx->msgcount; i++)
        if (ctx->hdrs[i]->read && !ctx->hdrs[i]->deleted &&
            !(ctx->hdrs[i]->flagged && option(OPTKEEPFLAGGED)))
          ctx->hdrs[i]->tagged = true;
        else
          ctx->hdrs[i]->tagged = false;

      i = imap_copy_messages(ctx, NULL, mbox, 1);
    }

    if (i == 0) /* success */
      mutt_clear_error();
    else if (i == -1) /* horrible error, bail */
    {
      ctx->closing = false;
      return -1;
    }
    else /* use regular append-copy mode */
#endif
    {
      if (mx_open_mailbox(mbox, MUTT_APPEND, &f) == NULL)
      {
        ctx->closing = false;
        return -1;
      }

      for (i = 0; i < ctx->msgcount; i++)
      {
        if (ctx->hdrs[i]->read && !ctx->hdrs[i]->deleted &&
            !(ctx->hdrs[i]->flagged && option(OPTKEEPFLAGGED)))
        {
          if (mutt_append_message(&f, ctx, ctx->hdrs[i], 0, CH_UPDATE_LEN) == 0)
          {
            mutt_set_flag(ctx, ctx->hdrs[i], MUTT_DELETE, 1);
            mutt_set_flag(ctx, ctx->hdrs[i], MUTT_PURGE, 1);
          }
          else
          {
            mx_close_mailbox(&f, NULL);
            ctx->closing = false;
            return -1;
          }
        }
      }

      mx_close_mailbox(&f, NULL);
    }
  }
  else if (!ctx->changed && ctx->deleted == 0)
  {
    if (!ctx->quiet)
      mutt_message(_("Mailbox is unchanged."));
    if (ctx->magic == MUTT_MBOX || ctx->magic == MUTT_MMDF)
      mbox_reset_atime(ctx, NULL);
    mx_fastclose_mailbox(ctx);
    return 0;
  }

  /* copy mails to the trash before expunging */
  if (purge && ctx->deleted && (mutt_strcmp(ctx->path, TrashPath) != 0))
  {
    if (trash_append(ctx) != 0)
    {
      ctx->closing = false;
      return -1;
    }
  }

#ifdef USE_IMAP
  /* allow IMAP to preserve the deleted flag across sessions */
  if (ctx->magic == MUTT_IMAP)
  {
    if ((check = imap_sync_mailbox(ctx, purge)) != 0)
    {
      ctx->closing = false;
      return check;
    }
  }
  else
#endif
  {
    if (!purge)
    {
      for (i = 0; i < ctx->msgcount; i++)
      {
        ctx->hdrs[i]->deleted = false;
        ctx->hdrs[i]->purge = false;
      }
      ctx->deleted = 0;
    }

    if (ctx->changed || ctx->deleted)
    {
      if ((check = sync_mailbox(ctx, index_hint)) != 0)
      {
        ctx->closing = false;
        return check;
      }
    }
  }

  if (!ctx->quiet)
  {
    if (move_messages)
      mutt_message(_("%d kept, %d moved, %d deleted."),
                   ctx->msgcount - ctx->deleted, read_msgs, ctx->deleted);
    else
      mutt_message(_("%d kept, %d deleted."), ctx->msgcount - ctx->deleted, ctx->deleted);
  }

  if (ctx->msgcount == ctx->deleted && (ctx->magic == MUTT_MMDF || ctx->magic == MUTT_MBOX) &&
      !mutt_is_spool(ctx->path) && !option(OPTSAVEEMPTY))
    mx_unlink_empty(ctx->path);

#ifdef USE_SIDEBAR
  if (purge && ctx->deleted)
  {
    int orig_msgcount = ctx->msgcount;

    for (i = 0; i < ctx->msgcount; i++)
    {
      if (ctx->hdrs[i]->deleted && !ctx->hdrs[i]->read)
        ctx->unread--;
      if (ctx->hdrs[i]->deleted && ctx->hdrs[i]->flagged)
        ctx->flagged--;
    }
    ctx->msgcount -= ctx->deleted;
    mutt_sb_set_buffystats(ctx);
    ctx->msgcount = orig_msgcount;
  }
#endif

  mx_fastclose_mailbox(ctx);

  return 0;
}

/* update a Context structure's internal tables. */
void mx_update_tables(struct Context *ctx, int committing)
{
  int i, j;

  /* update memory to reflect the new state of the mailbox */
  ctx->vcount = 0;
  ctx->vsize = 0;
  ctx->tagged = 0;
  ctx->deleted = 0;
  ctx->new = 0;
  ctx->unread = 0;
  ctx->changed = false;
  ctx->flagged = 0;
#define this_body ctx->hdrs[j]->content
  for (i = 0, j = 0; i < ctx->msgcount; i++)
  {
    if (!ctx->hdrs[i]->quasi_deleted &&
        ((committing && (!ctx->hdrs[i]->deleted ||
                         (ctx->magic == MUTT_MAILDIR && option(OPTMAILDIRTRASH)))) ||
         (!committing && ctx->hdrs[i]->active)))
    {
      if (i != j)
      {
        ctx->hdrs[j] = ctx->hdrs[i];
        ctx->hdrs[i] = NULL;
      }
      ctx->hdrs[j]->msgno = j;
      if (ctx->hdrs[j]->virtual != -1)
      {
        ctx->v2r[ctx->vcount] = j;
        ctx->hdrs[j]->virtual = ctx->vcount++;
        ctx->vsize += this_body->length + this_body->offset - this_body->hdr_offset;
      }

      if (committing)
        ctx->hdrs[j]->changed = false;
      else if (ctx->hdrs[j]->changed)
        ctx->changed = true;

      if (!committing || (ctx->magic == MUTT_MAILDIR && option(OPTMAILDIRTRASH)))
      {
        if (ctx->hdrs[j]->deleted)
          ctx->deleted++;
      }

      if (ctx->hdrs[j]->tagged)
        ctx->tagged++;
      if (ctx->hdrs[j]->flagged)
        ctx->flagged++;
      if (!ctx->hdrs[j]->read)
      {
        ctx->unread++;
        if (!ctx->hdrs[j]->old)
          ctx->new ++;
      }

      j++;
    }
    else
    {
      if (ctx->magic == MUTT_MH || ctx->magic == MUTT_MAILDIR)
        ctx->size -= (ctx->hdrs[i]->content->length + ctx->hdrs[i]->content->offset -
                      ctx->hdrs[i]->content->hdr_offset);
      /* remove message from the hash tables */
      if (ctx->subj_hash && ctx->hdrs[i]->env->real_subj)
        hash_delete(ctx->subj_hash, ctx->hdrs[i]->env->real_subj, ctx->hdrs[i], NULL);
      if (ctx->id_hash && ctx->hdrs[i]->env->message_id)
        hash_delete(ctx->id_hash, ctx->hdrs[i]->env->message_id, ctx->hdrs[i], NULL);
      mutt_label_hash_remove(ctx, ctx->hdrs[i]);
      /* The path mx_check_mailbox() -> imap_check_mailbox() ->
       *          imap_expunge_mailbox() -> mx_update_tables()
       * can occur before a call to mx_sync_mailbox(), resulting in
       * last_tag being stale if it's not reset here.
       */
      if (ctx->last_tag == ctx->hdrs[i])
        ctx->last_tag = NULL;
      mutt_free_header(&ctx->hdrs[i]);
    }
  }
#undef this_body
  ctx->msgcount = j;
}


/* save changes to mailbox
 *
 * return values:
 *      0               success
 *      -1              error
 */
int mx_sync_mailbox(struct Context *ctx, int *index_hint)
{
  int rc, i;
  int purge = 1;
  int msgcount, deleted;

  if (ctx->dontwrite)
  {
    char buf[STRING], tmp[STRING];
    if (km_expand_key(buf, sizeof(buf), km_find_func(MENU_MAIN, OP_TOGGLE_WRITE)))
      snprintf(tmp, sizeof(tmp), _(" Press '%s' to toggle write"), buf);
    else
      strfcpy(tmp, _("Use 'toggle-write' to re-enable write!"), sizeof(tmp));

    mutt_error(_("Mailbox is marked unwritable. %s"), tmp);
    return -1;
  }
  else if (ctx->readonly)
  {
    mutt_error(_("Mailbox is read-only."));
    return -1;
  }

  if (!ctx->changed && !ctx->deleted)
  {
    if (!ctx->quiet)
      mutt_message(_("Mailbox is unchanged."));
    return 0;
  }

  if (ctx->deleted)
  {
    char buf[SHORT_STRING];

    snprintf(buf, sizeof(buf),
             ctx->deleted == 1 ? _("Purge %d deleted message?") :
                                 _("Purge %d deleted messages?"),
             ctx->deleted);
    if ((purge = query_quadoption(OPT_DELETE, buf)) == MUTT_ABORT)
      return -1;
    else if (purge == MUTT_NO)
    {
      if (!ctx->changed)
        return 0; /* nothing to do! */
      /* let IMAP servers hold on to D flags */
      if (ctx->magic != MUTT_IMAP)
      {
        for (i = 0; i < ctx->msgcount; i++)
        {
          ctx->hdrs[i]->deleted = false;
          ctx->hdrs[i]->purge = false;
        }
        ctx->deleted = 0;
      }
    }
    else if (ctx->last_tag && ctx->last_tag->deleted)
      ctx->last_tag = NULL; /* reset last tagged msg now useless */
  }

  /* really only for IMAP - imap_sync_mailbox results in a call to
   * mx_update_tables, so ctx->deleted is 0 when it comes back */
  msgcount = ctx->msgcount;
  deleted = ctx->deleted;

  if (purge && ctx->deleted && (mutt_strcmp(ctx->path, TrashPath) != 0))
  {
    if (trash_append(ctx) != 0)
      return -1;
  }

#ifdef USE_IMAP
  if (ctx->magic == MUTT_IMAP)
    rc = imap_sync_mailbox(ctx, purge);
  else
#endif
    rc = sync_mailbox(ctx, index_hint);
  if (rc == 0)
  {
#ifdef USE_IMAP
    if (ctx->magic == MUTT_IMAP && !purge)
    {
      if (!ctx->quiet)
        mutt_message(_("Mailbox checkpointed."));
    }
    else
#endif
    {
      if (!ctx->quiet)
        mutt_message(_("%d kept, %d deleted."), msgcount - deleted, deleted);
    }

    mutt_sleep(0);

    if (ctx->msgcount == ctx->deleted && (ctx->magic == MUTT_MBOX || ctx->magic == MUTT_MMDF) &&
        !mutt_is_spool(ctx->path) && !option(OPTSAVEEMPTY))
    {
      unlink(ctx->path);
      mx_fastclose_mailbox(ctx);
      return 0;
    }

    /* if we haven't deleted any messages, we don't need to resort */
    /* ... except for certain folder formats which need "unsorted"
     * sort order in order to synchronize folders.
     *
     * MH and maildir are safe.  mbox-style seems to need re-sorting,
     * at least with the new threading code.
     */
    if (purge || (ctx->magic != MUTT_MAILDIR && ctx->magic != MUTT_MH))
    {
      /* IMAP does this automatically after handling EXPUNGE */
      if (ctx->magic != MUTT_IMAP)
      {
        mx_update_tables(ctx, 1);
        mutt_sort_headers(ctx, 1); /* rethread from scratch */
      }
    }
  }

  return rc;
}

/* args:
 *      dest    destination mailbox
 *      hdr     message being copied (required for maildir support, because
 *              the filename depends on the message flags)
 */
struct Message *mx_open_new_message(struct Context *dest, struct Header *hdr, int flags)
{
  struct Address *p = NULL;
  struct Message *msg = NULL;

  if (!dest->mx_ops || !dest->mx_ops->open_new_msg)
  {
    mutt_debug(
        1,
        "mx_open_new_message(): function unimplemented for mailbox type %d.\n",
        dest->magic);
    return NULL;
  }

  msg = safe_calloc(1, sizeof(struct Message));
  msg->write = true;

  if (hdr)
  {
    msg->flags.flagged = hdr->flagged;
    msg->flags.replied = hdr->replied;
    msg->flags.read = hdr->read;
    msg->flags.draft = (flags & MUTT_SET_DRAFT) ? true : false;
    msg->received = hdr->received;
  }

  if (msg->received == 0)
    time(&msg->received);

  if (dest->mx_ops->open_new_msg(msg, dest, hdr) == 0)
  {
    if (dest->magic == MUTT_MMDF)
      fputs(MMDF_SEP, msg->fp);

    if ((dest->magic == MUTT_MBOX || dest->magic == MUTT_MMDF) && flags & MUTT_ADD_FROM)
    {
      if (hdr)
      {
        if (hdr->env->return_path)
          p = hdr->env->return_path;
        else if (hdr->env->sender)
          p = hdr->env->sender;
        else
          p = hdr->env->from;
      }

      fprintf(msg->fp, "From %s %s", p ? p->mailbox : NONULL(Username),
              ctime(&msg->received));
    }
  }
  else
    FREE(&msg);

  return msg;
}

/* check for new mail */
int mx_check_mailbox(struct Context *ctx, int *index_hint)
{
  if (!ctx || !ctx->mx_ops)
  {
    mutt_debug(1, "mx_check_mailbox: null or invalid context.\n");
    return -1;
  }

  return ctx->mx_ops->check(ctx, index_hint);
}

/* return a stream pointer for a message */
struct Message *mx_open_message(struct Context *ctx, int msgno)
{
  struct Message *msg = NULL;

  if (!ctx->mx_ops || !ctx->mx_ops->open_msg)
  {
    mutt_debug(
        1, "mx_open_message(): function not implemented for mailbox type %d.\n", ctx->magic);
    return NULL;
  }

  msg = safe_calloc(1, sizeof(struct Message));
  if (ctx->mx_ops->open_msg(ctx, msg, msgno))
    FREE(&msg);

  return msg;
}

/* commit a message to a folder */
int mx_commit_message(struct Message *msg, struct Context *ctx)
{
  if (!ctx->mx_ops || !ctx->mx_ops->commit_msg)
    return -1;

  if (!(msg->write && ctx->append))
  {
    mutt_debug(1, "mx_commit_message(): msg->write = %d, ctx->append = %d\n",
               msg->write, ctx->append);
    return -1;
  }

  return ctx->mx_ops->commit_msg(ctx, msg);
}

/* close a pointer to a message */
int mx_close_message(struct Context *ctx, struct Message **msg)
{
  if (!ctx || !msg)
    return 0;
  int r = 0;

  if (ctx->mx_ops && ctx->mx_ops->close_msg)
    r = ctx->mx_ops->close_msg(ctx, *msg);

  if ((*msg)->path)
  {
    mutt_debug(1, "mx_close_message (): unlinking %s\n", (*msg)->path);
    unlink((*msg)->path);
    FREE(&(*msg)->path);
  }

  FREE(&(*msg)->commited_path);
  FREE(msg);
  return r;
}

void mx_alloc_memory(struct Context *ctx)
{
  size_t s = MAX(sizeof(struct Header *), sizeof(int));

  if ((ctx->hdrmax + 25) * s < ctx->hdrmax * s)
  {
    mutt_error(_("Integer overflow -- can't allocate memory."));
    sleep(1);
    mutt_exit(1);
  }

  if (ctx->hdrs)
  {
    safe_realloc(&ctx->hdrs, sizeof(struct Header *) * (ctx->hdrmax += 25));
    safe_realloc(&ctx->v2r, sizeof(int) * ctx->hdrmax);
  }
  else
  {
    ctx->hdrs = safe_calloc((ctx->hdrmax += 25), sizeof(struct Header *));
    ctx->v2r = safe_calloc(ctx->hdrmax, sizeof(int));
  }
  for (int i = ctx->msgcount; i < ctx->hdrmax; i++)
  {
    ctx->hdrs[i] = NULL;
    ctx->v2r[i] = -1;
  }
}

/* this routine is called to update the counts in the context structure for
 * the last message header parsed.
 */
void mx_update_context(struct Context *ctx, int new_messages)
{
  struct Header *h = NULL;
  for (int msgno = ctx->msgcount - new_messages; msgno < ctx->msgcount; msgno++)
  {
    h = ctx->hdrs[msgno];

    if (WithCrypto)
    {
      /* NOTE: this _must_ be done before the check for mailcap! */
      h->security = crypt_query(h->content);
    }

    if (!ctx->pattern)
    {
      ctx->v2r[ctx->vcount] = msgno;
      h->virtual = ctx->vcount++;
    }
    else
      h->virtual = -1;
    h->msgno = msgno;

    if (h->env->supersedes)
    {
      struct Header *h2 = NULL;

      if (!ctx->id_hash)
        ctx->id_hash = mutt_make_id_hash(ctx);

      h2 = hash_find(ctx->id_hash, h->env->supersedes);
      if (h2)
      {
        h2->superseded = true;
        if (option(OPTSCORE))
          mutt_score_message(ctx, h2, 1);
      }
    }

    /* add this message to the hash tables */
    if (ctx->id_hash && h->env->message_id)
      hash_insert(ctx->id_hash, h->env->message_id, h);
    if (ctx->subj_hash && h->env->real_subj)
      hash_insert(ctx->subj_hash, h->env->real_subj, h);
    mutt_label_hash_add(ctx, h);

    if (option(OPTSCORE))
      mutt_score_message(ctx, h, 0);

    if (h->changed)
      ctx->changed = true;
    if (h->flagged)
      ctx->flagged++;
    if (h->deleted)
      ctx->deleted++;
    if (!h->read)
    {
      ctx->unread++;
      if (!h->old)
        ctx->new ++;
    }
  }
}

/*
 * Return:
 * 1 if the specified mailbox contains 0 messages.
 * 0 if the mailbox contains messages
 * -1 on error
 */
int mx_check_empty(const char *path)
{
  switch (mx_get_magic(path))
  {
    case MUTT_MBOX:
    case MUTT_MMDF:
      return mbox_check_empty(path);
    case MUTT_MH:
      return mh_check_empty(path);
    case MUTT_MAILDIR:
      return maildir_check_empty(path);
    default:
      errno = EINVAL;
      return -1;
  }
  /* not reached */
}
