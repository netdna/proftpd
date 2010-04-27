/*
 * ProFTPD - mod_sftp Display files
 * Copyright (c) 2010 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 */

/* Display of files
 * $Id: display.c,v 1.2 2010-04-12 22:42:52 castaglia Exp $
 */

#include "mod_sftp.h"
#include "display.h"
#include "packet.h"
#include "msg.h"

static void format_size_str(char *buf, size_t buflen, off_t size) {
  char units[] = {'K', 'M', 'G', 'T', 'P'};
  register unsigned int i = 0;

  /* Determine the appropriate units label to use. */
  while (size > 1024) {
    size /= 1024;
    i++;
  }

  /* Now, prepare the buffer. */
  snprintf(buf, buflen, "%.3" PR_LU "%cB", (pr_off_t) size, units[i]);
}

static const char *get_display_msg(pool *p, pr_fh_t *fh) {
  struct stat st;
  char buf[PR_TUNABLE_BUFFER_SIZE] = {'\0'}, *msg = "";
  int len;
  unsigned int *current_clients = NULL;
  unsigned int *max_clients = NULL;
  off_t fs_size = 0;
  void *v;
  const char *serverfqdn = main_server->ServerFQDN;
  char *outs, mg_size[12] = {'\0'}, mg_size_units[12] = {'\0'},
    mg_max[12] = "unlimited";
  char mg_class_limit[12] = {'\0'}, mg_cur[12] = {'\0'},
    mg_cur_class[12] = {'\0'};
  const char *mg_time;
  char *rfc1413_ident = NULL, *user = NULL;

  /* Stat the opened file to determine the optimal buffer size for IO. */
  memset(&st, 0, sizeof(st));
  pr_fsio_fstat(fh, &st);
  fh->fh_iosz = st.st_blksize;

#if defined(HAVE_STATFS) || defined(HAVE_SYS_STATVFS_H) || \
   defined(HAVE_SYS_VFS_H)
  fs_size = pr_fs_getsize(fh->fh_path);
  snprintf(mg_size, sizeof(mg_size), "%" PR_LU, (pr_off_t) fs_size);
  format_size_str(mg_size_units, sizeof(mg_size_units), fs_size);
#else
  snprintf(mg_size, sizeof(mg_size), "%" PR_LU, (pr_off_t) fs_size);
  format_size_str(mg_size_units, sizeof(mg_size_units), fs_size);
#endif

  mg_time = pr_strtime(time(NULL));

  max_clients = get_param_ptr(main_server->conf, "MaxClients", FALSE);

  v = pr_table_get(session.notes, "client-count", NULL);
  if (v) {
    current_clients = v;
  }

  snprintf(mg_cur, sizeof(mg_cur), "%u", current_clients ? *current_clients: 1);

  if (session.class &&
      session.class->cls_name) {
    unsigned int *class_clients = NULL;
    config_rec *maxc = NULL;
    unsigned int maxclients = 0;

    v = pr_table_get(session.notes, "class-client-count", NULL);
    if (v) {
      class_clients = v;
    }

    snprintf(mg_cur_class, sizeof(mg_cur_class), "%u",
      class_clients ? *class_clients : 0);

    /* For the %z variable, first we scan through the MaxClientsPerClass,
     * and use the first applicable one.  If none are found, look for
     * any MaxClients set.
     */

    maxc = find_config(main_server->conf, CONF_PARAM, "MaxClientsPerClass",
      FALSE);

    while (maxc) {
      pr_signals_handle();

      if (strcmp(maxc->argv[0], session.class->cls_name) != 0) {
        maxc = find_config_next(maxc, maxc->next, CONF_PARAM,
          "MaxClientsPerClass", FALSE);
        continue;
      }

      maxclients = *((unsigned int *) maxc->argv[1]);
      break;
    }

    if (maxclients == 0) {
      maxc = find_config(main_server->conf, CONF_PARAM, "MaxClients", FALSE);

      if (maxc)
        maxclients = *((unsigned int *) maxc->argv[0]);
    }

    snprintf(mg_class_limit, sizeof(mg_class_limit), "%u", maxclients);

  } else {
    snprintf(mg_class_limit, sizeof(mg_class_limit), "%u",
      max_clients ? *max_clients : 0);
    snprintf(mg_cur_class, sizeof(mg_cur_class), "%u", 0);
  }

  snprintf(mg_max, sizeof(mg_max), "%u", max_clients ? *max_clients : 0);

  user = pr_table_get(session.notes, "mod_auth.orig-user", NULL);
  if (user == NULL)
    user = "";

  rfc1413_ident = pr_table_get(session.notes, "mod_ident.rfc1413-ident", NULL);
  if (rfc1413_ident == NULL) {
    rfc1413_ident = "UNKNOWN";
  }

  while (pr_fsio_gets(buf, sizeof(buf), fh) != NULL) {
    char *tmp;

    pr_signals_handle();

    buf[sizeof(buf)-1] = '\0';
    len = strlen(buf);

    while (len &&
           (buf[len-1] == '\r' || buf[len-1] == '\n')) {
      pr_signals_handle();
      buf[len-1] = '\0';
      len--;
    }

    /* Check for any Variable-type strings. */
    tmp = strstr(buf, "%{");
    while (tmp) {
      char *key, *tmp2;
      const char *val;

      pr_signals_handle();

      tmp2 = strchr(tmp, '}');
      if (tmp2 == NULL) {
        /* No closing '}' found in this string, so no need to look for any
         * aother '%{' opening sequence.  Just move on.
         */
        tmp = NULL;
        break;
      }

      key = pstrndup(p, tmp, tmp2 - tmp + 1);

      /* There are a couple of special-case keys to watch for:
       *
       *   env:$var
       *   time:$fmt
       *
       * The Var API does not easily support returning values for keys
       * where part of the value depends on part of the key.  That's why
       * these keys are handled here, instead of in pr_var_get().
       */

      if (strncmp(key, "%{time:", 7) == 0) {
        char time_str[128], *fmt;
        time_t now;
        struct tm *time_info;

        fmt = pstrndup(p, key + 7, strlen(key) - 8);

        now = time(NULL);
        time_info = pr_localtime(NULL, &now);

        memset(time_str, 0, sizeof(time_str));
        strftime(time_str, sizeof(time_str), fmt, time_info);

        val = pstrdup(p, time_str);

      } else if (strncmp(key, "%{env:", 6) == 0) {
        char *env_var;

        env_var = pstrndup(p, key + 6, strlen(key) - 7);
        val = pr_env_get(p, env_var);
        if (val == NULL) {
          pr_trace_msg("var", 4,
            "no value set for environment variable '%s', using \"(none)\"",
            env_var);
          val = "(none)";
        }

      } else {
        val = pr_var_get(key);
        if (val == NULL) {
          pr_trace_msg("var", 4,
            "no value set for name '%s', using \"(none)\"", key);
          val = "(none)";
        }
      }

      outs = sreplace(p, buf, key, val, NULL);
      sstrncpy(buf, outs, sizeof(buf));

      tmp = strstr(outs, "%{");
    }

    outs = sreplace(p, buf,
      "%C", (session.cwd[0] ? session.cwd : "(none)"),
      "%E", main_server->ServerAdmin,
      "%F", mg_size,
      "%f", mg_size_units,
      "%i", "0",
      "%K", "0",
      "%k", "0B",
      "%L", serverfqdn,
      "%M", mg_max,
      "%N", mg_cur,
      "%o", "0",
      "%R", (session.c && session.c->remote_name ?
        session.c->remote_name : "(unknown)"),
      "%T", mg_time,
      "%t", "0",
      "%U", user,
      "%u", rfc1413_ident,
      "%V", main_server->ServerName,
      "%x", session.class ? session.class->cls_name : "(unknown)",
      "%y", mg_cur_class,
      "%z", mg_class_limit,
      NULL);

    sstrncpy(buf, outs, sizeof(buf));

    msg = pstrcat(p, msg, *msg ? "\r\n" : "", buf, NULL);
  }

  return msg;
}

int sftp_display_fh(pool *p, pr_fh_t *fh, const char msg_type) {
  struct ssh2_packet *pkt;
  int res;
  char *buf, *ptr;
  const char *msg;
  uint32_t buflen, bufsz;

  if (fh == NULL) {
    errno = EINVAL;
    return -1;
  }

  msg = get_display_msg(p, fh);
  if (msg == NULL) {
    return -1;
  }

  pkt = sftp_ssh2_packet_create(p);

  buflen = bufsz = strlen(msg) + 32;
  ptr = buf = palloc(pkt->pool, bufsz);

  sftp_msg_write_byte(&buf, &buflen, msg_type);
  sftp_msg_write_string(&buf, &buflen, msg);

  /* XXX locale of banner */
  sftp_msg_write_string(&buf, &buflen, "");

  pkt->payload = ptr;
  pkt->payload_len = (bufsz - buflen);

  res = sftp_ssh2_packet_write(sftp_conn->wfd, pkt);
  destroy_pool(pkt->pool);

  return res; 
}