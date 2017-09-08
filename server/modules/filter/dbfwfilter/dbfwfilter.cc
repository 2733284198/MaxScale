/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2020-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file dbfwfilter.c
 * @author Markus Mäkelä
 * @date 13.2.2015
 * @version 1.0.0
 * @section secDesc Firewall Filter
 *
 * A filter that acts as a firewall, denying queries that do not meet a set of rules.
 *
 * Filter configuration parameters:
 *@code{.unparsed}
 *      rules=<path to file>            Location of the rule file
 *@endcode
 * Rules are defined in a separate rule file that lists all the rules and the users to whom the
 * rules are applied.
 * Rules follow a simple syntax that denies the queries that meet the requirements of the rules.
 * For example, to define a rule denying users from accessing the column 'salary' between
 * the times 15:00 and 17:00, the following rule is to be configured into the configuration file:
 *@code{.unparsed}
 *      rule block_salary deny columns salary at_times 15:00:00-17:00:00
 *@endcode
 * The users are matched by username and network address. Wildcard values can be provided by
 * using the '%' character.
 * For example, to apply this rule to users John, connecting from any address
 * that starts with the octets 198.168.%, and Jane, connecting from the address 192.168.0.1:
 *@code{.unparsed}
 *      users John@192.168.% Jane@192.168.0.1 match any rules block_salary
 *@endcode
 *
 * The 'match' keyword controls the way rules are matched. If it is set to
 * 'any' the first active rule that is triggered will cause the query to be denied.
 * If it is set to 'all' all the active rules need to match before the query is denied.
 *
 * @subsection secRule Rule syntax
 * This is the syntax used when defining rules.
 *@code{.unparsed}
 * rule NAME deny [wildcard | columns VALUE ... | regex REGEX |
 *           limit_queries COUNT TIMEPERIOD HOLDOFF | no_where_clause] [at_times VALUE...]
 *           [on_queries [select|update|insert|delete]]
 *@endcode
 * @subsection secUser User syntax
 * This is the syntax used when linking users to rules. It takes one or more
 * combinations of username and network, either the value any or all,
 * depending on how you want to match the rules, and one or more rule names.
 *@code{.unparsed}
 * users NAME ... match [any|all|strict_all] rules RULE ...
 *@endcode
 */

#include "dbfwfilter.hh"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <maxscale/atomic.h>
#include <maxscale/modulecmd.h>
#include <maxscale/modutil.h>
#include <maxscale/log_manager.h>
#include <maxscale/protocol/mysql.h>
#include <maxscale/platform.h>
#include <maxscale/thread.h>
#include <maxscale/pcre2.h>
#include <maxscale/alloc.h>

#include "rules.hh"
#include "users.hh"

MXS_BEGIN_DECLS
#include "ruleparser.yy.h"
#include "lex.yy.h"

/** Older versions of Bison don't include the parsing function in the header */
#ifndef dbfw_yyparse
int dbfw_yyparse(void*);
#endif
MXS_END_DECLS

/*
 * The filter entry points
 */
static MXS_FILTER *createInstance(const char *name, char **options, MXS_CONFIG_PARAMETER *);
static MXS_FILTER_SESSION *newSession(MXS_FILTER *instance, MXS_SESSION *session);
static void closeSession(MXS_FILTER *instance, MXS_FILTER_SESSION *session);
static void freeSession(MXS_FILTER *instance, MXS_FILTER_SESSION *session);
static void setDownstream(MXS_FILTER *instance, MXS_FILTER_SESSION *fsession, MXS_DOWNSTREAM *downstream);
static int routeQuery(MXS_FILTER *instance, MXS_FILTER_SESSION *fsession, GWBUF *queue);
static void diagnostic(MXS_FILTER *instance, MXS_FILTER_SESSION *fsession, DCB *dcb);
static json_t* diagnostic_json(const MXS_FILTER *instance, const MXS_FILTER_SESSION *fsession);
static uint64_t getCapabilities(MXS_FILTER* instance);

static const char* rule_names[] =
{
    "UNDEFINED",
    "COLUMN",
    "FUNCTION",
    "THROTTLE",
    "PERMISSION",
    "WILDCARD",
    "REGEX",
    "CLAUSE"
};

const int rule_names_len = sizeof(rule_names) / sizeof(char**);

/** The rules and users for each thread */
thread_local struct
{
    int        rule_version;
    RuleList   rules;
    UserMap    users;
} this_thread;

bool parse_at_times(const char** tok, char** saveptr, Rule* ruledef);
bool parse_limit_queries(FW_INSTANCE* instance, Rule* ruledef, const char* rule, char** saveptr);
static void rule_free_all(Rule* rule);
static bool process_rule_file(const char* filename, RuleList* rules, UserMap* users);
bool replace_rules(FW_INSTANCE* instance);

static inline bool query_is_sql(GWBUF* query)
{
    return modutil_is_SQL(query) || modutil_is_SQL_prepare(query);
}

static void print_rule(Rule *rules, char *dest)
{
    int type = 0;

    if ((int)rules->type > 0 && (int)rules->type < rule_names_len)
    {
        type = (int)rules->type;
    }

    sprintf(dest, "%s, %s, %d",
            rules->name.c_str(),
            rule_names[type],
            rules->times_matched);
}

static json_t* rule_to_json(const SRule& rule)
{
    int type = 0;

    if ((int)rule->type > 0 && (int)rule->type < rule_names_len)
    {
        type = (int)rule->type;
    }

    json_t* rval = json_object();

    json_object_set_new(rval, "name", json_string(rule->name.c_str()));
    json_object_set_new(rval, "type", json_string(rule_names[type]));
    json_object_set_new(rval, "times_matched", json_integer(rule->times_matched));

    return rval;
}

static json_t* rules_to_json(const RuleList& rules)
{
    json_t* rval = json_array();

    for (RuleList::const_iterator it = this_thread.rules.begin(); it != this_thread.rules.end(); it++)
    {
        const SRule& rule = *it;
        json_array_append_new(rval, rule_to_json(rule));
    }

    return rval;
}

/**
 * Push a string onto a string stack
 * @param head Head of the stack
 * @param value value to add
 * @return New top of the stack or NULL if memory allocation fails
 */
static STRLINK* strlink_push(STRLINK* head, const char* value)
{
    STRLINK* link = (STRLINK*)MXS_MALLOC(sizeof(STRLINK));

    if (link && (link->value = MXS_STRDUP(value)))
    {
        link->next = head;
    }
    else
    {
        MXS_FREE(link);
        link = NULL;
    }
    return link;
}

/**
 * Pop a string off of a string stack
 * @param head Head of the stack
 * @return New head of the stack or NULL if stack is empty
 */
static STRLINK* strlink_pop(STRLINK* head)
{
    if (head)
    {
        STRLINK* next = head->next;
        MXS_FREE(head->value);
        MXS_FREE(head);
        return next;
    }
    return NULL;
}

/**
 * Free a string stack
 * @param head Head of the stack
 */
static void strlink_free(STRLINK* head)
{
    while (head)
    {
        STRLINK* tmp = head;
        head = head->next;
        MXS_FREE(tmp->value);
        MXS_FREE(tmp);
    }
}

/**
 * Clone a string stack. This function reverses the order of the stack.
 * @param head Head of the stack to be cloned
 * @return Clone of the head or NULL if memory allocation failed
 */
static STRLINK* strlink_reverse_clone(STRLINK* head)
{
    STRLINK* clone = NULL;
    while (head)
    {
        STRLINK *tmp = strlink_push(clone, head->value);
        if (tmp)
        {
            clone = tmp;
        }
        else
        {
            strlink_free(clone);
            clone = NULL;
            break;
        }
        head = head->next;
    }
    return clone;
}

/**
 * Parses a string that contains an IP address and converts the last octet to '%'.
 * This modifies the string passed as the parameter.
 * @param str String to parse
 * @return Pointer to modified string or NULL if an error occurred or the string can't
 *         be made any less specific
 */
char* next_ip_class(char* str)
{
    assert(str != NULL);

    /**The least specific form is reached*/
    if (*str == '%')
    {
        return NULL;
    }

    char* ptr = strchr(str, '\0');

    if (ptr == NULL)
    {
        return NULL;
    }

    while (ptr > str)
    {
        ptr--;
        if (*ptr == '.' && *(ptr + 1) != '%')
        {
            break;
        }
    }

    if (ptr == str)
    {
        *ptr++ = '%';
        *ptr = '\0';
        return str;
    }

    *++ptr = '%';
    *++ptr = '\0';


    return str;
}

/**
 * Parses the string for the types of queries this rule should be applied to.
 * @param str String to parse
 * @param rule Pointer to a rule
 * @return True if the string was parses successfully, false if an error occurred
 */
bool parse_querytypes(const char* str, SRule rule)
{
    char buffer[512];
    bool done = false;
    rule->on_queries = 0;
    const char *ptr = str;
    char *dest = buffer;

    while (ptr - str < 512)
    {
        if (*ptr == '|' || *ptr == ' ' || (done = *ptr == '\0'))
        {
            *dest = '\0';
            if (strcmp(buffer, "select") == 0)
            {
                rule->on_queries |= FW_OP_SELECT;
            }
            else if (strcmp(buffer, "insert") == 0)
            {
                rule->on_queries |= FW_OP_INSERT;
            }
            else if (strcmp(buffer, "update") == 0)
            {
                rule->on_queries |= FW_OP_UPDATE;
            }
            else if (strcmp(buffer, "delete") == 0)
            {
                rule->on_queries |= FW_OP_DELETE;
            }
            else if (strcmp(buffer, "use") == 0)
            {
                rule->on_queries |= FW_OP_CHANGE_DB;
            }
            else if (strcmp(buffer, "grant") == 0)
            {
                rule->on_queries |= FW_OP_GRANT;
            }
            else if (strcmp(buffer, "revoke") == 0)
            {
                rule->on_queries |= FW_OP_REVOKE;
            }
            else if (strcmp(buffer, "drop") == 0)
            {
                rule->on_queries |= FW_OP_DROP;
            }
            else if (strcmp(buffer, "create") == 0)
            {
                rule->on_queries |= FW_OP_CREATE;
            }
            else if (strcmp(buffer, "alter") == 0)
            {
                rule->on_queries |= FW_OP_ALTER;
            }
            else if (strcmp(buffer, "load") == 0)
            {
                rule->on_queries |= FW_OP_LOAD;
            }

            if (done)
            {
                return true;
            }

            dest = buffer;
            ptr++;
        }
        else
        {
            *dest++ = *ptr++;
        }
    }
    return false;
}

/**
 * Checks whether a null-terminated string contains two ISO-8601 compliant times separated
 * by a single dash.
 * @param str String to check
 * @return True if the string is valid
 */
bool check_time(const char* str)
{
    assert(str != NULL);

    const char* ptr = str;
    int colons = 0, numbers = 0, dashes = 0;
    while (*ptr && ptr - str < 18)
    {
        if (isdigit(*ptr))
        {
            numbers++;
        }
        else if (*ptr == ':')
        {
            colons++;
        }
        else if (*ptr == '-')
        {
            dashes++;
        }
        ptr++;
    }
    return numbers == 12 && colons == 4 && dashes == 1;
}


#ifdef SS_DEBUG
#define CHK_TIMES(t) ss_dassert(t->tm_sec > -1 && t->tm_sec < 62        \
                                && t->tm_min > -1 && t->tm_min < 60     \
                                && t->tm_hour > -1 && t->tm_hour < 24)
#else
#define CHK_TIMES(t)
#endif

#define IS_RVRS_TIME(tr) (mktime(&tr->end) < mktime(&tr->start))

/**
 * Parses a null-terminated string into a timerange defined by two ISO-8601 compliant
 * times separated by a single dash. The times are interpreted at one second precision
 * and follow the extended format by separating the hours, minutes and seconds with
 * semicolons.
 * @param str String to parse
 * @param instance FW_FILTER instance
 * @return If successful returns a pointer to the new TIMERANGE instance. If errors occurred or
 * the timerange was invalid, a NULL pointer is returned.
 */
static TIMERANGE* parse_time(const char* str)
{
    assert(str != NULL);

    char strbuf[strlen(str) + 1];
    char *separator;
    struct tm start, end;
    TIMERANGE* tr = NULL;

    memset(&start, 0, sizeof(start));
    memset(&end, 0, sizeof(end));
    strcpy(strbuf, str);

    if ((separator = strchr(strbuf, '-')))
    {
        *separator++ = '\0';
        if (strptime(strbuf, "%H:%M:%S", &start) &&
            strptime(separator, "%H:%M:%S", &end))
        {
            /** The time string was valid */
            CHK_TIMES((&start));
            CHK_TIMES((&end));

            tr = (TIMERANGE*) MXS_MALLOC(sizeof(TIMERANGE));

            if (tr)
            {
                tr->start = start;
                tr->end = end;
                tr->next = NULL;
            }
        }
    }
    return tr;
}

/**
 * Splits the reversed timerange into two.
 *@param tr A reversed timerange
 *@return If the timerange is reversed, returns a pointer to the new TIMERANGE
 *        otherwise returns a NULL pointer
 */
TIMERANGE* split_reverse_time(TIMERANGE* tr)
{
    TIMERANGE* tmp = NULL;

    tmp = (TIMERANGE*) MXS_CALLOC(1, sizeof(TIMERANGE));
    MXS_ABORT_IF_NULL(tmp);
    tmp->next = tr;
    tmp->start.tm_hour = 0;
    tmp->start.tm_min = 0;
    tmp->start.tm_sec = 0;
    tmp->end = tr->end;
    tr->end.tm_hour = 23;
    tr->end.tm_min = 59;
    tr->end.tm_sec = 59;
    return tmp;
}

bool dbfw_reload_rules(const MODULECMD_ARG *argv, json_t** output)
{
    bool rval = true;
    MXS_FILTER_DEF *filter = argv->argv[0].value.filter;
    FW_INSTANCE *inst = (FW_INSTANCE*)filter_def_get_instance(filter);

    if (modulecmd_arg_is_present(argv, 1))
    {
        /** We need to change the rule file */
        char *newname = MXS_STRDUP(argv->argv[1].value.string);

        if (newname)
        {
            spinlock_acquire(&inst->lock);

            char *oldname = inst->rulefile;
            inst->rulefile = newname;

            spinlock_release(&inst->lock);

            MXS_FREE(oldname);
        }
        else
        {
            modulecmd_set_error("Memory allocation failed");
            rval = false;
        }
    }

    spinlock_acquire(&inst->lock);
    char filename[strlen(inst->rulefile) + 1];
    strcpy(filename, inst->rulefile);
    spinlock_release(&inst->lock);

    RuleList rules;
    UserMap  users;

    if (rval && access(filename, R_OK) == 0)
    {
        if (process_rule_file(filename, &rules, &users))
        {
            atomic_add(&inst->rule_version, 1);
            MXS_NOTICE("Reloaded rules from: %s", filename);
        }
        else
        {
            modulecmd_set_error("Failed to process rule file '%s'. See log "
                                "file for more details.", filename);
            rval = false;
        }
    }
    else
    {
        modulecmd_set_error("Failed to read rules at '%s': %d, %s", filename,
                            errno, mxs_strerror(errno));
        rval = false;
    }

    return rval;
}

bool dbfw_show_rules(const MODULECMD_ARG *argv, json_t** output)
{
    DCB *dcb = argv->argv[0].value.dcb;
    MXS_FILTER_DEF *filter = argv->argv[1].value.filter;
    FW_INSTANCE *inst = (FW_INSTANCE*)filter_def_get_instance(filter);

    dcb_printf(dcb, "Rule, Type, Times Matched\n");

    if (this_thread.rules.empty() || this_thread.users.empty())
    {
        if (!replace_rules(inst))
        {
            return 0;
        }
    }

    for (RuleList::const_iterator it = this_thread.rules.begin(); it != this_thread.rules.end(); it++)
    {
        const SRule& rule = *it;
        char buf[rule->name.length() + 200]; // Some extra space
        print_rule(rule.get(), buf);
        dcb_printf(dcb, "%s\n", buf);
    }

    return true;
}

bool dbfw_show_rules_json(const MODULECMD_ARG *argv, json_t** output)
{
    MXS_FILTER_DEF *filter = argv->argv[0].value.filter;
    FW_INSTANCE *inst = (FW_INSTANCE*)filter_def_get_instance(filter);

    json_t* arr = json_array();

    if (this_thread.rules.empty() || this_thread.users.empty())
    {
        if (!replace_rules(inst))
        {
            return 0;
        }
    }

    for (RuleList::const_iterator it = this_thread.rules.begin(); it != this_thread.rules.end(); it++)
    {
        const SRule& rule = *it;
        json_array_append_new(arr, rule_to_json(rule));
    }

    *output = arr;
    return true;
}

static const MXS_ENUM_VALUE action_values[] =
{
    {"allow",  FW_ACTION_ALLOW},
    {"block",  FW_ACTION_BLOCK},
    {"ignore", FW_ACTION_IGNORE},
    {NULL}
};

MXS_BEGIN_DECLS

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
MXS_MODULE* MXS_CREATE_MODULE()
{
    modulecmd_arg_type_t args_rules_reload[] =
    {
        {MODULECMD_ARG_FILTER | MODULECMD_ARG_NAME_MATCHES_DOMAIN, "Filter to reload"},
        {MODULECMD_ARG_STRING | MODULECMD_ARG_OPTIONAL, "Path to rule file"}
    };

    modulecmd_register_command(MXS_MODULE_NAME, "rules/reload", MODULECMD_TYPE_ACTIVE,
                               dbfw_reload_rules, 2, args_rules_reload,
                               "Reload dbfwfilter rules");

    modulecmd_arg_type_t args_rules_show[] =
    {
        {MODULECMD_ARG_OUTPUT, "DCB where result is written"},
        {MODULECMD_ARG_FILTER | MODULECMD_ARG_NAME_MATCHES_DOMAIN, "Filter to inspect"}
    };

    modulecmd_register_command(MXS_MODULE_NAME, "rules", MODULECMD_TYPE_PASSIVE,
                               dbfw_show_rules, 2, args_rules_show,
                               "(deprecated) Show dbfwfilter rule statistics");

    modulecmd_arg_type_t args_rules_show_json[] =
    {
        {MODULECMD_ARG_FILTER | MODULECMD_ARG_NAME_MATCHES_DOMAIN, "Filter to inspect"}
    };

    modulecmd_register_command(MXS_MODULE_NAME, "rules/json", MODULECMD_TYPE_PASSIVE,
                               dbfw_show_rules_json, 1, args_rules_show_json,
                               "Show dbfwfilter rule statistics as JSON");

    static MXS_FILTER_OBJECT MyObject =
    {
        createInstance,
        newSession,
        closeSession,
        freeSession,
        setDownstream,
        NULL, // No setUpStream
        routeQuery,
        NULL, // No clientReply
        diagnostic,
        diagnostic_json,
        getCapabilities,
        NULL, // No destroyInstance
    };

    static MXS_MODULE info =
    {
        MXS_MODULE_API_FILTER,
        MXS_MODULE_GA,
        MXS_FILTER_VERSION,
        "Firewall Filter",
        "V1.2.0",
        RCAP_TYPE_STMT_INPUT,
        &MyObject,
        NULL, /* Process init. */
        NULL, /* Process finish. */
        NULL, /* Thread init. */
        NULL, /* Thread finish. */
        {
            {
                "rules",
                MXS_MODULE_PARAM_PATH,
                NULL,
                MXS_MODULE_OPT_REQUIRED | MXS_MODULE_OPT_PATH_R_OK
            },
            {
                "log_match",
                MXS_MODULE_PARAM_BOOL,
                "false"
            },
            {
                "log_no_match",
                MXS_MODULE_PARAM_BOOL,
                "false"
            },
            {
                "action",
                MXS_MODULE_PARAM_ENUM,
                "block",
                MXS_MODULE_OPT_ENUM_UNIQUE,
                action_values
            },
            {MXS_END_MODULE_PARAMS}
        }
    };

    return &info;
}

MXS_END_DECLS

/**
 * Free a TIMERANGE struct
 * @param tr pointer to a TIMERANGE struct
 */
void timerange_free(TIMERANGE* tr)
{
    TIMERANGE *node, *tmp;

    node = tr;

    while (node)
    {
        tmp = node;
        node = node->next;
        MXS_FREE(tmp);
    }
}

/**
 * Retrieve the quoted regex string from a rule definition and
 * return the unquoted version of it.
 * @param saved Pointer to the last stored position in the string
 * @return The unquoted string or NULL if the string was malformed
 */
char* get_regex_string(char** saved)
{
    char *start = NULL, *ptr = *saved;
    bool escaped = false, quoted = false;
    char delimiter = 0;
    while (*ptr != '\0')
    {
        if (!escaped)
        {
            if (!isspace(*ptr))
            {
                switch (*ptr)
                {
                case '\'':
                case '"':
                    if (quoted)
                    {
                        if (*ptr == delimiter)
                        {
                            *ptr = '\0';
                            *saved = ptr + 1;
                            return start;
                        }
                    }
                    else
                    {
                        delimiter = *ptr;
                        start = ptr + 1;
                        quoted = true;
                    }
                    break;
                case '\\':
                    escaped = true;
                    break;
                default:
                    break;
                }
            }
        }
        else
        {
            escaped = false;
        }
        ptr++;
    }

    if (quoted)
    {
        MXS_ERROR("Missing ending quote, found '%c' but no matching unescaped"
                  " one was found.", delimiter);
    }

    return NULL;
}

/**
 * Structure used to hold rules and users that are being parsed
 */
struct parser_stack
{
    RuleList rule;
    ValueList user;
    ValueList active_rules;
    enum match_type active_mode;
    TemplateList templates;
    ValueList values;
    std::string name;
};

/**
 * Report parsing errors
 * @param scanner Currently active scanner
 * @param error Error message
 */
void dbfw_yyerror(void* scanner, const char* error)
{
    MXS_ERROR("Error on line %d, %s: %s\n", dbfw_yyget_lineno(scanner),
              error, dbfw_yyget_text(scanner));
}

/**
 * @brief Find a rule by name
 *
 * @param rules List of all rules
 * @param name Name of the rule
 * @return Pointer to the rule or NULL if rule was not found
 */
static SRule find_rule_by_name(const RuleList& rules, std::string name)
{
    for (RuleList::const_iterator it = rules.begin(); it != rules.end(); it++)
    {
        const SRule& rule = *it;

        if (rule->name == name)
        {
            return rule;
        }
    }

    return SRule();
}

/**
 * Create a new rule
 *
 * The rule is created with the default type which will always match. The rule
 * is later specialized by the definition of the actual rule.
 * @param scanner Current scanner
 * @param name Name of the rule
 */
static Rule* create_rule(const std::string& name)
{
    return new Rule(name);
}

bool set_rule_name(void* scanner, char* name)
{
    bool rval = true;
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t)scanner);
    ss_dassert(rstack);

    if (find_rule_by_name(rstack->rule, name))
    {
        MXS_ERROR("Redefinition of rule '%s' on line %d.", name, dbfw_yyget_lineno(scanner));
        rval = false;
    }
    else
    {
        rstack->name = name;
    }

    return rval;
}

/**
 * Remove backticks from a string
 * @param string String to parse
 * @return String without backticks
 */
static std::string strip_backticks(std::string str)
{
    size_t start = str.find_first_of('`');
    size_t end = str.find_last_of('`');

    if (end != std::string::npos && start != std::string::npos)
    {
        str = str.substr(start + 1, (end - 1) - (start + 1));
    }

    return str;
}

void push_value(void* scanner, char* value)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t)scanner);
    ss_dassert(rstack);
    rstack->values.push_back(strip_backticks(value));
}

/**
 * Add a user to the current rule linking expression
 * @param scanner Current scanner
 * @param name Name of the user
 */
void add_active_user(void* scanner, const char* name)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);
    rstack->user.push_back(name);
}

/**
 * Add a rule to the current rule linking expression
 * @param scanner Current scanner
 * @param name Name of the rule
 */
void add_active_rule(void* scanner, const char* name)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);
    rstack->active_rules.push_back(name);
}

/**
 * Add an optional at_times definition to the rule
 * @param scanner Current scanner
 * @param range two ISO-8601 compliant times separated by a single dash
 */
bool add_at_times_rule(void* scanner, const char* range)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);
    TIMERANGE* timerange = parse_time(range);
    ss_dassert(timerange);

    if (timerange)
    {
        timerange->next = rstack->rule.front()->active;
        rstack->rule.front()->active = timerange;
    }

    return timerange != NULL;
}

/**
 * Add an optional on_queries definition to the rule
 * @param scanner Current scanner
 * @param sql List of SQL operations separated by vertical bars
 */
void add_on_queries_rule(void* scanner, const char* sql)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);
    parse_querytypes(sql, rstack->rule.front());
}

/**
 * Link users and rules
 * @param scanner Current scanner
 */
bool create_user_templates(void* scanner)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);

    for (ValueList::const_iterator it = rstack->user.begin(); it != rstack->user.end(); it++)
    {
        SUserTemplate newtemp = SUserTemplate(new UserTemplate);
        newtemp->name = *it;
        newtemp->rulenames = rstack->active_rules;
        newtemp->type = rstack->active_mode;
        rstack->templates.push_back(newtemp);
    }

    rstack->user.clear();
    rstack->active_rules.clear();

    return true;
}

void set_matching_mode(void* scanner, enum match_type mode)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);
    rstack->active_mode = mode;
}

STRLINK* valuelist_to_strlink(ValueList* arr)
{

    STRLINK* list = NULL;

    for (ValueList::const_iterator it = arr->begin(); it != arr->end(); it++)
    {
        list = strlink_push(list, it->c_str());
    }

    arr->clear();

    return list;
}

/**
 * Define the topmost rule as a wildcard rule
 * @param scanner Current scanner
 */
void define_wildcard_rule(void* scanner)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);
    Rule* rule = create_rule(rstack->name);
    rule->type = RT_WILDCARD;
    rstack->rule.push_front(SRule(rule));
}

/**
 * Define the current rule as a columns rule
 * @param scanner Current scanner
 * @param columns List of column names
 */
void define_columns_rule(void* scanner)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);
    Rule* rule = create_rule(rstack->name);

    rule->type = RT_COLUMN;
    rule->data = valuelist_to_strlink(&rstack->values);
    rstack->rule.push_front(SRule(rule));
}

/**
 * Define the current rule as a function rule
 * @param scanner Current scanner
 * @param columns List of function names
 */
void define_function_rule(void* scanner)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);
    Rule* rule = create_rule(rstack->name);

    rule->type = RT_FUNCTION;
    rule->data = valuelist_to_strlink(&rstack->values);
    rstack->rule.push_front(SRule(rule));
}

/**
 * Define the current rule as a function usage rule
 *
 * @param scanner Current scanner
 * @param columns List of column names
 *
 * @return True if rule creation was successful
 */
void define_function_usage_rule(void* scanner)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);
    Rule* rule = create_rule(rstack->name);

    rule->type = RT_USES_FUNCTION;
    rule->data = valuelist_to_strlink(&rstack->values);
    rstack->rule.push_front(SRule(rule));
}

/**
 * Define the topmost rule as a no_where_clause rule
 * @param scanner Current scanner
 */
void define_where_clause_rule(void* scanner)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);
    Rule* rule = create_rule(rstack->name);

    rule->type = RT_CLAUSE;
    rstack->rule.push_front(SRule(rule));
}

/**
 * Define the topmost rule as a no_where_clause rule
 * @param scanner Current scanner
 */
void define_limit_queries_rule(void* scanner, int max, int timeperiod, int holdoff)
{
    struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
    ss_dassert(rstack);
    QUERYSPEED* qs = new QUERYSPEED;
    Rule* rule = create_rule(rstack->name);

    qs->limit = max;
    qs->period = timeperiod;
    qs->cooldown = holdoff;
    rule->type = RT_THROTTLE;
    rule->data = qs;
    rstack->rule.push_front(SRule(rule));
}

/**
 * Define the topmost rule as a regex rule
 * @param scanner Current scanner
 * @param pattern Quoted regex pattern
 */
bool define_regex_rule(void* scanner, char* pattern)
{
    /** This should never fail as long as the rule syntax is correct */
    PCRE2_SPTR start = (PCRE2_SPTR) get_regex_string(&pattern);
    ss_dassert(start);
    pcre2_code *re;
    int err;
    size_t offset;
    if ((re = pcre2_compile(start, PCRE2_ZERO_TERMINATED,
                            0, &err, &offset, NULL)))
    {
        struct parser_stack* rstack = (struct parser_stack*)dbfw_yyget_extra((yyscan_t) scanner);
        ss_dassert(rstack);
        Rule* rule = create_rule(rstack->name);
        rule->type = RT_REGEX;
        rule->data = re;
        rstack->rule.push_front(SRule(rule));
    }
    else
    {
        PCRE2_UCHAR errbuf[MXS_STRERROR_BUFLEN];
        pcre2_get_error_message(err, errbuf, sizeof(errbuf));
        MXS_ERROR("Invalid regular expression '%s': %s",
                  start, errbuf);
    }

    return re != NULL;
}

/**
 * @brief Process the user templates into actual user definitions
 *
 * @param instance Filter instance
 * @param templates User templates
 * @param rules List of all rules
 * @return True on success, false on error.
 */
static bool process_user_templates(UserMap& users, const TemplateList& templates,
                                   RuleList& rules)
{
    bool rval = true;

    if (templates.size() == 0)
    {
        MXS_ERROR("No user definitions found in the rule file.");
        rval = false;
    }

    for (TemplateList::const_iterator it = templates.begin(); it != templates.end(); it++)
    {
        const SUserTemplate& ut = *it;

        if (users.find(ut->name) == users.end())
        {
            users[ut->name] = SUser(new User(ut->name));
        }

        SUser& user = users[ut->name];
        RuleList newrules;

        for (ValueList::const_iterator r_it = ut->rulenames.begin();
             r_it != ut->rulenames.end(); r_it++)
        {
            SRule rule = find_rule_by_name(rules, r_it->c_str());

            if (rule)
            {
                newrules.push_front(rule);
            }
            else
            {
                MXS_ERROR("Could not find definition for rule '%s'.", r_it->c_str());
                rval = false;
            }
        }

        if (newrules.size() > 0)
        {
            user->append_rules(ut->type, newrules);
        }
    }

    return rval;
}

/**
 * Read a rule file from disk and process it into rule and user definitions
 * @param filename Name of the file
 * @param instance Filter instance
 * @return True on success, false on error.
 */
static bool do_process_rule_file(const char* filename, RuleList* rules, UserMap* users)
{
    int rc = 1;
    FILE *file = fopen(filename, "r");

    if (file)
    {
        yyscan_t scanner;
        struct parser_stack pstack;

        dbfw_yylex_init(&scanner);
        YY_BUFFER_STATE buf = dbfw_yy_create_buffer(file, YY_BUF_SIZE, scanner);
        dbfw_yyset_extra(&pstack, scanner);
        dbfw_yy_switch_to_buffer(buf, scanner);

        /** Parse the rule file */
        rc = dbfw_yyparse(scanner);

        dbfw_yy_delete_buffer(buf, scanner);
        dbfw_yylex_destroy(scanner);
        fclose(file);
        UserMap new_users;

        if (rc == 0 && process_user_templates(new_users, pstack.templates, pstack.rule))
        {
            rules->swap(pstack.rule);
            users->swap(new_users);
        }
        else
        {
            rc = 1;
            MXS_ERROR("Failed to process rule file '%s'.", filename);
        }
    }
    else
    {
        MXS_ERROR("Failed to open rule file '%s': %d, %s", filename, errno,
                  mxs_strerror(errno));

    }

    return rc == 0;
}

static bool process_rule_file(const char* filename, RuleList* rules, UserMap* users)
{
    bool rval = false;
    MXS_EXCEPTION_GUARD(rval = do_process_rule_file(filename, rules, users));
    return rval;
}

/**
 * @brief Replace the rule file used by this thread
 *
 * This function replaces or initializes the thread local list of rules and users.
 *
 * @param instance Filter instance
 * @return True if the session can continue, false on fatal error.
 */
bool replace_rules(FW_INSTANCE* instance)
{
    bool rval = true;
    spinlock_acquire(&instance->lock);

    size_t len = strlen(instance->rulefile);
    char filename[len + 1];
    strcpy(filename, instance->rulefile);

    spinlock_release(&instance->lock);

    RuleList rules;
    UserMap  users;

    if (process_rule_file(filename, &rules, &users))
    {
        this_thread.rules.swap(rules);
        this_thread.users.swap(users);
        rval = true;
    }
    else if (!this_thread.rules.empty() && !this_thread.users.empty())
    {
        MXS_ERROR("Failed to parse rules at '%s'. Old rules are still used.", filename);
    }
    else
    {
        MXS_ERROR("Failed to parse rules at '%s'. No previous rules available, "
                  "closing session.", filename);
        rval = false;
    }

    return rval;
}

/**
 * Create an instance of the filter for a particular service
 * within MaxScale.
 *
 * @param name     The name of the instance (as defined in the config file).
 * @param options  The options for this filter
 * @param params   The array of name/value pair parameters for the filter
 *
 * @return The instance data for this new instance
 */
static MXS_FILTER *
createInstance(const char *name, char **options, MXS_CONFIG_PARAMETER *params)
{
    FW_INSTANCE *my_instance = (FW_INSTANCE*)MXS_CALLOC(1, sizeof(FW_INSTANCE));

    if (my_instance == NULL)
    {
        MXS_FREE(my_instance);
        return NULL;
    }

    spinlock_init(&my_instance->lock);
    my_instance->action = (enum fw_actions)config_get_enum(params, "action", action_values);
    my_instance->log_match = FW_LOG_NONE;

    if (config_get_bool(params, "log_match"))
    {
        my_instance->log_match |= FW_LOG_MATCH;
    }

    if (config_get_bool(params, "log_no_match"))
    {
        my_instance->log_match |= FW_LOG_NO_MATCH;
    }

    RuleList rules;
    UserMap  users;
    my_instance->rulefile = MXS_STRDUP(config_get_string(params, "rules"));

    if (!my_instance->rulefile || !process_rule_file(my_instance->rulefile, &rules, &users))
    {
        MXS_FREE(my_instance);
        my_instance = NULL;
    }
    else
    {
        atomic_add(&my_instance->rule_version, 1);
    }

    return (MXS_FILTER *) my_instance;
}

/**
 * Associate a new session with this instance of the filter.
 *
 * @param instance  The filter instance data
 * @param session   The session itself
 * @return Session specific data for this session
 */
static MXS_FILTER_SESSION *
newSession(MXS_FILTER *instance, MXS_SESSION *session)
{
    FW_SESSION *my_session;

    if ((my_session = (FW_SESSION*)MXS_CALLOC(1, sizeof(FW_SESSION))) == NULL)
    {
        return NULL;
    }
    my_session->session = session;
    return (MXS_FILTER_SESSION*)my_session;
}

/**
 * Close a session with the filter, this is the mechanism
 * by which a filter may cleanup data structure etc.
 *
 * @param instance  The filter instance data
 * @param session   The session being closed
 */
static void
closeSession(MXS_FILTER *instance, MXS_FILTER_SESSION *session)
{
}

/**
 * Free the memory associated with the session
 *
 * @param instance  The filter instance
 * @param session   The filter session
 */
static void
freeSession(MXS_FILTER *instance, MXS_FILTER_SESSION *session)
{
    FW_SESSION *my_session = (FW_SESSION *) session;
    MXS_FREE(my_session->errmsg);
    MXS_FREE(my_session->query_speed);
    MXS_FREE(my_session);
}

/**
 * Set the downstream filter or router to which queries will be
 * passed from this filter.
 *
 * @param instance  The filter instance data
 * @param session   The filter session
 * @param downstream    The downstream filter or router.
 */
static void
setDownstream(MXS_FILTER *instance, MXS_FILTER_SESSION *session, MXS_DOWNSTREAM *downstream)
{
    FW_SESSION *my_session = (FW_SESSION *) session;
    my_session->down = *downstream;
}

/**
 * Generates a dummy error packet for the client with a custom message.
 * @param session The FW_SESSION object
 * @param msg Custom error message for the packet.
 * @return The dummy packet or NULL if an error occurred
 */
GWBUF* gen_dummy_error(FW_SESSION* session, char* msg)
{
    GWBUF* buf;
    char* errmsg;
    DCB* dcb;
    MYSQL_session* mysql_session;
    unsigned int errlen;

    if (session == NULL || session->session == NULL ||
        session->session->client_dcb == NULL ||
        session->session->client_dcb->data == NULL)
    {
        MXS_ERROR("Firewall filter session missing data.");
        return NULL;
    }

    dcb = session->session->client_dcb;
    const char* db = mxs_mysql_get_current_db(session->session);
    errlen = msg != NULL ? strlen(msg) : 0;
    errmsg = (char*) MXS_MALLOC((512 + errlen) * sizeof(char));

    if (errmsg == NULL)
    {
        return NULL;
    }


    if (db[0] == '\0')
    {
        sprintf(errmsg, "Access denied for user '%s'@'%s'", dcb->user, dcb->remote);
    }
    else
    {
        sprintf(errmsg, "Access denied for user '%s'@'%s' to database '%s'",
                dcb->user, dcb->remote, db);
    }

    if (msg != NULL)
    {
        char* ptr = strchr(errmsg, '\0');
        sprintf(ptr, ": %s", msg);

    }

    buf = modutil_create_mysql_err_msg(1, 0, 1141, "HY000", (const char*) errmsg);
    MXS_FREE(errmsg);

    return buf;
}

/**
 * Checks if the timerange object is active.
 * @return Whether the timerange is active
 */
bool inside_timerange(TIMERANGE* comp)
{

    struct tm tm_now;
    struct tm tm_before, tm_after;
    time_t before, after, now, time_now;
    double to_before, to_after;

    time(&time_now);
    localtime_r(&time_now, &tm_now);
    memcpy(&tm_before, &tm_now, sizeof(struct tm));
    memcpy(&tm_after, &tm_now, sizeof(struct tm));


    tm_before.tm_sec = comp->start.tm_sec;
    tm_before.tm_min = comp->start.tm_min;
    tm_before.tm_hour = comp->start.tm_hour;
    tm_after.tm_sec = comp->end.tm_sec;
    tm_after.tm_min = comp->end.tm_min;
    tm_after.tm_hour = comp->end.tm_hour;


    before = mktime(&tm_before);
    after = mktime(&tm_after);
    now = mktime(&tm_now);
    to_before = difftime(now, before);
    to_after = difftime(now, after);

    if (to_before > 0.0 && to_after < 0.0)
    {
        return true;
    }
    return false;
}

/**
 * Checks for active timeranges for a given rule.
 * @param rule Pointer to a RULE object
 * @return true if the rule is active
 */
bool rule_is_active(SRule rule)
{
    TIMERANGE* times;
    if (rule->active != NULL)
    {
        times = (TIMERANGE*) rule->active;
        while (times)
        {
            if (inside_timerange(times))
            {
                return true;
            }
            times = times->next;
        }
        return false;
    }
    return true;
}

/**
 * A convenience wrapper for snprintf and strdup
 *
 * @param format Format string
 * @param ...    Variable argument list
 *
 * @return Pointer to newly allocated and formatted string
 */
char* create_error(const char* format, ...)
{
    va_list valist;
    va_start(valist, format);
    int message_len = vsnprintf(NULL, 0, format, valist);
    va_end(valist);

    char* rval = (char*)MXS_MALLOC(message_len + 1);
    MXS_ABORT_IF_NULL(rval);

    va_start(valist, format);
    vsnprintf(rval, message_len + 1, format, valist);
    va_end(valist);

    return rval;
}

/**
 * Log and create an error message when a query could not be fully parsed.
 * @param my_instance The FwFilter instance.
 * @param reason The reason the query was rejected.
 * @param query The query that could not be parsed.
 * @param matchesp Pointer to variable that will receive the value indicating
 *                 whether the query was parsed or not.
 *
 * Note that the value of *matchesp depends on the the mode of the filter,
 * i.e., whether it is in whitelist or blacklist mode. The point is that
 * irrespective of the mode, the query must be rejected.
 */
static char* create_parse_error(FW_INSTANCE* my_instance,
                                const char* reason,
                                const char* query,
                                bool* matchesp)
{
    char *msg = NULL;

    char format[] =
        "Query could not be %s and will hence be rejected. "
        "Please ensure that the SQL syntax is correct";
    size_t len = sizeof(format) + strlen(reason); // sizeof includes the trailing NULL as well.
    char message[len];
    sprintf(message, format, reason);
    MXS_WARNING("%s: %s", message, query);

    if ((my_instance->action == FW_ACTION_ALLOW) || (my_instance->action == FW_ACTION_BLOCK))
    {
        msg = create_error("%s.", message);

        if (my_instance->action == FW_ACTION_ALLOW)
        {
            *matchesp = false;
        }
        else
        {
            *matchesp = true;
        }
    }

    return msg;
}

bool match_throttle(FW_SESSION* my_session, SRule rule, char **msg)
{
    bool matches = false;
    QUERYSPEED* rule_qs = (QUERYSPEED*)rule->data;
    QUERYSPEED* queryspeed = my_session->query_speed;
    time_t time_now = time(NULL);

    if (queryspeed == NULL)
    {
        /**No match found*/
        queryspeed = new QUERYSPEED;
        queryspeed->period = rule_qs->period;
        queryspeed->cooldown = rule_qs->cooldown;
        queryspeed->limit = rule_qs->limit;
        my_session->query_speed = queryspeed;
    }

    if (queryspeed->active)
    {
        if (difftime(time_now, queryspeed->triggered) < queryspeed->cooldown)
        {
            double blocked_for = queryspeed->cooldown - difftime(time_now, queryspeed->triggered);
            *msg = create_error("Queries denied for %f seconds", blocked_for);
            matches = true;

            MXS_INFO("rule '%s': user denied for %f seconds",
                     rule->name.c_str(), blocked_for);
        }
        else
        {
            queryspeed->active = false;
            queryspeed->count = 0;
        }
    }
    else
    {
        if (queryspeed->count >= queryspeed->limit)
        {
            MXS_INFO("rule '%s': query limit triggered (%d queries in %d seconds), "
                     "denying queries from user for %d seconds.", rule->name.c_str(),
                     queryspeed->limit, queryspeed->period, queryspeed->cooldown);

            queryspeed->triggered = time_now;
            queryspeed->active = true;
            matches = true;

            double blocked_for = queryspeed->cooldown - difftime(time_now, queryspeed->triggered);
            *msg = create_error("Queries denied for %f seconds", blocked_for);
        }
        else if (queryspeed->count > 0 &&
                 difftime(time_now, queryspeed->first_query) <= queryspeed->period)
        {
            queryspeed->count++;
        }
        else
        {
            queryspeed->first_query = time_now;
            queryspeed->count = 1;
        }
    }

    return matches;
}

void match_regex(SRule rule, const char *query, bool *matches, char **msg)
{

    pcre2_match_data *mdata = pcre2_match_data_create_from_pattern((pcre2_code*)rule->data, NULL);

    if (mdata)
    {
        if (pcre2_match((pcre2_code*)rule->data,
                        (PCRE2_SPTR)query, PCRE2_ZERO_TERMINATED,
                        0, 0, mdata, NULL) > 0)
        {
            MXS_NOTICE("rule '%s': regex matched on query", rule->name.c_str());
            *matches = true;
            *msg = create_error("Permission denied, query matched regular expression.");
        }

        pcre2_match_data_free(mdata);
    }
    else
    {
        MXS_ERROR("Allocation of matching data for PCRE2 failed."
                  " This is most likely caused by a lack of memory");
    }
}

void match_column(SRule rule, GWBUF *queue, bool *matches, char **msg)
{
    const QC_FIELD_INFO* infos;
    size_t n_infos;
    qc_get_field_info(queue, &infos, &n_infos);

    for (size_t i = 0; i < n_infos; ++i)
    {
        const char* tok = infos[i].column;

        STRLINK* strln = (STRLINK*)rule->data;
        while (strln)
        {
            if (strcasecmp(tok, strln->value) == 0)
            {
                MXS_NOTICE("rule '%s': query targets forbidden column: %s",
                           rule->name.c_str(), strln->value);
                *msg = create_error("Permission denied to column '%s'.", strln->value);
                *matches = true;
                break;
            }
            strln = strln->next;
        }
    }
}

void match_function(SRule rule, GWBUF *queue, enum fw_actions mode,
                    bool *matches, char **msg)
{
    const QC_FUNCTION_INFO* infos;
    size_t n_infos;
    qc_get_function_info(queue, &infos, &n_infos);

    if (n_infos == 0 && mode == FW_ACTION_ALLOW)
    {
        *matches = true;
    }

    for (size_t i = 0; i < n_infos; ++i)
    {
        const char* tok = infos[i].name;

        STRLINK* strln = (STRLINK*)rule->data;
        while (strln)
        {
            if (strcasecmp(tok, strln->value) == 0)
            {
                MXS_NOTICE("rule '%s': query uses forbidden function: %s",
                           rule->name.c_str(), strln->value);
                *msg = create_error("Permission denied to function '%s'.", strln->value);
                *matches = true;
                break;
            }
            strln = strln->next;
        }
    }
}

void match_function_usage(SRule rule, GWBUF *queue, enum fw_actions mode,
                          bool *matches, char **msg)
{
    const QC_FUNCTION_INFO* infos;
    size_t n_infos;
    qc_get_function_info(queue, &infos, &n_infos);

    for (size_t i = 0; i < n_infos; ++i)
    {
        for (size_t j = 0; j < infos[i].n_fields; j++)
        {
            const char* tok = infos[i].fields[j].column;

            for (STRLINK* s = (STRLINK*)rule->data; s; s = s->next)
            {
                if (strcasecmp(tok, s->value) == 0)
                {
                    MXS_NOTICE("rule '%s': query uses a function with forbidden column: %s",
                               rule->name.c_str(), s->value);
                    *msg = create_error("Permission denied to column '%s' with function.", s->value);
                    *matches = true;
                    return;
                }
            }
        }
    }
}

void match_wildcard(SRule rule, GWBUF *queue, bool *matches, char **msg)
{
    const QC_FIELD_INFO* infos;
    size_t n_infos;
    qc_get_field_info(queue, &infos, &n_infos);

    for (size_t i = 0; i < n_infos; ++i)
    {
        if (strcmp(infos[i].column, "*") == 0)
        {
            MXS_NOTICE("rule '%s': query contains a wildcard.", rule->name.c_str());
            *matches = true;
            *msg = create_error("Usage of wildcard denied.");
        }
    }
}

/**
 * Check if a query matches a single rule
 * @param my_instance Fwfilter instance
 * @param my_session Fwfilter session
 * @param queue The GWBUF containing the query
 * @param rule The rule to check
 * @param query Pointer to the null-terminated query string
 * @return true if the query matches the rule
 */
bool rule_matches(FW_INSTANCE* my_instance,
                  FW_SESSION* my_session,
                  GWBUF *queue,
                  SRule rule,
                  char* query)
{
    ss_dassert(GWBUF_IS_CONTIGUOUS(queue));
    char *msg = NULL;
    bool matches = false;
    bool is_sql = modutil_is_SQL(queue) || modutil_is_SQL_prepare(queue);

    if (is_sql)
    {
        qc_parse_result_t parse_result = qc_parse(queue, QC_COLLECT_ALL);

        if (parse_result == QC_QUERY_INVALID)
        {
            msg = create_parse_error(my_instance, "tokenized", query, &matches);
            goto queryresolved;
        }
        else if (parse_result != QC_QUERY_PARSED && rule->need_full_parsing(queue))
        {
            msg = create_parse_error(my_instance, "parsed completely", query, &matches);
            goto queryresolved;
        }
    }

    if (rule->matches_query_type(queue))
    {
        if (rule->matches_query(queue, &msg))
        {
            /** New style rule matched */
            matches = true;
            goto queryresolved;
        }

        /** No match, try old the style rule */
        switch (rule->type)
        {
        case RT_UNDEFINED:
            ss_dassert(false);
            MXS_ERROR("Undefined rule type found.");
            break;

        case RT_REGEX:
            match_regex(rule, query, &matches, &msg);
            break;

        case RT_PERMISSION:
            /** Handled in Rule::matches_query */
            break;

        case RT_COLUMN:
            if (is_sql)
            {
                match_column(rule, queue, &matches, &msg);
            }
            break;

        case RT_FUNCTION:
            if (is_sql)
            {
                match_function(rule, queue, my_instance->action, &matches, &msg);
            }
            break;

        case RT_USES_FUNCTION:
            if (is_sql)
            {
                match_function_usage(rule, queue, my_instance->action, &matches, &msg);
            }
            break;

        case RT_WILDCARD:
            if (is_sql)
            {
                match_wildcard(rule, queue, &matches, &msg);
            }
            break;

        case RT_THROTTLE:
            matches = match_throttle(my_session, rule, &msg);
            break;

        case RT_CLAUSE:
            if (is_sql && !qc_query_has_clause(queue))
            {
                matches = true;
                msg = create_error("Required WHERE/HAVING clause is missing.");
                MXS_NOTICE("rule '%s': query has no where/having "
                           "clause, query is denied.", rule->name.c_str());
            }
            break;

        default:
            break;

        }
    }

queryresolved:
    if (msg)
    {
        if (my_session->errmsg)
        {
            MXS_FREE(my_session->errmsg);
        }

        my_session->errmsg = msg;
    }

    if (matches)
    {
        rule->times_matched++;
    }

    return matches;
}

/**
 * Check if the query matches any of the rules in the user's rules.
 * @param my_instance Fwfilter instance
 * @param my_session Fwfilter session
 * @param queue The GWBUF containing the query
 * @param user The user whose rules are checked
 * @return True if the query matches at least one of the rules otherwise false
 */
bool check_match_any(FW_INSTANCE* my_instance, FW_SESSION* my_session,
                     GWBUF *queue, SUser user, char** rulename)
{

    bool rval = false;

    if (!user->rules_or.empty() &&
        (modutil_is_SQL(queue) || modutil_is_SQL_prepare(queue) ||
         MYSQL_IS_COM_INIT_DB((uint8_t*)GWBUF_DATA(queue))))
    {
        char *fullquery = modutil_get_SQL(queue);

        if (fullquery)
        {
            for (RuleList::iterator it = user->rules_or.begin(); it != user->rules_or.end(); it++)
            {
                if (rule_is_active(*it))
                {
                    if (rule_matches(my_instance, my_session, queue, *it, fullquery))
                    {
                        *rulename = MXS_STRDUP_A((*it)->name.c_str());
                        rval = true;
                        break;
                    }
                }
            }

            MXS_FREE(fullquery);
        }
    }
    return rval;
}

/**
 * Append and possibly reallocate string
 * @param dest Destination where the string is appended or NULL if nothing has
 * been allocated yet
 * @param size Size of @c dest
 * @param src String to append to @c dest
 */
void append_string(char** dest, size_t* size, const char* src)
{
    if (*dest == NULL)
    {
        *dest = MXS_STRDUP_A(src);
        *size = strlen(src);
    }
    else
    {
        if (*size < strlen(*dest) + strlen(src) + 3)
        {
            size_t newsize = strlen(*dest) + strlen(src) + 3;
            char* tmp = (char*)MXS_REALLOC(*dest, newsize);
            if (tmp)
            {
                *size = newsize;
                *dest = tmp;
            }
            else
            {
                return;
            }
        }
        strcat(*dest, ", ");
        strcat(*dest, src);
    }
}

/**
 * Check if the query matches all rules in the user's rules
 *
 * @param my_instance Filter instance
 * @param my_session  Filter session
 * @param queue       Buffer containing the query
 * @param user        The user whose rules are checked
 * @param strict_all  Whether the first match stops the processing
 * @param rulename    Pointer where error messages are stored
 *
 * @return True if the query matches all of the rules otherwise false
 */
bool check_match_all(FW_INSTANCE* my_instance, FW_SESSION* my_session,
                     GWBUF *queue, SUser user, bool strict_all, char** rulename)
{
    bool rval = false;
    bool have_active_rule = false;
    RuleList& rules = strict_all ? user->rules_strict_and : user->rules_and;
    char *matched_rules = NULL;
    size_t size = 0;

    if (!rules.empty() && (modutil_is_SQL(queue) || modutil_is_SQL_prepare(queue)))
    {
        char *fullquery = modutil_get_SQL(queue);

        if (fullquery)
        {
            rval = true;
            for (RuleList::iterator it = rules.begin(); it != rules.end(); it++)
            {
                if (!rule_is_active(*it))
                {
                    have_active_rule = true;

                    if (rule_matches(my_instance, my_session, queue, *it, fullquery))
                    {
                        append_string(&matched_rules, &size, (*it)->name.c_str());
                    }
                    else
                    {
                        rval = false;
                        if (strict_all)
                        {
                            break;
                        }
                    }
                }
            }

            if (!have_active_rule)
            {
                /** No active rules */
                rval = false;
            }
            MXS_FREE(fullquery);
        }
    }

    /** Set the list of matched rule names */
    *rulename = matched_rules;

    return rval;
}

/**
 * Retrieve the user specific data for this session
 *
 * @param users Map containing the user data
 * @param name Username
 * @param remote Remove network address
 * @return The user data or NULL if it was not found
 */
SUser find_user_data(const UserMap& users, const char *name, const char *remote)
{
    char nameaddr[strlen(name) + strlen(remote) + 2];
    snprintf(nameaddr, sizeof(nameaddr), "%s@%s", name, remote);
    UserMap::const_iterator it = users.find(nameaddr);

    if (it == users.end())
    {
        char *ip_start = strchr(nameaddr, '@') + 1;
        while (it == users.end() && next_ip_class(ip_start))
        {
            it = users.find(nameaddr);
        }

        if (it == users.end())
        {
            snprintf(nameaddr, sizeof(nameaddr), "%%@%s", remote);
            ip_start = strchr(nameaddr, '@') + 1;

            while (it == users.end() && next_ip_class(ip_start))
            {
                it = users.find(nameaddr);
            }
        }
    }

    return it != users.end() ? it->second : SUser();
}

static bool command_is_mandatory(const GWBUF *buffer)
{
    switch (MYSQL_GET_COMMAND((uint8_t*)GWBUF_DATA(buffer)))
    {
    case MYSQL_COM_QUIT:
    case MYSQL_COM_PING:
    case MYSQL_COM_CHANGE_USER:
    case MYSQL_COM_SET_OPTION:
    case MYSQL_COM_FIELD_LIST:
    case MYSQL_COM_PROCESS_KILL:
    case MYSQL_COM_PROCESS_INFO:
        return true;

    default:
        return false;
    }
}

/**
 * The routeQuery entry point. This is passed the query buffer
 * to which the filter should be applied. Once processed the
 * query is passed to the downstream component
 * (filter or router) in the filter chain.
 *
 * @param instance  The filter instance data
 * @param session   The filter session
 * @param queue     The query data
 */
static int
routeQuery(MXS_FILTER *instance, MXS_FILTER_SESSION *session, GWBUF *queue)
{
    FW_SESSION *my_session = (FW_SESSION *) session;
    FW_INSTANCE *my_instance = (FW_INSTANCE *) instance;
    DCB *dcb = my_session->session->client_dcb;
    int rval = 0;
    ss_dassert(dcb && dcb->session);
    int rule_version = my_instance->rule_version;

    if (this_thread.rule_version < rule_version)
    {
        if (!replace_rules(my_instance))
        {
            return 0;
        }
        this_thread.rule_version = rule_version;
    }

    uint32_t type = 0;

    if (modutil_is_SQL(queue) || modutil_is_SQL_prepare(queue))
    {
        type = qc_get_type_mask(queue);
    }

    if (modutil_is_SQL(queue) && modutil_count_statements(queue) > 1)
    {
        GWBUF* err = gen_dummy_error(my_session, (char*)"This filter does not support "
                                     "multi-statements.");
        gwbuf_free(queue);
        MXS_FREE(my_session->errmsg);
        my_session->errmsg = NULL;
        rval = dcb->func.write(dcb, err);
    }
    else
    {
        GWBUF* analyzed_queue = queue;

        // QUERY_TYPE_PREPARE_STMT need not be handled separately as the
        // information about statements in COM_STMT_PREPARE packets is
        // accessed exactly like the information of COM_QUERY packets. However,
        // with named prepared statements in COM_QUERY packets, we need to take
        // out the preparable statement and base our decisions on that.

        if (qc_query_is_type(type, QUERY_TYPE_PREPARE_NAMED_STMT))
        {
            analyzed_queue = qc_get_preparable_stmt(queue);
            ss_dassert(analyzed_queue);
        }

        SUser user = find_user_data(this_thread.users, dcb->user, dcb->remote);
        bool query_ok = command_is_mandatory(queue);

        if (user)
        {
            bool match = false;
            char* rname = NULL;

            if (check_match_any(my_instance, my_session, analyzed_queue, user, &rname) ||
                check_match_all(my_instance, my_session, analyzed_queue, user, false, &rname) ||
                check_match_all(my_instance, my_session, analyzed_queue, user, true, &rname))
            {
                match = true;
            }

            switch (my_instance->action)
            {
            case FW_ACTION_ALLOW:
                if (match)
                {
                    query_ok = true;
                }
                break;

            case FW_ACTION_BLOCK:
                if (!match)
                {
                    query_ok = true;
                }
                break;

            case FW_ACTION_IGNORE:
                query_ok = true;
                break;

            default:
                MXS_ERROR("Unknown dbfwfilter action: %d", my_instance->action);
                ss_dassert(false);
                break;
            }

            if (my_instance->log_match != FW_LOG_NONE)
            {
                char *sql;
                int len;
                if (modutil_extract_SQL(analyzed_queue, &sql, &len))
                {
                    len = MXS_MIN(len, FW_MAX_SQL_LEN);
                    if (match && my_instance->log_match & FW_LOG_MATCH)
                    {
                        ss_dassert(rname);
                        MXS_NOTICE("[%s] Rule '%s' for '%s' matched by %s@%s: %.*s",
                                   dcb->service->name, rname, user->name(),
                                   dcb->user, dcb->remote, len, sql);
                    }
                    else if (!match && my_instance->log_match & FW_LOG_NO_MATCH)
                    {
                        MXS_NOTICE("[%s] Query for '%s' by %s@%s was not matched: %.*s",
                                   dcb->service->name, user->name(), dcb->user,
                                   dcb->remote, len, sql);
                    }
                }
            }

            MXS_FREE(rname);
        }
        /** If the instance is in whitelist mode, only users that have a rule
         * defined for them are allowed */
        else if (my_instance->action != FW_ACTION_ALLOW)
        {
            query_ok = true;
        }

        if (query_ok)
        {
            rval = my_session->down.routeQuery(my_session->down.instance,
                                               my_session->down.session, queue);
        }
        else
        {
            GWBUF* forward = gen_dummy_error(my_session, my_session->errmsg);
            gwbuf_free(queue);
            MXS_FREE(my_session->errmsg);
            my_session->errmsg = NULL;
            rval = dcb->func.write(dcb, forward);
        }
    }

    return rval;
}

/**
 * Diagnostics routine
 *
 * Prints the connection details and the names of the exchange,
 * queue and the routing key.
 *
 * @param   instance    The filter instance
 * @param   fsession    Filter session, may be NULL
 * @param   dcb     The DCB for diagnostic output
 */
static void
diagnostic(MXS_FILTER *instance, MXS_FILTER_SESSION *fsession, DCB *dcb)
{
    FW_INSTANCE *my_instance = (FW_INSTANCE *) instance;

    dcb_printf(dcb, "Firewall Filter\n");
    dcb_printf(dcb, "Rule, Type, Times Matched\n");

    for (RuleList::const_iterator it = this_thread.rules.begin(); it != this_thread.rules.end(); it++)
    {
        const SRule& rule = *it;
        char buf[rule->name.length() + 200];
        print_rule(rule.get(), buf);
        dcb_printf(dcb, "%s\n", buf);
    }
}

/**
 * Diagnostics routine
 *
 * Prints the connection details and the names of the exchange,
 * queue and the routing key.
 *
 * @param   instance    The filter instance
 * @param   fsession    Filter session, may be NULL
 * @param   dcb     The DCB for diagnostic output
 */
static json_t* diagnostic_json(const MXS_FILTER *instance, const MXS_FILTER_SESSION *fsession)
{
    return rules_to_json(this_thread.rules);
}

/**
 * Capability routine.
 *
 * @return The capabilities of the filter.
 */
static uint64_t getCapabilities(MXS_FILTER* instance)
{
    return RCAP_TYPE_NONE;
}
