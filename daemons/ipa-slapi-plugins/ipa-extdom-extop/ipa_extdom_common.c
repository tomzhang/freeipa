/** BEGIN COPYRIGHT BLOCK
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Additional permission under GPLv3 section 7:
 *
 * In the following paragraph, "GPL" means the GNU General Public
 * License, version 3 or any later version, and "Non-GPL Code" means
 * code that is governed neither by the GPL nor a license
 * compatible with the GPL.
 *
 * You may link the code of this Program with Non-GPL Code and convey
 * linked combinations including the two, provided that such Non-GPL
 * Code only links to the code of this Program through those well
 * defined interfaces identified in the file named EXCEPTION found in
 * the source code files (the "Approved Interfaces"). The files of
 * Non-GPL Code may instantiate templates or use macros or inline
 * functions from the Approved Interfaces without causing the resulting
 * work to be covered by the GPL. Only the copyright holders of this
 * Program may make changes or additions to the list of Approved
 * Interfaces.
 *
 * Authors:
 * Sumit Bose <sbose@redhat.com>
 *
 * Copyright (C) 2011 Red Hat, Inc.
 * All rights reserved.
 * END COPYRIGHT BLOCK **/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* for asprintf() */
#endif

#include <errno.h>
#include <stdio.h>

#include "ipa_extdom.h"
#include "util.h"

#define MAX(a,b) (((a)>(b))?(a):(b))
#define SSSD_DOMAIN_SEPARATOR '@'

int parse_request_data(struct berval *req_val, struct extdom_req **_req)
{
    BerElement *ber = NULL;
    ber_tag_t tag;
    ber_int_t input_type;
    ber_int_t request_type;
    ber_int_t id;
    struct extdom_req *req;

/* We expect the following request:
 * ExtdomRequestValue ::= SEQUENCE {
 *    inputType ENUMERATED {
 *        sid (1),
 *        name (2),
 *        posix uid (3),
 *        posix gid (3)
 *    },
 *    requestType ENUMERATED {
 *        simple (1),
 *        full (2)
 *        full_with_groups (3)
 *    },
 *    data InputData
 * }
 *
 * InputData ::= CHOICE {
 *    sid OCTET STRING,
 *    name NameDomainData
 *    uid PosixUid,
 *    gid PosixGid
 * }
 *
 * NameDomainData ::= SEQUENCE {
 *    domain_name OCTET STRING,
 *    object_name OCTET STRING
 * }
 *
 * PosixUid ::= SEQUENCE {
 *    domain_name OCTET STRING,
 *    uid INTEGER
 * }
 *
 * PosixGid ::= SEQUENCE {
 *    domain_name OCTET STRING,
 *    gid INTEGER
 * }
 */

    if (req_val == NULL || req_val->bv_val == NULL || req_val->bv_len == 0) {
        return LDAP_PROTOCOL_ERROR;
    }

    ber = ber_init(req_val);
    if (ber == NULL) {
        return LDAP_PROTOCOL_ERROR;
    }

    tag = ber_scanf(ber, "{ee", &input_type, &request_type);
    if (tag == LBER_ERROR) {
        ber_free(ber, 1);
        return LDAP_PROTOCOL_ERROR;
    }

    req = calloc(sizeof(struct extdom_req), 1);
    if (req == NULL) {
        return LDAP_OPERATIONS_ERROR;
    }

    req->input_type = input_type;
    req->request_type = request_type;

    switch (req->input_type) {
        case INP_NAME:
            tag = ber_scanf(ber, "{aa}}", &req->data.name.domain_name,
                                            &req->data.name.object_name);
            break;
        case INP_SID:
            tag = ber_scanf(ber, "a}", &req->data.sid);
            break;
        case INP_POSIX_UID:
            tag = ber_scanf(ber, "{ai}}", &req->data.posix_uid.domain_name,
                                            &id);
            req->data.posix_uid.uid = (uid_t) id;
            break;
        case INP_POSIX_GID:
            tag = ber_scanf(ber, "{ai}}", &req->data.posix_gid.domain_name,
                                            &id);
            req->data.posix_gid.gid = (gid_t) id;
            break;
        default:
            ber_free(ber, 1);
            free(req);
            return LDAP_PROTOCOL_ERROR;
    }
    ber_free(ber, 1);
    if (tag == LBER_ERROR) {
        free(req);
        return LDAP_PROTOCOL_ERROR;
    }

    *_req = req;

    return LDAP_SUCCESS;
}

void free_req_data(struct extdom_req *req)
{
    if (req == NULL) {
        return;
    }

    switch (req->input_type) {
    case INP_NAME:
        ber_memfree(req->data.name.domain_name);
        ber_memfree(req->data.name.object_name);
        break;
    case INP_SID:
        ber_memfree(req->data.sid);
        break;
    case INP_POSIX_UID:
        ber_memfree(req->data.posix_uid.domain_name);
        break;
    case INP_POSIX_GID:
        ber_memfree(req->data.posix_gid.domain_name);
        break;
    }

    free(req);
}

int check_request(struct extdom_req *req, enum extdom_version version)
{
    if (version == EXTDOM_V0) {
        if (req->request_type == REQ_FULL_WITH_GROUPS) {
            return LDAP_PROTOCOL_ERROR;
        }
    }

    return LDAP_SUCCESS;
}

static int get_buffer(size_t *_buf_len, char **_buf)
{
    long pw_max;
    long gr_max;
    size_t buf_len;
    char *buf;

    pw_max = sysconf(_SC_GETPW_R_SIZE_MAX);
    gr_max = sysconf(_SC_GETGR_R_SIZE_MAX);

    if (pw_max == -1 && gr_max == -1) {
        buf_len = 16384;
    } else {
        buf_len = MAX(pw_max, gr_max);
    }

    buf = malloc(sizeof(char) * buf_len);
    if (buf == NULL) {
        return LDAP_OPERATIONS_ERROR;
    }

    *_buf_len = buf_len;
    *_buf = buf;

    return LDAP_SUCCESS;
}

static int get_user_grouplist(const char *name, gid_t gid,
                              size_t *_ngroups, gid_t **_groups )
{
    int ret;
    int ngroups;
    gid_t *groups;
    gid_t *new_groups;

    ngroups = 128;
    groups = malloc(ngroups * sizeof(gid_t));
    if (groups == NULL) {
        return LDAP_OPERATIONS_ERROR;
    }

    ret = getgrouplist(name, gid, groups, &ngroups);
    if (ret == -1) {
        new_groups = realloc(groups, ngroups);
        if (new_groups == NULL) {
            free(groups);
            return LDAP_OPERATIONS_ERROR;
        }
        groups = new_groups;

        ret = getgrouplist(name, gid, groups, &ngroups);
        if (ret == -1) {
            free(groups);
            return LDAP_OPERATIONS_ERROR;
        }
    }

    *_ngroups = ngroups;
    *_groups = groups;

    return LDAP_SUCCESS;
}

static int add_kv_list(BerElement *ber, struct sss_nss_kv *kv_list)
{
    size_t c;
    int ret;
    const char *single_value_string_array[] = {NULL, NULL};

    ret = ber_printf(ber,"{");
    if (ret == -1) {
        return LDAP_OPERATIONS_ERROR;
    }

    for (c = 0; kv_list[c].key != NULL; c++) {
        single_value_string_array[0] = kv_list[c].value;
        ret = ber_printf(ber,"{s{v}}", kv_list[c].key,
                                       single_value_string_array);
        if (ret == -1) {
            return LDAP_OPERATIONS_ERROR;
        }
    }

    ret = ber_printf(ber,"}");
    if (ret == -1) {
        return LDAP_OPERATIONS_ERROR;
    }

    return LDAP_SUCCESS;
}

static int pack_ber_sid(const char *sid, struct berval **berval)
{
    BerElement *ber = NULL;
    int ret;

    ber = ber_alloc_t( LBER_USE_DER );
    if (ber == NULL) {
        return LDAP_OPERATIONS_ERROR;
    }

    ret = ber_printf(ber,"{es}", RESP_SID, sid);
    if (ret == -1) {
        ber_free(ber, 1);
        return LDAP_OPERATIONS_ERROR;
    }

    ret = ber_flatten(ber, berval);
    ber_free(ber, 1);
    if (ret == -1) {
        return LDAP_OPERATIONS_ERROR;
    }

    return LDAP_SUCCESS;
}

#define SSSD_SYSDB_SID_STR "objectSIDString"

static int pack_ber_user(enum response_types response_type,
                         const char *domain_name, const char *user_name,
                         uid_t uid, gid_t gid,
                         const char *gecos, const char *homedir,
                         const char *shell, struct sss_nss_kv *kv_list,
                         struct berval **berval)
{
    BerElement *ber = NULL;
    int ret;
    size_t ngroups;
    gid_t *groups = NULL;
    size_t buf_len;
    char *buf = NULL;
    struct group grp;
    struct group *grp_result;
    size_t c;
    char *locat;
    char *short_user_name = NULL;

    short_user_name = strdup(user_name);
    if ((locat = strchr(short_user_name, SSSD_DOMAIN_SEPARATOR)) != NULL) {
        if (strcasecmp(locat+1, domain_name) == 0  ) {
            locat[0] = '\0';
        } else {
            ret = LDAP_NO_SUCH_OBJECT;
            goto done;
        }
    }

    ber = ber_alloc_t( LBER_USE_DER );
    if (ber == NULL) {
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    ret = ber_printf(ber,"{e{ssii", response_type, domain_name, short_user_name,
                                      uid, gid);
    if (ret == -1) {
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    if (response_type == RESP_USER_GROUPLIST) {
        ret = get_user_grouplist(user_name, gid, &ngroups, &groups);
        if (ret != LDAP_SUCCESS) {
            goto done;
        }

        ret = get_buffer(&buf_len, &buf);
        if (ret != LDAP_SUCCESS) {
            goto done;
        }

        ret = ber_printf(ber,"sss", gecos, homedir, shell);
        if (ret == -1) {
            ret = LDAP_OPERATIONS_ERROR;
            goto done;
        }

        ret = ber_printf(ber,"{");
        if (ret == -1) {
            ret = LDAP_OPERATIONS_ERROR;
            goto done;
        }

        for (c = 0; c < ngroups; c++) {
            ret = getgrgid_r(groups[c], &grp, buf, buf_len, &grp_result);
            if (ret != 0) {
                ret = LDAP_NO_SUCH_OBJECT;
                goto done;
            }
            if (grp_result == NULL) {
                ret = LDAP_NO_SUCH_OBJECT;
                goto done;
            }

            ret = ber_printf(ber, "s", grp.gr_name);
            if (ret == -1) {
                ret = LDAP_OPERATIONS_ERROR;
                goto done;
            }
        }

        ret = ber_printf(ber,"}");
        if (ret == -1) {
            ret = LDAP_OPERATIONS_ERROR;
            goto done;
        }

        if (kv_list != NULL) {
            ret = add_kv_list(ber, kv_list);
            if (ret != LDAP_SUCCESS) {
                goto done;
            }
        }
    }

    ret = ber_printf(ber,"}}");
    if (ret == -1) {
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    ret = ber_flatten(ber, berval);
    if (ret == -1) {
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    ret = LDAP_SUCCESS;
done:
    free(short_user_name);
    free(groups);
    free(buf);
    ber_free(ber, 1);
    return ret;
}

static int pack_ber_group(enum response_types response_type,
                          const char *domain_name, const char *group_name,
                          gid_t gid, char **members, struct sss_nss_kv *kv_list,
                          struct berval **berval)
{
    BerElement *ber = NULL;
    int ret;
    size_t c;
    char *locat;
    char *short_group_name = NULL;

    short_group_name = strdup(group_name);
    if ((locat = strchr(short_group_name, SSSD_DOMAIN_SEPARATOR)) != NULL) {
        if (strcasecmp(locat+1, domain_name) == 0  ) {
            locat[0] = '\0';
        } else {
            ret = LDAP_NO_SUCH_OBJECT;
            goto done;
        }
    }

    ber = ber_alloc_t( LBER_USE_DER );
    if (ber == NULL) {
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    ret = ber_printf(ber,"{e{ssi", response_type, domain_name, short_group_name,
                                   gid);
    if (ret == -1) {
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    if (response_type == RESP_GROUP_MEMBERS) {
        ret = ber_printf(ber,"{");
        if (ret == -1) {
            ret = LDAP_OPERATIONS_ERROR;
            goto done;
        }

        for (c = 0; members[c] != NULL; c++) {
            ret = ber_printf(ber, "s", members[c]);
            if (ret == -1) {
                ret = LDAP_OPERATIONS_ERROR;
                goto done;
            }
        }

        ret = ber_printf(ber,"}");
        if (ret == -1) {
            ret = LDAP_OPERATIONS_ERROR;
            goto done;
        }

        if (kv_list != NULL) {
            ret = add_kv_list(ber, kv_list);
            if (ret != LDAP_SUCCESS) {
                goto done;
            }
        }

    }

    ret = ber_printf(ber,"}}");
    if (ret == -1) {
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    ret = ber_flatten(ber, berval);
    if (ret == -1) {
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    ret = LDAP_SUCCESS;

done:
    free(short_group_name);
    ber_free(ber, 1);
    return ret;
}

static int pack_ber_name(const char *domain_name, const char *name,
                         struct berval **berval)
{
    BerElement *ber = NULL;
    int ret;

    ber = ber_alloc_t( LBER_USE_DER );
    if (ber == NULL) {
        return LDAP_OPERATIONS_ERROR;
    }

    ret = ber_printf(ber,"{e{ss}}", RESP_NAME, domain_name, name);
    if (ret == -1) {
        ber_free(ber, 1);
        return LDAP_OPERATIONS_ERROR;
    }

    ret = ber_flatten(ber, berval);
    ber_free(ber, 1);
    if (ret == -1) {
        return LDAP_OPERATIONS_ERROR;
    }

    return LDAP_SUCCESS;
}

static int handle_uid_request(enum request_types request_type, uid_t uid,
                              const char *domain_name, struct berval **berval)
{
    int ret;
    struct passwd pwd;
    struct passwd *pwd_result = NULL;
    char *sid_str = NULL;
    enum sss_id_type id_type;
    size_t buf_len;
    char *buf = NULL;
    struct sss_nss_kv *kv_list = NULL;

    ret = get_buffer(&buf_len, &buf);
    if (ret != LDAP_SUCCESS) {
        return ret;
    }

    if (request_type == REQ_SIMPLE) {
        ret = sss_nss_getsidbyid(uid, &sid_str, &id_type);
        if (ret != 0 || !(id_type == SSS_ID_TYPE_UID
                            || id_type == SSS_ID_TYPE_BOTH)) {
            if (ret == ENOENT) {
                ret = LDAP_NO_SUCH_OBJECT;
            } else {
                ret = LDAP_OPERATIONS_ERROR;
            }
            goto done;
        }

        ret = pack_ber_sid(sid_str, berval);
    } else {
        ret = getpwuid_r(uid, &pwd, buf, buf_len, &pwd_result);
        if (ret != 0) {
            ret = LDAP_NO_SUCH_OBJECT;
            goto done;
        }
        if (pwd_result == NULL) {
            ret = LDAP_NO_SUCH_OBJECT;
            goto done;
        }

        if (request_type == REQ_FULL_WITH_GROUPS) {
            ret = sss_nss_getorigbyname(pwd.pw_name, &kv_list, &id_type);
            if (ret != 0 || !(id_type == SSS_ID_TYPE_UID
                                || id_type == SSS_ID_TYPE_BOTH)) {
                if (ret == ENOENT) {
                    ret = LDAP_NO_SUCH_OBJECT;
                } else {
                    ret = LDAP_OPERATIONS_ERROR;
                }
                goto done;
            }
        }

        ret = pack_ber_user((request_type == REQ_FULL ? RESP_USER
                                                      : RESP_USER_GROUPLIST),
                            domain_name, pwd.pw_name, pwd.pw_uid,
                            pwd.pw_gid, pwd.pw_gecos, pwd.pw_dir,
                            pwd.pw_shell, kv_list, berval);
    }

done:
    sss_nss_free_kv(kv_list);
    free(sid_str);
    free(buf);
    return ret;
}

static int handle_gid_request(enum request_types request_type, gid_t gid,
                              const char *domain_name, struct berval **berval)
{
    int ret;
    struct group grp;
    struct group *grp_result = NULL;
    char *sid_str = NULL;
    enum sss_id_type id_type;
    size_t buf_len;
    char *buf = NULL;
    struct sss_nss_kv *kv_list = NULL;

    ret = get_buffer(&buf_len, &buf);
    if (ret != LDAP_SUCCESS) {
        return ret;
    }

    if (request_type == REQ_SIMPLE) {
        ret = sss_nss_getsidbyid(gid, &sid_str, &id_type);
        if (ret != 0 || id_type != SSS_ID_TYPE_GID) {
            if (ret == ENOENT) {
                ret = LDAP_NO_SUCH_OBJECT;
            } else {
                ret = LDAP_OPERATIONS_ERROR;
            }
            goto done;
        }

        ret = pack_ber_sid(sid_str, berval);
    } else {
        ret = getgrgid_r(gid, &grp, buf, buf_len, &grp_result);
        if (ret != 0) {
            ret = LDAP_NO_SUCH_OBJECT;
            goto done;
        }
        if (grp_result == NULL) {
            ret = LDAP_NO_SUCH_OBJECT;
            goto done;
        }

        if (request_type == REQ_FULL_WITH_GROUPS) {
            ret = sss_nss_getorigbyname(grp.gr_name, &kv_list, &id_type);
            if (ret != 0 || !(id_type == SSS_ID_TYPE_GID
                                || id_type == SSS_ID_TYPE_BOTH)) {
                if (ret == ENOENT) {
                    ret = LDAP_NO_SUCH_OBJECT;
                } else {
                    ret = LDAP_OPERATIONS_ERROR;
                }
                goto done;
            }
        }

        ret = pack_ber_group((request_type == REQ_FULL ? RESP_GROUP
                                                       : RESP_GROUP_MEMBERS),
                             domain_name, grp.gr_name, grp.gr_gid,
                             grp.gr_mem, kv_list, berval);
    }

done:
    sss_nss_free_kv(kv_list);
    free(sid_str);
    free(buf);
    return ret;
}

static int handle_sid_request(enum request_types request_type, const char *sid,
                              struct berval **berval)
{
    int ret;
    struct passwd pwd;
    struct passwd *pwd_result = NULL;
    struct group grp;
    struct group *grp_result = NULL;
    char *domain_name = NULL;
    char *fq_name = NULL;
    char *object_name = NULL;
    char *sep;
    size_t buf_len;
    char *buf = NULL;
    enum sss_id_type id_type;
    struct sss_nss_kv *kv_list = NULL;

    ret = sss_nss_getnamebysid(sid, &fq_name, &id_type);
    if (ret != 0) {
        if (ret == ENOENT) {
            ret = LDAP_NO_SUCH_OBJECT;
        } else {
            ret = LDAP_OPERATIONS_ERROR;
        }
        goto done;
    }

    sep = strchr(fq_name, SSSD_DOMAIN_SEPARATOR);
    if (sep == NULL) {
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    object_name = strndup(fq_name, (sep - fq_name));
    domain_name = strdup(sep + 1);
    if (object_name == NULL || domain_name == NULL) {
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    if (request_type == REQ_SIMPLE) {
        ret = pack_ber_name(domain_name, object_name, berval);
        goto done;
    }

    ret = get_buffer(&buf_len, &buf);
    if (ret != LDAP_SUCCESS) {
        goto done;
    }

    switch(id_type) {
    case SSS_ID_TYPE_UID:
    case SSS_ID_TYPE_BOTH:
        ret = getpwnam_r(fq_name, &pwd, buf, buf_len, &pwd_result);
        if (ret != 0) {
            ret = LDAP_NO_SUCH_OBJECT;
            goto done;
        }

        if (pwd_result == NULL) {
            ret = LDAP_NO_SUCH_OBJECT;
            goto done;
        }

        if (request_type == REQ_FULL_WITH_GROUPS) {
            ret = sss_nss_getorigbyname(pwd.pw_name, &kv_list, &id_type);
            if (ret != 0 || !(id_type == SSS_ID_TYPE_UID
                                || id_type == SSS_ID_TYPE_BOTH)) {
                if (ret == ENOENT) {
                    ret = LDAP_NO_SUCH_OBJECT;
                } else {
                    ret = LDAP_OPERATIONS_ERROR;
                }
                goto done;
            }
        }

        ret = pack_ber_user((request_type == REQ_FULL ? RESP_USER
                                                      : RESP_USER_GROUPLIST),
                            domain_name, pwd.pw_name, pwd.pw_uid,
                            pwd.pw_gid, pwd.pw_gecos, pwd.pw_dir,
                            pwd.pw_shell, kv_list, berval);
        break;
    case SSS_ID_TYPE_GID:
        ret = getgrnam_r(fq_name, &grp, buf, buf_len, &grp_result);
        if (ret != 0) {
            ret = LDAP_NO_SUCH_OBJECT;
            goto done;
        }

        if (grp_result == NULL) {
            ret = LDAP_NO_SUCH_OBJECT;
            goto done;
        }

        if (request_type == REQ_FULL_WITH_GROUPS) {
            ret = sss_nss_getorigbyname(grp.gr_name, &kv_list, &id_type);
            if (ret != 0 || !(id_type == SSS_ID_TYPE_GID
                                || id_type == SSS_ID_TYPE_BOTH)) {
                if (ret == ENOENT) {
                    ret = LDAP_NO_SUCH_OBJECT;
                } else {
                    ret = LDAP_OPERATIONS_ERROR;
                }
                goto done;
            }
        }

        ret = pack_ber_group((request_type == REQ_FULL ? RESP_GROUP
                                                       : RESP_GROUP_MEMBERS),
                             domain_name, grp.gr_name, grp.gr_gid,
                             grp.gr_mem, kv_list, berval);
        break;
    default:
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

done:
    sss_nss_free_kv(kv_list);
    free(fq_name);
    free(object_name);
    free(domain_name);
    free(buf);

    return ret;
}

static int handle_name_request(enum request_types request_type,
                               const char *name, const char *domain_name,
                               struct berval **berval)
{
    int ret;
    char *fq_name = NULL;
    struct passwd pwd;
    struct passwd *pwd_result = NULL;
    struct group grp;
    struct group *grp_result = NULL;
    char *sid_str = NULL;
    enum sss_id_type id_type;
    size_t buf_len;
    char *buf = NULL;
    struct sss_nss_kv *kv_list = NULL;

    ret = asprintf(&fq_name, "%s%c%s", name, SSSD_DOMAIN_SEPARATOR,
                                       domain_name);
    if (ret == -1) {
        ret = LDAP_OPERATIONS_ERROR;
        fq_name = NULL; /* content is undefined according to
                           asprintf(3) */
        goto done;
    }

    if (request_type == REQ_SIMPLE) {
        ret = sss_nss_getsidbyname(fq_name, &sid_str, &id_type);
        if (ret != 0) {
            if (ret == ENOENT) {
                ret = LDAP_NO_SUCH_OBJECT;
            } else {
                ret = LDAP_OPERATIONS_ERROR;
            }
            goto done;
        }

        ret = pack_ber_sid(sid_str, berval);
    } else {
        ret = get_buffer(&buf_len, &buf);
        if (ret != LDAP_SUCCESS) {
            goto done;
        }

        ret = getpwnam_r(fq_name, &pwd, buf, buf_len, &pwd_result);
        if (ret != 0) {
            /* according to the man page there are a couple of error codes
             * which can indicate that the user was not found. To be on the
             * safe side we fail back to the group lookup on all errors. */
            pwd_result = NULL;
        }

        if (pwd_result != NULL) {
            if (request_type == REQ_FULL_WITH_GROUPS) {
                ret = sss_nss_getorigbyname(pwd.pw_name, &kv_list, &id_type);
                if (ret != 0 || !(id_type == SSS_ID_TYPE_UID
                                    || id_type == SSS_ID_TYPE_BOTH)) {
                    if (ret == ENOENT) {
                        ret = LDAP_NO_SUCH_OBJECT;
                    } else {
                        ret = LDAP_OPERATIONS_ERROR;
                    }
                    goto done;
                }
            }
            ret = pack_ber_user((request_type == REQ_FULL ? RESP_USER
                                                          : RESP_USER_GROUPLIST),
                                domain_name, pwd.pw_name, pwd.pw_uid,
                                pwd.pw_gid, pwd.pw_gecos, pwd.pw_dir,
                                pwd.pw_shell, kv_list, berval);
        } else { /* no user entry found */
            ret = getgrnam_r(fq_name, &grp, buf, buf_len, &grp_result);
            if (ret != 0) {
                ret = LDAP_NO_SUCH_OBJECT;
                goto done;
            }

            if (grp_result == NULL) {
                ret = LDAP_NO_SUCH_OBJECT;
                goto done;
            }

            if (request_type == REQ_FULL_WITH_GROUPS) {
                ret = sss_nss_getorigbyname(grp.gr_name, &kv_list, &id_type);
                if (ret != 0 || !(id_type == SSS_ID_TYPE_GID
                                    || id_type == SSS_ID_TYPE_BOTH)) {
                    if (ret == ENOENT) {
                        ret = LDAP_NO_SUCH_OBJECT;
                    } else {
                        ret = LDAP_OPERATIONS_ERROR;
                    }
                    goto done;
                }
            }

            ret = pack_ber_group((request_type == REQ_FULL ? RESP_GROUP
                                                           : RESP_GROUP_MEMBERS),
                                 domain_name, grp.gr_name, grp.gr_gid,
                                 grp.gr_mem, kv_list, berval);
        }
    }

done:
    sss_nss_free_kv(kv_list);
    free(fq_name);
    free(sid_str);
    free(buf);

    return ret;
}

int handle_request(struct ipa_extdom_ctx *ctx, struct extdom_req *req,
                   struct berval **berval)
{
    int ret;

    switch (req->input_type) {
    case INP_POSIX_UID:
        ret = handle_uid_request(req->request_type, req->data.posix_uid.uid,
                                 req->data.posix_uid.domain_name, berval);

        break;
    case INP_POSIX_GID:
        ret = handle_gid_request(req->request_type, req->data.posix_gid.gid,
                                 req->data.posix_uid.domain_name, berval);

        break;
    case INP_SID:
        ret = handle_sid_request(req->request_type, req->data.sid, berval);
        break;
    case INP_NAME:
        ret = handle_name_request(req->request_type, req->data.name.object_name,
                                  req->data.name.domain_name, berval);

        break;
    default:
        ret = LDAP_PROTOCOL_ERROR;
        goto done;
    }


done:

    return ret;
}

int pack_response(struct extdom_res *res, struct berval **ret_val)
{
    BerElement *ber = NULL;
    int ret;

/* We send to follwing response:
 * ExtdomResponseValue ::= SEQUENCE {
 *    responseType ENUMERATED {
 *        sid (1),
 *        name (2),
 *        posix_user (3),
 *        posix_group (4)
 *    },
 *    data OutputData
 * }
 *
 * OutputData ::= CHOICE {
 *    sid OCTET STRING,
 *    name NameDomainData,
 *    user PosixUser,
 *    group PosixGroup
 * }
 *
 * NameDomainData ::= SEQUENCE {
 *    domain_name OCTET STRING,
 *    object_name OCTET STRING
 * }
 *
 * PosixUser ::= SEQUENCE {
 *    domain_name OCTET STRING,
 *    user_name OCTET STRING,
 *    uid INTEGER
 *    gid INTEGER
 * }
 *
 * PosixGroup ::= SEQUENCE {
 *    domain_name OCTET STRING,
 *    group_name OCTET STRING,
 *    gid INTEGER
 * }
 */

    ber = ber_alloc_t( LBER_USE_DER );
    if (ber == NULL) {
        return LDAP_OPERATIONS_ERROR;
    }

    switch (res->response_type) {
        case RESP_SID:
            ret = ber_printf(ber,"{es}", res->response_type, res->data.sid);
            break;
        case RESP_NAME:
            ret = ber_printf(ber,"{e{ss}}", res->response_type,
                                            res->data.name.domain_name,
                                            res->data.name.object_name);
            break;
        case RESP_USER:
            ret = ber_printf(ber,"{e{ssii}}", res->response_type,
                                              res->data.user.domain_name,
                                              res->data.user.user_name,
                                              res->data.user.uid,
                                              res->data.user.gid);
            break;
        case RESP_GROUP:
            ret = ber_printf(ber,"{e{ssi}}", res->response_type,
                                             res->data.group.domain_name,
                                             res->data.group.group_name,
                                             res->data.group.gid);
            break;
        default:
            ber_free(ber, 1);
            return LDAP_OPERATIONS_ERROR;
    }

    if (ret == -1) {
        ber_free(ber, 1);
        return LDAP_OPERATIONS_ERROR;
    }

    ret = ber_flatten(ber, ret_val);
    if (ret == -1) {
        ber_free(ber, 1);
        return LDAP_OPERATIONS_ERROR;
    }

    ber_free(ber, 1);

    return LDAP_SUCCESS;
}
