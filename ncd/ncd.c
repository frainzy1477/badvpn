/**
 * @file ncd.c
 * @author Ambroz Bizjak <ambrop7@gmail.com>
 * 
 * @section LICENSE
 * 
 * This file is part of BadVPN.
 * 
 * BadVPN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 * 
 * BadVPN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include <misc/version.h>
#include <misc/loggers_string.h>
#include <misc/loglevel.h>
#include <misc/offset.h>
#include <misc/read_file.h>
#include <misc/balloc.h>
#include <structure/LinkedList2.h>
#include <system/BLog.h>
#include <system/BReactor.h>
#include <system/BProcess.h>
#include <system/BSignal.h>
#include <system/BSocket.h>
#include <ncdconfig/NCDConfigParser.h>
#include <ncd/NCDModule.h>
#include <ncd/modules/modules.h>

#ifndef BADVPN_USE_WINAPI
#include <system/BLog_syslog.h>
#endif

#include <ncd/ncd.h>

#include <generated/blog_channel_ncd.h>

#define LOGGER_STDOUT 1
#define LOGGER_SYSLOG 2

#define SSTATE_CHILD 1
#define SSTATE_ADULT 2
#define SSTATE_DYING 3
#define SSTATE_FORGOTTEN 4

struct statement {
    const struct NCDModule *module;
    struct argument_elem *first_arg;
    char *name;
};

struct argument_elem {
    int is_var;
    union {
        struct {
            char *modname;
            char *varname;
        } var;
        NCDValue val;
    };
    struct argument_elem *next_arg;
};

struct process {
    char *name;
    size_t num_statements;
    struct process_statement *statements;
    size_t ap;
    size_t fp;
    BTimer wait_timer;
    LinkedList2Node list_node; // node in processes
};

struct process_statement {
    struct process *p;
    size_t i;
    struct statement s;
    int state;
    int have_error;
    btime_t error_until;
    NCDModuleInst inst;
    NCDValue inst_args;
    char logprefix[50];
};

// command-line options
struct {
    int help;
    int version;
    int logger;
    #ifndef BADVPN_USE_WINAPI
    char *logger_syslog_facility;
    char *logger_syslog_ident;
    #endif
    int loglevel;
    int loglevels[BLOG_NUM_CHANNELS];
    char *config_file;
} options;

// reactor
BReactor ss;

// are we terminating
int terminating;

// process manager
BProcessManager manager;

// configuration
struct NCDConfig_interfaces *configuration;

// processes
LinkedList2 processes;

static void terminate (void);
static void print_help (const char *name);
static void print_version (void);
static int parse_arguments (int argc, char *argv[]);
static void signal_handler (void *unused);
static int statement_init (struct statement *s, struct NCDConfig_statements *conf);
static void statement_free (struct statement *s);
static void statement_free_args (struct statement *s);
static int process_new (struct NCDConfig_interfaces *conf);
static void process_free (struct process *p);
static void process_free_statements (struct process *p);
static void process_assert_pointers (struct process *p);
static void process_assert (struct process *p);
static void process_log (struct process *p, int level, const char *fmt, ...);
static void process_work (struct process *p);
static void process_fight (struct process *p);
static void process_advance (struct process *p);
static void process_wait (struct process *p);
static void process_wait_timer_handler (struct process *p);
static void process_retreat (struct process *p);
static void process_statement_log (struct process_statement *ps, int level, const char *fmt, ...);
static void process_statement_set_error (struct process_statement *ps);
static void process_statement_instance_handler_event (struct process_statement *ps, int event);
static void process_statement_instance_handler_died (struct process_statement *ps, int is_error);

int main (int argc, char **argv)
{
    if (argc <= 0) {
        return 1;
    }
    
    // parse command-line arguments
    if (!parse_arguments(argc, argv)) {
        fprintf(stderr, "Failed to parse arguments\n");
        print_help(argv[0]);
        goto fail0;
    }
    
    // handle --help and --version
    if (options.help) {
        print_version();
        print_help(argv[0]);
        return 0;
    }
    if (options.version) {
        print_version();
        return 0;
    }
    
    // initialize logger
    switch (options.logger) {
        case LOGGER_STDOUT:
            BLog_InitStdout();
            break;
        #ifndef BADVPN_USE_WINAPI
        case LOGGER_SYSLOG:
            if (!BLog_InitSyslog(options.logger_syslog_ident, options.logger_syslog_facility)) {
                fprintf(stderr, "Failed to initialize syslog logger\n");
                goto fail0;
            }
            break;
        #endif
        default:
            ASSERT(0);
    }
    
    // configure logger channels
    for (int i = 0; i < BLOG_NUM_CHANNELS; i++) {
        if (options.loglevels[i] >= 0) {
            BLog_SetChannelLoglevel(i, options.loglevels[i]);
        }
        else if (options.loglevel >= 0) {
            BLog_SetChannelLoglevel(i, options.loglevel);
        }
    }
    
    BLog(BLOG_NOTICE, "initializing "GLOBAL_PRODUCT_NAME" "PROGRAM_NAME" "GLOBAL_VERSION);
    
    // initialize sockets
    if (BSocket_GlobalInit() < 0) {
        BLog(BLOG_ERROR, "BSocket_GlobalInit failed");
        goto fail1;
    }
    
    // init time
    BTime_Init();
    
    // init reactor
    if (!BReactor_Init(&ss)) {
        BLog(BLOG_ERROR, "BReactor_Init failed");
        goto fail1;
    }
    
    // set not terminating
    terminating = 0;
    
    // init process manager
    if (!BProcessManager_Init(&manager, &ss)) {
        BLog(BLOG_ERROR, "BProcessManager_Init failed");
        goto fail1a;
    }
    
    // setup signal handler
    if (!BSignal_Init(&ss, signal_handler, NULL)) {
        BLog(BLOG_ERROR, "BSignal_Init failed");
        goto fail2;
    }
    
    // read config file
    uint8_t *file;
    size_t file_len;
    if (!read_file(options.config_file, &file, &file_len)) {
        BLog(BLOG_ERROR, "failed to read config file");
        goto fail3;
    }
    
    // parse config file
    if (!NCDConfigParser_Parse((char *)file, file_len, &configuration)) {
        BLog(BLOG_ERROR, "NCDConfigParser_Parse failed");
        free(file);
        goto fail3;
    }
    
    // fee config file memory
    free(file);
    
    // init modules
    for (const struct NCDModule **m = ncd_modules; *m; m++) {
        if ((*m)->func_globalinit && !(*m)->func_globalinit()) {
            BLog(BLOG_ERROR, "globalinit failed for module %s", (*m)->type);
            goto fail5;
        }
    }
    
    // init processes list
    LinkedList2_Init(&processes);
    
    // init processes
    struct NCDConfig_interfaces *pc = configuration;
    while (pc) {
        process_new(pc);
        pc = pc->next;
    }
    
    // enter event loop
    BLog(BLOG_NOTICE, "entering event loop");
    BReactor_Exec(&ss);
    
    // free processes
    LinkedList2Node *n;
    while (n = LinkedList2_GetFirst(&processes)) {
        struct process *p = UPPER_OBJECT(n, struct process, list_node);
        process_free(p);
    }
fail5:
    // free configuration
    NCDConfig_free_interfaces(configuration);
fail3:
    // remove signal handler
    BSignal_Finish();
fail2:
    // free process manager
    BProcessManager_Free(&manager);
fail1a:
    // free reactor
    BReactor_Free(&ss);
fail1:
    // free logger
    BLog(BLOG_NOTICE, "exiting");
    BLog_Free();
fail0:
    // finish objects
    DebugObjectGlobal_Finish();
    
    return 1;
}

void terminate (void)
{
    ASSERT(!terminating)
    
    BLog(BLOG_NOTICE, "tearing down");
    
    terminating = 1;
    
    if (LinkedList2_IsEmpty(&processes)) {
        BReactor_Quit(&ss, 1);
        return;
    }
    
    LinkedList2Iterator it;
    LinkedList2Iterator_InitForward(&it, &processes);
    LinkedList2Node *n;
    while (n = LinkedList2Iterator_Next(&it)) {
        struct process *p = UPPER_OBJECT(n, struct process, list_node);
        process_work(p);
    }
}

void print_help (const char *name)
{
    printf(
        "Usage:\n"
        "    %s\n"
        "        [--help]\n"
        "        [--version]\n"
        "        [--logger <"LOGGERS_STRING">]\n"
        #ifndef BADVPN_USE_WINAPI
        "        (logger=syslog?\n"
        "            [--syslog-facility <string>]\n"
        "            [--syslog-ident <string>]\n"
        "        )\n"
        #endif
        "        [--loglevel <0-5/none/error/warning/notice/info/debug>]\n"
        "        [--channel-loglevel <channel-name> <0-5/none/error/warning/notice/info/debug>] ...\n"
        "        --config-file <file>\n",
        name
    );
}

void print_version (void)
{
    printf(GLOBAL_PRODUCT_NAME" "PROGRAM_NAME" "GLOBAL_VERSION"\n"GLOBAL_COPYRIGHT_NOTICE"\n");
}

int parse_arguments (int argc, char *argv[])
{
    if (argc <= 0) {
        return 0;
    }
    
    options.help = 0;
    options.version = 0;
    options.logger = LOGGER_STDOUT;
    #ifndef BADVPN_USE_WINAPI
    options.logger_syslog_facility = "daemon";
    options.logger_syslog_ident = argv[0];
    #endif
    options.loglevel = -1;
    for (int i = 0; i < BLOG_NUM_CHANNELS; i++) {
        options.loglevels[i] = -1;
    }
    options.config_file = NULL;
    
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (!strcmp(arg, "--help")) {
            options.help = 1;
        }
        else if (!strcmp(arg, "--version")) {
            options.version = 1;
        }
        else if (!strcmp(arg, "--logger")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            char *arg2 = argv[i + 1];
            if (!strcmp(arg2, "stdout")) {
                options.logger = LOGGER_STDOUT;
            }
            #ifndef BADVPN_USE_WINAPI
            else if (!strcmp(arg2, "syslog")) {
                options.logger = LOGGER_SYSLOG;
            }
            #endif
            else {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        #ifndef BADVPN_USE_WINAPI
        else if (!strcmp(arg, "--syslog-facility")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.logger_syslog_facility = argv[i + 1];
            i++;
        }
        else if (!strcmp(arg, "--syslog-ident")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.logger_syslog_ident = argv[i + 1];
            i++;
        }
        #endif
        else if (!strcmp(arg, "--loglevel")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if ((options.loglevel = parse_loglevel(argv[i + 1])) < 0) {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--channel-loglevel")) {
            if (2 >= argc - i) {
                fprintf(stderr, "%s: requires two arguments\n", arg);
                return 0;
            }
            int channel = BLogGlobal_GetChannelByName(argv[i + 1]);
            if (channel < 0) {
                fprintf(stderr, "%s: wrong channel argument\n", arg);
                return 0;
            }
            int loglevel = parse_loglevel(argv[i + 2]);
            if (loglevel < 0) {
                fprintf(stderr, "%s: wrong loglevel argument\n", arg);
                return 0;
            }
            options.loglevels[channel] = loglevel;
            i += 2;
        }
        else if (!strcmp(arg, "--config-file")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.config_file = argv[i + 1];
            i++;
        }
        else {
            fprintf(stderr, "unknown option: %s\n", arg);
            return 0;
        }
    }
    
    if (options.help || options.version) {
        return 1;
    }
    
    if (!options.config_file) {
        fprintf(stderr, "--config-file is required\n");
        return 0;
    }
    
    return 1;
}

void signal_handler (void *unused)
{
    BLog(BLOG_NOTICE, "termination requested");
    
    if (!terminating) {
        terminate();
    }
}

int statement_init (struct statement *s, struct NCDConfig_statements *conf)
{
    // find module
    char *module_name = NCDConfig_concat_strings(conf->names);
    if (!module_name) {
        goto fail0;
    }
    const struct NCDModule **m;
    for (m = ncd_modules; *m; m++) {
        if (!strcmp(module_name, (*m)->type)) {
            break;
        }
    }
    if (!*m) {
        BLog(BLOG_ERROR, "no module for statement %s", module_name);
        free(module_name);
        goto fail0;
    }
    free(module_name);
    
    // set module
    s->module = *m;
    
    // init arguments
    s->first_arg = NULL;
    struct argument_elem **prevptr = &s->first_arg;
    struct NCDConfig_arguments *arg = conf->args;
    while (arg) {
        struct argument_elem *e = malloc(sizeof(*e));
        if (!e) {
            goto fail1;
        }
        
        switch (arg->type) {
            case NCDCONFIG_ARG_STRING: {
                if (!NCDValue_InitString(&e->val, arg->string)) {
                    free(e);
                    goto fail1;
                }
                
                e->is_var = 0;
            } break;
            
            case NCDCONFIG_ARG_VAR: {
                if (!(e->var.modname = strdup(arg->var->value))) {
                    free(e);
                    goto fail1;
                }
                
                if (!arg->var->next) {
                    e->var.varname = NULL;
                } else {
                    if (!(e->var.varname = NCDConfig_concat_strings(arg->var->next))) {
                        free(e->var.modname);
                        free(e);
                        goto fail1;
                    }
                }
                
                e->is_var = 1;
            } break;
            
            default:
                ASSERT(0);
        }
        
        *prevptr = e;
        e->next_arg = NULL;
        prevptr = &e->next_arg;
        
        arg = arg->next;
    }
    
    // init name
    if (!conf->name) {
        s->name = NULL;
    } else {
        if (!(s->name = strdup(conf->name))) {
            goto fail1;
        }
    }
    
    return 1;
    
fail1:
    statement_free_args(s);
fail0:
    return 0;
}

void statement_free (struct statement *s)
{
    // free name
    free(s->name);
    
    // free arguments
    statement_free_args(s);
}

void statement_free_args (struct statement *s)
{
    struct argument_elem *e = s->first_arg;
    while (e) {
        if (e->is_var) {
            free(e->var.modname);
            free(e->var.varname);
        } else {
            NCDValue_Free(&e->val);
        }
        struct argument_elem *n = e->next_arg;
        free(e);
        e = n;
    }
}

int process_new (struct NCDConfig_interfaces *conf)
{
    // allocate strucure
    struct process *p = malloc(sizeof(*p));
    if (!p) {
        goto fail0;
    }
    
    // init name
    if (!(p->name = strdup(conf->name))) {
        goto fail1;
    }
    
    // count statements
    size_t num_st = 0;
    struct NCDConfig_statements *st = conf->statements;
    while (st) {
        num_st++;
        st = st->next;
    }
    
    // statements array
    if (!(p->statements = BAllocArray(num_st, sizeof(p->statements[0])))) {
        goto fail2;
    }
    p->num_statements = 0;
    
    // init statements
    st = conf->statements;
    while (st) {
        struct process_statement *ps = &p->statements[p->num_statements];
        
        ps->p = p;
        ps->i = p->num_statements;
        
        if (!statement_init(&ps->s, st)) {
            goto fail3;
        }
        
        ps->state = SSTATE_FORGOTTEN;
        
        ps->have_error = 0;
        
        p->num_statements++;
        
        st = st->next;
    }
    
    // set AP=0
    p->ap = 0;
    
    // set FP=0
    p->fp = 0;
    
    // init timer
    BTimer_Init(&p->wait_timer, RETRY_TIME, (BTimer_handler)process_wait_timer_handler, p);
    
    // insert to processes list
    LinkedList2_Append(&processes, &p->list_node);
    
    process_work(p);
    
    return 1;
    
fail3:
    process_free_statements(p);
fail2:
    free(p->name);
fail1:
    free(p);
fail0:
    return 0;
}

void process_free (struct process *p)
{
    ASSERT(p->ap == 0)
    ASSERT(p->fp == 0)
    
    // remove from processes list
    LinkedList2_Remove(&processes, &p->list_node);
    
    // free timer
    BReactor_RemoveTimer(&ss, &p->wait_timer);
    
    // free statements
    process_free_statements(p);
    
    // free name
    free(p->name);
    
    // free strucure
    free(p);
}

void process_free_statements (struct process *p)
{
    // free statments
    for (size_t i = 0; i < p->num_statements; i++) {
        struct process_statement *ps = &p->statements[i];
        statement_free(&ps->s);
    }
    
    // free stataments array
    free(p->statements);
}

void process_assert_pointers (struct process *p)
{
    ASSERT(p->ap <= p->num_statements)
    ASSERT(p->fp >= p->ap)
    ASSERT(p->fp <= p->num_statements)
    
    // check AP
    for (size_t i = 0; i < p->ap; i++) {
        if (i == p->ap - 1) {
            ASSERT(p->statements[i].state == SSTATE_ADULT || p->statements[i].state == SSTATE_CHILD)
        } else {
            ASSERT(p->statements[i].state == SSTATE_ADULT)
        }
    }
    
    // check FP
    size_t fp = p->num_statements;
    while (fp > 0 && p->statements[fp - 1].state == SSTATE_FORGOTTEN) {
        fp--;
    }
    ASSERT(p->fp == fp)
}

void process_assert (struct process *p)
{
    process_assert_pointers(p);
}

void process_log (struct process *p, int level, const char *fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    BLog_Append("process %s: ", p->name);
    BLog_LogToChannelVarArg(BLOG_CURRENT_CHANNEL, level, fmt, vl);
    va_end(vl);
}

void process_work (struct process *p)
{
    process_assert_pointers(p);
    
    // stop timer in case we were WAITING
    BReactor_RemoveTimer(&ss, &p->wait_timer);
    
    if (terminating) {
        process_retreat(p);
        return;
    }
    
    process_fight(p);
}

void process_fight (struct process *p)
{
    if (p->ap == p->fp) {
        if (!(p->ap > 0 && p->statements[p->ap - 1].state == SSTATE_CHILD)) {
            // advance
            process_advance(p);
        }
        
        return;
    }
    
    // order the last living statement to die, if needed
    struct process_statement *ps = &p->statements[p->fp - 1];
    if (ps->state != SSTATE_DYING) {
        process_statement_log(ps, BLOG_INFO, "killing");
        
        // order it to die
        NCDModuleInst_Die(&ps->inst);
        
        // set statement state DYING
        ps->state = SSTATE_DYING;
    }
    
    process_assert(p);
}

void process_advance (struct process *p)
{
    ASSERT(p->ap == p->fp)
    ASSERT(!(p->ap > 0) || p->statements[p->ap - 1].state == SSTATE_ADULT)
    
    if (p->ap == p->num_statements) {
        process_log(p, BLOG_INFO, "victory");
        
        process_assert(p);
        return;
    }
    
    struct process_statement *ps = &p->statements[p->ap];
    
    // check if we need to wait
    if (ps->have_error && ps->error_until > btime_gettime()) {
        process_wait(p);
        return;
    }
    
    process_statement_log(ps, BLOG_INFO, "initializing");
    
    // init arguments list
    NCDValue_InitList(&ps->inst_args);
    
    // build arguments
    struct argument_elem *arg = ps->s.first_arg;
    while (arg) {
        NCDValue v;
        
        if (arg->is_var) {
            // find referred-to statement
            struct process_statement *rps;
            size_t i;
            for (i = p->ap; i > 0; i--) {
                rps = &p->statements[i - 1];
                if (rps->s.name && !strcmp(rps->s.name, arg->var.modname)) {
                    break;
                }
            }
            if (i == 0) {
                process_statement_log(ps, BLOG_ERROR, "unknown statement name in variable: %s.%s", arg->var.modname, arg->var.varname);
                goto fail1;
            }
            ASSERT(rps->state == SSTATE_ADULT)
            
            // resolve variable
            const char *real_varname = (arg->var.varname ? arg->var.varname : "");
            if (!NCDModuleInst_GetVar(&rps->inst, real_varname, &v)) {
                process_statement_log(ps, BLOG_ERROR, "failed to resolve variable: %s.%s", arg->var.modname, real_varname);
                goto fail1;
            }
        } else {
            if (!NCDValue_InitCopy(&v, &arg->val)) {
                process_statement_log(ps, BLOG_ERROR, "NCDValue_InitCopy failed");
                goto fail1;
            }
        }
        
        // move to list
        if (!NCDValue_ListAppend(&ps->inst_args, v)) {
            process_statement_log(ps, BLOG_ERROR, "NCDValue_ListAppend failed");
            NCDValue_Free(&v);
            goto fail1;
        }
        
        arg = arg->next_arg;
    }
    
    // generate log prefix
    snprintf(ps->logprefix, sizeof(ps->logprefix), "process %s: statement %zu: module: ", p->name, ps->i);
    
    // initialize module instance
    if (!NCDModuleInst_Init(
        &ps->inst, ps->s.name, ps->s.module, &ps->inst_args, ps->logprefix, &ss, &manager,
        (NCDModule_handler_event)process_statement_instance_handler_event, (NCDModule_handler_died)process_statement_instance_handler_died, ps
    )) {
        process_statement_log(ps, BLOG_ERROR, "failed to initialize");
        goto fail1;
    }
    
    // set statement state CHILD
    ps->state = SSTATE_CHILD;
    
    // increment AP
    p->ap++;
    
    // increment FP
    p->fp++;
    
    process_assert(p);
    
    return;
    
fail1:
    NCDValue_Free(&ps->inst_args);
    process_statement_set_error(ps);
    process_wait(p);
}

void process_wait (struct process *p)
{
    ASSERT(p->ap == p->fp)
    ASSERT(!(p->ap > 0) || p->statements[p->ap - 1].state == SSTATE_ADULT)
    ASSERT(p->ap < p->num_statements)
    ASSERT(p->statements[p->ap].have_error)
    
    process_statement_log(&p->statements[p->ap], BLOG_INFO, "waiting after error");
    
    // set timer
    BReactor_SetTimerAbsolute(&ss, &p->wait_timer, p->statements[p->ap].error_until);
    
    process_assert(p);
}

void process_wait_timer_handler (struct process *p)
{
    ASSERT(p->ap == p->fp)
    ASSERT(!(p->ap > 0) || p->statements[p->ap - 1].state == SSTATE_ADULT)
    ASSERT(p->ap < p->num_statements)
    ASSERT(p->statements[p->ap].have_error)
    
    process_log(p, BLOG_INFO, "retrying");
    
    // clear error
    p->statements[p->ap].have_error = 0;
    
    process_advance(p);
}

void process_retreat (struct process *p)
{
    if (p->fp == 0) {
        // finished retreating
        process_free(p);
        
        // if there are no more processes, exit program
        if (LinkedList2_IsEmpty(&processes)) {
            BReactor_Quit(&ss, 1);
        }
        
        return;
    }
    
    // order the last living statement to die, if needed
    struct process_statement *ps = &p->statements[p->fp - 1];
    if (ps->state != SSTATE_DYING) {
        process_statement_log(ps, BLOG_INFO, "killing");
        
        // order it to die
        NCDModuleInst_Die(&ps->inst);
        
        // set statement state DYING
        ps->state = SSTATE_DYING;
        
        // update AP
        if (p->ap > ps->i) {
            p->ap = ps->i;
        }
    }
    
    process_assert(p);
}

void process_statement_log (struct process_statement *ps, int level, const char *fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    BLog_Append("process %s: statement %zu: ", ps->p->name, ps->i);
    BLog_LogToChannelVarArg(BLOG_CURRENT_CHANNEL, level, fmt, vl);
    va_end(vl);
}

void process_statement_set_error (struct process_statement *ps)
{
    ASSERT(ps->state == SSTATE_FORGOTTEN)
    
    ps->have_error = 1;
    ps->error_until = btime_gettime() + RETRY_TIME;
}

void process_statement_instance_handler_event (struct process_statement *ps, int event)
{
    ASSERT(ps->state == SSTATE_CHILD || ps->state == SSTATE_ADULT)
    
    struct process *p = ps->p;
    
    switch (event) {
        case NCDMODULE_EVENT_UP: {
            ASSERT(ps->state == SSTATE_CHILD)
            
            process_statement_log(ps, BLOG_INFO, "up");
            
            // set state ADULT
            ps->state = SSTATE_ADULT;
        } break;
        
        case NCDMODULE_EVENT_DOWN: {
            ASSERT(ps->state == SSTATE_ADULT)
            
            process_statement_log(ps, BLOG_INFO, "down");
            
            // set state CHILD
            ps->state = SSTATE_CHILD;
            
            // update AP
            if (p->ap > ps->i + 1) {
                p->ap = ps->i + 1;
            }
        } break;
        
        case NCDMODULE_EVENT_DYING: {
            ASSERT(ps->state == SSTATE_CHILD || ps->state == SSTATE_ADULT)
            
            process_statement_log(ps, BLOG_INFO, "dying");
            
            // set state DYING
            ps->state = SSTATE_DYING;
            
            // update AP
            if (p->ap > ps->i) {
                p->ap = ps->i;
            }
        } break;
    }
    
    process_work(p);
    return;
}

void process_statement_instance_handler_died (struct process_statement *ps, int is_error)
{
    ASSERT(ps->state == SSTATE_CHILD || ps->state == SSTATE_ADULT || ps->state == SSTATE_DYING)
    
    struct process *p = ps->p;
    
    // free instance
    NCDModuleInst_Free(&ps->inst);
    
    // free instance arguments
    NCDValue_Free(&ps->inst_args);
    
    // set state FORGOTTEN
    ps->state = SSTATE_FORGOTTEN;
    
    // set error
    if (is_error) {
        process_statement_set_error(ps);
    } else {
        ps->have_error = 0;
    }
    
    // update AP
    if (p->ap > ps->i) {
        p->ap = ps->i;
    }
    
    // update FP
    while (p->fp > 0 && p->statements[p->fp - 1].state == SSTATE_FORGOTTEN) {
        p->fp--;
    }
    
    process_statement_log(ps, BLOG_INFO, "died");
    
    if (is_error) {
        process_statement_log(ps, BLOG_ERROR, "with error");
    }
    
    process_work(p);
    return;
}