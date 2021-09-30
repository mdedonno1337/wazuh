/* Copyright (C) 2015-2021, Wazuh Inc.
 * All right reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
*/

#ifndef WIN32

#include "wazuh_modules/wmodules.h"
#include "wazuh_modules/wm_gcp.h"

static const char *XML_ENABLED = "enabled";
static const char *XML_PROJECT_ID = "project_id";
static const char *XML_SUBSCRIPTION_NAME = "subscription_name";
static const char *XML_CREDENTIALS_FILE = "credentials_file";
static const char *XML_MAX_MESSAGES = "max_messages";
static const char *XML_NUM_THREADS = "num_threads";
static const char *XML_PULL_ON_START = "pull_on_start";
static const char *XML_LOGGING = "logging";
static const char *XML_RUN_ON_START = "run_on_start";
static const char *XML_BUCKET = "bucket";
static const char *XML_BUCKET_TYPE = "type";
static const char *XML_BUCKET_NAME = "name";
static const char *XML_PREFIX = "path";
static const char *XML_ONLY_LOGS_AFTER = "only_logs_after";
static const char *XML_REMOVE_FROM_BUCKET = "remove_from_bucket";

static const char *ACCESS_LOGS_BUCKET_TYPE = "access_logs";

static short eval_bool(const char *str) {
    return !str ? OS_INVALID : !strcmp(str, "yes") ? 1 : !strcmp(str, "no") ? 0 : OS_INVALID;
}

/* Read XML configuration */

int wm_gcp_pubsub_read(xml_node **nodes, wmodule *module) {
    unsigned int i;
    wm_gcp_pubsub *gcp;

    if (!module->data) {
        os_calloc(1, sizeof(wm_gcp_pubsub), gcp);
        gcp->enabled = 1;
        gcp->max_messages = 100;
        gcp->num_threads = 1;
        gcp->project_id = NULL;
        sched_scan_init(&(gcp->scan_config));
        gcp->scan_config.interval = WM_GCP_DEF_INTERVAL;
        gcp->subscription_name = NULL;
        gcp->credentials_file = NULL;
        gcp->pull_on_start = 1;
        gcp->logging = 2;
        module->context = &WM_GCP_PUBSUB_CONTEXT;
        module->tag = strdup(module->context->name);
        module->data = gcp;
    }

    gcp = module->data;

    if (!nodes) {
        mwarn("Empty configuration at module '%s'.", WM_GCP_PUBSUB_CONTEXT.name);
        return OS_INVALID;
    }

    for (i = 0; nodes[i]; i++) {
        if (!nodes[i]->element) {
            merror(XML_ELEMNULL);
            return OS_INVALID;
        }
        else if (!strcmp(nodes[i]->element, XML_ENABLED)) {
            int enabled = eval_bool(nodes[i]->content);

            if(enabled == OS_INVALID){
                merror("Invalid content for tag '%s'", XML_ENABLED);
                return OS_INVALID;
            }

            gcp->enabled = enabled;
        }
        else if (!strcmp(nodes[i]->element, XML_PROJECT_ID)) {
            if (strlen(nodes[i]->content) == 0) {
                merror("Empty content for tag '%s' at module '%s'", XML_PROJECT_ID, WM_GCP_PUBSUB_CONTEXT.name);
                return OS_INVALID;
            }
            os_strdup(nodes[i]->content, gcp->project_id);
        }
        else if (!strcmp(nodes[i]->element, XML_SUBSCRIPTION_NAME)) {
            if (strlen(nodes[i]->content) == 0) {
                merror("Empty content for tag '%s' at module '%s'", XML_SUBSCRIPTION_NAME, WM_GCP_PUBSUB_CONTEXT.name);
                return OS_INVALID;
            }
            os_strdup(nodes[i]->content, gcp->subscription_name);
        }
        else if (!strcmp(nodes[i]->element, XML_CREDENTIALS_FILE)) {
            if(strlen(nodes[i]->content) >= PATH_MAX) {
                merror("File path is too long. Max path length is %d.", PATH_MAX);
                return OS_INVALID;
            } else if (strlen(nodes[i]->content) == 0) {
                merror("Empty content for tag '%s' at module '%s'", XML_CREDENTIALS_FILE, WM_GCP_PUBSUB_CONTEXT.name);
                return OS_INVALID;
            }

            char realpath_buffer[PATH_MAX] = {0};

            if(nodes[i]->content[0] == '/') {
                sprintf(realpath_buffer, "%s", nodes[i]->content);
            } else {
                const char * const realpath_buffer_ref = realpath(nodes[i]->content, realpath_buffer);
                if (!realpath_buffer_ref) {
                    mwarn("File '%s' from tag '%s' not found.", realpath_buffer, XML_CREDENTIALS_FILE);
                    return OS_INVALID;
                }
            }

            // Beware of IsFile inverted, twisted logic.
            if (IsFile(realpath_buffer)) {
                mwarn("File '%s' not found. Check your configuration.", realpath_buffer);
                return OS_INVALID;
            }

            os_strdup(realpath_buffer, gcp->credentials_file);
        }
        else if (!strcmp(nodes[i]->element, XML_MAX_MESSAGES)) {
            if (strlen(nodes[i]->content) == 0) {
                merror("Empty content for tag '%s'", XML_MAX_MESSAGES);
                return OS_INVALID;
            }

            unsigned int j;
            for(j=0; j < strlen(nodes[i]->content); j++) {
                if (!isdigit(nodes[i]->content[j])) {
                    merror("Tag '%s' from the '%s' module should not have an alphabetic character.", XML_MAX_MESSAGES, WM_GCP_PUBSUB_CONTEXT.name);
                    return OS_INVALID;
                }
            }

            char *endptr;
            gcp->max_messages = strtoul(nodes[i]->content, &endptr, 0);
        }
        else if (!strcmp(nodes[i]->element, XML_NUM_THREADS)) {
            if (strlen(nodes[i]->content) == 0) {
                merror("Empty content for tag '%s'", XML_NUM_THREADS);
                return OS_INVALID;
            }

            unsigned int j;
            for(j=0; j < strlen(nodes[i]->content); j++) {
                if (!isdigit(nodes[i]->content[j])) {
                    merror("Tag '%s' from the '%s' module should not have an alphabetic character.", XML_NUM_THREADS, WM_GCP_CONTEXT.name);
                    return OS_INVALID;
                }
            }

            char *endptr;
            gcp->num_threads = strtoul(nodes[i]->content, &endptr, 0);
        }
        else if (!strcmp(nodes[i]->element, XML_PULL_ON_START)) {
            int pull_on_start = eval_bool(nodes[i]->content);

            if(pull_on_start == OS_INVALID){
                merror("Invalid content for tag '%s'", XML_PULL_ON_START);
                return OS_INVALID;
            }

            gcp->pull_on_start = pull_on_start;
        }
        else if (!strcmp(nodes[i]->element, XML_LOGGING)) {
            if (!strcmp(nodes[i]->content, "disabled")) {
                gcp->logging = 0;
            } else if (!strcmp(nodes[i]->content, "debug")) {
                gcp->logging = 1;
            } else if (!strcmp(nodes[i]->content, "info")) {
                gcp->logging = 2;
            } else if (!strcmp(nodes[i]->content, "warning")) {
                gcp->logging = 3;
            } else if (!strcmp(nodes[i]->content, "error")) {
                gcp->logging = 4;
            } else if (!strcmp(nodes[i]->content, "critical")) {
                gcp->logging = 5;
            } else if (strlen(nodes[i]->content) == 0) {
                merror("Empty content for tag '%s'", XML_LOGGING);
                return OS_INVALID;
            } else {
                merror("Invalid content for tag '%s'", XML_LOGGING);
                return OS_INVALID;
            }
        } 
        else if (is_sched_tag(nodes[i]->element)) {
            // Do nothing
        } else {
            merror("No such tag '%s' at module '%s'.", nodes[i]->element, WM_GCP_PUBSUB_CONTEXT.name);
            return OS_INVALID;
        }
        
    }

    const int sched_read = sched_scan_read(&(gcp->scan_config), nodes, module->context->name);
    if ( sched_read != 0 ) {
        return OS_INVALID;
    }

    if (!gcp->project_id) {
        merror("No value defined for tag '%s' in module '%s'", XML_PROJECT_ID, WM_GCP_PUBSUB_CONTEXT.name);
        return OS_INVALID;
    }

    if (!gcp->subscription_name) {
        merror("No value defined for tag '%s' in module '%s'", XML_SUBSCRIPTION_NAME, WM_GCP_PUBSUB_CONTEXT.name);
        return OS_INVALID;
    }

    if (!gcp->credentials_file) {
        merror("No value defined for tag '%s' in module '%s'", XML_CREDENTIALS_FILE, WM_GCP_PUBSUB_CONTEXT.name);
        return OS_INVALID;
    }

    return 0;
}
int wm_gcp_bucket_read(const OS_XML *xml, xml_node **nodes, wmodule *module) {
    unsigned int i;
    unsigned int j;
    xml_node **children = NULL;
    wm_gcp_bucket_base *gcp;
    wm_gcp_bucket *cur_bucket = NULL;

    if (!module->data) {
        os_calloc(1, sizeof(wm_gcp_bucket_base), gcp);
        gcp->enabled = 1;
        sched_scan_init(&(gcp->scan_config));
        gcp->scan_config.interval = WM_GCP_DEF_INTERVAL;
        gcp->run_on_start = 1;
        gcp->logging = 2;
        module->context = &WM_GCP_BUCKET_CONTEXT;
        module->tag = strdup(module->context->name);
        module->data = gcp;
    }

    gcp = module->data;

    if (!nodes) {
        mwarn("Empty configuration at module '%s'.", WM_GCP_BUCKET_CONTEXT.name);
        return OS_INVALID;
    }

    for (i = 0; nodes[i]; i++) {
        if (!nodes[i]->element) {
            merror(XML_ELEMNULL);
            return OS_INVALID;
        }
        else if (!strcmp(nodes[i]->element, XML_ENABLED)) {
            int enabled = eval_bool(nodes[i]->content);

            if(enabled == OS_INVALID){
                merror("Invalid content for tag '%s'", XML_ENABLED);
                return OS_INVALID;
            }

            gcp->enabled = enabled;
        }
        else if (!strcmp(nodes[i]->element, XML_RUN_ON_START)) {
            int run_on_start = eval_bool(nodes[i]->content);

            if(run_on_start == OS_INVALID){
                merror("Invalid content for tag '%s'", XML_RUN_ON_START);
                return OS_INVALID;
            }

            gcp->run_on_start = run_on_start;
        }
        else if (!strcmp(nodes[i]->element, XML_LOGGING)) {
            if (!strcmp(nodes[i]->content, "disabled")) {
                gcp->logging = 0;
            } else if (!strcmp(nodes[i]->content, "debug")) {
                gcp->logging = 1;
            } else if (!strcmp(nodes[i]->content, "info")) {
                gcp->logging = 2;
            } else if (!strcmp(nodes[i]->content, "warning")) {
                gcp->logging = 3;
            } else if (!strcmp(nodes[i]->content, "error")) {
                gcp->logging = 4;
            } else if (!strcmp(nodes[i]->content, "critical")) {
                gcp->logging = 5;
            } else if (strlen(nodes[i]->content) == 0) {
                merror("Empty content for tag '%s'", XML_LOGGING);
                return OS_INVALID;
            } else {
                merror("Invalid content for tag '%s'", XML_LOGGING);
                return OS_INVALID;
            }
        } else if (!strcmp(nodes[i]->element, XML_BUCKET)) {
            mtdebug2(WM_GCP_BUCKET_LOGTAG, "Found a bucket tag");
            // Create bucket node
            if (cur_bucket) {
                os_calloc(1, sizeof(wm_gcp_bucket), cur_bucket->next);
                cur_bucket = cur_bucket->next;
                mtdebug2(WM_GCP_BUCKET_LOGTAG, "Creating another bucket structure");
            } else {
                // First bucket
                os_calloc(1, sizeof(wm_gcp_bucket), cur_bucket);
                gcp->buckets = cur_bucket;
                mtdebug2(WM_GCP_BUCKET_LOGTAG, "Creating first bucket structure");
            }

            // Expand bucket Child Nodes

            if (!(children = OS_GetElementsbyNode(xml, nodes[i]))) {
                continue;
            }

            // type is an attribute of the bucket tag
            if (!strcmp(*nodes[i]->attributes, XML_BUCKET_TYPE)) {
                if (!strcmp(*nodes[i]->values, ACCESS_LOGS_BUCKET_TYPE)) {
                    os_strdup(*nodes[i]->values, cur_bucket->type);
                } else {
                    mterror(WM_GCP_BUCKET_LOGTAG, "Invalid bucket type '%s'. Valid one is '%s'",
                        *nodes[i]->values, ACCESS_LOGS_BUCKET_TYPE);
                    OS_ClearNode(children);
                    return OS_INVALID;
                }
            } else {
                mterror(WM_GCP_BUCKET_LOGTAG, "Attribute name '%s' is not valid. The valid one is '%s'.", *nodes[i]->attributes, XML_BUCKET_TYPE);
                OS_ClearNode(children);
                return OS_INVALID;
            }

            mtdebug2(WM_GCP_BUCKET_LOGTAG, "Loop thru child nodes");
            for (j = 0; children[j]; j++) {

                mtdebug2(WM_GCP_BUCKET_LOGTAG, "Parse child node: %s", children[j]->element);

                if (!children[j]->element) {
                    merror(XML_ELEMNULL);
                    OS_ClearNode(children);
                    return OS_INVALID;
                }

                // Start
                if (!strcmp(children[j]->element, XML_BUCKET_NAME)) {
                    if (strlen(children[j]->content) == 0) {
                        merror("Empty content for tag '%s' at module '%s'.", XML_BUCKET_NAME, WM_GCP_BUCKET_CONTEXT.name);
                        OS_ClearNode(children);
                        return OS_INVALID;
                    }
                    free(cur_bucket->bucket);
                    os_strdup(children[j]->content, cur_bucket->bucket);
                } else if (!strcmp(children[j]->element, XML_REMOVE_FROM_BUCKET)) {
                    if (!strcmp(children[j]->content, "yes")) {
                        cur_bucket->remove_from_bucket = 1;
                    } else if (!strcmp(children[j]->content, "no")) {
                        cur_bucket->remove_from_bucket = 0;
                    } else {
                        merror("Invalid content for tag '%s' at module '%s'.", XML_REMOVE_FROM_BUCKET, WM_GCP_BUCKET_CONTEXT.name);
                        OS_ClearNode(children);
                        return OS_INVALID;
                    }
                } else if (!strcmp(children[j]->element, XML_PREFIX)) {
                    if (strlen(children[j]->content) != 0) {
                        free(cur_bucket->prefix);
                        os_strdup(children[j]->content, cur_bucket->prefix);
                    }
                } else if (!strcmp(children[j]->element, XML_ONLY_LOGS_AFTER)) {
                    if (strlen(children[j]->content) != 0) {
                        free(cur_bucket->only_logs_after);
                        os_strdup(children[j]->content, cur_bucket->only_logs_after);
                    }
                } else if (!strcmp(children[j]->element, XML_CREDENTIALS_FILE)) {
                    if(strlen(children[j]->content) >= PATH_MAX) {
                        merror("File path is too long. Max path length is %d.", PATH_MAX);
                        return OS_INVALID;
                    } else if (strlen(children[j]->content) == 0) {
                        merror("Empty content for tag '%s' at module '%s'", XML_CREDENTIALS_FILE, WM_GCP_BUCKET_CONTEXT.name);
                        return OS_INVALID;
                    }

                    char realpath_buffer[PATH_MAX] = {0};

                    if(children[j]->content[0] == '/') {
                        sprintf(realpath_buffer, "%s", children[j]->content);
                    } else {
                        const char * const realpath_buffer_ref = realpath(children[j]->content, realpath_buffer);
                        if (!realpath_buffer_ref) {
                            mwarn("File '%s' from tag '%s' not found.", realpath_buffer, XML_CREDENTIALS_FILE);
                            return OS_INVALID;
                        }
                    }

                    // Beware of IsFile inverted, twisted logic.
                    if (IsFile(realpath_buffer)) {
                        mwarn("File '%s' not found. Check your configuration.", realpath_buffer);
                        return OS_INVALID;
                    }

                    os_strdup(realpath_buffer, cur_bucket->credentials_file);
                } else {
                    merror("No such child tag '%s' of bucket at module '%s'.", children[j]->element, WM_GCP_BUCKET_CONTEXT.name);
                    OS_ClearNode(children);
                    return OS_INVALID;
                }
            }
            OS_ClearNode(children);
        } else if (is_sched_tag(nodes[i]->element)) {
            // Do nothing
        } else {
            merror("No such tag '%s' at module '%s'.", nodes[i]->element, WM_GCP_BUCKET_CONTEXT.name);
            return OS_INVALID;
        }

    }

    const int sched_read = sched_scan_read(&(gcp->scan_config), nodes, module->context->name);
    if ( sched_read != 0 ) {
        return OS_INVALID;
    }

    return 0;
}

#endif /* WIN32 */
