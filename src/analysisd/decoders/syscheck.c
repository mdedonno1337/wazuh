/* Copyright (C) 2015-2019, Wazuh Inc.
 * Copyright (C) 2009 Trend Micro Inc.
 * All right reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

/* Syscheck decoder */

#include "eventinfo.h"
#include "os_regex/os_regex.h"
#include "config.h"
#include "alerts/alerts.h"
#include "decoder.h"
#include "syscheck_op.h"
#include "wazuh_modules/wmodules.h"
#include "os_net/os_net.h"
#include "wazuhdb_op.h"

#ifdef UNIT_TESTING
/* Remove static qualifier when testing */
#define static

/* Replace assert with mock_assert */
extern void mock_assert(const int result, const char* const expression,
                        const char * const file, const int line);
#undef assert
#define assert(expression) \
    mock_assert((int)(expression), #expression, __FILE__, __LINE__);
#endif

// Add events into sqlite DB for FIM
static int fim_db_search (char *f_name, char *c_sum, char *w_sum, Eventinfo *lf, _sdb *sdb);

// Build FIM alert
static int fim_alert (char *f_name, sk_sum_t *oldsum, sk_sum_t *newsum, Eventinfo *lf, _sdb *localsdb);
// Build fileds whodata alert
static void InsertWhodata (const sk_sum_t * sum, _sdb *localsdb);
// Compare the first common fields between sum strings
static int SumCompare (const char *s1, const char *s2);
// Check for exceed num of changes
static int fim_check_changes (int saved_frequency, long saved_time, Eventinfo *lf);
// Send control message to wazuhdb
static int fim_control_msg (char *key, time_t value, Eventinfo *lf, _sdb *sdb);
//Update field date at last event generated
int fim_update_date (char *file, Eventinfo *lf, _sdb *sdb);
// Clean for old entries
int fim_database_clean (Eventinfo *lf, _sdb *sdb);
// Clean sdb memory
void sdb_clean(_sdb *localsdb);
// Get timestamp for last scan from wazuhdb
int fim_get_scantime (long *ts, Eventinfo *lf, _sdb *sdb, const char *param);

// Process fim alert
static int fim_process_alert(_sdb *sdb, Eventinfo *lf, cJSON *event);

// Generate fim alert
static int fim_generate_alert(Eventinfo *lf, char *mode, char *event_type,
        cJSON *attributes, cJSON *old_attributes, cJSON *audit);

// Send save query to Wazuh DB
static void fim_send_db_save(_sdb * sdb, const char * agent_id, cJSON * data);

// Send delete query to Wazuh DB
void fim_send_db_delete(_sdb * sdb, const char * agent_id, const char * path);

// Send a query to Wazuh DB
void fim_send_db_query(int * sock, const char * query);

// Build change comment
static size_t fim_generate_comment(char * str, long size, const char * format, const char * a1, const char * a2);

// Process scan info event
static void fim_process_scan_info(_sdb * sdb, const char * agent_id, fim_scan_event event, cJSON * data);

// Extract the file attributes from the JSON object
static int fim_fetch_attributes(cJSON *new_attrs, cJSON *old_attrs, Eventinfo *lf);
static int fim_fetch_attributes_state(cJSON *attr, Eventinfo *lf, char new_state);

// Replace the coded fields with the decoded ones in the checksum
static void fim_adjust_checksum(sk_sum_t *newsum, char **checksum);

// Mutexes
static pthread_mutex_t control_msg_mutex = PTHREAD_MUTEX_INITIALIZER;

static int decode_event_add;
static int decode_event_delete;
static int decode_event_modify;

// Initialize the necessary information to process the syscheck information
// LCOV_EXCL_START
int fim_init(void) {
    //Create hash table for agent information
    fim_agentinfo = OSHash_Create();
    decode_event_add = getDecoderfromlist(SYSCHECK_NEW);
    decode_event_modify = getDecoderfromlist(SYSCHECK_MOD);
    decode_event_delete = getDecoderfromlist(SYSCHECK_DEL);
    if (fim_agentinfo == NULL) return 0;
    return 1;
}

// Initialize the necessary information to process the syscheck information
void sdb_init(_sdb *localsdb, OSDecoderInfo *fim_decoder) {
    localsdb->db_err = 0;
    localsdb->socket = -1;

    sdb_clean(localsdb);

    // Create decoder
    fim_decoder->id = getDecoderfromlist(SYSCHECK_MOD);
    fim_decoder->name = SYSCHECK_MOD;
    fim_decoder->type = OSSEC_RL;
    fim_decoder->fts = 0;

    os_calloc(Config.decoder_order_size, sizeof(char *), fim_decoder->fields);
    fim_decoder->fields[FIM_FILE] = "file";
    fim_decoder->fields[FIM_SIZE] = "size";
    fim_decoder->fields[FIM_HARD_LINKS] = "hard_links";
    fim_decoder->fields[FIM_PERM] = "perm";
    fim_decoder->fields[FIM_UID] = "uid";
    fim_decoder->fields[FIM_GID] = "gid";
    fim_decoder->fields[FIM_MD5] = "md5";
    fim_decoder->fields[FIM_SHA1] = "sha1";
    fim_decoder->fields[FIM_UNAME] = "uname";
    fim_decoder->fields[FIM_GNAME] = "gname";
    fim_decoder->fields[FIM_MTIME] = "mtime";
    fim_decoder->fields[FIM_INODE] = "inode";
    fim_decoder->fields[FIM_SHA256] = "sha256";
    fim_decoder->fields[FIM_DIFF] = "changed_content";
    fim_decoder->fields[FIM_ATTRS] = "win_attributes";
    fim_decoder->fields[FIM_CHFIELDS] = "changed_fields";
    fim_decoder->fields[FIM_TAG] = "tag";
    fim_decoder->fields[FIM_SYM_PATH] = "symbolic_path";

    fim_decoder->fields[FIM_USER_ID] = "user_id";
    fim_decoder->fields[FIM_USER_NAME] = "user_name";
    fim_decoder->fields[FIM_GROUP_ID] = "group_id";
    fim_decoder->fields[FIM_GROUP_NAME] = "group_name";
    fim_decoder->fields[FIM_PROC_NAME] = "process_name";
    fim_decoder->fields[FIM_AUDIT_ID] = "audit_uid";
    fim_decoder->fields[FIM_AUDIT_NAME] = "audit_name";
    fim_decoder->fields[FIM_EFFECTIVE_UID] = "effective_uid";
    fim_decoder->fields[FIM_EFFECTIVE_NAME] = "effective_name";
    fim_decoder->fields[FIM_PPID] = "ppid";
    fim_decoder->fields[FIM_PROC_ID] = "process_id";
}

// Initialize the necessary information to process the syscheck information
void sdb_clean(_sdb *localsdb) {
    *localsdb->comment = '\0';
    *localsdb->size = '\0';
    *localsdb->perm = '\0';
    *localsdb->attrs = '\0';
    *localsdb->sym_path = '\0';
    *localsdb->owner = '\0';
    *localsdb->gowner = '\0';
    *localsdb->md5 = '\0';
    *localsdb->sha1 = '\0';
    *localsdb->sha256 = '\0';
    *localsdb->mtime = '\0';
    *localsdb->inode = '\0';

    // Whodata fields
    *localsdb->user_id = '\0';
    *localsdb->user_name = '\0';
    *localsdb->group_id = '\0';
    *localsdb->group_name = '\0';
    *localsdb->process_name = '\0';
    *localsdb->audit_uid = '\0';
    *localsdb->audit_name = '\0';
    *localsdb->effective_uid = '\0';
    *localsdb->effective_name = '\0';
    *localsdb->ppid = '\0';
    *localsdb->process_id = '\0';
}

/* Special decoder for syscheck
 * Not using the default decoding lib for simplicity
 * and to be less resource intensive
 */
int DecodeSyscheck(Eventinfo *lf, _sdb *sdb)
{
    char *c_sum;
    char *w_sum = NULL;
    char *f_name;

    /* Every syscheck message must be in the following format (OSSEC - Wazuh v3.10):
     * 'checksum' 'filename'
     * or
     * 'checksum'!'extradata' 'filename'
     * or
     *                                             |v2.1       |v3.4  |v3.4         |v3.6  |v3.9               |v1.0
     *                                             |->         |->    |->           |->   |->                  |->
     * "size:permision:uid:gid:md5:sha1:uname:gname:mtime:inode:sha256!w:h:o:d:a:t:a:tags:symbolic_path:silent filename\nreportdiff"
     *  ^^^^^^^^^^^^^^^^^^^^^^^^^^^checksum^^^^^^^^^^^^^^^^^^^^^^^^^^^!^^^^^^^^^^^^^^extradata^^^^^^^^^^^^^^^^ filename\n^^^diff^^^
     */

    sdb_clean(sdb);

    f_name = wstr_chr(lf->log, ' ');
    if (f_name == NULL) {
        mdebug2("Scan's control message agent '%s': '%s'", lf->log, lf->agent_id);
        switch (fim_control_msg(lf->log, lf->time.tv_sec, lf, sdb)) {
        case -2:
        case -1:
            return (-1);
        case 0:
            merror(FIM_INVALID_MESSAGE);
            return (-1);
        default:
            return(0);
        }
    }

    // Zero to get the check sum
    *f_name = '\0';
    f_name++;

    //Change in Windows paths all slashes for backslashes for compatibility agent<3.4 with manager>=3.4
    normalize_path(f_name);

    // Get diff
    char *diff = strchr(f_name, '\n');
    if (diff) {
        *(diff++) = '\0';
        os_strdup(diff, lf->diff);
        os_strdup(diff, lf->fields[FIM_DIFF].value);
    }

    // Checksum is at the beginning of the log
    c_sum = lf->log;

    // Get w_sum
    if (w_sum = wstr_chr(c_sum, '!'), w_sum) {
        *(w_sum++) = '\0';
    }

    // Search for file changes
    return (fim_db_search(f_name, c_sum, w_sum, lf, sdb));
}


int fim_db_search(char *f_name, char *c_sum, char *w_sum, Eventinfo *lf, _sdb *sdb) {
    int decode_newsum = 0;
    int db_result = 0;
    int changes = 0;
    int i = 0;
    char *ttype[OS_SIZE_128];
    char *wazuhdb_query = NULL;
    char *new_check_sum = NULL;
    char *old_check_sum = NULL;
    char *response = NULL;
    char *check_sum = NULL;
    char *sym_path = NULL;
    sk_sum_t oldsum = { .size = NULL };
    sk_sum_t newsum = { .size = NULL };
    time_t *end_first_scan = NULL;
    time_t end_scan = 0;

    memset(&oldsum, 0, sizeof(sk_sum_t));
    memset(&newsum, 0, sizeof(sk_sum_t));

    os_calloc(OS_SIZE_6144 + 1, sizeof(char), wazuhdb_query);
    os_strdup(c_sum, new_check_sum);

    snprintf(wazuhdb_query, OS_SIZE_6144, "agent %s syscheck load %s", lf->agent_id, f_name);

    os_calloc(OS_SIZE_6144, sizeof(char), response);
    db_result = wdbc_query_ex(&sdb->socket, wazuhdb_query, response, OS_SIZE_6144);

    // Fail trying load info from DDBB

    switch (db_result) {
    case -2:
        merror("FIM decoder: Bad load query: '%s'.", wazuhdb_query);
        // Fallthrough
    case -1:
        os_free(lf->data);
        goto exit_fail;
    }

    if(check_sum = wstr_chr(response, ' '), !check_sum) {
        merror("FIM decoder: Bad response: '%s' '%s'.", wazuhdb_query, response);
        goto exit_fail;
    }
    *(check_sum++) = '\0';

    //extract changes and date_alert fields only available from wazuh_db
    sk_decode_extradata(&oldsum, check_sum);

    os_strdup(check_sum, old_check_sum);
    mdebug2("Agent '%s' File '%s'", lf->agent_id, f_name);
    mdebug2("Agent '%s' Old checksum '%s'", lf->agent_id, old_check_sum);
    mdebug2("Agent '%s' New checksum '%s'", lf->agent_id, new_check_sum);

    if (decode_newsum = sk_decode_sum(&newsum, c_sum, w_sum), decode_newsum != -1) {
        InsertWhodata(&newsum, sdb);
    }

    fim_adjust_checksum(&newsum, &new_check_sum);

    // Checksum match, we can just return and keep going
    if (SumCompare(old_check_sum, new_check_sum) == 0) {
        mdebug1("Agent '%s' Alert discarded '%s' same check_sum", lf->agent_id, f_name);
        fim_update_date (f_name, lf, sdb);
        goto exit_ok;
    }

    wazuhdb_query[0] = '\0';
    switch (decode_newsum) {
        case 1: // File deleted
            lf->event_type = FIM_DELETED;

            if(!*old_check_sum){
                mdebug2("Agent '%s' Alert already reported (double delete alert)", lf->agent_id);
                goto exit_ok;
            }

            snprintf(wazuhdb_query, OS_SIZE_6144, "agent %s syscheck delete %s",
                    lf->agent_id,
                    f_name
            );

            db_result = wdbc_query_ex(&sdb->socket, wazuhdb_query, response, OS_SIZE_6144);

            switch (db_result) {
            case -2:
                merror("FIM decoder: Bad delete query: '%s'.", wazuhdb_query);
                // Fallthrough
            case -1:
                goto exit_fail;
            }

            mdebug2("Agent '%s' File %s deleted from FIM DDBB", lf->agent_id, f_name);

            break;
        case 0:
            if (*old_check_sum) {
                // File modified
                lf->event_type = FIM_MODIFIED;
                changes = fim_check_changes(oldsum.changes, oldsum.date_alert, lf);
                sk_decode_sum(&oldsum, old_check_sum, NULL);

                // Alert discarded, frequency exceeded
                if (changes == -1) {
                    mdebug1("Agent '%s' Alert discarded '%s' frequency exceeded", lf->agent_id, f_name);
                    goto exit_ok;
                }
            } else {
                // File added
                lf->event_type = FIM_ADDED;
            }

            if (strstr(lf->location, "syscheck-registry")) {
                *ttype = "registry";
            } else {
                *ttype = "file";
            }

            if (newsum.symbolic_path) {
                sym_path = escape_syscheck_field(newsum.symbolic_path);
            }

            // We need to escape the checksum because it will have
            // spaces if the event comes from Windows
            char *checksum_esc = wstr_replace(new_check_sum, " ", "\\ ");
            snprintf(wazuhdb_query, OS_SIZE_6144, "agent %s syscheck save %s %s!%d:%ld:%s %s",
                    lf->agent_id,
                    *ttype,
                    checksum_esc,
                    changes,
                    lf->time.tv_sec,
                    sym_path ? sym_path : "",
                    f_name
            );
            os_free(sym_path);
            os_free(checksum_esc);
            db_result = wdbc_query_ex(&sdb->socket, wazuhdb_query, response, OS_SIZE_6144);

            switch (db_result) {
            case -2:
                merror("FIM decoder: Bad save/update query: '%s'.", wazuhdb_query);
                // Fallthrough
            case -1:
                goto exit_fail;
            }

            mdebug2("Agent '%s' File %s saved/updated in FIM DDBB", lf->agent_id, f_name);

            if(end_first_scan = (time_t*)OSHash_Get_ex(fim_agentinfo, lf->agent_id), end_first_scan == NULL) {
                fim_get_scantime(&end_scan, lf, sdb, "end_scan");
                os_calloc(1, sizeof(time_t), end_first_scan);
                *end_first_scan = end_scan;
                int res;
                if(res = OSHash_Add_ex(fim_agentinfo, lf->agent_id, end_first_scan), res != 2) {
                    os_free(end_first_scan);
                    if(res == 0) {
                        merror("Unable to add scan_info to hash table for agent: %s", lf->agent_id);
                    }
                }
            } else {
                end_scan = *end_first_scan;
            }

            if(lf->event_type == FIM_ADDED) {
                if(end_scan == 0) {
                    mdebug2("Agent '%s' Alert discarded, first scan. File '%s'", lf->agent_id, f_name);
                    goto exit_ok;
                } else if(lf->time.tv_sec < end_scan) {
                    mdebug2("Agent '%s' Alert discarded, first scan (delayed event). File '%s'", lf->agent_id, f_name);
                    goto exit_ok;
                } else if(Config.syscheck_alert_new == 0) {
                    mdebug2("Agent '%s' Alert discarded (alert_new_files = no). File '%s'", lf->agent_id, f_name);
                    goto exit_ok;
                }
            }

            mdebug2("Agent '%s' End end_scan is '%ld' (lf->time: '%ld')", lf->agent_id, end_scan, lf->time.tv_sec);
            break;

        default: // Error in fim check sum
            mwarn("at fim_db_search: Agent '%s' Couldn't decode fim sum '%s' from file '%s'.",
                    lf->agent_id, new_check_sum, f_name);
            goto exit_fail;
    }

    if (!newsum.silent) {
        sk_fill_event(lf, f_name, &newsum);

        /* Dyanmic Fields */
        lf->nfields = FIM_NFIELDS;
        for (i = 0; i < FIM_NFIELDS; i++) {
            os_strdup(lf->decoder_info->fields[i], lf->fields[i].key);
        }

        if(fim_alert(f_name, &oldsum, &newsum, lf, sdb) == -1) {
            //No changes in checksum
            goto exit_ok;
        }
        sk_sum_clean(&newsum);
        sk_sum_clean(&oldsum);
        os_free(response);
        os_free(new_check_sum);
        os_free(old_check_sum);
        os_free(wazuhdb_query);

        return (1);
    } else {
        mdebug2("Ignoring FIM event on '%s'.", f_name);
    }

exit_ok:
    sk_sum_clean(&newsum);
    sk_sum_clean(&oldsum);
    os_free(response);
    os_free(new_check_sum);
    os_free(old_check_sum);
    os_free(wazuhdb_query);
    return (0);

exit_fail:
    sk_sum_clean(&newsum);
    sk_sum_clean(&oldsum);
    os_free(response);
    os_free(new_check_sum);
    os_free(old_check_sum);
    os_free(wazuhdb_query);
    return (-1);
}

int fim_alert (char *f_name, sk_sum_t *oldsum, sk_sum_t *newsum, Eventinfo *lf, _sdb *localsdb) {
    int changes = 0;
    char msg_type[OS_FLSIZE];
    char buf_ptr[26];

    switch (lf->event_type) {
        case FIM_DELETED:
            snprintf(msg_type, sizeof(msg_type), "was deleted.");
            lf->decoder_info->id = decode_event_delete;
            lf->decoder_syscheck_id = lf->decoder_info->id;
            lf->decoder_info->name = SYSCHECK_MOD;
            changes=1;
            break;
        case FIM_ADDED:
            snprintf(msg_type, sizeof(msg_type), "was added.");
            lf->decoder_info->id = decode_event_add;
            lf->decoder_syscheck_id = lf->decoder_info->id;
            lf->decoder_info->name = SYSCHECK_NEW;
            changes=1;
            break;
        case FIM_MODIFIED:
            snprintf(msg_type, sizeof(msg_type), "checksum changed.");
            lf->decoder_info->id = decode_event_modify;
            lf->decoder_syscheck_id = lf->decoder_info->id;
            lf->decoder_info->name = SYSCHECK_MOD;
            if (oldsum->size && newsum->size) {
                if (strcmp(oldsum->size, newsum->size) == 0) {
                    localsdb->size[0] = '\0';
                } else {
                    changes = 1;
                    wm_strcat(&lf->fields[FIM_CHFIELDS].value, "size", ',');
                    snprintf(localsdb->size, OS_FLSIZE,
                             "Size changed from '%s' to '%s'\n",
                             oldsum->size, newsum->size);

                    os_strdup(oldsum->size, lf->size_before);
                }
            }

            /* Permission message */
            if (oldsum->perm && newsum->perm) {
                if (oldsum->perm == newsum->perm) {
                    localsdb->perm[0] = '\0';
                } else if (oldsum->perm > 0 && newsum->perm > 0) {
                    changes = 1;
                    wm_strcat(&lf->fields[FIM_CHFIELDS].value, "perm", ',');
                    char opstr[10];
                    char npstr[10];
                    lf->perm_before =  agent_file_perm(oldsum->perm);
                    char *new_perm =  agent_file_perm(newsum->perm);

                    strncpy(opstr, lf->perm_before, sizeof(opstr) - 1);
                    strncpy(npstr, new_perm, sizeof(npstr) - 1);
                    free(new_perm);

                    opstr[9] = npstr[9] = '\0';
                    snprintf(localsdb->perm, OS_FLSIZE, "Permissions changed from "
                             "'%9.9s' to '%9.9s'\n", opstr, npstr);
                }
            } else if (oldsum->win_perm && newsum->win_perm) { // Check for Windows permissions
                // We need to unescape the old permissions at this point
                char *unesc_perms = wstr_replace(oldsum->win_perm, "\\:", ":");
                free(oldsum->win_perm);
                oldsum->win_perm = unesc_perms;
                if (!strcmp(oldsum->win_perm, newsum->win_perm)) {
                    localsdb->perm[0] = '\0';
                } else if (*oldsum->win_perm != '\0' && *newsum->win_perm != '\0') {
                    changes = 1;
                    wm_strcat(&lf->fields[FIM_CHFIELDS].value, "perm", ',');
                    snprintf(localsdb->perm, OS_FLSIZE, "Permissions changed.\n");
                    os_strdup(oldsum->win_perm, lf->perm_before);
                }
            }

            /* Ownership message */
            if (newsum->uid && oldsum->uid) {
                if (strcmp(newsum->uid, oldsum->uid) == 0) {
                    localsdb->owner[0] = '\0';
                } else {
                    changes = 1;
                    wm_strcat(&lf->fields[FIM_CHFIELDS].value, "uid", ',');
                    if (oldsum->uname && newsum->uname) {
                        snprintf(localsdb->owner, OS_FLSIZE, "Ownership was '%s (%s)', now it is '%s (%s)'\n", oldsum->uname, oldsum->uid, newsum->uname, newsum->uid);
                        os_strdup(oldsum->uname, lf->uname_before);
                    } else {
                        snprintf(localsdb->owner, OS_FLSIZE, "Ownership was '%s', now it is '%s'\n", oldsum->uid, newsum->uid);
                    }
                    os_strdup(oldsum->uid, lf->owner_before);
                }
            }

            /* Group ownership message */
            if (newsum->gid && oldsum->gid) {
                if (strcmp(newsum->gid, oldsum->gid) == 0) {
                    localsdb->gowner[0] = '\0';
                } else {
                    changes = 1;
                    wm_strcat(&lf->fields[FIM_CHFIELDS].value, "gid", ',');
                    if (oldsum->gname && newsum->gname) {
                        snprintf(localsdb->gowner, OS_FLSIZE, "Group ownership was '%s (%s)', now it is '%s (%s)'\n", oldsum->gname, oldsum->gid, newsum->gname, newsum->gid);
                        os_strdup(oldsum->gname, lf->gname_before);
                    } else {
                        snprintf(localsdb->gowner, OS_FLSIZE, "Group ownership was '%s', now it is '%s'\n", oldsum->gid, newsum->gid);
                    }
                    os_strdup(oldsum->gid, lf->gowner_before);
                }
            }
            /* MD5 message */
            if (!*newsum->md5 || !*oldsum->md5 || strcmp(newsum->md5, oldsum->md5) == 0) {
                localsdb->md5[0] = '\0';
            } else {
                changes = 1;
                wm_strcat(&lf->fields[FIM_CHFIELDS].value, "md5", ',');
                snprintf(localsdb->md5, OS_FLSIZE, "Old md5sum was: '%s'\nNew md5sum is : '%s'\n",
                         oldsum->md5, newsum->md5);
                os_strdup(oldsum->md5, lf->md5_before);
            }

            /* SHA-1 message */
            if (!*newsum->sha1 || !*oldsum->sha1 || strcmp(newsum->sha1, oldsum->sha1) == 0) {
                localsdb->sha1[0] = '\0';
            } else {
                changes = 1;
                wm_strcat(&lf->fields[FIM_CHFIELDS].value, "sha1", ',');
                snprintf(localsdb->sha1, OS_FLSIZE, "Old sha1sum was: '%s'\nNew sha1sum is : '%s'\n",
                         oldsum->sha1, newsum->sha1);
                os_strdup(oldsum->sha1, lf->sha1_before);
            }

            /* SHA-256 message */
            if(newsum->sha256 && newsum->sha256[0] != '\0')
            {
                if(oldsum->sha256) {
                    if (strcmp(newsum->sha256, oldsum->sha256) == 0) {
                        localsdb->sha256[0] = '\0';
                    } else {
                        changes = 1;
                        wm_strcat(&lf->fields[FIM_CHFIELDS].value, "sha256", ',');
                        snprintf(localsdb->sha256, OS_FLSIZE, "Old sha256sum was: '%s'\nNew sha256sum is : '%s'\n",
                                oldsum->sha256, newsum->sha256);
                        os_strdup(oldsum->sha256, lf->sha256_before);
                    }
                } else {
                    changes = 1;
                    wm_strcat(&lf->fields[FIM_CHFIELDS].value, "sha256", ',');
                    snprintf(localsdb->sha256, OS_FLSIZE, "New sha256sum is : '%s'\n", newsum->sha256);
                }
            } else {
                localsdb->sha256[0] = '\0';
            }

            /* Modification time message */
            if (oldsum->mtime && newsum->mtime && oldsum->mtime != newsum->mtime) {
                changes = 1;
                wm_strcat(&lf->fields[FIM_CHFIELDS].value, "mtime", ',');
                char *old_ctime = strdup(ctime_r(&oldsum->mtime, buf_ptr));
                char *new_ctime = strdup(ctime_r(&newsum->mtime, buf_ptr));
                old_ctime[strlen(old_ctime) - 1] = '\0';
                new_ctime[strlen(new_ctime) - 1] = '\0';

                snprintf(localsdb->mtime, OS_FLSIZE, "Old modification time was: '%s', now it is '%s'\n", old_ctime, new_ctime);
                lf->mtime_before = oldsum->mtime;
                os_free(old_ctime);
                os_free(new_ctime);
            } else {
                localsdb->mtime[0] = '\0';
            }

            /* Inode message */
            if (oldsum->inode && newsum->inode && oldsum->inode != newsum->inode) {
                changes = 1;
                wm_strcat(&lf->fields[FIM_CHFIELDS].value, "inode", ',');
                snprintf(localsdb->inode, OS_FLSIZE, "Old inode was: '%ld', now it is '%ld'\n", oldsum->inode, newsum->inode);
                lf->inode_before = oldsum->inode;
            } else {
                localsdb->inode[0] = '\0';
            }

            /* Attributes message */
            if (oldsum->attributes && newsum->attributes
                && strcmp(oldsum->attributes, newsum->attributes)) {
                changes = 1;
                wm_strcat(&lf->fields[FIM_CHFIELDS].value, "attributes", ',');
                snprintf(localsdb->attrs, OS_SIZE_1024, "Old attributes were: '%s'\nNow they are '%s'\n", oldsum->attributes, newsum->attributes);
                os_strdup(oldsum->attributes, lf->attributes_before);
            } else {
                localsdb->attrs[0] = '\0';
            }

            break;
        default:
            return (-1);
            break;
    }

    /* Symbolic path message */
    if (newsum->symbolic_path && *newsum->symbolic_path) {
        snprintf(localsdb->sym_path, OS_FLSIZE, "Symbolic path: '%s'.\n", newsum->symbolic_path);
    } else {
        *localsdb->sym_path = '\0';
    }

    // Provide information about the file
    snprintf(localsdb->comment, OS_MAXSTR, "File"
            " '%.756s' "
            "%s\n"
            "%s"
            "%s"
            "%s"
            "%s"
            "%s"
            "%s"
            "%s"
            "%s"
            "%s"
            "%s"
            "%s",
            f_name,
            msg_type,
            localsdb->sym_path,
            localsdb->size,
            localsdb->perm,
            localsdb->owner,
            localsdb->gowner,
            localsdb->md5,
            localsdb->sha1,
            localsdb->sha256,
            localsdb->attrs,
            localsdb->mtime,
            localsdb->inode
    );
    if(!changes) {
        os_free(lf->data);
        return(-1);
    } else if (lf->fields[FIM_CHFIELDS].value != NULL) {
        wm_strcat(&lf->fields[FIM_CHFIELDS].value, ",", '\0');
    }

    // Create a new log message
    free(lf->full_log);
    os_strdup(localsdb->comment, lf->full_log);
    lf->log = lf->full_log;

    return (0);
}

void InsertWhodata(const sk_sum_t * sum, _sdb *sdb) {
    // Whodata user
    if(sum->wdata.user_id && sum->wdata.user_name && *sum->wdata.user_id != '\0') {
        snprintf(sdb->user_name, OS_FLSIZE, "(Audit) User: '%s (%s)'\n",
                sum->wdata.user_name, sum->wdata.user_id);
    } else {
        *sdb->user_name = '\0';
    }

    // Whodata effective user
    if(sum->wdata.effective_uid && sum->wdata.effective_name && *sum->wdata.effective_uid != '\0') {
        snprintf(sdb->effective_name, OS_FLSIZE, "(Audit) Effective user: '%s (%s)'\n",
                sum->wdata.effective_name, sum->wdata.effective_uid);
    } else {
        *sdb->effective_name = '\0';
    }

    // Whodata Audit user
    if(sum->wdata.audit_uid && sum->wdata.audit_name && *sum->wdata.audit_uid != '\0') {
        snprintf(sdb->audit_name, OS_FLSIZE, "(Audit) Login user: '%s (%s)'\n",
                sum->wdata.audit_name, sum->wdata.audit_uid);
    } else {
        *sdb->audit_name = '\0';
    }

    // Whodata Group
    if(sum->wdata.group_id && sum->wdata.group_name && *sum->wdata.group_id != '\0') {
        snprintf(sdb->group_name, OS_FLSIZE, "(Audit) Group: '%s (%s)'\n",
                sum->wdata.group_name, sum->wdata.group_id);
    } else {
        *sdb->group_name = '\0';
    }

    // Whodata process
    if(sum->wdata.process_id && *sum->wdata.process_id != '\0' && strcmp(sum->wdata.process_id, "0")) {
        snprintf(sdb->process_id, OS_FLSIZE, "(Audit) Process id: '%s'\n",
                sum->wdata.process_id);
    } else {
        *sdb->process_id = '\0';
    }

    if(sum->wdata.process_name && *sum->wdata.process_name != '\0') {
        snprintf(sdb->process_name, OS_FLSIZE, "(Audit) Process name: '%s'\n",
                sum->wdata.process_name);
    } else {
        *sdb->process_name = '\0';
    }
}


// Compare the first common fields between sum strings
int SumCompare(const char *s1, const char *s2) {
    unsigned int longs1;
    unsigned int longs2;

    longs1 = strlen(s1);
    longs2 = strlen(s2);

    if(longs1 != longs2) {
        return 1;
    }

    const char *ptr1 = strchr(s1, ':');
    const char *ptr2 = strchr(s2, ':');
    size_t size1;
    size_t size2;

    while (ptr1 && ptr2) {
        ptr1 = strchr(ptr1 + 1, ':');
        ptr2 = strchr(ptr2 + 1, ':');
    }

    size1 = ptr1 ? (size_t)(ptr1 - s1) : longs1;
    size2 = ptr2 ? (size_t)(ptr2 - s2) : longs2;

    return size1 == size2 ? strncmp(s1, s2, size1) : 1;
}

int fim_check_changes (int saved_frequency, long saved_time, Eventinfo *lf) {
    int freq = 1;

    if (!Config.syscheck_auto_ignore) {
        freq = 1;
    } else {
        if (lf->time.tv_sec - saved_time < Config.syscheck_ignore_time) {
            if (saved_frequency >= Config.syscheck_ignore_frequency) {
                // No send alert
                freq = -1;
            }
            else {
                freq = saved_frequency + 1;
            }
        }
    }

    return freq;
}

int fim_control_msg(char *key, time_t value, Eventinfo *lf, _sdb *sdb) {
    char *wazuhdb_query = NULL;
    char *response = NULL;
    char *msg = NULL;
    int db_result;
    int result;
    time_t *ts_end;
    time_t ts_start;

    os_calloc(OS_SIZE_128, sizeof(char), msg);

    // If we don't have a valid syscheck message, it may be a scan control message
    if(strcmp(key, HC_FIM_DB_SFS) == 0) {
        snprintf(msg, OS_SIZE_128, "first_start");
    }
    if(strcmp(key, HC_FIM_DB_EFS) == 0) {
        if (fim_get_scantime(&ts_start, lf, sdb, "start_scan") == 1) {
            if (ts_start == 0) {
                free(msg);
                return (-1);
            }
        }
        snprintf(msg, OS_SIZE_128, "first_end");
    }
    if(strcmp(key, HC_FIM_DB_SS) == 0) {
        snprintf(msg, OS_SIZE_128, "start_scan");
    }
    if(strcmp(key, HC_FIM_DB_ES) == 0) {
        if (fim_get_scantime(&ts_start, lf, sdb, "start_scan") == 1) {
            if (ts_start == 0) {
                free(msg);
                return (-1);
            }
        }
        snprintf(msg, OS_SIZE_128, "end_scan");
    }
    if(strcmp(key, HC_SK_DB_COMPLETED) == 0) {
        snprintf(msg, OS_SIZE_128, "end_scan");
    }

    if (*msg != '\0') {
        os_calloc(OS_SIZE_6144 + 1, sizeof(char), wazuhdb_query);

        snprintf(wazuhdb_query, OS_SIZE_6144, "agent %s syscheck scan_info_update %s %ld",
                lf->agent_id,
                msg,
                (long int)value
        );

        os_calloc(OS_SIZE_6144, sizeof(char), response);
        db_result = wdbc_query_ex(&sdb->socket, wazuhdb_query, response, OS_SIZE_6144);

        switch (db_result) {
        case -2:
            merror("FIM decoder: Bad result from scan_info query: '%s'.", wazuhdb_query);
            // Fallthrough
        case -1:
            os_free(wazuhdb_query);
            os_free(response);
            os_free(msg);
            return db_result;
        }

        // If end first scan store timestamp in a hash table
        w_mutex_lock(&control_msg_mutex);
        if(strcmp(key, HC_FIM_DB_EFS) == 0 || strcmp(key, HC_FIM_DB_ES) == 0 ||
                strcmp(key, HC_SK_DB_COMPLETED) == 0) {
            if (ts_end = (time_t *) OSHash_Get_ex(fim_agentinfo, lf->agent_id),
                    !ts_end) {
                os_calloc(1, sizeof(time_t), ts_end);
                *ts_end = value + 2;

                if (result = OSHash_Add_ex(fim_agentinfo, lf->agent_id, ts_end), result != 2) {
                    os_free(ts_end);
                    merror("Unable to add last scan_info to hash table for agent: %s. Error: %d.",
                            lf->agent_id, result);
                }
            }
            else {
                *ts_end = value;
                if (!OSHash_Update_ex(fim_agentinfo, lf->agent_id, ts_end)) {
                    os_free(ts_end);
                    merror("Unable to update metadata to hash table for agent: %s",
                            lf->agent_id);
                }
            }
        }
        w_mutex_unlock(&control_msg_mutex);

        // Start scan 3rd_check=2nd_check 2nd_check=1st_check 1st_check=value
        if (strcmp(key, HC_FIM_DB_SFS) == 0) {
            snprintf(wazuhdb_query, OS_SIZE_6144, "agent %s syscheck control %ld",
                    lf->agent_id,
                    (long int)value
            );

            db_result = wdbc_query_ex(&sdb->socket, wazuhdb_query, response, OS_SIZE_6144);

            switch (db_result) {
            case -2:
                merror("FIM decoder: Bad result from checks control query: '%s'.", wazuhdb_query);
                // Fallthrough
            case -1:
                os_free(wazuhdb_query);
                os_free(response);
                os_free(msg);
                return db_result;
            }
        }

        // At the end of first scan check and clean DB
        if (strcmp(key, HC_FIM_DB_EFS) == 0) {
            fim_database_clean(lf, sdb);
        }

        os_free(wazuhdb_query);
        os_free(response);
        os_free(msg);
        return (1);
    }

    os_free(msg);
    return (0);
}

int fim_update_date (char *file, Eventinfo *lf, _sdb *sdb) {
    char *wazuhdb_query = NULL;
    char *response = NULL;
    int db_result;

    os_calloc(OS_SIZE_6144 + 1, sizeof(char), wazuhdb_query);

    snprintf(wazuhdb_query, OS_SIZE_6144, "agent %s syscheck updatedate %s",
            lf->agent_id,
            file
    );

    os_calloc(OS_SIZE_6144, sizeof(char), response);
    db_result = wdbc_query_ex(&sdb->socket, wazuhdb_query, response, OS_SIZE_6144);

    switch (db_result) {
    case -2:
        merror("FIM decoder: Bad result updating date field: '%s'.", wazuhdb_query);
        // Fallthrough
    case -1:
        os_free(wazuhdb_query);
        os_free(response);
        return (-1);
    }

    mdebug2("FIM Agent '%s' file %s update timestamp for last event", lf->agent_id, file);

    os_free(wazuhdb_query);
    os_free(response);
    return (1);
}

int fim_database_clean (Eventinfo *lf, _sdb *sdb) {
    // If any entry has a date less than last_check it should be deleted.
    char *wazuhdb_query = NULL;
    char *response = NULL;
    int db_result;

    os_calloc(OS_SIZE_6144 + 1, sizeof(char), wazuhdb_query);

    snprintf(wazuhdb_query, OS_SIZE_6144, "agent %s syscheck cleandb ",
            lf->agent_id
    );

    os_calloc(OS_SIZE_6144, sizeof(char), response);
    db_result = wdbc_query_ex(&sdb->socket, wazuhdb_query, response, OS_SIZE_6144);

    switch (db_result) {
    case -2:
        merror("FIM decoder: Bad result from cleandb query: '%s'.", wazuhdb_query);
        // Fallthrough
    case -1:
        os_free(wazuhdb_query);
        os_free(response);
        return (-1);
    }

    mdebug2("Agent '%s' FIM database has been cleaned", lf->agent_id);

    os_free(wazuhdb_query);
    os_free(response);
    return (1);

}

int fim_get_scantime (long *ts, Eventinfo *lf, _sdb *sdb, const char* param) {
    char *wazuhdb_query = NULL;
    char *response = NULL;
    char *output;
    int db_result;

    os_calloc(OS_SIZE_6144 + 1, sizeof(char), wazuhdb_query);

    snprintf(wazuhdb_query, OS_SIZE_6144, "agent %s syscheck scan_info_get %s",
            lf->agent_id, param
    );

    os_calloc(OS_SIZE_6144, sizeof(char), response);
    db_result = wdbc_query_ex(&sdb->socket, wazuhdb_query, response, OS_SIZE_6144);

    switch (db_result) {
    case -2:
        merror("FIM decoder: Bad result getting scan date '%s'.", wazuhdb_query);
        // Fallthrough
    case -1:
        os_free(wazuhdb_query);
        os_free(response);
        return (-1);
    }

    output = strchr(response, ' ');

    if (!output) {
        merror("FIM decoder: Bad formatted response '%s'", response);
        os_free(wazuhdb_query);
        os_free(response);
        return (-1);
    }

    *(output++) = '\0';
    *ts = atol(output);

    mdebug2("Agent '%s' FIM %s '%ld'", lf->agent_id, param, *ts);

    os_free(wazuhdb_query);
    os_free(response);
    return (1);
}
// LCOV_EXCL_STOP

int decode_fim_event(_sdb *sdb, Eventinfo *lf) {
    /* Every syscheck message must be in the following JSON format, as of agent version v3.11
     * {
     *   type:                  "event"
     *   data: {
     *     path:                string
     *     hard_links:          array
     *     mode:                "scheduled"|"real-time"|"whodata"
     *     type:                "added"|"deleted"|"modified"
     *     timestamp:           number
     *     changed_attributes: [
     *       "size"
     *       "permission"
     *       "uid"
     *       "user_name"
     *       "gid"
     *       "group_name"
     *       "mtime"
     *       "inode"
     *       "md5"
     *       "sha1"
     *       "sha256"
     *     ]
     *     tags:                string
     *     content_changes:     string
     *     old_attributes: {
     *       type:              "file"|"registry"
     *       size:              number
     *       perm:              string
     *       user_name:         string
     *       group_name:        string
     *       uid:               string
     *       gid:               string
     *       inode:             number
     *       mtime:             number
     *       hash_md5:          string
     *       hash_sha1:         string
     *       hash_sha256:       string
     *       win_attributes:    string
     *       symlink_path:      string
     *       checksum:          string
     *     }
     *     attributes: {
     *       type:              "file"|"registry"
     *       size:              number
     *       perm:              string
     *       user_name:         string
     *       group_name:        string
     *       uid:               string
     *       gid:               string
     *       inode:             number
     *       mtime:             number
     *       hash_md5:          string
     *       hash_sha1:         string
     *       hash_sha256:       string
     *       win_attributes:    string
     *       symlink_path:      string
     *       checksum:          string
     *     }
     *     audit: {
     *       user_id:           string
     *       user_name:         string
     *       group_id:          string
     *       group_name:        string
     *       process_name:      string
     *       audit_uid:         string
     *       audit_name:        string
     *       effective_uid:     string
     *       effective_name:    string
     *       ppid:              number
     *       process_id:        number
     *     }
     *   }
     * }
     *
     * Scan info events:
     * {
     *   type:                  "scan_start"|"scan_end"
     *   data: {
     *     timestamp:           number
     *   }
     * }
     */

    cJSON *root_json = NULL;
    int retval = 0;

    assert(sdb != NULL);
    assert(lf != NULL);

    if (root_json = cJSON_Parse(lf->log), !root_json) {
        merror("Malformed FIM JSON event");
        return retval;
    }

    char * type = cJSON_GetStringValue(cJSON_GetObjectItem(root_json, "type"));
    cJSON * data = cJSON_GetObjectItem(root_json, "data");

    if (type != NULL && data != NULL) {
        if (strcmp(type, "event") == 0) {
            if (fim_process_alert(sdb, lf, data) == -1) {
                merror("Can't generate fim alert for event: '%s'", lf->log);
                cJSON_Delete(root_json);
                return retval;
            }

            retval = 1;
        } else if (strcmp(type, "scan_start") == 0) {
            fim_process_scan_info(sdb, lf->agent_id, FIM_SCAN_START, data);
        } else if (strcmp(type, "scan_end") == 0) {
            fim_process_scan_info(sdb, lf->agent_id, FIM_SCAN_END, data);
        }
    } else {
        merror("Invalid FIM event");
        cJSON_Delete(root_json);
        return retval;
    }

    cJSON_Delete(root_json);
    return retval;
}


static int fim_process_alert(_sdb * sdb, Eventinfo *lf, cJSON * event) {
    cJSON *attributes = NULL;
    cJSON *old_attributes = NULL;
    cJSON *audit = NULL;
    cJSON *object = NULL;
    char *mode = NULL;
    char *event_type = NULL;

    cJSON_ArrayForEach(object, event) {
        if (object->string == NULL) {
            mdebug1("FIM event contains an item with no key.");
            return -1;
        }

        switch (object->type) {
        case cJSON_String:
            if (strcmp(object->string, "path") == 0) {
                os_strdup(object->valuestring, lf->filename);
                os_strdup(object->valuestring, lf->fields[FIM_FILE].value);
            } else if (strcmp(object->string, "mode") == 0) {
                mode = object->valuestring;
            } else if (strcmp(object->string, "type") == 0) {
                event_type = object->valuestring;
            } else if (strcmp(object->string, "tags") == 0) {
                os_strdup(object->valuestring, lf->fields[FIM_TAG].value);
                os_strdup(object->valuestring, lf->sk_tag);
            } else if (strcmp(object->string, "content_changes") == 0) {
                os_strdup(object->valuestring, lf->fields[FIM_DIFF].value);
            }

            break;

        case cJSON_Array:
            if (strcmp(object->string, "changed_attributes") == 0) {
                cJSON *item;

                cJSON_ArrayForEach(item, object) {
                    wm_strcat(&lf->fields[FIM_CHFIELDS].value, item->valuestring, ',');
                }
            } else if (strcmp(object->string, "hard_links") == 0) {
                lf->fields[FIM_HARD_LINKS].value = cJSON_PrintUnformatted(object);
            }

            break;

        case cJSON_Object:
            if (strcmp(object->string, "attributes") == 0) {
                attributes = object;
            } else if (strcmp(object->string, "old_attributes") == 0) {
                old_attributes = object;
            } else if (strcmp(object->string, "audit") == 0) {
                audit = object;
            }

            break;
        }
    }

    if (event_type == NULL) {
        mdebug1("No member 'type' in Syscheck JSON payload");
        return -1;
    }

    if (strcmp("added", event_type) == 0) {
        lf->event_type = FIM_ADDED;
        lf->decoder_info->name = SYSCHECK_NEW;
        lf->decoder_info->id = decode_event_add;
    } else if (strcmp("modified", event_type) == 0) {
        lf->event_type = FIM_MODIFIED;
        lf->decoder_info->name = SYSCHECK_MOD;
        lf->decoder_info->id = decode_event_modify;
    } else if (strcmp("deleted", event_type) == 0) {
        lf->event_type = FIM_DELETED;
        lf->decoder_info->name = SYSCHECK_DEL;
        lf->decoder_info->id = decode_event_delete;
    } else {
        mdebug1("Invalid 'type' value '%s' in JSON payload.", event_type);
        return -1;
    }

    lf->decoder_syscheck_id = lf->decoder_info->id;

    fim_generate_alert(lf, mode, event_type, attributes, old_attributes, audit);

    switch (lf->event_type) {
    case FIM_ADDED:
    case FIM_MODIFIED:
        fim_send_db_save(sdb, lf->agent_id, event);
        break;

    case FIM_DELETED:
        fim_send_db_delete(sdb, lf->agent_id, lf->filename);

    default:
        ;
    }

    return 0;
}

void fim_send_db_save(_sdb * sdb, const char * agent_id, cJSON * data) {
    cJSON_DeleteItemFromObject(data, "mode");
    cJSON_DeleteItemFromObject(data, "type");
    cJSON_DeleteItemFromObject(data, "tags");
    cJSON_DeleteItemFromObject(data, "content_changes");
    cJSON_DeleteItemFromObject(data, "changed_attributes");
    cJSON_DeleteItemFromObject(data, "hard_links");
    cJSON_DeleteItemFromObject(data, "old_attributes");
    cJSON_DeleteItemFromObject(data, "audit");

    char * data_plain = cJSON_PrintUnformatted(data);
    char * query;

    os_malloc(OS_MAXSTR, query);

    if (snprintf(query, OS_MAXSTR, "agent %s syscheck save2 %s", agent_id, data_plain) >= OS_MAXSTR) {
        merror("FIM decoder: Cannot build save2 query: input is too long.");
        goto end;
    }

    fim_send_db_query(&sdb->socket, query);

end:
    free(data_plain);
    free(query);
}

void fim_send_db_delete(_sdb * sdb, const char * agent_id, const char * path) {
    char query[OS_SIZE_6144];

    if (snprintf(query, sizeof(query), "agent %s syscheck delete %s", agent_id, path) >= OS_SIZE_6144) {
        merror("FIM decoder: Cannot build delete query: input is too long.");
        return;
    }

    fim_send_db_query(&sdb->socket, query);
}

void fim_send_db_query(int * sock, const char * query) {
    char * response;
    char * arg;

    os_malloc(OS_MAXSTR, response);

    switch (wdbc_query_ex(sock, query, response, OS_MAXSTR)) {
    case -2:
        merror("FIM decoder: Cannot communicate with database.");
        goto end;
    case -1:
        merror("FIM decoder: Cannot get response from database.");
        goto end;
    }

    switch (wdbc_parse_result(response, &arg)) {
    case WDBC_OK:
        break;
    case WDBC_ERROR:
        merror("FIM decoder: Bad response from database: %s", arg);
        // Fallthrough
    default:
        goto end;
    }

end:
    free(response);
}


static int fim_generate_alert(Eventinfo *lf, char *mode, char *event_type,
    cJSON *attributes, cJSON *old_attributes, cJSON *audit) {

    cJSON *object = NULL;
    char change_size[OS_FLSIZE + 1] = {'\0'};
    char change_perm[OS_FLSIZE + 1] = {'\0'};
    char change_owner[OS_FLSIZE + 1] = {'\0'};
    char change_user[OS_FLSIZE + 1] = {'\0'};
    char change_gowner[OS_FLSIZE + 1] = {'\0'};
    char change_group[OS_FLSIZE + 1] = {'\0'};
    char change_md5[OS_FLSIZE + 1] = {'\0'};
    char change_sha1[OS_FLSIZE + 1] = {'\0'};
    char change_sha256[OS_FLSIZE + 1] = {'\0'};
    char change_mtime[OS_FLSIZE + 1] = {'\0'};
    char change_inode[OS_FLSIZE + 1] = {'\0'};
    char change_win_attributes[OS_SIZE_256 + 1] = {'\0'};
    int it;

    /* Dynamic Fields */
    lf->nfields = FIM_NFIELDS;
    for (it = 0; it < FIM_NFIELDS; it++) {
        os_strdup(lf->decoder_info->fields[it], lf->fields[it].key);
    }

    if (fim_fetch_attributes(attributes, old_attributes, lf)) {
        return -1;
    }

    cJSON_ArrayForEach(object, audit) {
        if (object->string == NULL) {
            mdebug1("FIM audit set contains an item with no key.");
            return -1;
        }

        switch (object->type) {
        case cJSON_Number:
            if (strcmp(object->string, "ppid") == 0) {
                os_calloc(OS_SIZE_32, sizeof(char), lf->fields[FIM_PPID].value);
                snprintf(lf->fields[FIM_PPID].value, OS_SIZE_32, "%ld", (long)object->valuedouble);
            } else if (strcmp(object->string, "process_id") == 0) {
                os_calloc(OS_SIZE_32, sizeof(char), lf->fields[FIM_PROC_ID].value);
                snprintf(lf->fields[FIM_PROC_ID].value, OS_SIZE_32, "%ld", (long)object->valuedouble);
            }

            break;

        case cJSON_String:
            if (strcmp(object->string, "user_id") == 0) {
                os_strdup(object->valuestring, lf->fields[FIM_USER_ID].value);
            } else if (strcmp(object->string, "user_name") == 0) {
                os_strdup(object->valuestring, lf->fields[FIM_USER_NAME].value);
            } else if (strcmp(object->string, "group_id") == 0) {
                os_strdup(object->valuestring, lf->fields[FIM_GROUP_ID].value);
            } else if (strcmp(object->string, "group_name") == 0) {
                os_strdup(object->valuestring, lf->fields[FIM_GROUP_NAME].value);
            } else if (strcmp(object->string, "process_name") == 0) {
                os_strdup(object->valuestring, lf->fields[FIM_PROC_NAME].value);
            } else if (strcmp(object->string, "audit_uid") == 0) {
                os_strdup(object->valuestring, lf->fields[FIM_AUDIT_ID].value);
            } else if (strcmp(object->string, "audit_name") == 0) {
                os_strdup(object->valuestring, lf->fields[FIM_AUDIT_NAME].value);
            } else if (strcmp(object->string, "effective_uid") == 0) {
                os_strdup(object->valuestring, lf->fields[FIM_EFFECTIVE_UID].value);
            } else if (strcmp(object->string, "effective_name") == 0) {
                os_strdup(object->valuestring, lf->fields[FIM_EFFECTIVE_NAME].value);
            }
        }
    }

    // Format comment
    if (lf->event_type == FIM_MODIFIED) {
        fim_generate_comment(change_size, sizeof(change_size), "Size changed from '%s' to '%s'\n", lf->size_before, lf->fields[FIM_SIZE].value);
        size_t size = fim_generate_comment(change_perm, sizeof(change_perm), "Permissions changed from '%s' to '%s'\n", lf->perm_before, lf->fields[FIM_PERM].value);
        if (size >= sizeof(change_perm)) {
            snprintf(change_perm, sizeof(change_perm), "Permissions changed.\n"); //LCOV_EXCL_LINE
        }
        fim_generate_comment(change_owner, sizeof(change_owner), "Ownership was '%s', now it is '%s'\n", lf->owner_before, lf->fields[FIM_UID].value);
        fim_generate_comment(change_user, sizeof(change_owner), "User name was '%s', now it is '%s'\n", lf->uname_before, lf->fields[FIM_UNAME].value);
        fim_generate_comment(change_gowner, sizeof(change_gowner), "Group ownership was '%s', now it is '%s'\n", lf->gowner_before, lf->fields[FIM_GID].value);
        fim_generate_comment(change_group, sizeof(change_gowner), "Group name was '%s', now it is '%s'\n", lf->gname_before, lf->fields[FIM_GNAME].value);

        if (lf->mtime_before != lf->mtime_after) {
            snprintf(change_mtime, sizeof(change_mtime), "Old modification time was: '%ld', now it is '%ld'\n", lf->mtime_before, lf->mtime_after);
        }
        if (lf->inode_before != lf->inode_after) {
            snprintf(change_inode, sizeof(change_inode), "Old inode was: '%ld', now it is '%ld'\n", lf->inode_before, lf->inode_after);
        }

        fim_generate_comment(change_md5, sizeof(change_md5), "Old md5sum was: '%s'\nNew md5sum is : '%s'\n", lf->md5_before, lf->fields[FIM_MD5].value);
        fim_generate_comment(change_sha1, sizeof(change_sha1), "Old sha1sum was: '%s'\nNew sha1sum is : '%s'\n", lf->sha1_before, lf->fields[FIM_SHA1].value);
        fim_generate_comment(change_sha256, sizeof(change_sha256), "Old sha256sum was: '%s'\nNew sha256sum is : '%s'\n", lf->sha256_before, lf->fields[FIM_SHA256].value);
        fim_generate_comment(change_win_attributes, sizeof(change_win_attributes), "Old attributes were: '%s'\nNow they are '%s'\n", lf->attributes_before, lf->fields[FIM_ATTRS].value);
    }

    // Provide information about the file
    char changed_attributes[OS_SIZE_256];
    snprintf(changed_attributes, OS_SIZE_256, "Changed attributes: %s\n", lf->fields[FIM_CHFIELDS].value);

    char hard_links[OS_SIZE_256];
    cJSON *tmp = cJSON_Parse(lf->fields[FIM_HARD_LINKS].value);
    if (lf->fields[FIM_HARD_LINKS].value) {
        cJSON *item;
        char * hard_links_tmp = NULL;
        cJSON_ArrayForEach(item, tmp) {
            wm_strcat(&hard_links_tmp, item->valuestring, ',');
        }

        snprintf(hard_links, OS_SIZE_256, "Hard links: %s\n", hard_links_tmp);
        os_free(hard_links_tmp);
    }

    // When full_log field is too long (max 756), it is fixed to show the last part of the path (more relevant)
    char * aux = NULL;
    if (strlen(lf->fields[FIM_FILE].value) > 756){
        int len = strlen(lf->fields[FIM_FILE].value);
        aux = lf->fields[FIM_FILE].value + len - 30;
    }

    snprintf(lf->full_log, OS_MAXSTR,
            "File '%.719s [...] %s' %s\n"
            "%s"
            "Mode: %s\n"
            "%s"
            "%s%s%s%s%s%s%s%s%s%s%s%s",
            lf->fields[FIM_FILE].value, aux, event_type,
            lf->fields[FIM_HARD_LINKS].value ? hard_links : "",
            mode,
            lf->fields[FIM_CHFIELDS].value ? changed_attributes : "",
            change_size,
            change_perm,
            change_owner,
            change_user,
            change_gowner,
            change_group,
            change_mtime,
            change_inode,
            change_md5,
            change_sha1,
            change_sha256,
            change_win_attributes
            //lf->fields[FIM_SYM_PATH].value
    );

    cJSON_Delete(tmp);

    return 0;
}

// Build change comment

size_t fim_generate_comment(char * str, long size, const char * format, const char * a1, const char * a2) {
    a1 = a1 != NULL ? a1 : "";
    a2 = a2 != NULL ? a2 : "";

    size_t str_size = 0;
    if (strcmp(a1, a2) != 0) {
        str_size = snprintf(str, size, format, a1, a2);
    }

    return str_size;
}

// Process scan info event

void fim_process_scan_info(_sdb * sdb, const char * agent_id, fim_scan_event event, cJSON * data) {
    cJSON * timestamp = cJSON_GetObjectItem(data, "timestamp");

    if (!cJSON_IsNumber(timestamp)) {
        mdebug1("No such member \"timestamp\" in FIM scan info event.");
        return;
    }

    char query[OS_SIZE_6144];

    if (snprintf(query, sizeof(query), "agent %s syscheck scan_info_update %s %ld", agent_id, event == FIM_SCAN_START ? "start_scan" : "end_scan", (long)timestamp->valuedouble) >= OS_SIZE_6144) {
        merror("FIM decoder: Cannot build save query: input is too long.");
        return;
    }

    fim_send_db_query(&sdb->socket, query);
}

int fim_fetch_attributes(cJSON *new_attrs, cJSON *old_attrs, Eventinfo *lf) {
    if (fim_fetch_attributes_state(new_attrs, lf, 1) ||
        fim_fetch_attributes_state(old_attrs, lf, 0)) {
        return -1;
    }

    return 0;
}

int fim_fetch_attributes_state(cJSON *attr, Eventinfo *lf, char new_state) {
    cJSON *attr_it;

    assert(lf != NULL);

    cJSON_ArrayForEach(attr_it, attr) {
        if (!attr_it->string) {
            mdebug1("FIM attribute set contains an item with no key.");
            return -1;
        }

        if (attr_it->type == cJSON_Number) {
            assert(lf->fields != NULL);
            if (!strcmp(attr_it->string, "size")) {
                if (new_state) {
                    lf->fields[FIM_SIZE].value = w_long_str((long) attr_it->valuedouble);
                } else {
                    lf->size_before = w_long_str((long) attr_it->valuedouble);
                }
            } else if (!strcmp(attr_it->string, "inode")) {
                if (new_state) {
                    lf->fields[FIM_INODE].value = w_long_str((long) attr_it->valuedouble);
                    lf->inode_after = (long) attr_it->valuedouble;
                } else {
                    lf->inode_before = (long) attr_it->valuedouble;
                }
            } else if (!strcmp(attr_it->string, "mtime")) {
                if (new_state) {
                    lf->fields[FIM_MTIME].value = w_long_str((long) attr_it->valuedouble);
                    lf->mtime_after = (long)attr_it->valuedouble;
                } else {
                    lf->mtime_before = (long) attr_it->valuedouble;
                }
            }
        } else if (attr_it->type == cJSON_String) {
            char **dst_data = NULL;

            if (!strcmp(attr_it->string, "perm")) {
                dst_data = new_state ? &lf->fields[FIM_PERM].value : &lf->perm_before;
            } else if (!strcmp(attr_it->string, "user_name")) {
                dst_data = new_state ? &lf->fields[FIM_UNAME].value : &lf->uname_before;
            } else if (!strcmp(attr_it->string, "group_name")) {
                dst_data = new_state ? &lf->fields[FIM_GNAME].value : &lf->gname_before;
            } else if (!strcmp(attr_it->string, "uid")) {
                dst_data = new_state ? &lf->fields[FIM_UID].value : &lf->owner_before;
            } else if (!strcmp(attr_it->string, "gid")) {
                dst_data = new_state ? &lf->fields[FIM_GID].value : &lf->gowner_before;
            } else if (!strcmp(attr_it->string, "hash_md5")) {
                dst_data = new_state ? &lf->fields[FIM_MD5].value : &lf->md5_before;
            } else if (!strcmp(attr_it->string, "hash_sha1")) {
                dst_data = new_state ? &lf->fields[FIM_SHA1].value : &lf->sha1_before;
            } else if (strcmp(attr_it->string, "hash_sha256") == 0) {
                dst_data = new_state ? &lf->fields[FIM_SHA256].value : &lf->sha256_before;
            } else if (strcmp(attr_it->string, "attributes") == 0) {
                dst_data = new_state ? &lf->fields[FIM_ATTRS].value : &lf->attributes_before; //LCOV_EXCL_LINE
            } else if (new_state && strcmp(attr_it->string, "symlink_path") == 0) {
                dst_data = &lf->fields[FIM_SYM_PATH].value;
            }

            if (dst_data) {
                os_strdup(attr_it->valuestring, *dst_data);
            }
        } else {
            mdebug1("Unknown FIM data type.");
        }
    }

    return 0;
}

void fim_adjust_checksum(sk_sum_t *newsum, char **checksum) {
    // Adjust attributes
    if (newsum->attributes) {
        os_realloc(*checksum,
                strlen(*checksum) + strlen(newsum->attributes) + 2,
                *checksum);
        char *found = strrchr(*checksum, ':');
        if (found) {
            snprintf(found + 1, strlen(newsum->attributes) + 1, "%s", newsum->attributes);
        }
    }

    // Adjust permissions
    if (newsum->win_perm && *newsum->win_perm) {
        char *first_part = strchr(*checksum, ':');
        if (!first_part) return;
        first_part++;
        *(first_part++) = '\0';
        char *second_part = strchr(first_part, ':');
        if (!second_part) return;
        os_strdup(second_part, second_part);

        // We need to escape the character ':' from the permissions
        //because we are going to compare against escaped permissions
        // sent by wazuh-db
        char *esc_perms = wstr_replace(newsum->win_perm, ":", "\\:");
        wm_strcat(checksum, esc_perms, 0);
        free(esc_perms);

        wm_strcat(checksum, second_part, 0);
        free(second_part);
    }
}
