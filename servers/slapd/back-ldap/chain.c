/* chain.c - chain LDAP operations */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2003-2005 The OpenLDAP Foundation.
 * Portions Copyright 2003 Howard Chu.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by the Howard Chu for inclusion
 * in OpenLDAP Software.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"
#include "back-ldap.h"

static BackendInfo *lback;

#if 0
static int
ldap_chain_chk_referrals( Operation *op, SlapReply *rs )
{
	return LDAP_SUCCESS;
}
#endif

static int
ldap_chain_operational( Operation *op, SlapReply *rs )
{
	/* trap entries generated by back-ldap.
	 * FIXME: we need a better way to recognize them; a cleaner
	 * solution would be to be able to intercept the response
	 * of be_operational(), so that we can divert only those
	 * calls that fail because operational attributes were
	 * requested for entries that do not belong to the underlying
	 * database.  This fix is likely to intercept also entries
	 * generated by back-perl and so. */
	if ( rs->sr_entry->e_private == NULL ) {
		return 0;
	}

	return SLAP_CB_CONTINUE;
}

static int
ldap_chain_cb_response( Operation *op, SlapReply *rs )
{
	assert( op->o_tag == LDAP_REQ_SEARCH );

	if ( rs->sr_type == REP_SEARCH ) {
		Attribute	**ap = &rs->sr_entry->e_attrs;

		for ( ; *ap != NULL; ap = &(*ap)->a_next ) {
			/* will be generated later by frontend
			 * (a cleaner solution would be that
			 * the frontend checks if it already exists */
			if ( ad_cmp( (*ap)->a_desc, slap_schema.si_ad_entryDN ) == 0 )
			{
				Attribute *a = *ap;

				*ap = (*ap)->a_next;
				attr_free( a );

				/* there SHOULD be one only! */
				break;
			}
		}
		
		return SLAP_CB_CONTINUE;
	}

	return 0;
}

static int
ldap_chain_response( Operation *op, SlapReply *rs )
{
	slap_overinst	*on = (slap_overinst *) op->o_bd->bd_info;
	void		*private = op->o_bd->be_private;
	slap_callback	*sc = op->o_callback;
	int		rc = 0;
	int		cache = op->o_do_not_cache;
	char		*authzid = NULL;
	BerVarray	ref;
	struct berval	ndn = op->o_ndn;

	struct ldapinfo	li, *lip = (struct ldapinfo *)on->on_bi.bi_private;

	if ( rs->sr_err != LDAP_REFERRAL && rs->sr_type != REP_SEARCHREF )
		return SLAP_CB_CONTINUE;

	ref = rs->sr_ref;
	rs->sr_ref = NULL;

	op->o_callback = NULL;

	if ( lip->url == NULL ) {
		/* if we parse the URI then by no means 
		 * we can cache stuff or reuse connections, 
		 * because in back-ldap there's no caching
		 * based on the URI value, which is supposed
		 * to be set once for all (correct?) */
		op->o_do_not_cache = 1;

		/* FIXME: we're setting the URI of the first referral;
		 * what if there are more?  Is this something we should
		 * worry about? */
		li = *lip;
		op->o_bd->be_private = &li;

		if ( rs->sr_type != REP_SEARCHREF ) {
			LDAPURLDesc	*srv;
			char		*save_dn;

			/* parse reference and use 
			 * proto://[host][:port]/ only */
			rc = ldap_url_parse_ext( ref[0].bv_val, &srv );
			if ( rc != LDAP_URL_SUCCESS ) {
				/* error */
				return 1;
			}

			/* remove DN essentially because later on 
			 * ldap_initialize() will parse the URL 
			 * as a comma-separated URL list */
			save_dn = srv->lud_dn;
			srv->lud_dn = "";
			srv->lud_scope = LDAP_SCOPE_DEFAULT;
			li.url = ldap_url_desc2str( srv );
			srv->lud_dn = save_dn;
			ldap_free_urldesc( srv );

			if ( li.url == NULL ) {
				/* error */
				return 1;
			}
		}

	} else {
		op->o_bd->be_private = on->on_bi.bi_private;
	}

	/* Chaining can be performed by a privileged user on behalf
	 * of normal users, using the ProxyAuthz control, by exploiting
	 * the identity assertion feature of back-ldap; see idassert-*
	 * directives in slapd-ldap(5).
	 */

	switch ( op->o_tag ) {
	case LDAP_REQ_BIND: {
		struct berval	rndn = op->o_req_ndn;
		Connection	*conn = op->o_conn;

		op->o_req_ndn = slap_empty_bv;

		op->o_conn = NULL;
		rc = lback->bi_op_bind( op, rs );
		op->o_req_ndn = rndn;
		op->o_conn = conn;
		}
		break;
	case LDAP_REQ_ADD:
		{
		int		cleanup_attrs = 0;

		if ( op->ora_e->e_attrs == NULL ) {
			char		textbuf[ SLAP_TEXT_BUFLEN ];
			size_t		textlen = sizeof( textbuf );

			/* global overlay; create entry */
			/* NOTE: this is a hack to use the chain overlay
			 * as global.  I expect to be able to remove this
			 * soon by using slap_mods2entry() earlier in
			 * do_add(), adding the operational attrs later
			 * if required. */
			rs->sr_err = slap_mods2entry( op->ora_modlist,
					&op->ora_e, 0, 1,
					&rs->sr_text, textbuf, textlen );
			if ( rs->sr_err != LDAP_SUCCESS ) {
				send_ldap_result( op, rs );
				rc = 1;
				break;
			}
		}
		rc = lback->bi_op_add( op, rs );
		if ( cleanup_attrs ) {
			attrs_free( op->ora_e->e_attrs );
			op->ora_e->e_attrs = NULL;
		}
		break;
		}
	case LDAP_REQ_DELETE:
		rc = lback->bi_op_delete( op, rs );
		break;
	case LDAP_REQ_MODRDN:
		rc = lback->bi_op_modrdn( op, rs );
	    	break;
	case LDAP_REQ_MODIFY:
		rc = lback->bi_op_modify( op, rs );
		break;
	case LDAP_REQ_COMPARE:
		rc = lback->bi_op_compare( op, rs );
		break;
	case LDAP_REQ_SEARCH:
		if ( rs->sr_type == REP_SEARCHREF ) {
			struct berval	*curr = ref,
					odn = op->o_req_dn,
					ondn = op->o_req_ndn;
			slap_callback	sc2 = { 0 };
			int		tmprc = 0;
			ber_len_t	refcnt = 0;
			BerVarray	newref = NULL;

			sc2.sc_response = ldap_chain_cb_response;
			op->o_callback = &sc2;

			rs->sr_type = REP_SEARCH;

			/* copy the private info because we need to modify it */
			for ( ; !BER_BVISNULL( &curr[0] ); curr++ ) {
				LDAPURLDesc	*srv;
				char		*save_dn;

				/* parse reference and use
				 * proto://[host][:port]/ only */
				tmprc = ldap_url_parse_ext( curr[0].bv_val, &srv );
				if ( tmprc != LDAP_URL_SUCCESS ) {
					/* error */
					rc = 1;
					goto end_of_searchref;
				}

				/* remove DN essentially because later on 
				 * ldap_initialize() will parse the URL 
				 * as a comma-separated URL list */
				save_dn = srv->lud_dn;
				srv->lud_dn = "";
				srv->lud_scope = LDAP_SCOPE_DEFAULT;
				li.url = ldap_url_desc2str( srv );
				if ( li.url != NULL ) {
					ber_str2bv_x( save_dn, 0, 1, &op->o_req_dn,
							op->o_tmpmemctx );
					ber_dupbv_x( &op->o_req_ndn, &op->o_req_dn,
							op->o_tmpmemctx );
				}

				srv->lud_dn = save_dn;
				ldap_free_urldesc( srv );

				if ( li.url == NULL ) {
					/* error */
					rc = 1;
					goto end_of_searchref;
				}


				/* FIXME: should we also copy filter and scope?
				 * according to RFC3296, no */
				tmprc = lback->bi_op_search( op, rs );

				ldap_memfree( li.url );
				li.url = NULL;

				op->o_tmpfree( op->o_req_dn.bv_val,
						op->o_tmpmemctx );
				op->o_tmpfree( op->o_req_ndn.bv_val,
						op->o_tmpmemctx );

				if ( tmprc ) {
					/* error */
					rc = 1;
					goto end_of_searchref;
				}

				if ( rs->sr_err != LDAP_SUCCESS ) {
					/* if search was not successful,
					 * at least return the referral! */
					/* FIXME: assumes referrals 
					 * are always created via
					 * referral_rewrite() and freed via
					 * ber_bvarray_free( rs->sr_ref ) */
					newref = ch_realloc( newref, sizeof( struct berval ) * (refcnt + 2) );
					ber_dupbv( &newref[ refcnt ], &curr[ 0 ] );
					refcnt++;
					BER_BVZERO( &newref[ refcnt ] );
				}
			}

end_of_searchref:;
			op->o_req_dn = odn;
			op->o_req_ndn = ondn;
			rs->sr_type = REP_SEARCHREF;
			rs->sr_entry = NULL;

			/* if the error was bad, it was already returned
			 * by back-ldap; destroy the referrals left;
			 * otherwise, let the frontend return them. */
			if ( newref ) {
				if ( rc == 0 ) {
					rc = SLAP_CB_CONTINUE;
					if ( ref != default_referral ) {
						ber_bvarray_free( ref );
					}
					ref = newref;

				} else {
					ber_bvarray_free( newref );
				}
			}
			
		} else {
			rc = lback->bi_op_search( op, rs );
		}
	    	break;
	case LDAP_REQ_EXTENDED:
		rc = lback->bi_extended( op, rs );
		/* FIXME: ldap_back_extended() by design 
		 * doesn't send result; frontend is expected
		 * to send it... */
		if ( rc != SLAPD_ABANDON ) {
			send_ldap_extended( op, rs );
		}
		break;
	default:
		rc = SLAP_CB_CONTINUE;
		break;
	}
	op->o_do_not_cache = cache;
	op->o_bd->be_private = private;
	op->o_callback = sc;
	op->o_ndn = ndn;
	if ( authzid ) {
		op->o_tmpfree( authzid, op->o_tmpmemctx );
	}
	rs->sr_ref = ref;
	if ( lip->url == NULL && li.url != NULL ) {
		ldap_memfree( li.url );
	}

	return rc;
}

static int
ldap_chain_db_config(
	BackendDB	*be,
	const char	*fname,
	int		lineno,
	int		argc,
	char	**argv
)
{
	slap_overinst	*on = (slap_overinst *) be->bd_info;
	void		*private = be->be_private;
	char		*argv0 = NULL;
	int		rc;

	be->be_private = on->on_bi.bi_private;
	if ( strncasecmp( argv[ 0 ], "chain-", sizeof( "chain-" ) - 1 ) == 0 ) {
		argv0 = argv[ 0 ];
		argv[ 0 ] = &argv[ 0 ][ sizeof( "chain-" ) - 1 ];
	}
	rc = lback->bi_db_config( be, fname, lineno, argc, argv );
	if ( argv0 ) {
		argv[ 0 ] = argv0;
	}
	
	be->be_private = private;
	return rc;
}

static int
ldap_chain_db_init(
	BackendDB *be
)
{
	slap_overinst *on = (slap_overinst *) be->bd_info;
	void *private = be->be_private;
	int rc;

	if ( lback == NULL ) {
		lback = backend_info( "ldap" );

		if ( lback == NULL ) {
			return -1;
		}
	}

	be->be_private = NULL;
	rc = lback->bi_db_init( be );
	on->on_bi.bi_private = be->be_private;
	be->be_private = private;

	return rc;
}

static int
ldap_chain_db_destroy(
	BackendDB *be
)
{
	slap_overinst *on = (slap_overinst *) be->bd_info;
	void *private = be->be_private;
	int rc;

	be->be_private = on->on_bi.bi_private;
	rc = lback->bi_db_destroy( be );
	on->on_bi.bi_private = be->be_private;
	be->be_private = private;
	return rc;
}

static slap_overinst ldapchain;

int
chain_init( void )
{
	ldapchain.on_bi.bi_type = "chain";
	ldapchain.on_bi.bi_db_init = ldap_chain_db_init;
	ldapchain.on_bi.bi_db_config = ldap_chain_db_config;
	ldapchain.on_bi.bi_db_destroy = ldap_chain_db_destroy;
	
	/* ... otherwise the underlying backend's function would be called,
	 * likely passing an invalid entry; on the contrary, the requested
	 * operational attributes should have been returned while chasing
	 * the referrals.  This all in all is a bit messy, because part
	 * of the operational attributes are generated by they backend;
	 * part by the frontend; back-ldap should receive all the available
	 * ones from the remote server, but then, on it own, it strips those
	 * it assumes will be (re)generated by the frontend (e.g.
	 * subschemaSubentry.) */
	ldapchain.on_bi.bi_operational = ldap_chain_operational;
	
	ldapchain.on_response = ldap_chain_response;

#if 0
	ldapchain.on_bi.bi_chk_referrals = ldap_chain_chk_referrals;
#endif

	return overlay_register( &ldapchain );
}

