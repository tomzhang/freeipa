/* Authors: Simo Sorce <ssorce@redhat.com>
 *
 * Copyright (C) 2007  Red Hat
 * see file 'COPYING' for use and warranty information
 *
 * This program is free software you can redistribute it and/or modify
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
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <krb5.h>
#include <ldap.h>
#include <sasl/sasl.h>
#include <popt.h>

#include "config.h"

#include "ipa_krb5.h"
#include "ipa_asn1.h"
#include "ipa-client-common.h"

static int ldap_sasl_interact(LDAP *ld, unsigned flags, void *priv_data, void *sit)
{
	sasl_interact_t *in = NULL;
	int ret = LDAP_OTHER;
	krb5_principal princ = (krb5_principal)priv_data;
	krb5_context krbctx;
	char *outname = NULL;
	krb5_error_code krberr;

	if (!ld) return LDAP_PARAM_ERROR;

	for (in = sit; in && in->id != SASL_CB_LIST_END; in++) {
		switch(in->id) {
		case SASL_CB_USER:
			krberr = krb5_init_context(&krbctx);

			if (krberr) {
				fprintf(stderr, _("Kerberos context initialization failed: %1$s (%2$d)\n"),
                                error_message(krberr), krberr);
				in->result = NULL;
				in->len = 0;
				ret = LDAP_LOCAL_ERROR;
				break;
			}

			krberr = krb5_unparse_name(krbctx, princ, &outname);

			if (krberr) {
                fprintf(stderr, _("Unable to parse principal: %1$s (%2$d)\n"),
                                error_message(krberr), krberr);
				in->result = NULL;
				in->len = 0;
				ret = LDAP_LOCAL_ERROR;
				break;
			}

			in->result = outname;
			in->len = strlen(outname);
			ret = LDAP_SUCCESS;

			krb5_free_context(krbctx);

			break;
		case SASL_CB_GETREALM:
			in->result = princ->realm.data;
			in->len = princ->realm.length;
			ret = LDAP_SUCCESS;
			break;
		default:
			in->result = NULL;
			in->len = 0;
			ret = LDAP_OTHER;
		}
	}
	return ret;
}

int filter_keys(krb5_context krbctx, struct keys_container *keys,
                ber_int_t *enctypes)
{
    struct krb_key_salt *ksdata;
    int i, j, n;

    n = keys->nkeys;
    ksdata = keys->ksdata;
    for (i = 0; i < n; i++) {
        if (ksdata[i].enctype == enctypes[i]) continue;
        if (enctypes[i] == 0) {
            /* remove unsupported one */
            krb5_free_keyblock_contents(krbctx, &ksdata[i].key);
            krb5_free_data_contents(krbctx, &ksdata[i].salt);
            for (j = i; j < n-1; j++) {
                ksdata[j] = ksdata[j + 1];
                enctypes[j] = enctypes[j + 1];
            }
            n--;
            /* new key has been moved to this position, make sure
             * we do not skip it, by neutralizing next i increment */
            i--;
        }
    }

    if (n == 0) {
        fprintf(stderr, _("No keys accepted by KDC\n"));
        return 0;
    }

    keys->nkeys = n;
    return n;
}

static int ipa_ldap_init(LDAP ** ld, const char * scheme, const char * servername, const int  port)
{
	char* url = NULL;
	int  url_len = snprintf(url,0,"%s://%s:%d",scheme,servername,port) +1;

	url = (char *)malloc (url_len);
	if (!url){
		fprintf(stderr, _("Out of memory \n"));
		return LDAP_NO_MEMORY;
	}
	sprintf(url,"%s://%s:%d",scheme,servername,port);
	int rc = ldap_initialize(ld, url);

	free(url);
	return rc;
}

const char *ca_cert_file = "/etc/ipa/ca.crt";

static int ipa_ldap_bind(const char *server_name, krb5_principal bind_princ,
			 const char *bind_dn, const char *bind_pw, LDAP **_ld)
{
    char *msg = NULL;
    struct berval bv;
    int version;
    LDAP *ld;
    int ssl;
    int ret;

    /* TODO: support referrals ? */
    if (bind_dn) {
        ret = ldap_set_option(NULL, LDAP_OPT_X_TLS_CACERTFILE, ca_cert_file);
        if (ret != LDAP_OPT_SUCCESS) {
            fprintf(stderr, _("Unable to set LDAP_OPT_X_TLS_CERTIFICATE\n"));
            return ret;
        }

        ret = ipa_ldap_init(&ld, "ldaps", server_name, 636);
        if (ret != LDAP_SUCCESS) {
            fprintf(stderr, _("Unable to init for ldaps(636) connection\n"));
            return ret;
        }

        ssl = LDAP_OPT_X_TLS_HARD;;
        ret = ldap_set_option(ld, LDAP_OPT_X_TLS, &ssl);
        if (ret != LDAP_OPT_SUCCESS) {
            fprintf(stderr, _("Unable to set LDAP_OPT_X_TLS\n"));
            goto done;
        }
    } else {
        ret = ipa_ldap_init(&ld, "ldap", server_name, 389);
        if (ret != LDAP_SUCCESS) {
            fprintf(stderr, _("Unable to init for ldap(389) connection\n"));
            return ret;
        }
    }

    if (ld == NULL) {
        fprintf(stderr, _("Unable to initialize ldap library!\n"));
        return LDAP_OPERATIONS_ERROR;
    }

#ifdef LDAP_OPT_X_SASL_NOCANON
    /* Don't do DNS canonicalization */
    ret = ldap_set_option(ld, LDAP_OPT_X_SASL_NOCANON, LDAP_OPT_ON);
    if (ret != LDAP_SUCCESS) {
	fprintf(stderr, _("Unable to set LDAP_OPT_X_SASL_NOCANON\n"));
        goto done;
    }
#endif

    version = LDAP_VERSION3;
    ret = ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, &version);
    if (ret != LDAP_SUCCESS) {
	fprintf(stderr, _("Unable to set LDAP_OPT_PROTOCOL_VERSION\n"));
	goto done;
    }

    if (bind_dn) {
        bv.bv_val = discard_const(bind_pw);
        bv.bv_len = strlen(bind_pw);

        ret = ldap_sasl_bind_s(ld, bind_dn, LDAP_SASL_SIMPLE,
                               &bv, NULL, NULL, NULL);
        if (ret != LDAP_SUCCESS) {
            fprintf(stderr, _("Simple bind failed\n"));
            goto done;
        }
    } else {
        ret = ldap_sasl_interactive_bind_s(ld, NULL, "GSSAPI",
                                           NULL, NULL, LDAP_SASL_QUIET,
                                           ldap_sasl_interact, bind_princ);
        if (ret != LDAP_SUCCESS) {
#ifdef LDAP_OPT_DIAGNOSTIC_MESSAGE
            ldap_get_option(ld, LDAP_OPT_DIAGNOSTIC_MESSAGE, (void*)&msg);
#endif
            fprintf(stderr, "SASL Bind failed %s (%d) %s!\n",
                            ldap_err2string(ret), ret, msg ? msg : "");
            goto done;
        }
    }

    ret = LDAP_SUCCESS;

done:
    if (ret != LDAP_SUCCESS) {
        if (ld) ldap_unbind_ext(ld, NULL, NULL);
    } else {
        *_ld = ld;
    }
    return ret;
}

static int ipa_ldap_extended_op(LDAP *ld, const char *reqoid,
                                struct berval *control,
                                LDAPControl ***srvctrl)
{
    struct berval *retdata = NULL;
    LDAPMessage *res = NULL;
    char *retoid = NULL;
    struct timeval tv;
    char *err = NULL;
    int msgid;
    int ret, rc;

    ret = ldap_extended_operation(ld, reqoid, control,
                                  NULL, NULL, &msgid);
    if (ret != LDAP_SUCCESS) {
        fprintf(stderr, _("Operation failed: %s\n"), ldap_err2string(ret));
        return ret;
    }

    /* wait max 10 secs for the answer */
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    ret = ldap_result(ld, msgid, 1, &tv, &res);
    if (ret == -1) {
        fprintf(stderr, _("Failed to get result: %s\n"), ldap_err2string(ret));
        goto done;
    }

    ret = ldap_parse_extended_result(ld, res, &retoid, &retdata, 0);
    if (ret != LDAP_SUCCESS) {
        fprintf(stderr, _("Failed to parse extended result: %s\n"),
                        ldap_err2string(ret));
        goto done;
    }

    ret = ldap_parse_result(ld, res, &rc, NULL, &err, NULL, srvctrl, 0);
    if (ret != LDAP_SUCCESS || rc != LDAP_SUCCESS) {
        fprintf(stderr, _("Failed to parse result: %s\n"),
                        err ? err : ldap_err2string(ret));
        if (ret == LDAP_SUCCESS) ret = rc;
        goto done;
    }

done:
    if (err) ldap_memfree(err);
    if (res) ldap_msgfree(res);
    return ret;
}

static int find_control_data(LDAPControl **list, const char *repoid,
                             struct berval *data)
{
    LDAPControl *control = NULL;
    int i;

    if (!list) {
        fprintf(stderr, _("Missing reply control list!\n"));
        return LDAP_OPERATIONS_ERROR;
    }

    for (i = 0; list[i]; i++) {
        if (strcmp(list[i]->ldctl_oid, repoid) == 0) {
            control = list[i];
        }
    }
    if (!control) {
        fprintf(stderr, _("Missing reply control!\n"));
        return LDAP_OPERATIONS_ERROR;
    }

    *data = control->ldctl_value;
    return LDAP_SUCCESS;
}

static BerElement *get_control_data(LDAPControl **list, const char *repoid)
{
    struct berval data;
    int ret;

    ret = find_control_data(list, repoid, &data);
    if (ret != LDAP_SUCCESS) return NULL;

    return ber_init(&data);
}

static int ldap_set_keytab(krb5_context krbctx,
			   const char *servername,
			   const char *principal_name,
			   krb5_principal princ,
			   const char *binddn,
			   const char *bindpw,
			   struct keys_container *keys)
{
	LDAP *ld = NULL;
	BerElement *sctrl = NULL;
	struct berval *control = NULL;
	LDAPControl **srvctrl = NULL;
	int ret;
	int kvno, i;
	ber_tag_t rtag;
	ber_int_t *encs = NULL;
	int successful_keys = 0;

	/* cant' return more than nkeys, sometimes less */
	encs = calloc(keys->nkeys + 1, sizeof(ber_int_t));
	if (!encs) {
		fprintf(stderr, _("Out of Memory!\n"));
		return 0;
	}

	/* build password change control */
	control = create_key_control(keys, principal_name);
	if (!control) {
		fprintf(stderr, _("Failed to create control!\n"));
		goto error_out;
	}

    ret = ipa_ldap_bind(servername, princ, binddn, bindpw, &ld);
    if (ret != LDAP_SUCCESS) {
        fprintf(stderr, _("Failed to bind to server!\n"));
        goto error_out;
    }

    /* perform password change */
    ret = ipa_ldap_extended_op(ld, KEYTAB_SET_OID, control, &srvctrl);
    if (ret != LDAP_SUCCESS) {
        fprintf(stderr, _("Failed to get keytab!\n"));
        goto error_out;
    }

	ber_bvfree(control);
	control = NULL;

	sctrl = get_control_data(srvctrl, KEYTAB_RET_OID);
	if (!sctrl) {
		fprintf(stderr, _("ber_init() failed, Invalid control ?!\n"));
		goto error_out;
	}

	/* Format of response
	 *
	 * KeytabGetRequest ::= SEQUENCE {
	 * 	new_kvno	Int32
	 * 	SEQUENCE OF	KeyTypes
	 * }
	 *
	 * * List of accepted enctypes *
	 * KeyTypes ::= SEQUENCE {
	 * 	enctype		Int32
	 * }
	 */

	rtag = ber_scanf(sctrl, "{i{", &kvno);
	if (rtag == LBER_ERROR) {
		fprintf(stderr, _("ber_scanf() failed, unable to find kvno ?!\n"));
		goto error_out;
	}

	for (i = 0; i < keys->nkeys; i++) {
		ret = ber_scanf(sctrl, "{i}", &encs[i]);
		if (ret == LBER_ERROR) {
			char enc[79]; /* fit std terminal or truncate */
			krb5_error_code krberr;
			krberr = krb5_enctype_to_string(
				keys->ksdata[i].enctype, enc, 79);
			if (krberr) {
				fprintf(stderr, _("Failed to retrieve "
					"encryption type type #%d\n"),
					keys->ksdata[i].enctype);
			} else {
				fprintf(stderr, _("Failed to retrieve "
					"encryption type %1$s (#%2$d)\n"),
					enc, keys->ksdata[i].enctype);
			}
                } else {
			successful_keys++;
		}
	}

	if (successful_keys == 0) {
		fprintf(stderr, _("Failed to retrieve any keys"));
		goto error_out;
	}

	ret = filter_keys(krbctx, keys, encs);
	if (ret == 0) goto error_out;

	ber_free(sctrl, 1);
	ldap_controls_free(srvctrl);
	ldap_unbind_ext(ld, NULL, NULL);
	free(encs);
	return kvno;

error_out:
	if (sctrl) ber_free(sctrl, 1);
	if (srvctrl) ldap_controls_free(srvctrl);
	if (ld) ldap_unbind_ext(ld, NULL, NULL);
	if (control) ber_bvfree(control);
	free(encs);
	return -1;
}

/* use asn1c generated code to fill up control */
static struct berval *create_getkeytab_control(const char *svc_princ, bool gen,
                                               const char *password,
                                               struct krb_key_salt *encsalts,
                                               int num_encsalts)
{
    struct berval *result = NULL;
    void *buffer = NULL;
    size_t buflen;
    long ets[num_encsalts];
    bool ret;
    int i;

    if (gen) {
        for (i = 0; i < num_encsalts; i++) {
            ets[i] = encsalts[i].enctype;
        }
    }
    ret = ipaasn1_enc_getkt(gen, svc_princ,
                            password, ets, num_encsalts,
                            &buffer, &buflen);
    if (!ret) goto done;

    result = malloc(sizeof(struct berval));
    if (!result) goto done;

    result->bv_val = buffer;
    result->bv_len = buflen;

done:
    if (result == NULL) {
        if (buffer) {
            free(buffer);
        }
    }
    return result;
}

#define GK_REPLY_TAG (LBER_CLASS_CONTEXT | LBER_CONSTRUCTED | 2)
#define GKREP_KEY_TAG (LBER_CLASS_CONTEXT | LBER_CONSTRUCTED | 0)
#define GKREP_SALT_TAG (LBER_CLASS_CONTEXT | LBER_CONSTRUCTED | 1)

static int ldap_get_keytab(krb5_context krbctx, bool generate, char *password,
                           const char *enctypes, const char *bind_server,
                           const char *svc_princ, krb5_principal bind_princ,
                           const char *bind_dn, const char *bind_pw,
                           struct keys_container *keys, int *kvno,
                           char **err_msg)
{
    struct krb_key_salt *es = NULL;
    int num_es = 0;
    struct berval *control = NULL;
    LDAP *ld = NULL;
    LDAPControl **srvctrl = NULL;
    struct berval data;
    bool res;
    int ret;

    *err_msg = NULL;

    if (enctypes) {
        ret = ipa_string_to_enctypes(enctypes, &es, &num_es, err_msg);
        if (ret || num_es == 0) {
            return LDAP_OPERATIONS_ERROR;
        }
    }

    control = create_getkeytab_control(svc_princ, generate,
                                       password, es, num_es);
    if (!control) {
        *err_msg = _("Failed to create control!\n");
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    ret = ipa_ldap_bind(bind_server, bind_princ, bind_dn, bind_pw, &ld);
    if (ret != LDAP_SUCCESS) {
        *err_msg = _("Failed to bind to server!\n");
        goto done;
    }

    /* perform extedned opt to get keytab */
    ret = ipa_ldap_extended_op(ld, KEYTAB_GET_OID, control, &srvctrl);
    if (ret != LDAP_SUCCESS) {
        goto done;
    }

    ret = find_control_data(srvctrl, KEYTAB_GET_OID, &data);
    if (ret != LDAP_SUCCESS) goto done;

    res = ipaasn1_dec_getktreply(data.bv_val, data.bv_len, kvno, keys);
    if (!res) {
        *err_msg = _("Failed to decode control reply!\n");
        ret = LDAP_OPERATIONS_ERROR;
        goto done;
    }

    ret = LDAP_SUCCESS;

done:
    if (ld) ldap_unbind_ext(ld, NULL, NULL);
    if (control) ber_bvfree(control);
    free(es);
    if (ret) {
        free_keys_contents(krbctx, keys);
    }
    return ret;
}

static char *ask_password(krb5_context krbctx)
{
    krb5_prompt ap_prompts[2];
    krb5_data k5d_pw0;
    krb5_data k5d_pw1;
    char pw0[256];
    char pw1[256];
    char *password;

    k5d_pw0.length = sizeof(pw0);
    k5d_pw0.data = pw0;
    ap_prompts[0].prompt = _("New Principal Password");
    ap_prompts[0].hidden = 1;
    ap_prompts[0].reply = &k5d_pw0;

    k5d_pw1.length = sizeof(pw1);
    k5d_pw1.data = pw1;
    ap_prompts[1].prompt = _("Verify Principal Password");
    ap_prompts[1].hidden = 1;
    ap_prompts[1].reply = &k5d_pw1;

    krb5_prompter_posix(krbctx, NULL,
                NULL, NULL,
                2, ap_prompts);

    if (strcmp(pw0, pw1)) {
        fprintf(stderr, _("Passwords do not match!"));
        return NULL;
    }

    password = malloc(k5d_pw0.length + 1);
    if (!password) return NULL;
    memcpy(password, pw0, k5d_pw0.length);
    password[k5d_pw0.length] = '\0';

    return password;
}

int main(int argc, const char *argv[])
{
	static const char *server = NULL;
	static const char *principal = NULL;
	static const char *keytab = NULL;
	static const char *enctypes_string = NULL;
	static const char *binddn = NULL;
	static const char *bindpw = NULL;
	int quiet = 0;
	int askpass = 0;
	int permitted_enctypes = 0;
	int retrieve = 0;
        struct poptOption options[] = {
            { "quiet", 'q', POPT_ARG_NONE, &quiet, 0,
              _("Print as little as possible"), _("Output only on errors")},
            { "server", 's', POPT_ARG_STRING, &server, 0,
              _("Contact this specific KDC Server"),
              _("Server Name") },
            { "principal", 'p', POPT_ARG_STRING, &principal, 0,
              _("The principal to get a keytab for (ex: ftp/ftp.example.com@EXAMPLE.COM)"),
              _("Kerberos Service Principal Name") },
            { "keytab", 'k', POPT_ARG_STRING, &keytab, 0,
              _("File were to store the keytab information"),
              _("Keytab File Name") },
	    { "enctypes", 'e', POPT_ARG_STRING, &enctypes_string, 0,
              _("Encryption types to request"),
              _("Comma separated encryption types list") },
	    { "permitted-enctypes", 0, POPT_ARG_NONE, &permitted_enctypes, 0,
              _("Show the list of permitted encryption types and exit"),
              _("Permitted Encryption Types") },
	    { "password", 'P', POPT_ARG_NONE, &askpass, 0,
              _("Asks for a non-random password to use for the principal"), NULL },
	    { "binddn", 'D', POPT_ARG_STRING, &binddn, 0,
              _("LDAP DN"), _("DN to bind as if not using kerberos") },
	    { "bindpw", 'w', POPT_ARG_STRING, &bindpw, 0,
              _("LDAP password"), _("password to use if not using kerberos") },
	    { "retrieve", 'r', POPT_ARG_NONE, &retrieve, 0,
              _("Retrieve current keys without changing them"), NULL },
            POPT_AUTOHELP
            POPT_TABLEEND
	};
	poptContext pc;
	char *ktname;
	char *password = NULL;
	krb5_context krbctx;
	krb5_ccache ccache;
	krb5_principal uprinc = NULL;
	krb5_principal sprinc;
	krb5_error_code krberr;
	struct keys_container keys = { 0 };
	krb5_keytab kt;
	int kvno;
	int i, ret;
	char *err_msg;

    ret = init_gettext();
    if (ret) {
        exit(1);
    }

	krberr = krb5_init_context(&krbctx);
	if (krberr) {
		fprintf(stderr, _("Kerberos context initialization failed\n"));
		exit(1);
	}

	pc = poptGetContext("ipa-getkeytab", argc, (const char **)argv, options, 0);
	ret = poptGetNextOpt(pc);
	if (ret == -1 && permitted_enctypes &&
	    !(server || principal || keytab || quiet)) {
		krb5_enctype *ktypes;
		char enc[79]; /* fit std terminal or truncate */

		krberr = krb5_get_permitted_enctypes(krbctx, &ktypes);
		if (krberr) {
			fprintf(stderr, _("No system preferred enctypes ?!\n"));
			exit(1);
		}
		fprintf(stdout, _("Supported encryption types:\n"));
		for (i = 0; ktypes[i]; i++) {
			krberr = krb5_enctype_to_string(ktypes[i], enc, 79);
			if (krberr) {
				fprintf(stderr, _("Warning: "
                                        "failed to convert type (#%d)\n"), i);
				continue;
			}
			fprintf(stdout, "%s\n", enc);
		}
		ipa_krb5_free_ktypes(krbctx, ktypes);
		exit (0);
	}

	if (ret != -1 || !server || !principal || !keytab || permitted_enctypes) {
		if (!quiet) {
			poptPrintUsage(pc, stderr, 0);
		}
		exit(2);
	}

	if (NULL!=binddn && NULL==bindpw) {
		fprintf(stderr,
                        _("Bind password required when using a bind DN.\n"));
		if (!quiet)
			poptPrintUsage(pc, stderr, 0);
		exit(10);
	}

    if (askpass && retrieve) {
        fprintf(stderr, _("Incompatible options provided (-r and -P)\n"));
        exit(2);
    }

        if (askpass) {
		password = ask_password(krbctx);
		if (!password) {
			exit(2);
		}
	} else if (enctypes_string && strchr(enctypes_string, ':')) {
		if (!quiet) {
			fprintf(stderr, _("Warning: salt types are not honored"
                                " with randomized passwords (see opt. -P)\n"));
		}
	}

	ret = asprintf(&ktname, "WRFILE:%s", keytab);
	if (ret == -1) {
		exit(3);
	}

	krberr = krb5_parse_name(krbctx, principal, &sprinc);
	if (krberr) {
		fprintf(stderr, _("Invalid Service Principal Name\n"));
		exit(4);
	}

	if (NULL == bindpw) {
		krberr = krb5_cc_default(krbctx, &ccache);
		if (krberr) {
			fprintf(stderr,
                                _("Kerberos Credential Cache not found. "
				  "Do you have a Kerberos Ticket?\n"));
			exit(5);
		}

		krberr = krb5_cc_get_principal(krbctx, ccache, &uprinc);
		if (krberr) {
			fprintf(stderr,
                                _("Kerberos User Principal not found. "
				  "Do you have a valid Credential Cache?\n"));
			exit(6);
		}
	}

	krberr = krb5_kt_resolve(krbctx, ktname, &kt);
	if (krberr) {
		fprintf(stderr, _("Failed to open Keytab\n"));
		exit(7);
	}

    kvno = -1;
    ret = ldap_get_keytab(krbctx, (retrieve == 0), password, enctypes_string,
                          server, principal, uprinc, binddn, bindpw,
                          &keys, &kvno, &err_msg);
    if (ret) {
        if (!quiet && err_msg != NULL) {
            fprintf(stderr, "%s", err_msg);
        }
    }

    if (retrieve == 0 && kvno == -1) {
        if (!quiet) {
            fprintf(stderr,
                    _("Retrying with pre-4.0 keytab retrieval method...\n"));
        }

        /* create key material */
        ret = create_keys(krbctx, sprinc, password, enctypes_string, &keys, &err_msg);
        if (!ret) {
            if (err_msg != NULL) {
                fprintf(stderr, "%s", err_msg);
            }

            fprintf(stderr, _("Failed to create key material\n"));
            exit(8);
        }

        kvno = ldap_set_keytab(krbctx, server, principal, uprinc, binddn, bindpw, &keys);
    }

    if (kvno == -1) {
        fprintf(stderr, _("Failed to get keytab\n"));
        exit(9);
    }

	for (i = 0; i < keys.nkeys; i++) {
		krb5_keytab_entry kt_entry;
		memset((char *)&kt_entry, 0, sizeof(kt_entry));
		kt_entry.principal = sprinc;
		kt_entry.key = keys.ksdata[i].key;
		kt_entry.vno = kvno;

		krberr = krb5_kt_add_entry(krbctx, kt, &kt_entry);
		if (krberr) {
			fprintf(stderr,
                                _("Failed to add key to the keytab\n"));
			exit (11);
		}
	}

	free_keys_contents(krbctx, &keys);

	krberr = krb5_kt_close(krbctx, kt);
	if (krberr) {
		fprintf(stderr, _("Failed to close the keytab\n"));
		exit (12);
	}

	if (!quiet) {
		fprintf(stderr,
			_("Keytab successfully retrieved and stored in: %s\n"),
			keytab);
	}
	exit(0);
}
