/* $OpenLDAP$ */
/*
 * Copyright 1998-2000 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/* ad.c - routines for dealing with attribute descriptions */

#include "portable.h"

#include <stdio.h>

#include <ac/ctype.h>
#include <ac/errno.h>
#include <ac/socket.h>
#include <ac/string.h>
#include <ac/time.h>

#include "ldap_pvt.h"
#include "slap.h"

extern ldap_pvt_thread_mutex_t	ad_mutex;	/* init.c */

static int ad_keystring(
	struct berval *bv )
{
	ber_len_t i;

	if( !AD_CHAR( bv->bv_val[0] ) ) {
		return 1;
	}

	for( i=1; i<bv->bv_len; i++ ) {
		if( !AD_CHAR( bv->bv_val[i] ) ) {
			return 1;
		}
	}
	return 0;
}

void ad_destroy( void *in )
{
	AttributeDescription *ad = in, *n;

	for (;ad;ad = n) {
		n = ad->ad_next;
		ldap_memfree(ad);
	}
}

int slap_str2ad(
	const char *str,
	AttributeDescription **ad,
	const char **text )
{
	struct berval bv;
	bv.bv_val = (char *) str;
	bv.bv_len = strlen( str );

	return slap_bv2ad( &bv, ad, text );
}

int slap_bv2ad(
	struct berval *bv,
	AttributeDescription **ad,
	const char **text )
{
	int rtn = LDAP_UNDEFINED_TYPE;
	int i;
	AttributeDescription desc, *d2;
	char *name, *options;

	assert( ad != NULL );
	assert( *ad == NULL ); /* temporary */

	if( bv == NULL || bv->bv_len == 0 ) {
		*text = "empty attribute description";
		return rtn;
	}

	/* make sure description is IA5 */
	if( ad_keystring( bv ) ) {
		*text = "attribute description contains inappropriate characters";
		return rtn;
	}

	/* find valid base attribute type; parse in place */
	name = bv->bv_val;
	options = strchr(name, ';');
	if (options != NULL) *options = '\0';
	desc.ad_type = at_find( name );
	if (options != NULL) *options = ';';
	if( desc.ad_type == NULL ) {
		*text = "attribute type undefined";

		return rtn;
	}

	if (options != NULL)
		desc.ad_cname.bv_len = options - name;
	else
		desc.ad_cname.bv_len = strlen(name);

	desc.ad_flags = SLAP_DESC_NONE;
	desc.ad_lang.bv_len = 0;
	desc.ad_lang.bv_val = NULL;

	/* parse options in place */
	for( ; options != NULL; ) {
		name = options+1;
		options = strchr( name, ';' );
		if ( options != NULL )
			i = options - name;
		else
			i = strlen(name);

		if( i == sizeof("binary")-1 && strncasecmp( name, "binary", i) == 0 ) {
			if( slap_ad_is_binary( &desc ) ) {
				*text = "option \"binary\" specified multiple times";
				goto done;
			}

			if( !slap_syntax_is_binary( desc.ad_type->sat_syntax )) {
				/* not stored in binary, disallow option */
				*text = "option \"binary\" with type not supported";
				goto done;
			}

			desc.ad_flags |= SLAP_DESC_BINARY;

		} else if ( i >= sizeof("lang-") && strncasecmp( name, "lang-",
			sizeof("lang-")-1 ) == 0)
		{
			if( desc.ad_lang.bv_len != 0 ) {
				*text = "multiple language tag options specified";
				goto done;
			}

			desc.ad_lang.bv_val = name;
			desc.ad_lang.bv_len = i;
		} else {
			*text = "unrecognized option";
			goto done;
		}
	}

	/* see if a matching description is already cached */
	for (d2 = desc.ad_type->sat_ad; d2; d2=d2->ad_next) {
		if (d2->ad_flags != desc.ad_flags)
			continue;
		if (d2->ad_lang.bv_len != desc.ad_lang.bv_len)
			continue;
		if (d2->ad_lang.bv_len == 0)
			break;
		if (strncasecmp(d2->ad_lang.bv_val, desc.ad_lang.bv_val,
			desc.ad_lang.bv_len) == 0)
			break;
	}

	/* Not found, add new one */
	while (d2 == NULL) {
		int dlen = 0;
		/* uses a single mutex instead of one per attributetype.
		 * I don't believe this is a significant bottleneck. If
		 * necessary, could change to a per-AttrType rwlock.
		 */
		ldap_pvt_thread_mutex_lock( &ad_mutex );
		/* Check again just in case another thread added it */
		for (d2 = desc.ad_type->sat_ad; d2; d2=d2->ad_next) {
			if (d2->ad_flags != desc.ad_flags)
				continue;
			if (d2->ad_lang.bv_len != desc.ad_lang.bv_len)
				continue;
			if (d2->ad_lang.bv_len == 0)
				break;
			if (strncasecmp(d2->ad_lang.bv_val,desc.ad_lang.bv_val,
				desc.ad_lang.bv_len) == 0)
				break;
		}
		/* Some other thread added it before we got the lock. */
		if (d2 != NULL) {
			ldap_pvt_thread_mutex_unlock( &ad_mutex );
			break;
		}

		/* Allocate a single contiguous block. If there are no
		 * options, we just need space for the AttrDesc structure.
		 * Otherwise, we need to tack on the full name length +
		 * options length.
		 */
		i = sizeof(AttributeDescription);
		if (desc.ad_lang.bv_len || desc.ad_flags != SLAP_DESC_NONE) {
			if (desc.ad_lang.bv_len)
				dlen = desc.ad_lang.bv_len+1;
			dlen += strlen(desc.ad_type->sat_cname)+1;
			if( slap_ad_is_binary( &desc ) ) {
				dlen += sizeof("binary");
			}
		}

		d2 = ch_malloc(i + dlen);
		d2->ad_type = desc.ad_type;
		d2->ad_flags = desc.ad_flags;
		d2->ad_cname.bv_len = desc.ad_cname.bv_len;
		d2->ad_lang.bv_len = desc.ad_lang.bv_len;
		if (dlen == 0) {
			d2->ad_cname.bv_val = d2->ad_type->sat_cname;
			d2->ad_lang.bv_val = NULL;
		} else {
			d2->ad_cname.bv_val = (char *)(d2+1);
			strcpy(d2->ad_cname.bv_val, d2->ad_type->sat_cname);
			if( slap_ad_is_binary( &desc ) ) {
				strcpy(d2->ad_cname.bv_val+d2->ad_cname.bv_len,
					";binary");
				d2->ad_cname.bv_len += sizeof("binary");
			}
			if( d2->ad_lang.bv_len ) {
				d2->ad_cname.bv_val[d2->ad_cname.bv_len++]=';';
				d2->ad_lang.bv_val = d2->ad_cname.bv_val+
					d2->ad_cname.bv_len;
				strncpy(d2->ad_lang.bv_val,desc.ad_lang.bv_val,
					d2->ad_lang.bv_len);
				d2->ad_lang.bv_val[d2->ad_lang.bv_len] = '\0';
				ldap_pvt_str2lower(d2->ad_lang.bv_val);
				d2->ad_cname.bv_len += d2->ad_lang.bv_len;
			}
		}
		/* Add new desc to list. We always want the bare Desc with
		 * no options to stay at the head of the list, assuming
		 * that one will be used most frequently.
		 */
		if (desc.ad_type->sat_ad == NULL || dlen == 0) {
			d2->ad_next = desc.ad_type->sat_ad;
			desc.ad_type->sat_ad = d2;
		} else {
			d2->ad_next = desc.ad_type->sat_ad->ad_next;
			desc.ad_type->sat_ad->ad_next = d2;
		}
		ldap_pvt_thread_mutex_unlock( &ad_mutex );
	}

	if( *ad == NULL ) {
		*ad = d2;
	} else {
		**ad = *d2;
	}

	rtn = LDAP_SUCCESS;

done:
	return rtn;
}

int is_ad_subtype(
	AttributeDescription *sub,
	AttributeDescription *super
)
{
	if( !is_at_subtype( sub->ad_type, super->ad_type ) ) {
		return 0;
	}

	if( super->ad_flags && ( super->ad_flags != sub->ad_flags )) {
		return 0;
	}

	if( super->ad_lang.bv_len && (sub->ad_lang.bv_len !=
		super->ad_lang.bv_len || strcmp( super->ad_lang.bv_val,
		sub->ad_lang.bv_val)))
	{
		return 0;
	}

	return 1;
}


int ad_inlist(
	AttributeDescription *desc,
	char **attrs )
{
	int i;
	for( i=0; attrs[i] != NULL; i++ ) {
		AttributeDescription *ad = NULL;
		const char *text;
		int rc;
		
		rc = slap_str2ad( attrs[i], &ad, &text );

		if( rc != LDAP_SUCCESS ) continue;

		rc = is_ad_subtype( desc, ad );

		if( rc ) return 1;
	}

	return 0;
}


int slap_str2undef_ad(
	const char *str,
	AttributeDescription **ad,
	const char **text )
{
	struct berval bv;
	bv.bv_val = (char *) str;
	bv.bv_len = strlen( str );

	return slap_bv2undef_ad( &bv, ad, text );
}

int slap_bv2undef_ad(
	struct berval *bv,
	AttributeDescription **ad,
	const char **text )
{
	AttributeDescription *desc;

	assert( ad != NULL );

	if( bv == NULL || bv->bv_len == 0 ) {
		*text = "empty attribute description";
		return LDAP_UNDEFINED_TYPE;
	}

	/* make sure description is IA5 */
	if( ad_keystring( bv ) ) {
		*text = "attribute description contains inappropriate characters";
		return LDAP_UNDEFINED_TYPE;
	}

	for (desc = slap_schema.si_at_undefined->sat_ad; desc;
		desc=desc->ad_next)
		if (desc->ad_cname.bv_len == bv->bv_len &&
		    !strcasecmp(desc->ad_cname.bv_val, bv->bv_val))
		    	break;
	
	if (!desc) {
		desc = ch_malloc(sizeof(AttributeDescription) +
			bv->bv_len + 1);
		
		desc->ad_flags = SLAP_DESC_NONE;
		desc->ad_lang.bv_val = NULL;
		desc->ad_lang.bv_len = 0;

		desc->ad_cname.bv_len = bv->bv_len;
		desc->ad_cname.bv_val = (char *)(desc+1);
		strcpy(desc->ad_cname.bv_val, bv->bv_val);

		/* canonical to upper case */
		ldap_pvt_str2upper( desc->ad_cname.bv_val );

		desc->ad_type = slap_schema.si_at_undefined;
		desc->ad_next = desc->ad_type->sat_ad;
		desc->ad_type->sat_ad = desc;
	}

	if (!*ad)
		*ad = desc;
	else
		**ad = *desc;

	return LDAP_SUCCESS;
}
