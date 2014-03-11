/* Redis CLI (command line interface)
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include "version.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>

#include "hiredis.h"
#include "sds.h"
#include "zmalloc.h"
#include "linenoise.h"
#include "help.h"
#include "anet.h"
#include "ae.h"

#define REDIS_NOTUSED(V) ((void) V)

#define OUTPUT_STANDARD 0
#define OUTPUT_RAW 1
#define OUTPUT_CSV 2
#define REDIS_CLI_KEEPALIVE_INTERVAL 15 /* seconds */
#define REDIS_DEFAULT_PIPE_TIMEOUT 30 /* seconds */

#define DEBUG(...) { \
    fprintf(stderr, "DEBUG: "); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
}
#define ERROR(...) { \
    fprintf(stderr, "ERROR: "); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
}

#define NOTICE(...) { \
    fprintf(stderr, "NOTICE: (%lld)", (long long)time(NULL)); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
}

#undef DEBUG
#define DEBUG(...) { }


static redisContext *context;
static struct config {
    char *hostip;
    int hostport;
    char *hostsocket;
    long repeat;
    long interval;
    int dbnum;
    int interactive;
    int shutdown;
    int monitor_mode;
    int pubsub_mode;
    int latency_mode;
    int latency_history;
    int cluster_mode;
    int cluster_reissue_command;
    int slave_mode;
    int pipe_mode;
    int pipe_timeout;

    int replay_mode;
    char *replay_filename;
    char *replay_filter;
    char *replay_orig;
    char *replay_rewrite;

    int getrdb_mode;
    int stat_mode;
    char *rdb_filename;
    int bigkeys;
    int stdinarg; /* get last arg from stdin. (-x option) */
    char *auth;
    int output; /* output mode, see OUTPUT_* defines */
    sds mb_delim;
    char prompt[128];
    char *eval;
} config;

static void usage();
char *redisGitSHA1(void);
char *redisGitDirty(void);

/*------------------------------------------------------------------------------
 * Utility functions
 *--------------------------------------------------------------------------- */

static long long mstime(void) {
    struct timeval tv;
    long long mst;

    gettimeofday(&tv, NULL);
    mst = ((long long)tv.tv_sec)*1000;
    mst += tv.tv_usec/1000;
    return mst;
}

static void cliRefreshPrompt(void) {
    int len;

    if (config.hostsocket != NULL)
        len = snprintf(config.prompt,sizeof(config.prompt),"redis %s",
                       config.hostsocket);
    else
        len = snprintf(config.prompt,sizeof(config.prompt),
                       strchr(config.hostip,':') ? "[%s]:%d" : "%s:%d",
                       config.hostip, config.hostport);
    /* Add [dbnum] if needed */
    if (config.dbnum != 0)
        len += snprintf(config.prompt+len,sizeof(config.prompt)-len,"[%d]",
            config.dbnum);
    snprintf(config.prompt+len,sizeof(config.prompt)-len,"> ");
}

/*------------------------------------------------------------------------------
 * Help functions
 *--------------------------------------------------------------------------- */

#define CLI_HELP_COMMAND 1
#define CLI_HELP_GROUP 2

typedef struct {
    int type;
    int argc;
    sds *argv;
    sds full;

    /* Only used for help on commands */
    struct commandHelp *org;
} helpEntry;

static helpEntry *helpEntries;
static int helpEntriesLen;

static sds cliVersion() {
    sds version;
    version = sdscatprintf(sdsempty(), "%s", REDIS_VERSION);

    /* Add git commit and working tree status when available */
    if (strtoll(redisGitSHA1(),NULL,16)) {
        version = sdscatprintf(version, " (git:%s", redisGitSHA1());
        if (strtoll(redisGitDirty(),NULL,10))
            version = sdscatprintf(version, "-dirty");
        version = sdscat(version, ")");
    }
    return version;
}

static void cliInitHelp() {
    int commandslen = sizeof(commandHelp)/sizeof(struct commandHelp);
    int groupslen = sizeof(commandGroups)/sizeof(char*);
    int i, len, pos = 0;
    helpEntry tmp;

    helpEntriesLen = len = commandslen+groupslen;
    helpEntries = malloc(sizeof(helpEntry)*len);

    for (i = 0; i < groupslen; i++) {
        tmp.argc = 1;
        tmp.argv = malloc(sizeof(sds));
        tmp.argv[0] = sdscatprintf(sdsempty(),"@%s",commandGroups[i]);
        tmp.full = tmp.argv[0];
        tmp.type = CLI_HELP_GROUP;
        tmp.org = NULL;
        helpEntries[pos++] = tmp;
    }

    for (i = 0; i < commandslen; i++) {
        tmp.argv = sdssplitargs(commandHelp[i].name,&tmp.argc);
        tmp.full = sdsnew(commandHelp[i].name);
        tmp.type = CLI_HELP_COMMAND;
        tmp.org = &commandHelp[i];
        helpEntries[pos++] = tmp;
    }
}

/* Output command help to stdout. */
static void cliOutputCommandHelp(struct commandHelp *help, int group) {
    printf("\r\n  \x1b[1m%s\x1b[0m \x1b[90m%s\x1b[0m\r\n", help->name, help->params);
    printf("  \x1b[33msummary:\x1b[0m %s\r\n", help->summary);
    printf("  \x1b[33msince:\x1b[0m %s\r\n", help->since);
    if (group) {
        printf("  \x1b[33mgroup:\x1b[0m %s\r\n", commandGroups[help->group]);
    }
}

/* Print generic help. */
static void cliOutputGenericHelp() {
    sds version = cliVersion();
    printf(
        "redis-cli %s\r\n"
        "Type: \"help @<group>\" to get a list of commands in <group>\r\n"
        "      \"help <command>\" for help on <command>\r\n"
        "      \"help <tab>\" to get a list of possible help topics\r\n"
        "      \"quit\" to exit\r\n",
        version
    );
    sdsfree(version);
}

/* Output all command help, filtering by group or command name. */
static void cliOutputHelp(int argc, char **argv) {
    int i, j, len;
    int group = -1;
    helpEntry *entry;
    struct commandHelp *help;

    if (argc == 0) {
        cliOutputGenericHelp();
        return;
    } else if (argc > 0 && argv[0][0] == '@') {
        len = sizeof(commandGroups)/sizeof(char*);
        for (i = 0; i < len; i++) {
            if (strcasecmp(argv[0]+1,commandGroups[i]) == 0) {
                group = i;
                break;
            }
        }
    }

    assert(argc > 0);
    for (i = 0; i < helpEntriesLen; i++) {
        entry = &helpEntries[i];
        if (entry->type != CLI_HELP_COMMAND) continue;

        help = entry->org;
        if (group == -1) {
            /* Compare all arguments */
            if (argc == entry->argc) {
                for (j = 0; j < argc; j++) {
                    if (strcasecmp(argv[j],entry->argv[j]) != 0) break;
                }
                if (j == argc) {
                    cliOutputCommandHelp(help,1);
                }
            }
        } else {
            if (group == help->group) {
                cliOutputCommandHelp(help,0);
            }
        }
    }
    printf("\r\n");
}

static void completionCallback(const char *buf, linenoiseCompletions *lc) {
    size_t startpos = 0;
    int mask;
    int i;
    size_t matchlen;
    sds tmp;

    if (strncasecmp(buf,"help ",5) == 0) {
        startpos = 5;
        while (isspace(buf[startpos])) startpos++;
        mask = CLI_HELP_COMMAND | CLI_HELP_GROUP;
    } else {
        mask = CLI_HELP_COMMAND;
    }

    for (i = 0; i < helpEntriesLen; i++) {
        if (!(helpEntries[i].type & mask)) continue;

        matchlen = strlen(buf+startpos);
        if (strncasecmp(buf+startpos,helpEntries[i].full,matchlen) == 0) {
            tmp = sdsnewlen(buf,startpos);
            tmp = sdscat(tmp,helpEntries[i].full);
            linenoiseAddCompletion(lc,tmp);
            sdsfree(tmp);
        }
    }
}

/*------------------------------------------------------------------------------
 * Networking / parsing
 *--------------------------------------------------------------------------- */

/* Send AUTH command to the server */
static int cliAuth() {
    redisReply *reply;
    if (config.auth == NULL) return REDIS_OK;

    reply = redisCommand(context,"AUTH %s",config.auth);
    if (reply != NULL) {
        freeReplyObject(reply);
        return REDIS_OK;
    }
    return REDIS_ERR;
}

/* Send SELECT dbnum to the server */
static int cliSelect() {
    redisReply *reply;
    if (config.dbnum == 0) return REDIS_OK;

    reply = redisCommand(context,"SELECT %d",config.dbnum);
    if (reply != NULL) {
        freeReplyObject(reply);
        return REDIS_OK;
    }
    return REDIS_ERR;
}

/* Connect to the server. If force is not zero the connection is performed
 * even if there is already a connected socket. */
static int cliConnect(int force) {
    if (context == NULL || force) {
        if (context != NULL)
            redisFree(context);

        if (config.hostsocket == NULL) {
            context = redisConnect(config.hostip,config.hostport);
        } else {
            context = redisConnectUnix(config.hostsocket);
        }

        if (context->err) {
            fprintf(stderr,"Could not connect to Redis at ");
            if (config.hostsocket == NULL)
                fprintf(stderr,"%s:%d: %s\n",config.hostip,config.hostport,context->errstr);
            else
                fprintf(stderr,"%s: %s\n",config.hostsocket,context->errstr);
            redisFree(context);
            context = NULL;
            return REDIS_ERR;
        }

        /* Set aggressive KEEP_ALIVE socket option in the Redis context socket
         * in order to prevent timeouts caused by the execution of long
         * commands. At the same time this improves the detection of real
         * errors. */
        anetKeepAlive(NULL, context->fd, REDIS_CLI_KEEPALIVE_INTERVAL);

        /* Do AUTH and select the right DB. */
        if (cliAuth() != REDIS_OK)
            return REDIS_ERR;
        if (cliSelect() != REDIS_OK)
            return REDIS_ERR;
    }
    return REDIS_OK;
}

static void cliPrintContextError() {
    if (context == NULL) return;
    fprintf(stderr,"Error: %s\n",context->errstr);
}

static sds cliFormatReplyTTY(redisReply *r, char *prefix) {
    sds out = sdsempty();
    switch (r->type) {
    case REDIS_REPLY_ERROR:
        out = sdscatprintf(out,"(error) %s\n", r->str);
    break;
    case REDIS_REPLY_STATUS:
        out = sdscat(out,r->str);
        out = sdscat(out,"\n");
    break;
    case REDIS_REPLY_INTEGER:
        out = sdscatprintf(out,"(integer) %lld\n",r->integer);
    break;
    case REDIS_REPLY_STRING:
        /* If you are producing output for the standard output we want
        * a more interesting output with quoted characters and so forth */
        out = sdscatrepr(out,r->str,r->len);
        out = sdscat(out,"\n");
    break;
    case REDIS_REPLY_NIL:
        out = sdscat(out,"(nil)\n");
    break;
    case REDIS_REPLY_ARRAY:
        if (r->elements == 0) {
            out = sdscat(out,"(empty list or set)\n");
        } else {
            unsigned int i, idxlen = 0;
            char _prefixlen[16];
            char _prefixfmt[16];
            sds _prefix;
            sds tmp;

            /* Calculate chars needed to represent the largest index */
            i = r->elements;
            do {
                idxlen++;
                i /= 10;
            } while(i);

            /* Prefix for nested multi bulks should grow with idxlen+2 spaces */
            memset(_prefixlen,' ',idxlen+2);
            _prefixlen[idxlen+2] = '\0';
            _prefix = sdscat(sdsnew(prefix),_prefixlen);

            /* Setup prefix format for every entry */
            snprintf(_prefixfmt,sizeof(_prefixfmt),"%%s%%%dd) ",idxlen);

            for (i = 0; i < r->elements; i++) {
                /* Don't use the prefix for the first element, as the parent
                 * caller already prepended the index number. */
                out = sdscatprintf(out,_prefixfmt,i == 0 ? "" : prefix,i+1);

                /* Format the multi bulk entry */
                tmp = cliFormatReplyTTY(r->element[i],_prefix);
                out = sdscatlen(out,tmp,sdslen(tmp));
                sdsfree(tmp);
            }
            sdsfree(_prefix);
        }
    break;
    default:
        fprintf(stderr,"Unknown reply type: %d\n", r->type);
        exit(1);
    }
    return out;
}

static sds cliFormatReplyRaw(redisReply *r) {
    sds out = sdsempty(), tmp;
    size_t i;

    switch (r->type) {
    case REDIS_REPLY_NIL:
        /* Nothing... */
        break;
    case REDIS_REPLY_ERROR:
        out = sdscatlen(out,r->str,r->len);
        out = sdscatlen(out,"\n",1);
        break;
    case REDIS_REPLY_STATUS:
    case REDIS_REPLY_STRING:
        out = sdscatlen(out,r->str,r->len);
        break;
    case REDIS_REPLY_INTEGER:
        out = sdscatprintf(out,"%lld",r->integer);
        break;
    case REDIS_REPLY_ARRAY:
        for (i = 0; i < r->elements; i++) {
            if (i > 0) out = sdscat(out,config.mb_delim);
            tmp = cliFormatReplyRaw(r->element[i]);
            out = sdscatlen(out,tmp,sdslen(tmp));
            sdsfree(tmp);
        }
        break;
    default:
        fprintf(stderr,"Unknown reply type: %d\n", r->type);
        exit(1);
    }
    return out;
}

static sds cliFormatReplyCSV(redisReply *r) {
    unsigned int i;

    sds out = sdsempty();
    switch (r->type) {
    case REDIS_REPLY_ERROR:
        out = sdscat(out,"ERROR,");
        out = sdscatrepr(out,r->str,strlen(r->str));
    break;
    case REDIS_REPLY_STATUS:
        out = sdscatrepr(out,r->str,r->len);
    break;
    case REDIS_REPLY_INTEGER:
        out = sdscatprintf(out,"%lld",r->integer);
    break;
    case REDIS_REPLY_STRING:
        out = sdscatrepr(out,r->str,r->len);
    break;
    case REDIS_REPLY_NIL:
        out = sdscat(out,"NIL\n");
    break;
    case REDIS_REPLY_ARRAY:
        for (i = 0; i < r->elements; i++) {
            sds tmp = cliFormatReplyCSV(r->element[i]);
            out = sdscatlen(out,tmp,sdslen(tmp));
            if (i != r->elements-1) out = sdscat(out,",");
            sdsfree(tmp);
        }
    break;
    default:
        fprintf(stderr,"Unknown reply type: %d\n", r->type);
        exit(1);
    }
    return out;
}

static int cliReadReply(int output_raw_strings) {
    void *_reply;
    redisReply *reply;
    sds out = NULL;
    int output = 1;

    if (redisGetReply(context,&_reply) != REDIS_OK) {
        if (config.shutdown)
            return REDIS_OK;
        if (config.interactive) {
            /* Filter cases where we should reconnect */
            if (context->err == REDIS_ERR_IO && errno == ECONNRESET)
                return REDIS_ERR;
            if (context->err == REDIS_ERR_EOF)
                return REDIS_ERR;
        }
        cliPrintContextError();
        exit(1);
        return REDIS_ERR; /* avoid compiler warning */
    }

    reply = (redisReply*)_reply;

    /* Check if we need to connect to a different node and reissue the
     * request. */
    if (config.cluster_mode && reply->type == REDIS_REPLY_ERROR &&
        (!strncmp(reply->str,"MOVED",5) || !strcmp(reply->str,"ASK")))
    {
        char *p = reply->str, *s;
        int slot;

        output = 0;
        /* Comments show the position of the pointer as:
         *
         * [S] for pointer 's'
         * [P] for pointer 'p'
         */
        s = strchr(p,' ');      /* MOVED[S]3999 127.0.0.1:6381 */
        p = strchr(s+1,' ');    /* MOVED[S]3999[P]127.0.0.1:6381 */
        *p = '\0';
        slot = atoi(s+1);
        s = strchr(p+1,':');    /* MOVED 3999[P]127.0.0.1[S]6381 */
        *s = '\0';
        sdsfree(config.hostip);
        config.hostip = sdsnew(p+1);
        config.hostport = atoi(s+1);
        if (config.interactive)
            printf("-> Redirected to slot [%d] located at %s:%d\n",
                slot, config.hostip, config.hostport);
        config.cluster_reissue_command = 1;
    }

    if (output) {
        if (output_raw_strings) {
            out = cliFormatReplyRaw(reply);
        } else {
            if (config.output == OUTPUT_RAW) {
                out = cliFormatReplyRaw(reply);
                out = sdscat(out,"\n");
            } else if (config.output == OUTPUT_STANDARD) {
                out = cliFormatReplyTTY(reply,"");
            } else if (config.output == OUTPUT_CSV) {
                out = cliFormatReplyCSV(reply);
                out = sdscat(out,"\n");
            }
        }
        fwrite(out,sdslen(out),1,stdout);
        sdsfree(out);
    }
    freeReplyObject(reply);
    return REDIS_OK;
}

static int cliSendCommand(int argc, char **argv, int repeat) {
    char *command = argv[0];
    size_t *argvlen;
    int j, output_raw;

    if (!strcasecmp(command,"help") || !strcasecmp(command,"?")) {
        cliOutputHelp(--argc, ++argv);
        return REDIS_OK;
    }

    if (context == NULL) return REDIS_ERR;

    output_raw = 0;
    if (!strcasecmp(command,"info") ||
        (argc == 2 && !strcasecmp(command,"cluster") &&
                      (!strcasecmp(argv[1],"nodes") ||
                       !strcasecmp(argv[1],"info"))) ||
        (argc == 2 && !strcasecmp(command,"client") &&
                       !strcasecmp(argv[1],"list")))

    {
        output_raw = 1;
    }

    if (!strcasecmp(command,"shutdown")) config.shutdown = 1;
    if (!strcasecmp(command,"monitor")) config.monitor_mode = 1;
    if (!strcasecmp(command,"subscribe") ||
        !strcasecmp(command,"psubscribe")) config.pubsub_mode = 1;

    /* Setup argument length */
    argvlen = malloc(argc*sizeof(size_t));
    for (j = 0; j < argc; j++)
        argvlen[j] = sdslen(argv[j]);

    while(repeat--) {
        redisAppendCommandArgv(context,argc,(const char**)argv,argvlen);
        while (config.monitor_mode) {
            if (cliReadReply(output_raw) != REDIS_OK) exit(1);
            fflush(stdout);
        }

        if (config.pubsub_mode) {
            if (config.output != OUTPUT_RAW)
                printf("Reading messages... (press Ctrl-C to quit)\n");
            while (1) {
                if (cliReadReply(output_raw) != REDIS_OK) exit(1);
            }
        }

        if (cliReadReply(output_raw) != REDIS_OK) {
            free(argvlen);
            return REDIS_ERR;
        } else {
            /* Store database number when SELECT was successfully executed. */
            if (!strcasecmp(command,"select") && argc == 2) {
                config.dbnum = atoi(argv[1]);
                cliRefreshPrompt();
            }
        }
        if (config.interval) usleep(config.interval);
        fflush(stdout); /* Make it grep friendly */
    }

    free(argvlen);
    return REDIS_OK;
}

/* Send the INFO command, reconnecting the link if needed. */
static redisReply *reconnectingInfo(void) {
    redisContext *c = context;
    redisReply *reply = NULL;
    int tries = 0;

    assert(!c->err);
    while(reply == NULL) {
        while (c->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
            printf("Reconnecting (%d)...\r", ++tries);
            fflush(stdout);

            redisFree(c);
            c = redisConnect(config.hostip,config.hostport);
            usleep(1000000);
        }

        reply = redisCommand(c,"INFO");
        if (c->err && !(c->err & (REDIS_ERR_IO | REDIS_ERR_EOF))) {
            fprintf(stderr, "Error: %s\n", c->errstr);
            exit(1);
        } else if (tries > 0) {
            printf("\n");
        }
    }

    context = c;
    return reply;
}

/*------------------------------------------------------------------------------
 * User interface
 *--------------------------------------------------------------------------- */

static int parseOptions(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; i++) {
        int lastarg = i==argc-1;

        if (!strcmp(argv[i],"-h") && !lastarg) {
            sdsfree(config.hostip);
            config.hostip = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i],"-h") && lastarg) {
            usage();
        } else if (!strcmp(argv[i],"--help")) {
            usage();
        } else if (!strcmp(argv[i],"-x")) {
            config.stdinarg = 1;
        } else if (!strcmp(argv[i],"-p") && !lastarg) {
            config.hostport = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-s") && !lastarg) {
            config.hostsocket = argv[++i];
        } else if (!strcmp(argv[i],"-r") && !lastarg) {
            config.repeat = strtoll(argv[++i],NULL,10);
        } else if (!strcmp(argv[i],"-i") && !lastarg) {
            double seconds = atof(argv[++i]);
            config.interval = seconds*1000000;
        } else if (!strcmp(argv[i],"-n") && !lastarg) {
            config.dbnum = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-a") && !lastarg) {
            config.auth = argv[++i];
        } else if (!strcmp(argv[i],"--raw")) {
            config.output = OUTPUT_RAW;
        } else if (!strcmp(argv[i],"--csv")) {
            config.output = OUTPUT_CSV;
        } else if (!strcmp(argv[i],"--latency")) {
            config.latency_mode = 1;
        } else if (!strcmp(argv[i],"--latency-history")) {
            config.latency_mode = 1;
            config.latency_history = 1;
        } else if (!strcmp(argv[i],"--slave")) {
            config.slave_mode = 1;
        } else if (!strcmp(argv[i],"--stat")) {
            config.stat_mode = 1;
        } else if (!strcmp(argv[i],"--rdb") && !lastarg) {
            config.getrdb_mode = 1;
            config.rdb_filename = argv[++i];
        } else if (!strcmp(argv[i],"--pipe")) {
            config.pipe_mode = 1;
        } else if (!strcmp(argv[i],"--pipe-timeout") && !lastarg) {
            config.pipe_timeout = atoi(argv[++i]);

        } else if (!strcmp(argv[i],"--replay") && !lastarg) {
            config.replay_mode = 1;
            config.replay_filename = argv[++i];
        } else if (!strcmp(argv[i],"--filter") && !lastarg) {
            config.replay_filter = argv[++i];
        } else if (!strcmp(argv[i],"--orig") && !lastarg) {
            config.replay_orig = argv[++i];
        } else if (!strcmp(argv[i],"--rewrite") && !lastarg) {
            config.replay_rewrite = argv[++i];

        } else if (!strcmp(argv[i],"--bigkeys")) {
            config.bigkeys = 1;
        } else if (!strcmp(argv[i],"--eval") && !lastarg) {
            config.eval = argv[++i];
        } else if (!strcmp(argv[i],"-c")) {
            config.cluster_mode = 1;
        } else if (!strcmp(argv[i],"-d") && !lastarg) {
            sdsfree(config.mb_delim);
            config.mb_delim = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i],"-v") || !strcmp(argv[i], "--version")) {
            sds version = cliVersion();
            printf("redis-cli %s\n", version);
            sdsfree(version);
            exit(0);
        } else {
            if (argv[i][0] == '-') {
                fprintf(stderr,
                    "Unrecognized option or bad number of args for: '%s'\n",
                    argv[i]);
                exit(1);
            } else {
                /* Likely the command name, stop here. */
                break;
            }
        }
    }
    return i;
}

static sds readArgFromStdin(void) {
    char buf[1024];
    sds arg = sdsempty();

    while(1) {
        int nread = read(fileno(stdin),buf,1024);

        if (nread == 0) break;
        else if (nread == -1) {
            perror("Reading from standard input");
            exit(1);
        }
        arg = sdscatlen(arg,buf,nread);
    }
    return arg;
}

static void usage() {
    sds version = cliVersion();
    fprintf(stderr,
"redis-cli %s\n"
"\n"
"Usage: redis-cli [OPTIONS] [cmd [arg [arg ...]]]\n"
"  -h <hostname>      Server hostname (default: 127.0.0.1)\n"
"  -p <port>          Server port (default: 6379)\n"
"  -s <socket>        Server socket (overrides hostname and port)\n"
"  -a <password>      Password to use when connecting to the server\n"
"  -r <repeat>        Execute specified command N times\n"
"  -i <interval>      When -r is used, waits <interval> seconds per command.\n"
"                     It is possible to specify sub-second times like -i 0.1\n"
"  -n <db>            Database number\n"
"  -x                 Read last argument from STDIN\n"
"  -d <delimiter>     Multi-bulk delimiter in for raw formatting (default: \\n)\n"
"  -c                 Enable cluster mode (follow -ASK and -MOVED redirections)\n"
"  --raw              Use raw formatting for replies (default when STDOUT is\n"
"                     not a tty)\n"
"  --csv              Output in CSV format\n"
"  --latency          Enter a special mode continuously sampling latency\n"
"  --latency-history  Like --latency but tracking latency changes over time.\n"
"                     Default time interval is 15 sec. Change it using -i.\n"
"  --slave            Simulate a slave showing commands received from the master\n"
"  --rdb <filename>   Transfer an RDB dump from remote server to local file.\n"
"  --pipe             Transfer raw Redis protocol from stdin to server\n"
"  --pipe-timeout <n> In --pipe mode, abort with error if after sending all data\n"
"                     no reply is received within <n> seconds.\n"
"                     Default timeout: %d. Use 0 to wait forever.\n"
"  --bigkeys          Sample Redis keys looking for big keys\n"
"  --eval <file>      Send an EVAL command using the Lua script at <file>\n"
"  --help             Output this help and exit\n"
"  --version          Output version and exit\n"
"\n"
"Examples:\n"
"  cat /etc/passwd | redis-cli -x set mypasswd\n"
"  redis-cli get mypasswd\n"
"  redis-cli -r 100 lpush mylist x\n"
"  redis-cli -r 100 -i 1 info | grep used_memory_human:\n"
"  redis-cli --eval myscript.lua key1 key2 , arg1 arg2 arg3\n"
"  (Note: when using --eval the comma separates KEYS[] from ARGV[] items)\n"
"\n"
"When no command is given, redis-cli starts in interactive mode.\n"
"Type \"help\" in interactive mode for information on available commands.\n"
"\n",
        version, REDIS_DEFAULT_PIPE_TIMEOUT);
    sdsfree(version);
    exit(1);
}

/* Turn the plain C strings into Sds strings */
static char **convertToSds(int count, char** args) {
  int j;
  char **sds = zmalloc(sizeof(char*)*count);

  for(j = 0; j < count; j++)
    sds[j] = sdsnew(args[j]);

  return sds;
}

#define LINE_BUFLEN 4096
static void repl() {
    sds historyfile = NULL;
    int history = 0;
    char *line;
    int argc;
    sds *argv;

    config.interactive = 1;
    linenoiseSetCompletionCallback(completionCallback);

    /* Only use history when stdin is a tty. */
    if (isatty(fileno(stdin))) {
        history = 1;

        if (getenv("HOME") != NULL) {
            historyfile = sdscatprintf(sdsempty(),"%s/.rediscli_history",getenv("HOME"));
            linenoiseHistoryLoad(historyfile);
        }
    }

    cliRefreshPrompt();
    while((line = linenoise(context ? config.prompt : "not connected> ")) != NULL) {
        if (line[0] != '\0') {
            argv = sdssplitargs(line,&argc);
            if (history) linenoiseHistoryAdd(line);
            if (historyfile) linenoiseHistorySave(historyfile);

            if (argv == NULL) {
                printf("Invalid argument(s)\n");
                free(line);
                continue;
            } else if (argc > 0) {
                if (strcasecmp(argv[0],"quit") == 0 ||
                    strcasecmp(argv[0],"exit") == 0)
                {
                    exit(0);
                } else if (argc == 3 && !strcasecmp(argv[0],"connect")) {
                    sdsfree(config.hostip);
                    config.hostip = sdsnew(argv[1]);
                    config.hostport = atoi(argv[2]);
                    cliRefreshPrompt();
                    cliConnect(1);
                } else if (argc == 1 && !strcasecmp(argv[0],"clear")) {
                    linenoiseClearScreen();
                } else {
                    long long start_time = mstime(), elapsed;
                    int repeat, skipargs = 0;

                    repeat = atoi(argv[0]);
                    if (argc > 1 && repeat) {
                        skipargs = 1;
                    } else {
                        repeat = 1;
                    }

                    while (1) {
                        config.cluster_reissue_command = 0;
                        if (cliSendCommand(argc-skipargs,argv+skipargs,repeat)
                            != REDIS_OK)
                        {
                            cliConnect(1);

                            /* If we still cannot send the command print error.
                             * We'll try to reconnect the next time. */
                            if (cliSendCommand(argc-skipargs,argv+skipargs,repeat)
                                != REDIS_OK)
                                cliPrintContextError();
                        }
                        /* Issue the command again if we got redirected in cluster mode */
                        if (config.cluster_mode && config.cluster_reissue_command) {
                            cliConnect(1);
                        } else {
                            break;
                        }
                    }
                    elapsed = mstime()-start_time;
                    if (elapsed >= 500) {
                        printf("(%.2fs)\n",(double)elapsed/1000);
                    }
                }
            }
            /* Free the argument vector */
            sdsfreesplitres(argv,argc);
        }
        /* linenoise() returns malloc-ed lines like readline() */
        free(line);
    }
    exit(0);
}

static int noninteractive(int argc, char **argv) {
    int retval = 0;
    if (config.stdinarg) {
        argv = zrealloc(argv, (argc+1)*sizeof(char*));
        argv[argc] = readArgFromStdin();
        retval = cliSendCommand(argc+1, argv, config.repeat);
    } else {
        /* stdin is probably a tty, can be tested with S_ISCHR(s.st_mode) */
        retval = cliSendCommand(argc, argv, config.repeat);
    }
    return retval;
}

static int evalMode(int argc, char **argv) {
    sds script = sdsempty();
    FILE *fp;
    char buf[1024];
    size_t nread;
    char **argv2;
    int j, got_comma = 0, keys = 0;

    /* Load the script from the file, as an sds string. */
    fp = fopen(config.eval,"r");
    if (!fp) {
        fprintf(stderr,
            "Can't open file '%s': %s\n", config.eval, strerror(errno));
        exit(1);
    }
    while((nread = fread(buf,1,sizeof(buf),fp)) != 0) {
        script = sdscatlen(script,buf,nread);
    }
    fclose(fp);

    /* Create our argument vector */
    argv2 = zmalloc(sizeof(sds)*(argc+3));
    argv2[0] = sdsnew("EVAL");
    argv2[1] = script;
    for (j = 0; j < argc; j++) {
        if (!got_comma && argv[j][0] == ',' && argv[j][1] == 0) {
            got_comma = 1;
            continue;
        }
        argv2[j+3-got_comma] = sdsnew(argv[j]);
        if (!got_comma) keys++;
    }
    argv2[2] = sdscatprintf(sdsempty(),"%d",keys);

    /* Call it */
    return cliSendCommand(argc+3-got_comma, argv2, config.repeat);
}

#define LATENCY_SAMPLE_RATE 10 /* milliseconds. */
#define LATENCY_HISTORY_DEFAULT_INTERVAL 15000 /* milliseconds. */
static void latencyMode(void) {
    redisReply *reply;
    long long start, latency, min = 0, max = 0, tot = 0, count = 0;
    long long history_interval =
        config.interval ? config.interval/1000 :
                          LATENCY_HISTORY_DEFAULT_INTERVAL;
    double avg;
    long long history_start = mstime();

    if (!context) exit(1);
    while(1) {
        start = mstime();
        reply = redisCommand(context,"PING");
        if (reply == NULL) {
            fprintf(stderr,"\nI/O error\n");
            exit(1);
        }
        latency = mstime()-start;
        freeReplyObject(reply);
        count++;
        if (count == 1) {
            min = max = tot = latency;
            avg = (double) latency;
        } else {
            if (latency < min) min = latency;
            if (latency > max) max = latency;
            tot += latency;
            avg = (double) tot/count;
        }
        printf("\x1b[0G\x1b[2Kmin: %lld, max: %lld, avg: %.2f (%lld samples)",
            min, max, avg, count);
        fflush(stdout);
        if (config.latency_history && mstime()-history_start > history_interval)
        {
            printf(" -- %.2f seconds range\n", (float)(mstime()-history_start)/1000);
            history_start = mstime();
            min = max = tot = count = 0;
        }
        usleep(LATENCY_SAMPLE_RATE * 1000);
    }
}

/* Sends SYNC and reads the number of bytes in the payload. Used both by
 * slaveMode() and getRDB(). */
unsigned long long sendSync(int fd) {
    /* To start we need to send the SYNC command and return the payload.
     * The hiredis client lib does not understand this part of the protocol
     * and we don't want to mess with its buffers, so everything is performed
     * using direct low-level I/O. */
    char buf[4096], *p;
    ssize_t nread;

    /* Send the SYNC command. */
    if (write(fd,"SYNC\r\n",6) != 6) {
        fprintf(stderr,"Error writing to master\n");
        exit(1);
    }

    /* Read $<payload>\r\n, making sure to read just up to "\n" */
    p = buf;
    while(1) {
        nread = read(fd,p,1);
        if (nread <= 0) {
            fprintf(stderr,"Error reading bulk length while SYNCing\n");
            exit(1);
        }
        if (*p == '\n' && p != buf) break;
        if (*p != '\n') p++;
    }
    *p = '\0';
    if (buf[0] == '-') {
        printf("SYNC with master failed: %s\n", buf);
        exit(1);
    }
    return strtoull(buf+1,NULL,10);
}

static void slaveMode(void) {
    int fd = context->fd;
    unsigned long long payload = sendSync(fd);
    char buf[1024];

    fprintf(stderr,"SYNC with master, discarding %llu "
                   "bytes of bulk transfer...\n", payload);

    /* Discard the payload. */
    while(payload) {
        ssize_t nread;

        nread = read(fd,buf,(payload > sizeof(buf)) ? sizeof(buf) : payload);
        if (nread <= 0) {
            fprintf(stderr,"Error reading RDB payload while SYNCing\n");
            exit(1);
        }
        payload -= nread;
    }
    fprintf(stderr,"SYNC done. Logging commands from master.\n");

    /* Now we can use hiredis to read the incoming protocol. */
    config.output = OUTPUT_CSV;
    while (cliReadReply(0) == REDIS_OK);
}

/* This function implements --rdb, so it uses the replication protocol in order
 * to fetch the RDB file from a remote server. */
static void getRDB(void) {
    int s = context->fd;
    int fd;
    unsigned long long payload = sendSync(s);
    char buf[4096];

    fprintf(stderr,"SYNC sent to master, writing %llu bytes to '%s'\n",
        payload, config.rdb_filename);

    /* Write to file. */
    if (!strcmp(config.rdb_filename,"-")) {
        fd = STDOUT_FILENO;
    } else {
        fd = open(config.rdb_filename, O_CREAT|O_WRONLY, 0644);
        if (fd == -1) {
            fprintf(stderr, "Error opening '%s': %s\n", config.rdb_filename,
                strerror(errno));
            exit(1);
        }
    }

    while(payload) {
        ssize_t nread, nwritten;

        nread = read(s,buf,(payload > sizeof(buf)) ? sizeof(buf) : payload);
        if (nread <= 0) {
            fprintf(stderr,"I/O Error reading RDB payload from socket\n");
            exit(1);
        }
        nwritten = write(fd, buf, nread);
        if (nwritten != nread) {
            fprintf(stderr,"Error writing data to file: %s\n",
                strerror(errno));
            exit(1);
        }
        payload -= nread;
    }
    close(s); /* Close the file descriptor ASAP as fsync() may take time. */
    fsync(fd);
    fprintf(stderr,"Transfer finished with success.\n");
    exit(0);
}

int _blockWait(FILE * fp){
    fd_set rfds;
    struct timeval tv;
    int ret;

    DEBUG("blockWait");
    while(1){
        FD_ZERO(&rfds);
        FD_SET(fileno(fp), &rfds);

        tv.tv_sec = 1;          /* Wait up to 1 seconds. */
        tv.tv_usec = 0;

        ret= select(fileno(fp)+1, &rfds, NULL, NULL, &tv);
        DEBUG("select return %d", ret);
        /*DEBUG("feof return : %d", feof(fp));*/
        /*clearerr(fp);*/
        if (ret== -1){
            perror("select() on blockWait");
            return -1;
        }
        else if (ret){
            return 0;
        }
        else{
            continue;
        }
    }
}

/*wait for file append*/
int blockWait(FILE * fp){
    off_t size = 0;
    struct stat stats;
    if (fstat (fileno(fp), &stats) != 0)
    {
        ERROR("error on fstat");
        return -1;
    }
    size = stats.st_size;

    DEBUG("blockWait");
    while(1){
        if (fstat (fileno(fp), &stats) != 0) {
            ERROR("error on fstat");
            return -1;
        }
        if (size != stats.st_size){
            break;
        }
        sleep(1);
    }
    return 0;
}

//like fgets,but block,
//return 0 on success
//return -1 if can not find '\n' after 'size' bytes
int blockReadline(FILE * fp, char * buf, int size){
    int i = 0;
    int c = 0;

    while(1){
        c = fgetc(fp);
        if(c == EOF) {
            NOTICE("check tail")
            if (blockWait(fp) == -1){
                return -1;
            }
            continue;
        }

        buf[i++] = c;
        if(c == '\n'){
            buf[i] = 0;
            break;
        }
        if(i >= size){
            return -1;
        }
    }
    return 0;
}

//like fread, but block until readed 'size' bytes.
//return 0 on success
//return -1 on error
int blockReadBytes(FILE * fp, char * buf, int size){
    int ret = 0;
    int readed = 0;
    while(1){
        ret = fread(buf+readed, 1, size-readed, fp);
        if(ret == EOF) {
            if (blockWait(fp) == -1){
                return -1;
            }
            continue;
        }

        readed += ret;
        if (readed == size){
            return readed;
        }
    }
}

int readLong(FILE *fp, char prefix) {
    char buf[128];
    if (blockReadline(fp, buf, sizeof(buf)) == -1) {
        ERROR("blockReadline return: -1");
        return -1;
    }
    if (buf[0] != prefix) {
        ERROR("prefix error on readLong, [expect: %c], [got: %c]", prefix, buf[0]);
        return -1;
    }
    /*DEBUG("blockReadline return: %s", buf);*/
    return strtol(buf+1, NULL, 10);
}

typedef struct Msg{
    int argc;
    char ** argv;
    size_t *argvlen;
}Msg;

static Msg* newMsg(int argc){
    Msg * msg;
    msg = zmalloc(sizeof(Msg));
    msg->argc = argc;
    msg->argv = zmalloc(sizeof(char*) * msg->argc);
    msg->argvlen = zmalloc(msg->argc);
    return msg;
}

void freeMsg(Msg * msg){
    int i = 0;
    for(i=0; i < msg->argc; i++){
        sdsfree(msg->argv[i]);
        msg->argv[i] = NULL;
    }
    zfree(msg->argv);
    zfree(msg->argvlen);
    zfree(msg);
}

//read one msg
//return 0 on success
//return -1 on error
static Msg* readMsg(FILE * fp){
    int i = 0;
    int len = 0;
    int ret = 0;
    char buf[128];
    Msg * msg;

    len = readLong(fp, '*');
    if (len < 1) {
        ERROR("readLong return: %d", len);
        return NULL;
    }
    DEBUG("got argc: %d", len);
    msg = newMsg(len);

    for (i = 0; i < msg->argc; i++) {
        len = readLong(fp, '$');
        msg->argvlen[i] = len;

        if (len < 1) {
            ERROR("readLong return: %d", len);
            return NULL;
        }
        msg->argv[i] = sdsnewlen(NULL, len);
        ret = blockReadBytes(fp, msg->argv[i], len);
        if (ret == -1) {
            ERROR("blockReadBytes return: %d", ret);
            return NULL;
        }

        ret = blockReadBytes(fp, buf, 2); //CRCL
        if (ret == -1) {
            ERROR("blockReadBytes return: %d", ret);
            return NULL;
        }

        DEBUG("got arg: %s", msg->argv[i]);
    }
    return msg;
}

#define CMD_SUPPORTED_NUM 78

static char * cmd_supported[] = { //78 cmd gen by:  cat notes/redis.md  | grep 'Yes ' | awk '{print $2}' | vim -
    "MSET", //change to many
    "SET",
    "HMSET",
    "EXPIRE",
    "EXPIREAT",
    "PERSIST",
    "PEXPIRE",
    "PEXPIREAT",
    "DEL",  //change to many
    "DUMP",
    "EXISTS",
    "PTTL",
    "RESTORE",
    "TTL",
    "TYPE",
    "APPEND",
    "BITCOUNT",
    "DECR",
    "DECRBY",
    "GET",
    "GETBIT",
    "GETRANGE",
    "GETSET",
    "INCR",
    "INCRBY",
    "INCRBYFLOAT",
    "MGET",
    "PSETEX",
    "SETBIT",
    "SETEX",
    "SETNX",
    "SETRANGE",
    "STRLEN",
    "HDEL",
    "HEXISTS",
    "HGET",
    "HGETALL",
    "HINCRBY",
    "HINCRBYFLOAT",
    "HKEYS",
    "HLEN",
    "HMGET",
    "HSET",
    "HSETNX",
    "HVALS",
    "LINDEX",
    "LINSERT",
    "LLEN",
    "LPOP",
    "LPUSH",
    "LPUSHX",
    "LRANGE",
    "LREM",
    "LSET",
    "LTRIM",
    "RPOP",
    "RPUSH",
    "RPUSHX",
    "SADD",
    "SCARD",
    "SISMEMBER",
    "SMEMBERS",
    "SPOP",
    "SRANDMEMBER",
    "SREM",
    "ZADD",
    "ZCARD",
    "ZCOUNT",
    "ZINCRBY",
    "ZRANGE",
    "ZRANGEBYSCORE",
    "ZRANK",
    "ZREM",
    "ZREMRANGEBYRANK",
    "ZREMRANGEBYSCORE",
    "ZREVRANGE",
    "ZREVRANGEBYSCORE",
    "ZREVRANK",
    "ZSCORE",
};

static int cmdSupport(char * cmd){
    int i = 0;
    for(i=0; i<CMD_SUPPORTED_NUM; i++){
        if(0 == strcmp(cmd_supported[i], cmd)){
            return 1;
        }
    }
    return 0;
}

//TODO: if is delete, rewrite all keys.
//1. filter by cmd
//2. filter by key
//3. rewrite keys
static int filterMsg(Msg *msg){
    char * cmd = msg->argv[0];
    sdstoupper(cmd);
    if(!cmdSupport(cmd)){
        msg->argc = 0;
    }
    return 0;
}

static int sizeofMsg(Msg * msg){
    if(0 == msg->argc){
        return 0;
    }
    int ret = 0, i = 0;
    int len = 0;
    ret = snprintf(NULL, 0, "*%d\r\n", msg->argc);
    len += ret;
    for (i = 0; i<msg->argc; i++){
        ret = snprintf(NULL, 0, "$%ld\r\n", sdslen(msg->argv[i]));
        len += ret;

        len += sdslen(msg->argv[i]);

        len += 2;
    }
    return len;
}

//return len
static int formatMsg(char *buf, Msg *msg){
    char * orig = buf;
    int ret = 0, i = 0;

    if(0 == msg->argc){
        return 0;
    }

    ret = sprintf(buf, "*%d\r\n", msg->argc);
    buf += ret;

    for (i = 0; i<msg->argc; i++){
        ret = sprintf(buf, "$%ld\r\n", sdslen(msg->argv[i]));
        buf += ret;

        memcpy(buf, msg->argv[i], sdslen(msg->argv[i]));
        buf += sdslen(msg->argv[i]);

        ret = sprintf(buf, "\r\n");
        buf += ret;
    }
    return buf - orig;
}


/*TODO: if batchsize is not 1, and at the end of file, it will wait (may lost msgs)*/
#define BATCHSIZE 1
static int myread(FILE * fp, char ** buf, int * buf_size){
    Msg* msgs[BATCHSIZE];
    int i;
    int totSize = 0;
    int ret = 0;
    int len = 0;

    for(i = 0; i< BATCHSIZE; i++){
        msgs[i] = readMsg(fp);
        if(!msgs[i]){
            ERROR("error on readMsg");
            return -1;
        }
        ret = filterMsg(msgs[i]);
        if(ret < 0){
            ERROR("error on filterMsg");
            return -1;
        }
        totSize += sizeofMsg(msgs[i]);
    }
    if (totSize > *buf_size){
        *buf_size = totSize + 1024;
        *buf = zrealloc(*buf, *buf_size);
    }

    for(i = 0; i< BATCHSIZE; i++){
        ret = formatMsg(*buf + len, msgs[i]);
        len += ret;
        freeMsg(msgs[i]);
    }
    DEBUG("myread return %d", len);
    return len;
}

static void sighandler( int sig_no ) { exit(0); }
/**
 *
 * safe replay (one by one)
 * 7k - 8k on twemproxy
 * 11k on redis
 */
static int safeReplayMode(void){
    int cnt = 0;
    int ret;
    Msg* msg;
    redisReply *reply;
    FILE *fp = NULL;

    signal(SIGUSR1, sighandler );// for gprof

    fp = fopen(config.replay_filename, "r");
    if (!fp) {
        fprintf(stderr,
            "Can't open file '%s': %s\n", config.eval, strerror(errno));
        exit(1);
    }

    NOTICE("start");
    cnt = 0;
    while(1){
        msg = readMsg(fp);
        if(!msg){
            ERROR("error on readMsg");
            return -1;
        }

        ret = filterMsg(msg);
        if(ret < 0){
            ERROR("error on filterMsg");
            return -1;
        }

        if (msg->argc == 0){
            continue;
        }

        cnt ++;
        if (cnt%100000 == 0){
            NOTICE("%d done!", cnt);
        }
        reply = redisCommandArgv(context, msg->argc, (const char **)msg->argv, msg->argvlen);
        if (!reply){
            ERROR("Error on cmd %d %s %s", msg->argc, msg->argv[0], msg->argv[1]);
            return -1;
        }
        freeReplyObject(reply);
        freeMsg(msg);
    }
}

static void replayMode(void) {
    int fd = context->fd;

    long long requests = 0, replies = 0, obuf_len = 0, obuf_pos = 0;
    char ibuf[1024*16];         /* Input buffer */
    int obuf_size = 1024*1024;
    char* obuf = zmalloc(obuf_size);

    char aneterr[ANET_ERR_LEN];
    redisReader *reader = redisReaderCreate();
    redisReply *reply;

    FILE *fp = NULL;
    fp = fopen(config.replay_filename, "r");
    if (!fp) {
        fprintf(stderr,
            "Can't open file '%s': %s\n", config.eval, strerror(errno));
        exit(1);
    }
    NOTICE("start")

    /* setup non blocking I/O. */
    if (anetNonBlock(aneterr,fd) == ANET_ERR) {
        fprintf(stderr, "Can't set the socket in non blocking mode: %s\n",
            aneterr);
        exit(1);
    }

    while(1) {
        int mask = AE_READABLE;
        DEBUG("request: %lld, replies: %lld", requests, replies);
        if ((replies == requests) || (obuf_len != 0))
            mask |= AE_WRITABLE;
        mask = aeWait(fd,mask,1000);

        DEBUG("mask: %d", mask);
        if (mask & AE_READABLE) {
            ssize_t nread;

            do {
                nread = read(fd,ibuf,sizeof(ibuf));
                if (nread == -1 && errno != EAGAIN && errno != EINTR) {
                    ERROR("Error reading from the server: %s", strerror(errno));
                    exit(1);
                }
                if (nread > 0) {
                    redisReaderFeed(reader,ibuf,nread);
                }
            } while(nread > 0);

            /* Consume replies. */
            do {
                if (redisReaderGetReply(reader,(void**)&reply) == REDIS_ERR) {
                    ERROR("Error reading replies from server");
                    exit(1);
                }
                if (reply) {
                    if (reply->type == REDIS_REPLY_ERROR) {
                        ERROR("got error: %s", reply->str);
                        exit(1);
                    }
                    replies++;
                    if (replies % 10000 == 0){
                        NOTICE("%lld done!", replies);
                    }
                    freeReplyObject(reply);
                }
            } while(reply);
        }

        if (mask & AE_WRITABLE) {
            if (obuf_len != 0) {
                ssize_t nwritten = write(fd,obuf+obuf_pos,obuf_len);

                if (nwritten == -1) {
                    if (errno != EAGAIN && errno != EINTR) {
                        ERROR("Error writing to the server: %s", strerror(errno));
                        exit(1);
                    } else {
                        nwritten = 0;
                    }
                }
                obuf_len -= nwritten;
                obuf_pos += nwritten;
            }
            /* If buffer is empty, load from file. */
            if (obuf_len == 0) {
                ssize_t nread = myread(fp, &obuf, &obuf_size);
                DEBUG("myread return %d: %s", nread, obuf);
                if (nread == 0) {
                    DEBUG("myread got 0 bytes");
                } else if (nread == -1) {
                    ERROR("Error on reading from input: %s", strerror(errno));
                    exit(1);
                } else {
                    obuf_len = nread;
                    obuf_pos = 0;
                    requests ++;//TODO +=?
                }
            }
        }
    }
    redisReaderFree(reader);
    exit(0);
}

static void pipeMode(void) {
    int fd = context->fd;
    long long errors = 0, replies = 0, obuf_len = 0, obuf_pos = 0;
    char ibuf[1024*16], obuf[1024*16]; /* Input and output buffers */
    char aneterr[ANET_ERR_LEN];
    redisReader *reader = redisReaderCreate();
    redisReply *reply;
    int eof = 0; /* True once we consumed all the standard input. */
    int done = 0;
    char magic[20]; /* Special reply we recognize. */
    time_t last_read_time = time(NULL);

    srand(time(NULL));

    /* Use non blocking I/O. */
    if (anetNonBlock(aneterr,fd) == ANET_ERR) {
        fprintf(stderr, "Can't set the socket in non blocking mode: %s\n",
            aneterr);
        exit(1);
    }

    /* Transfer raw protocol and read replies from the server at the same
     * time. */
    while(!done) {
        int mask = AE_READABLE;

        if (!eof || obuf_len != 0) mask |= AE_WRITABLE;
        mask = aeWait(fd,mask,1000);

        /* Handle the readable state: we can read replies from the server. */
        if (mask & AE_READABLE) {
            ssize_t nread;

            /* Read from socket and feed the hiredis reader. */
            do {
                nread = read(fd,ibuf,sizeof(ibuf));
                if (nread == -1 && errno != EAGAIN && errno != EINTR) {
                    fprintf(stderr, "Error reading from the server: %s\n",
                        strerror(errno));
                    exit(1);
                }
                if (nread > 0) {
                    redisReaderFeed(reader,ibuf,nread);
                    last_read_time = time(NULL);
                }
            } while(nread > 0);

            /* Consume replies. */
            do {
                if (redisReaderGetReply(reader,(void**)&reply) == REDIS_ERR) {
                    fprintf(stderr, "Error reading replies from server\n");
                    exit(1);
                }
                if (reply) {
                    if (reply->type == REDIS_REPLY_ERROR) {
                        fprintf(stderr,"%s\n", reply->str);
                        errors++;
                    } else if (eof && reply->type == REDIS_REPLY_STRING &&
                                      reply->len == 20) {
                        /* Check if this is the reply to our final ECHO
                         * command. If so everything was received
                         * from the server. */
                        if (memcmp(reply->str,magic,20) == 0) {
                            printf("Last reply received from server.\n");
                            done = 1;
                            replies--;
                        }
                    }
                    replies++;
                    freeReplyObject(reply);
                }
            } while(reply);
        }

        /* Handle the writable state: we can send protocol to the server. */
        if (mask & AE_WRITABLE) {
            while(1) {
                /* Transfer current buffer to server. */
                if (obuf_len != 0) {
                    ssize_t nwritten = write(fd,obuf+obuf_pos,obuf_len);

                    if (nwritten == -1) {
                        if (errno != EAGAIN && errno != EINTR) {
                            fprintf(stderr, "Error writing to the server: %s\n",
                                strerror(errno));
                            exit(1);
                        } else {
                            nwritten = 0;
                        }
                    }
                    obuf_len -= nwritten;
                    obuf_pos += nwritten;
                    if (obuf_len != 0) break; /* Can't accept more data. */
                }
                /* If buffer is empty, load from stdin. */
                if (obuf_len == 0 && !eof) {
                    ssize_t nread = read(STDIN_FILENO,obuf,sizeof(obuf));

                    if (nread == 0) {
                        /* The ECHO sequence starts with a "\r\n" so that if there
                         * is garbage in the protocol we read from stdin, the ECHO
                         * will likely still be properly formatted.
                         * CRLF is ignored by Redis, so it has no effects. */
                        char echo[] =
                        "\r\n*2\r\n$4\r\nECHO\r\n$20\r\n01234567890123456789\r\n";
                        int j;

                        eof = 1;
                        /* Everything transferred, so we queue a special
                         * ECHO command that we can match in the replies
                         * to make sure everything was read from the server. */
                        for (j = 0; j < 20; j++)
                            magic[j] = rand() & 0xff;
                        memcpy(echo+21,magic,20);
                        memcpy(obuf,echo,sizeof(echo)-1);
                        obuf_len = sizeof(echo)-1;
                        obuf_pos = 0;
                        printf("All data transferred. Waiting for the last reply...\n");
                    } else if (nread == -1) {
                        fprintf(stderr, "Error reading from stdin: %s\n",
                            strerror(errno));
                        exit(1);
                    } else {
                        obuf_len = nread;
                        obuf_pos = 0;
                    }
                }
                if (obuf_len == 0 && eof) break;
            }
        }

        /* Handle timeout, that is, we reached EOF, and we are not getting
         * replies from the server for a few seconds, nor the final ECHO is
         * received. */
        if (eof && config.pipe_timeout > 0 &&
            time(NULL)-last_read_time > config.pipe_timeout)
        {
            fprintf(stderr,"No replies for %d seconds: exiting.\n",
                config.pipe_timeout);
            errors++;
            break;
        }
    }
    redisReaderFree(reader);
    printf("errors: %lld, replies: %lld\n", errors, replies);
    if (errors)
        exit(1);
    else
        exit(0);
}

#define TYPE_STRING 0
#define TYPE_LIST   1
#define TYPE_SET    2
#define TYPE_HASH   3
#define TYPE_ZSET   4

static void findBigKeys(void) {
    unsigned long long biggest[5] = {0,0,0,0,0};
    unsigned long long samples = 0;
    redisReply *reply1, *reply2, *reply3 = NULL;
    char *sizecmd, *typename[] = {"string","list","set","hash","zset"};
    char *typeunit[] = {"bytes","items","members","fields","members"};
    int type;

    printf("\n# Press ctrl+c when you have had enough of it... :)\n");
    printf("# You can use -i 0.1 to sleep 0.1 sec every 100 sampled keys\n");
    printf("# in order to reduce server load (usually not needed).\n\n");
    while(1) {
        /* Sample with RANDOMKEY */
        reply1 = redisCommand(context,"RANDOMKEY");
        if (reply1 == NULL) {
            fprintf(stderr,"\nI/O error\n");
            exit(1);
        } else if (reply1->type == REDIS_REPLY_ERROR) {
            fprintf(stderr, "RANDOMKEY error: %s\n",
                reply1->str);
            exit(1);
        } else if (reply1->type == REDIS_REPLY_NIL) {
            fprintf(stderr, "It looks like the database is empty!\n");
            exit(1);
        }

        /* Get the key type */
        reply2 = redisCommand(context,"TYPE %s",reply1->str);
        assert(reply2 && reply2->type == REDIS_REPLY_STATUS);
        samples++;

        /* Get the key "size" */
        if (!strcmp(reply2->str,"string")) {
            sizecmd = "STRLEN";
            type = TYPE_STRING;
        } else if (!strcmp(reply2->str,"list")) {
            sizecmd = "LLEN";
            type = TYPE_LIST;
        } else if (!strcmp(reply2->str,"set")) {
            sizecmd = "SCARD";
            type = TYPE_SET;
        } else if (!strcmp(reply2->str,"hash")) {
            sizecmd = "HLEN";
            type = TYPE_HASH;
        } else if (!strcmp(reply2->str,"zset")) {
            sizecmd = "ZCARD";
            type = TYPE_ZSET;
        } else if (!strcmp(reply2->str,"none")) {
            freeReplyObject(reply1);
            freeReplyObject(reply2);
            continue;
        } else {
            fprintf(stderr, "Unknown key type '%s' for key '%s'\n",
                reply2->str, reply1->str);
            exit(1);
        }

        reply3 = redisCommand(context,"%s %s", sizecmd, reply1->str);
        if (reply3 && reply3->type == REDIS_REPLY_INTEGER) {
            if (biggest[type] < reply3->integer) {
                printf("Biggest %-6s found so far '%s' with %llu %s.\n",
                    typename[type], reply1->str,
                    (unsigned long long) reply3->integer,
                    typeunit[type]);
                biggest[type] = reply3->integer;
            }
        }

        if ((samples % 1000000) == 0)
            printf("(%llu keys sampled)\n", samples);

        if ((samples % 100) == 0 && config.interval)
            usleep(config.interval);

        freeReplyObject(reply1);
        freeReplyObject(reply2);
        if (reply3) freeReplyObject(reply3);
    }
}

/* Return the specified INFO field from the INFO command output "info".
 * A new buffer is allocated for the result, that needs to be free'd.
 * If the field is not found NULL is returned. */
static char *getInfoField(char *info, char *field) {
    char *p = strstr(info,field);
    char *n1, *n2;
    char *result;

    if (!p) return NULL;
    p += strlen(field)+1;
    n1 = strchr(p,'\r');
    n2 = strchr(p,',');
    if (n2 && n2 < n1) n1 = n2;
    result = malloc(sizeof(char)*(n1-p)+1);
    memcpy(result,p,(n1-p));
    result[n1-p] = '\0';
    return result;
}

/* Like the above function but automatically convert the result into
 * a long. On error (missing field) LONG_MIN is returned. */
static long getLongInfoField(char *info, char *field) {
    char *value = getInfoField(info,field);
    long l;

    if (!value) return LONG_MIN;
    l = strtol(value,NULL,10);
    free(value);
    return l;
}

/* Convert number of bytes into a human readable string of the form:
 * 100B, 2G, 100M, 4K, and so forth. */
void bytesToHuman(char *s, long long n) {
    double d;

    if (n < 0) {
        *s = '-';
        s++;
        n = -n;
    }
    if (n < 1024) {
        /* Bytes */
        sprintf(s,"%lluB",n);
        return;
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        sprintf(s,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        sprintf(s,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        sprintf(s,"%.2fG",d);
    }
}

static void statMode() {
    redisReply *reply;
    long aux, requests = 0;
    int i = 0;

    while(1) {
        char buf[64];
        int j;

        reply = reconnectingInfo();
        if (reply->type == REDIS_REPLY_ERROR) {
            printf("ERROR: %s\n", reply->str);
            exit(1);
        }

        if ((i++ % 20) == 0) {
            printf(
"------- data ------ --------------------- load -------------------- - child -\n"
"keys       mem      clients blocked requests            connections          \n");
        }

        /* Keys */
        aux = 0;
        for (j = 0; j < 20; j++) {
            long k;

            sprintf(buf,"db%d:keys",j);
            k = getLongInfoField(reply->str,buf);
            if (k == LONG_MIN) continue;
            aux += k;
        }
        sprintf(buf,"%ld",aux);
        printf("%-11s",buf);

        /* Used memory */
        aux = getLongInfoField(reply->str,"used_memory");
        bytesToHuman(buf,aux);
        printf("%-8s",buf);

        /* Clients */
        aux = getLongInfoField(reply->str,"connected_clients");
        sprintf(buf,"%ld",aux);
        printf(" %-8s",buf);

        /* Blocked (BLPOPPING) Clients */
        aux = getLongInfoField(reply->str,"blocked_clients");
        sprintf(buf,"%ld",aux);
        printf("%-8s",buf);

        /* Requets */
        aux = getLongInfoField(reply->str,"total_commands_processed");
        sprintf(buf,"%ld (+%ld)",aux,requests == 0 ? 0 : aux-requests);
        printf("%-19s",buf);
        requests = aux;

        /* Connections */
        aux = getLongInfoField(reply->str,"total_connections_received");
        sprintf(buf,"%ld",aux);
        printf(" %-12s",buf);

        /* Children */
        aux = getLongInfoField(reply->str,"bgsave_in_progress");
        aux |= getLongInfoField(reply->str,"aof_rewrite_in_progress") << 1;
        switch(aux) {
        case 0: break;
        case 1:
            printf("SAVE");
            break;
        case 2:
            printf("AOF");
            break;
        case 3:
            printf("SAVE+AOF");
            break;
        }

        printf("\n");
        freeReplyObject(reply);
        usleep(config.interval);
    }
}

int main(int argc, char **argv) {
    int firstarg;

    config.hostip = sdsnew("127.0.0.1");
    config.hostport = 6379;
    config.hostsocket = NULL;
    config.repeat = 1;
    config.interval = 0;
    config.dbnum = 0;
    config.interactive = 0;
    config.shutdown = 0;
    config.monitor_mode = 0;
    config.pubsub_mode = 0;
    config.latency_mode = 0;
    config.latency_history = 0;
    config.cluster_mode = 0;
    config.slave_mode = 0;
    config.getrdb_mode = 0;
    config.rdb_filename = NULL;
    config.pipe_mode = 0;
    config.pipe_timeout = REDIS_DEFAULT_PIPE_TIMEOUT;

    config.replay_mode = 0;
    config.replay_filename = NULL;
    config.replay_filter = NULL;
    config.replay_orig = NULL;
    config.replay_rewrite = NULL;

    config.bigkeys = 0;
    config.stdinarg = 0;
    config.auth = NULL;
    config.eval = NULL;
    if (!isatty(fileno(stdout)) && (getenv("FAKETTY") == NULL))
        config.output = OUTPUT_RAW;
    else
        config.output = OUTPUT_STANDARD;
    config.mb_delim = sdsnew("\n");
    cliInitHelp();

    firstarg = parseOptions(argc,argv);
    argc -= firstarg;
    argv += firstarg;

    /* Latency mode */
    if (config.latency_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        latencyMode();
    }

    /* Slave mode */
    if (config.slave_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        slaveMode();
    }

    /* Get RDB mode. */
    if (config.getrdb_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        getRDB();
    }

    /* Pipe mode */
    if (config.pipe_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        pipeMode();
    }

    /* Replay mode */
    if (config.replay_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        replayMode();
        /*safeReplayMode();*/
    }

    /* Find big keys */
    if (config.bigkeys) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        findBigKeys();
    }

    /* Stat mode */
    if (config.stat_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        if (config.interval == 0) config.interval = 1000000;
        statMode();
    }

    /* Start interactive mode when no command is provided */
    if (argc == 0 && !config.eval) {
        /* Note that in repl mode we don't abort on connection error.
         * A new attempt will be performed for every command send. */
        cliConnect(0);
        repl();
    }

    /* Otherwise, we have some arguments to execute */
    if (cliConnect(0) != REDIS_OK) exit(1);
    if (config.eval) {
        return evalMode(argc,argv);
    } else {
        return noninteractive(argc,convertToSds(argc,argv));
    }
}
