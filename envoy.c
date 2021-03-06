/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) Simon Gomizelj, 2012
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <getopt.h>
#include <err.h>
#include <errno.h>
#include <pwd.h>
#include <unistd.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "lib/envoy.h"
#include "lib/gpg-protocol.h"

static struct termios old_termios;

enum action {
    ACTION_FISH_PRINT,
    ACTION_SH_PRINT,
    ACTION_NONE,
    ACTION_FORCE_ADD,
    ACTION_CLEAR,
    ACTION_KILL,
    ACTION_LIST,
    ACTION_UNLOCK,
    ACTION_INVALID
};

static void term_cleanup(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
}

static ssize_t read_password(char **password)
{
    struct termios new_termios;
    size_t len = 0;
    ssize_t nbytes_r;

    fputs("Password: ", stdout);
    fflush(stdout);

    if (tcgetattr(fileno(stdin), &old_termios) < 0)
        err(EXIT_FAILURE, "failed to get terminal attributes");

    atexit(term_cleanup);

    new_termios = old_termios;
    new_termios.c_lflag &= ~ECHO;

    if (tcsetattr(fileno(stdin), TCSAFLUSH, &new_termios) < 0)
        err(EXIT_FAILURE, "failed to set terminal attributes");

    nbytes_r = getline(password, &len, stdin);
    if (nbytes_r < 0)
        errx(EXIT_FAILURE, "failed to read password");

    (*password)[--nbytes_r] = 0;
    tcsetattr(fileno(stdin), TCSAFLUSH, &old_termios);

    putchar('\n');
    return nbytes_r;
}

static int get_agent(struct agent_data_t *data, enum agent id, bool start)
{
    int ret = envoy_agent(data, id, start);
    if (ret < 0)
        err(EXIT_FAILURE, "failed to fetch agent");

    switch (data->status) {
    case ENVOY_STOPPED:
    case ENVOY_STARTED:
    case ENVOY_RUNNING:
        break;
    case ENVOY_FAILED:
        errx(EXIT_FAILURE, "agent failed to start, check envoyd's log");
    case ENVOY_BADUSER:
        errx(EXIT_FAILURE, "connection rejected, user is unauthorized to use this agent");
    }

    return ret;
}

static char *get_key_path(const char *home, const char *fragment)
{
    char *out;

    /* path exists, add it */
    if (access(fragment, F_OK) == 0)
        return strdup(fragment);

    /* assume it's a key in $HOME/.ssh */
    safe_asprintf(&out, "%s/.ssh/%s", home, fragment);
    return out;
}

static void __attribute__((__noreturn__)) add_keys(char **keys, int count)
{
    /* command + end-of-opts + NULL + keys */
    char *args[count + 3];
    struct passwd *pwd;
    int i;

    pwd = getpwuid(getuid());
    if (pwd == NULL || pwd->pw_dir == NULL)
        err(EXIT_FAILURE, "failed to lookup passwd entry");

    args[0] = "/usr/bin/ssh-add";
    args[1] = "--";

    for (i = 0; i < count; i++)
        args[2 + i] = get_key_path(pwd->pw_dir, keys[i]);

    args[2 + count] = NULL;

    execv(args[0], args);
    err(EXIT_FAILURE, "failed to launch ssh-add");
}

static void print_sh_env(struct agent_data_t *data)
{
    if (data->type == AGENT_GPG_AGENT)
        printf("export GPG_AGENT_INFO='%s'\n", data->gpg);

    printf("export SSH_AUTH_SOCK='%s'\n", data->sock);
    printf("export SSH_AGENT_PID='%d'\n", data->pid);
}

static void print_fish_env(struct agent_data_t *data)
{
    if (data->type == AGENT_GPG_AGENT)
        printf("set -x GPG_AGENT_INFO '%s';", data->gpg);

    printf("set -x SSH_AUTH_SOCK '%s';", data->sock);
    printf("set -x SSH_AGENT_PID '%d';", data->pid);
}

static void source_env(struct agent_data_t *data)
{
    if (data->type == AGENT_GPG_AGENT) {
        struct gpg_t *agent = gpg_agent_connection(data->gpg);
        gpg_update_tty(agent);
        gpg_close(agent);
    }

    setenv("SSH_AUTH_SOCK", data->sock, true);
}

static int unlock(const struct agent_data_t *data, char *password)
{
    struct gpg_t *agent = gpg_agent_connection(data->gpg);
    if (!agent)
        err(EXIT_FAILURE, "failed to open connection to gpg-agent");

    if (!password)
        read_password(&password);

    const struct fingerprint_t *fgpt = gpg_keyinfo(agent);
    for (; fgpt; fgpt = fgpt->next) {
        if (gpg_preset_passphrase(agent, fgpt->fingerprint, -1, password) < 0) {
            warnx("failed to unlock key '%s'", fgpt->fingerprint);
            return 1;
        }
    }

    gpg_close(agent);
    return 0;
}

static void __attribute__((__noreturn__)) usage(FILE *out)
{
    fprintf(out, "usage: %s [options] [key ...]\n", program_invocation_short_name);
    fputs("Options:\n"
        " -h, --help            display this help\n"
        " -v, --version         display version\n"
        " -a, --add             add private key identities\n"
        " -k, --clear           force identities to expire (gpg-agent only)\n"
        " -K, --kill            kill the running agent\n"
        " -l, --list            list fingerprints of all loaded identities\n"
        " -u, --unlock=[PASS]   unlock the agent's keyring (gpg-agent only)\n"
        " -p, --print           print out sh environmental arguments\n"
        " -f, --fish            print out fish environmental arguments\n"
        " -t, --agent=AGENT     set the prefered to start\n", out);

    exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
    bool source = true;
    struct agent_data_t data;
    char *password = NULL;
    enum action verb = ACTION_NONE;
    enum agent type = AGENT_DEFAULT;

    static const struct option opts[] = {
        { "help",    no_argument, 0, 'h' },
        { "version", no_argument, 0, 'v' },
        { "add",     no_argument, 0, 'a' },
        { "clear",   no_argument, 0, 'k' },
        { "kill",    no_argument, 0, 'K' },
        { "list",    no_argument, 0, 'l' },
        { "unlock",  optional_argument, 0, 'u' },
        { "print",   no_argument, 0, 'p' },
        { "fish",    no_argument, 0, 'f' },
        { "agent",   required_argument, 0, 't' },
        { 0, 0, 0, 0 }
    };

    while (true) {
        int opt = getopt_long(argc, argv, "hvakKlu::pft:", opts, NULL);
        if (opt == -1)
            break;

        switch (opt) {
        case 'h':
            usage(stdout);
            break;
        case 'v':
            printf("%s %s\n", program_invocation_short_name, ENVOY_VERSION);
            return 0;
        case 'a':
            verb = ACTION_FORCE_ADD;
            break;
        case 'k':
            verb = ACTION_CLEAR;
            source = false;
            break;
        case 'K':
            verb = ACTION_KILL;
            source = false;
            break;
        case 'l':
            verb = ACTION_LIST;
            break;
        case 'u':
            verb = ACTION_UNLOCK;
            password = optarg;
            break;
        case 'p':
            verb = ACTION_SH_PRINT;
            break;
        case 'f':
            verb = ACTION_FISH_PRINT;
            break;
        case 't':
            type = lookup_agent(optarg);
            if (type == LAST_AGENT)
                errx(EXIT_FAILURE, "unknown agent: %s", optarg);
            break;
        default:
            usage(stderr);
        }
    }

    if (get_agent(&data, type, source) < 0)
        errx(EXIT_FAILURE, "recieved no data, did the agent fail to start?");

    if (data.status == ENVOY_STOPPED)
        return 0;

    if (source)
        source_env(&data);

    if (verb == ACTION_SH_PRINT)
        print_sh_env(&data);
    else if (verb == ACTION_FISH_PRINT)
        print_fish_env(&data);

    switch (verb) {
    case ACTION_NONE:
        if (data.status == ENVOY_RUNNING || data.type == AGENT_GPG_AGENT)
            break;
        /* fall through */
    case ACTION_FORCE_ADD:
        add_keys(&argv[optind], argc - optind);
        break;
    case ACTION_CLEAR:
        if (data.type == AGENT_GPG_AGENT)
            kill(data.pid, SIGHUP);
        else
            errx(EXIT_FAILURE, "only gpg-agent supports this operation");
        break;
    case ACTION_KILL:
        kill(data.pid, SIGTERM);
        break;
    case ACTION_LIST:
        execlp("ssh-add", "ssh-add", "-l", NULL);
        err(EXIT_FAILURE, "failed to launch ssh-add");
    case ACTION_UNLOCK:
        unlock(&data, password);
        break;
    default:
        break;
    }

    return 0;
}

// vim: et:sts=4:sw=4:cino=(0
