/*
 * Copyright (c) 2021 Todd C. Miller <Todd.Miller@sudo.ws>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <config.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(HAVE_STDINT_H)
# include <stdint.h>
#elif defined(HAVE_INTTYPES_H)
# include <inttypes.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "sudoers.h"
#include "interfaces.h"

extern char **environ;
extern sudo_dso_public struct policy_plugin sudoers_policy;

const char *path_plugin_dir = _PATH_SUDO_PLUGIN_DIR;
char *audit_msg;

static FILE *
open_data(const uint8_t *data, size_t size)
{
#ifdef HAVE_FMEMOPEN
    /* Operate in-memory. */
    return fmemopen((void *)data, size, "r");
#else
    char tempfile[] = "/tmp/sudoers.XXXXXX";
    size_t nwritten;
    int fd;

    /* Use (unlinked) temporary file. */
    fd = mkstemp(tempfile);
    if (fd == -1)
	return NULL;
    unlink(tempfile);
    nwritten = write(fd, data, size);
    if (nwritten != size) {
	close(fd);
	return NULL;
    }
    lseek(fd, 0, SEEK_SET);
    return fdopen(fd, "r");
#endif
}

/*
 * Array that gets resized as needed.
 */
struct dynamic_array {
    char **entries;
    size_t len;
    size_t size;
};

static void
free_strvec(char **vec)
{
    int i;

    for (i = 0; vec[i] != NULL; i++)
	free(vec[i]);
}

static void
free_dynamic_array(struct dynamic_array *arr)
{
    if (arr->entries != NULL) {
	free_strvec(arr->entries);
	free(arr->entries);
    }
    memset(arr, 0, sizeof(*arr));
}

static bool
push(struct dynamic_array *arr, const char *entry)
{
    char *copy = NULL;

    if (entry != NULL) {
	if ((copy = strdup(entry)) == NULL)
	    return false;
    }

    if (arr->len + (entry != NULL) >= arr->size) {
	char **tmp = reallocarray(arr->entries, arr->size + 128, sizeof(char *));
	if (tmp == NULL) {
	    free(copy);
	    return false;
	}
	arr->entries = tmp;
	arr->size += 128;
    }
    if (copy != NULL)
	arr->entries[arr->len++] = copy;
    arr->entries[arr->len] = NULL;

    return true;
}

static int
fuzz_conversation(int num_msgs, const struct sudo_conv_message msgs[],
    struct sudo_conv_reply replies[], struct sudo_conv_callback *callback)
{
    int n;

    for (n = 0; n < num_msgs; n++) {
	const struct sudo_conv_message *msg = &msgs[n];
	FILE *fp = stdout;

	switch (msg->msg_type & 0xff) {
	    case SUDO_CONV_PROMPT_ECHO_ON:
	    case SUDO_CONV_PROMPT_MASK:
	    case SUDO_CONV_PROMPT_ECHO_OFF:
		/* input not supported */
		return -1;
	    case SUDO_CONV_ERROR_MSG:
		fp = stderr;
		FALLTHROUGH;
	    case SUDO_CONV_INFO_MSG:
		if (msg->msg != NULL) {
		    size_t len = strlen(msg->msg);

		    if (len == 0)
			break;

		    if (fwrite(msg->msg, 1, len, fp) == 0 || fputc('\n', fp) == EOF)
			return -1;
		}
		break;
	    default:
		return -1;
	}
    }
    return 0;
}

static int
fuzz_printf(int msg_type, const char *fmt, ...)
{
    FILE *fp = stdout;
    va_list ap;
    int len;

    switch (msg_type & 0xff) {
    case SUDO_CONV_ERROR_MSG:
        fp = stderr;
        FALLTHROUGH;
    case SUDO_CONV_INFO_MSG:
        va_start(ap, fmt);
        len = vfprintf(fp, fmt, ap);
        va_end(ap);
        break;
    default:
        len = -1;
        errno = EINVAL;
        break;
    }

    return len;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct dynamic_array plugin_args = { NULL };
    struct dynamic_array settings = { NULL };
    struct dynamic_array user_info = { NULL };
    struct dynamic_array argv = { NULL };
    struct dynamic_array env_add = { NULL };
    char **command_info = NULL, **argv_out = NULL, **user_env_out = NULL;
    const char *errstr = NULL;
    char *line = NULL;
    size_t linesize = 0;
    ssize_t linelen;
    int res;
    FILE *fp;

    fp = open_data(data, size);
    if (fp == NULL)
        return 0;

    /* user_info and settings must be non-NULL (even if empty). */
    push(&user_info, NULL);
    push(&settings, NULL);

    /* Iterate over each line of data. */
    while ((linelen = getdelim(&line, &linesize, '\n', fp)) != -1) {
	if (line[linelen - 1] == '\n')
	    line[linelen - 1] = '\0';

	/* Skip comments and blank lines. */
	if (line[0] == '#' || line[0] == '\0')
	    continue;

	/* plugin args */
	if (strncmp(line, "error_recovery=", sizeof("error_recovery=") - 1) == 0) {
	    push(&plugin_args, line);
	    continue;
	}
	if (strncmp(line, "sudoers_file=", sizeof("sudoers_file=") - 1) == 0) {
	    push(&plugin_args, line);
	    continue;
	}
	if (strncmp(line, "sudoers_mode=", sizeof("sudoers_mode=") - 1) == 0) {
	    push(&plugin_args, line);
	    continue;
	}
	if (strncmp(line, "sudoers_gid=", sizeof("sudoers_gid=") - 1) == 0) {
	    push(&plugin_args, line);
	    continue;
	}
	if (strncmp(line, "sudoers_uid=", sizeof("sudoers_uid=") - 1) == 0) {
	    push(&plugin_args, line);
	    continue;
	}
	if (strncmp(line, "ldap_conf=", sizeof("ldap_conf=") - 1) == 0) {
	    push(&plugin_args, line);
	    continue;
	}
	if (strncmp(line, "ldap_secret=", sizeof("ldap_secret=") - 1) == 0) {
	    push(&plugin_args, line);
	    continue;
	}

	/* user info */
	if (strncmp(line, "user=", sizeof("user=") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}
	if (strncmp(line, "uid=", sizeof("uid=") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}
	if (strncmp(line, "gid=", sizeof("gid=") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}
	if (strncmp(line, "groups=", sizeof("groups=") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}
	if (strncmp(line, "cwd=", sizeof("cwd=") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}
	if (strncmp(line, "tty=", sizeof("tty=") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}
	if (strncmp(line, "host=", sizeof("host=") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}
	if (strncmp(line, "lines=", sizeof("lines=") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}
	if (strncmp(line, "cols=", sizeof("cols=") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}
	if (strncmp(line, "sid=", sizeof("sid=") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}
	if (strncmp(line, "umask=", sizeof("umask=") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}
	if (strncmp(line, "rlimit_", sizeof("rlimit_") - 1) == 0) {
	    push(&user_info, line);
	    continue;
	}

	/* First argv entry is the command, the rest are args. */
	if (strncmp(line, "argv=", sizeof("argv=") - 1) == 0) {
	    push(&argv, line);
	    continue;
	}

	/* First argv entry is the command, the rest are args. */
	if (strncmp(line, "env=", sizeof("env=") - 1) == 0) {
	    push(&env_add, line);
	    continue;
	}

	/* Treat anything else as a setting. */
	push(&settings, line);
    }
    fclose(fp);
    free(line);
    line = NULL;

    /* Call policy open function */
    res = sudoers_policy.open(SUDO_API_VERSION, fuzz_conversation, fuzz_printf,
	settings.entries, user_info.entries, environ, plugin_args.entries,
	&errstr);

    switch (res) {
    case 0:
	/* failure */
	break;
    case 1:
	/* success */
	if (argv.len == 0) {
	    /* Must have a command to check. */
	    push(&argv, "/usr/bin/id");
	}

	/* Call policy check function */
	sudoers_policy.check_policy(argv.len, argv.entries, env_add.entries,
	    &command_info, &argv_out, &user_env_out, &errstr);

	/* XXX - may need to free argv_out and user_env_out */
	break;
    default:
	/* fatal or usage error */
	goto done;
    }

    if (sudoers_policy.close != NULL)
	sudoers_policy.close(0, 0);

done:
    /* Cleanup. */

    /* Free dynamic contents of sudo_user. */
    if (sudo_user.pw != NULL)
	sudo_pw_delref(sudo_user.pw);
    if (sudo_user.gid_list != NULL)
	sudo_gidlist_delref(sudo_user.gid_list);
    if (sudo_user._runas_pw != NULL)
	sudo_pw_delref(sudo_user._runas_pw);
    if (sudo_user._runas_gr != NULL)
	sudo_gr_delref(sudo_user._runas_gr);
    free(sudo_user.cwd);
    free(sudo_user.name);
    if (sudo_user.ttypath != NULL)
	free(sudo_user.ttypath);
    else
	free(sudo_user.tty);
    if (sudo_user.shost != sudo_user.host)
	    free(sudo_user.shost);
    free(sudo_user.host);
    if (sudo_user.srunhost != sudo_user.runhost)
	    free(sudo_user.srunhost);
    free(sudo_user.runhost);
    if (argv.len == 0 || (argv.len != 0 && sudo_user.cmnd != argv.entries[0]))
	free(sudo_user.cmnd);
    free(sudo_user.cmnd_args);
    free(sudo_user.cmnd_safe);
    free(sudo_user.cmnd_stat);
    /* XXX - sudo_user.env_vars */
#ifdef HAVE_SELINUX
    free(sudo_user.role);
    free(sudo_user.type);
#endif
#ifdef HAVE_PRIV_SET
    free(sudo_user.privs);
    free(sudo_user.limitprivs);
#endif
    free(sudo_user.iolog_file);
    free(sudo_user.iolog_path);
    free(sudo_user.gids);
    memset(&sudo_user, 0, sizeof(sudo_user));

    env_init(NULL);

    free_dynamic_array(&plugin_args);
    free_dynamic_array(&settings);
    free_dynamic_array(&user_info);
    free_dynamic_array(&argv);
    free_dynamic_array(&env_add);

    return 0;
}

/* STUB */
bool
user_is_exempt(void)
{
    return false;
}

/* STUB */
int
group_plugin_query(const char *user, const char *group, const struct passwd *pw)
{
    return false;
}

/* STUB */
struct interface_list *
get_interfaces(void)
{
    static struct interface_list empty = SLIST_HEAD_INITIALIZER(interfaces);
    return &empty;
}

/* STUB */
void
init_eventlog_config(void)
{
    return;
}

/* STUB */
bool
set_interfaces(const char *ai)
{
    return true;
}

/* STUB */
void
dump_interfaces(const char *ai)
{
    return;
}

/* STUB */
void
dump_auth_methods(void)
{
    return;
}

/* STUB */
int
sudo_auth_begin_session(struct passwd *pw, char **user_env[])
{
    return 1;
}

/* STUB */
int
sudo_auth_end_session(struct passwd *pw)
{
    return 1;
}

/* STUB */
bool
sudo_auth_needs_end_session(void)
{
    return false;
}

/* STUB */
bool
set_perms(int perm)
{
    return true;
}

/* STUB */
bool
restore_perms(void)
{
    return true;
}

/* STUB */
bool
rewind_perms(void)
{
    return true;
}

/* STUB */
int
timestamp_remove(bool unlink_it)
{
    return true;
}

/* STUB */
int
create_admin_success_flag(void)
{
    return true;
}

/* STUB */
static int
sudo_file_open(struct sudo_nss *nss)
{
    return 0;
}

/* STUB */
static int
sudo_file_close(struct sudo_nss *nss)
{
    return 0;
}

/* STUB */
static struct sudoers_parse_tree *
sudo_file_parse(struct sudo_nss *nss)
{
    static struct sudoers_parse_tree parse_tree;

    return &parse_tree;
}

/* STUB */
static int
sudo_file_query(struct sudo_nss *nss, struct passwd *pw)
{
    return 0;
}

/* STUB */
static int
sudo_file_getdefs(struct sudo_nss *nss)
{
    return 0;
}

static struct sudo_nss sudo_nss_file = {
    { NULL, NULL },
    sudo_file_open,
    sudo_file_close,
    sudo_file_parse,
    sudo_file_query,
    sudo_file_getdefs
};

struct sudo_nss_list *
sudo_read_nss(void)
{
    static struct sudo_nss_list snl = TAILQ_HEAD_INITIALIZER(snl);

    if (TAILQ_EMPTY(&snl))
	TAILQ_INSERT_TAIL(&snl, &sudo_nss_file, entries);

    return &snl;
}

/* STUB */
int
check_user(int validated, int mode)
{
    return true;
}

/* STUB */
bool
check_user_shell(const struct passwd *pw)
{
    return true;
}

/* STUB */
void
group_plugin_unload(void)
{
    return;
}

bool
log_warning(int flags, const char *fmt, ...)
{
    va_list ap;

    /* Just display on stderr. */
    va_start(ap, fmt);
    sudo_vwarn_nodebug(fmt, ap);
    va_end(ap);

    return true;
}

bool
log_warningx(int flags, const char *fmt, ...)
{
    va_list ap;

    /* Just display on stderr. */
    va_start(ap, fmt);
    sudo_vwarnx_nodebug(fmt, ap);
    va_end(ap);

    return true;
}

bool
gai_log_warning(int flags, int errnum, const char *fmt, ...)
{
    va_list ap;

    /* Note: ignores errnum */
    va_start(ap, fmt);
    sudo_vwarnx_nodebug(fmt, ap);
    va_end(ap);

    return true;
}

/* STUB */
bool
log_denial(int status, bool inform_user)
{
    return true;
}

/* STUB */
bool
log_failure(int status, int flags)
{
    return true;
}

/* STUB */
int
audit_failure(char *const argv[], char const *const fmt, ...)
{
    return 0;
}

/* STUB */
int
sudoers_lookup(struct sudo_nss_list *snl, struct passwd *pw, int *cmnd_status,
    int pwflag)
{
    return VALIDATE_SUCCESS;
}

/* STUB */
int
display_cmnd(struct sudo_nss_list *snl, struct passwd *pw)
{
    return true;
}

/* STUB */
int
display_privs(struct sudo_nss_list *snl, struct passwd *pw, bool verbose)
{
    return true;
}

/* STUB */
int
find_path(const char *infile, char **outfile, struct stat *sbp,
    const char *path, const char *runchroot, int ignore_dot,
    char * const *allowlist)
{
    if (infile[0] == '/') {
	*outfile = strdup(infile);
    } else {
	if (asprintf(outfile, "/usr/bin/%s", infile) == -1)
	    *outfile = NULL;
    }
    return *outfile ? FOUND : NOT_FOUND;
}

/* STUB */
bool
expand_iolog_path(const char *inpath, char *path, size_t pathlen,
    const struct iolog_path_escape *escapes, void *closure)
{
    return strlcpy(path, inpath, pathlen) < pathlen;
}

/* STUB */
bool
iolog_nextid(char *iolog_dir, char sessid[7])
{
    strlcpy(sessid, "000001", 7);
    return true;
}

/* STUB */
void
eventlog_set_type(int type)
{
    return;
}

/* STUB */
void
eventlog_set_format(enum eventlog_format format)
{
    return;
}

/* STUB */
void
eventlog_set_syslog_acceptpri(int pri)
{
    return;
}

/* STUB */
void
eventlog_set_syslog_rejectpri(int pri)
{
    return;
}

/* STUB */
void
eventlog_set_syslog_alertpri(int pri)
{
    return;
}

/* STUB */
void
eventlog_set_syslog_maxlen(int len)
{
    return;
}

/* STUB */
void
eventlog_set_file_maxlen(int len)
{
    return;
}

/* STUB */
void
eventlog_set_mailuid(uid_t uid)
{
    return;
}

/* STUB */
void
eventlog_set_omit_hostname(bool omit_hostname)
{
    return;
}

/* STUB */
void
eventlog_set_logpath(const char *path)
{
    return;
}

/* STUB */
void
eventlog_set_time_fmt(const char *fmt)
{
    return;
}

/* STUB */
void
eventlog_set_mailerpath(const char *path)
{
    return;
}

/* STUB */
void
eventlog_set_mailerflags(const char *mflags)
{
    return;
}

/* STUB */
void
eventlog_set_mailfrom(const char *from_addr)
{
    return;
}

/* STUB */
void
eventlog_set_mailto(const char *to_addr)
{
    return;
}

/* STUB */
void
eventlog_set_mailsub(const char *subject)
{
    return;
}

/* STUB */
void
eventlog_set_open_log(FILE *(*fn)(int type, const char *))
{
    return;
}

/* STUB */
void
eventlog_set_close_log(void (*fn)(int type, FILE *))
{
    return;
}

/* STUB */
bool
cb_maxseq(const union sudo_defs_val *sd_un)
{
    return true;
}

/* STUB */
bool
cb_iolog_user(const union sudo_defs_val *sd_un)
{
    return true;
}

/* STUB */
bool
cb_iolog_group(const union sudo_defs_val *sd_un)
{
    return true;
}

/* STUB */
bool
cb_iolog_mode(const union sudo_defs_val *sd_un)
{
    return true;
}

/* STUB */
bool
cb_group_plugin(const union sudo_defs_val *sd_un)
{
    return true;
}
