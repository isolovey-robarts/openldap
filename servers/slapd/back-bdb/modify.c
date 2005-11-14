/* modify.c - bdb backend modify routine */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2000-2005 The OpenLDAP Foundation.
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

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>
#include <ac/time.h>

#include "back-bdb.h"

static struct berval scbva[] = {
	BER_BVC("glue"),
	BER_BVNULL
};

int bdb_modify_internal(
	Operation *op,
	DB_TXN *tid,
	Modifications *modlist,
	Entry *e,
	const char **text,
	char *textbuf,
	size_t textlen )
{
	int rc, err;
	Modification	*mod;
	Modifications	*ml;
	Attribute	*save_attrs;
	Attribute 	*ap;
	int			glue_attr_delete = 0;

	Debug( LDAP_DEBUG_TRACE, "bdb_modify_internal: 0x%08lx: %s\n",
		e->e_id, e->e_dn, 0);

	if ( !acl_check_modlist( op, e, modlist )) {
		return LDAP_INSUFFICIENT_ACCESS;
	}

	/* save_attrs will be disposed of by bdb_cache_modify */
	save_attrs = e->e_attrs;
	e->e_attrs = attrs_dup( e->e_attrs );

	for ( ml = modlist; ml != NULL; ml = ml->sml_next ) {
		int match;
		mod = &ml->sml_mod;
		switch( mod->sm_op ) {
		case LDAP_MOD_ADD:
		case LDAP_MOD_REPLACE:
			if ( mod->sm_desc == slap_schema.si_ad_structuralObjectClass ) {
				value_match( &match, slap_schema.si_ad_structuralObjectClass,
					slap_schema.si_ad_structuralObjectClass->
						ad_type->sat_equality,
					SLAP_MR_VALUE_OF_ATTRIBUTE_SYNTAX,
					&mod->sm_values[0], &scbva[0], text );
				if ( !match ) glue_attr_delete = 1;
			}
		}
		if ( glue_attr_delete )
			break;
	}

	if ( glue_attr_delete ) {
		Attribute	**app = &e->e_attrs;
		while ( *app != NULL ) {
			if ( !is_at_operational( (*app)->a_desc->ad_type )) {
				Attribute *save = *app;
				*app = (*app)->a_next;
				attr_free( save );
				continue;
			}
			app = &(*app)->a_next;
		}
	}

	for ( ml = modlist; ml != NULL; ml = ml->sml_next ) {
		mod = &ml->sml_mod;

		switch ( mod->sm_op ) {
		case LDAP_MOD_ADD:
			Debug(LDAP_DEBUG_ARGS, "bdb_modify_internal: add\n", 0, 0, 0);
			err = modify_add_values( e, mod, get_permissiveModify(op),
				text, textbuf, textlen );
			if( err != LDAP_SUCCESS ) {
				Debug(LDAP_DEBUG_ARGS, "bdb_modify_internal: %d %s\n",
					err, *text, 0);
			}
			break;

		case LDAP_MOD_DELETE:
			if ( glue_attr_delete ) {
				err = LDAP_SUCCESS;
				break;
			}

			Debug(LDAP_DEBUG_ARGS, "bdb_modify_internal: delete\n", 0, 0, 0);
			err = modify_delete_values( e, mod, get_permissiveModify(op),
				text, textbuf, textlen );
			assert( err != LDAP_TYPE_OR_VALUE_EXISTS );
			if( err != LDAP_SUCCESS ) {
				Debug(LDAP_DEBUG_ARGS, "bdb_modify_internal: %d %s\n",
					err, *text, 0);
			}
			break;

		case LDAP_MOD_REPLACE:
			Debug(LDAP_DEBUG_ARGS, "bdb_modify_internal: replace\n", 0, 0, 0);
			err = modify_replace_values( e, mod, get_permissiveModify(op),
				text, textbuf, textlen );
			if( err != LDAP_SUCCESS ) {
				Debug(LDAP_DEBUG_ARGS, "bdb_modify_internal: %d %s\n",
					err, *text, 0);
			}
			break;

		case LDAP_MOD_INCREMENT:
			Debug(LDAP_DEBUG_ARGS,
				"bdb_modify_internal: increment\n", 0, 0, 0);
			err = modify_increment_values( e, mod, get_permissiveModify(op),
				text, textbuf, textlen );
			if( err != LDAP_SUCCESS ) {
				Debug(LDAP_DEBUG_ARGS,
					"bdb_modify_internal: %d %s\n",
					err, *text, 0);
			}
			break;

		case SLAP_MOD_SOFTADD:
			Debug(LDAP_DEBUG_ARGS, "bdb_modify_internal: softadd\n", 0, 0, 0);
 			/* Avoid problems in index_add_mods()
 			 * We need to add index if necessary.
 			 */
 			mod->sm_op = LDAP_MOD_ADD;

			err = modify_add_values( e, mod, get_permissiveModify(op),
				text, textbuf, textlen );

 			mod->sm_op = SLAP_MOD_SOFTADD;

 			if ( err == LDAP_TYPE_OR_VALUE_EXISTS ) {
 				err = LDAP_SUCCESS;
 			}

			if( err != LDAP_SUCCESS ) {
				Debug(LDAP_DEBUG_ARGS, "bdb_modify_internal: %d %s\n",
					err, *text, 0);
			}
 			break;

		default:
			Debug(LDAP_DEBUG_ANY, "bdb_modify_internal: invalid op %d\n",
				mod->sm_op, 0, 0);
			*text = "Invalid modify operation";
			err = LDAP_OTHER;
			Debug(LDAP_DEBUG_ARGS, "bdb_modify_internal: %d %s\n",
				err, *text, 0);
		}

		if ( err != LDAP_SUCCESS ) {
			attrs_free( e->e_attrs );
			e->e_attrs = save_attrs;
			/* unlock entry, delete from cache */
			return err; 
		}

		/* If objectClass was modified, reset the flags */
		if ( mod->sm_desc == slap_schema.si_ad_objectClass ) {
			e->e_ocflags = 0;
		}

		if ( glue_attr_delete ) e->e_ocflags = 0;

		/* check if modified attribute was indexed
		 * but not in case of NOOP... */
		err = bdb_index_is_indexed( op->o_bd, mod->sm_desc );
		if ( err == LDAP_SUCCESS && !op->o_noop ) {
			ap = attr_find( save_attrs, mod->sm_desc );
			if ( ap ) ap->a_flags |= SLAP_ATTR_IXDEL;

			ap = attr_find( e->e_attrs, mod->sm_desc );
			if ( ap ) ap->a_flags |= SLAP_ATTR_IXADD;
		}
	}

	/* check that the entry still obeys the schema */
	rc = entry_schema_check( op, e, save_attrs, get_manageDIT(op),
		text, textbuf, textlen );
	if ( rc != LDAP_SUCCESS || op->o_noop ) {
		attrs_free( e->e_attrs );
		/* clear the indexing flags */
		for ( ap = save_attrs; ap != NULL; ap = ap->a_next ) {
			ap->a_flags = 0;
		}
		e->e_attrs = save_attrs;

		if ( rc != LDAP_SUCCESS ) {
			Debug( LDAP_DEBUG_ANY,
				"entry failed schema check: %s\n",
				*text, 0, 0 );
		}

		/* if NOOP then silently revert to saved attrs */
		return rc;
	}

	/* update the indices of the modified attributes */

	/* start with deleting the old index entries */
	for ( ap = save_attrs; ap != NULL; ap = ap->a_next ) {
		if ( ap->a_flags & SLAP_ATTR_IXDEL ) {
			rc = bdb_index_values( op, tid, ap->a_desc,
				ap->a_nvals,
				e->e_id, SLAP_INDEX_DELETE_OP );
			if ( rc != LDAP_SUCCESS ) {
				attrs_free( e->e_attrs );
				e->e_attrs = save_attrs;
				Debug( LDAP_DEBUG_ANY,
				       "Attribute index delete failure",
				       0, 0, 0 );
				return rc;
			}
			ap->a_flags &= ~SLAP_ATTR_IXDEL;
		}
	}

	/* add the new index entries */
	for ( ap = e->e_attrs; ap != NULL; ap = ap->a_next ) {
		if (ap->a_flags & SLAP_ATTR_IXADD) {
			rc = bdb_index_values( op, tid, ap->a_desc,
				ap->a_nvals,
				e->e_id, SLAP_INDEX_ADD_OP );
			if ( rc != LDAP_SUCCESS ) {
				attrs_free( e->e_attrs );
				e->e_attrs = save_attrs;
				Debug( LDAP_DEBUG_ANY,
				       "Attribute index add failure",
				       0, 0, 0 );
				return rc;
			}
			ap->a_flags &= ~SLAP_ATTR_IXADD;
		}
	}

	return rc;
}


int
bdb_modify( Operation *op, SlapReply *rs )
{
	struct bdb_info *bdb = (struct bdb_info *) op->o_bd->be_private;
	Entry		*e = NULL;
	EntryInfo	*ei = NULL;
	int		manageDSAit = get_manageDSAit( op );
	char textbuf[SLAP_TEXT_BUFLEN];
	size_t textlen = sizeof textbuf;
	DB_TXN	*ltid = NULL, *lt2;
	struct bdb_op_info opinfo = {0};
	Entry		dummy = {0};
	int			fakeroot = 0;

	u_int32_t	locker = 0;
	DB_LOCK		lock;

	int		num_retries = 0;

	LDAPControl **preread_ctrl = NULL;
	LDAPControl **postread_ctrl = NULL;
	LDAPControl *ctrls[SLAP_MAX_RESPONSE_CONTROLS];
	int num_ctrls = 0;

	int rc;

	Debug( LDAP_DEBUG_ARGS, LDAP_XSTRING(bdb_modify) ": %s\n",
		op->o_req_dn.bv_val, 0, 0 );

	ctrls[num_ctrls] = NULL;

	if ( !SLAP_SHADOW( op->o_bd ))
		slap_mods_opattrs( op, op->orm_modlist, 1 );

	if( 0 ) {
retry:	/* transaction retry */
		if ( dummy.e_attrs ) {
			attrs_free( dummy.e_attrs );
			dummy.e_attrs = NULL;
		}
		if( e != NULL ) {
			bdb_unlocked_cache_return_entry_w(&bdb->bi_cache, e);
			e = NULL;
		}
		Debug(LDAP_DEBUG_TRACE,
			LDAP_XSTRING(bdb_modify) ": retrying...\n", 0, 0, 0);

		rs->sr_err = TXN_ABORT( ltid );
		ltid = NULL;
		op->o_private = NULL;
		op->o_do_not_cache = opinfo.boi_acl_cache;
		if( rs->sr_err != 0 ) {
			rs->sr_err = LDAP_OTHER;
			rs->sr_text = "internal error";
			goto return_results;
		}
		if ( op->o_abandon ) {
			rs->sr_err = SLAPD_ABANDON;
			goto return_results;
		}
		ldap_pvt_thread_yield();
		bdb_trans_backoff( ++num_retries );
	}

	/* begin transaction */
	rs->sr_err = TXN_BEGIN( bdb->bi_dbenv, NULL, &ltid, 
		bdb->bi_db_opflags );
	rs->sr_text = NULL;
	if( rs->sr_err != 0 ) {
		Debug( LDAP_DEBUG_TRACE,
			LDAP_XSTRING(bdb_modify) ": txn_begin failed: "
			"%s (%d)\n", db_strerror(rs->sr_err), rs->sr_err, 0 );
		rs->sr_err = LDAP_OTHER;
		rs->sr_text = "internal error";
		goto return_results;
	}

	locker = TXN_ID ( ltid );

	opinfo.boi_bdb = op->o_bd;
	opinfo.boi_txn = ltid;
	opinfo.boi_locker = locker;
	opinfo.boi_err = 0;
	opinfo.boi_acl_cache = op->o_do_not_cache;
	op->o_private = &opinfo;

	/* get entry or ancestor */
	rs->sr_err = bdb_dn2entry( op, ltid, &op->o_req_ndn, &ei, 1,
		locker, &lock );

	if ( rs->sr_err != 0 ) {
		Debug( LDAP_DEBUG_TRACE,
			LDAP_XSTRING(bdb_modify) ": dn2entry failed (%d)\n",
			rs->sr_err, 0, 0 );
		switch( rs->sr_err ) {
		case DB_LOCK_DEADLOCK:
		case DB_LOCK_NOTGRANTED:
			goto retry;
		case DB_NOTFOUND:
			if ( BER_BVISEMPTY( &op->o_req_ndn )) {
				struct berval gluebv = BER_BVC("glue");
				e = ch_calloc( 1, sizeof(Entry));
				e->e_name.bv_val = ch_strdup( "" );
				ber_dupbv( &e->e_nname, &e->e_name );
				attr_merge_one( e, slap_schema.si_ad_objectClass,
					&gluebv, NULL );
				attr_merge_one( e, slap_schema.si_ad_structuralObjectClass,
					&gluebv, NULL );
				e->e_private = ei;
				fakeroot = 1;
				rs->sr_err = 0;
			}
			break;
		case LDAP_BUSY:
			rs->sr_text = "ldap server busy";
			goto return_results;
		default:
			rs->sr_err = LDAP_OTHER;
			rs->sr_text = "internal error";
			goto return_results;
		}
	}

	if ( !fakeroot ) {
		e = ei->bei_e;
	}

	/* acquire and lock entry */
	/* FIXME: dn2entry() should return non-glue entry */
	if (( rs->sr_err == DB_NOTFOUND ) ||
		( !manageDSAit && e && is_entry_glue( e )))
	{
		if ( e != NULL ) {
			rs->sr_matched = ch_strdup( e->e_dn );
			rs->sr_ref = is_entry_referral( e )
				? get_entry_referrals( op, e )
				: NULL;
			bdb_unlocked_cache_return_entry_r (&bdb->bi_cache, e);
			e = NULL;

		} else {
			rs->sr_ref = referral_rewrite( default_referral, NULL,
				&op->o_req_dn, LDAP_SCOPE_DEFAULT );
		}

		rs->sr_err = LDAP_REFERRAL;
		send_ldap_result( op, rs );

		if ( rs->sr_ref != default_referral ) {
			ber_bvarray_free( rs->sr_ref );
		}
		free( (char *)rs->sr_matched );
		rs->sr_ref = NULL;
		rs->sr_matched = NULL;

		goto done;
	}

	if ( !manageDSAit && is_entry_referral( e ) ) {
		/* entry is a referral, don't allow modify */
		rs->sr_ref = get_entry_referrals( op, e );

		Debug( LDAP_DEBUG_TRACE,
			LDAP_XSTRING(bdb_modify) ": entry is referral\n",
			0, 0, 0 );

		rs->sr_err = LDAP_REFERRAL;
		rs->sr_matched = e->e_name.bv_val;
		send_ldap_result( op, rs );

		ber_bvarray_free( rs->sr_ref );
		rs->sr_ref = NULL;
		rs->sr_matched = NULL;
		goto done;
	}

	if ( get_assert( op ) &&
		( test_filter( op, e, get_assertion( op )) != LDAP_COMPARE_TRUE ))
	{
		rs->sr_err = LDAP_ASSERTION_FAILED;
		goto return_results;
	}

	if( op->o_preread ) {
		if( preread_ctrl == NULL ) {
			preread_ctrl = &ctrls[num_ctrls++];
			ctrls[num_ctrls] = NULL;
		}
		if ( slap_read_controls( op, rs, e,
			&slap_pre_read_bv, preread_ctrl ) )
		{
			Debug( LDAP_DEBUG_TRACE,
				"<=- " LDAP_XSTRING(bdb_modify) ": pre-read failed!\n",
				0, 0, 0 );
			goto return_results;
		}
	}

	/* nested transaction */
	rs->sr_err = TXN_BEGIN( bdb->bi_dbenv, ltid, &lt2, bdb->bi_db_opflags );
	rs->sr_text = NULL;
	if( rs->sr_err != 0 ) {
		Debug( LDAP_DEBUG_TRACE,
			LDAP_XSTRING(bdb_modify) ": txn_begin(2) failed: " "%s (%d)\n",
			db_strerror(rs->sr_err), rs->sr_err, 0 );
		rs->sr_err = LDAP_OTHER;
		rs->sr_text = "internal error";
		goto return_results;
	}
	/* Modify the entry */
	dummy = *e;
	rs->sr_err = bdb_modify_internal( op, lt2, op->oq_modify.rs_modlist,
		&dummy, &rs->sr_text, textbuf, textlen );

	if( rs->sr_err != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE,
			LDAP_XSTRING(bdb_modify) ": modify failed (%d)\n",
			rs->sr_err, 0, 0 );
		if ( (rs->sr_err == LDAP_INSUFFICIENT_ACCESS) && opinfo.boi_err ) {
			rs->sr_err = opinfo.boi_err;
		}
		/* Only free attrs if they were dup'd.  */
		if ( dummy.e_attrs == e->e_attrs ) dummy.e_attrs = NULL;
		switch( rs->sr_err ) {
		case DB_LOCK_DEADLOCK:
		case DB_LOCK_NOTGRANTED:
			goto retry;
		}
		goto return_results;
	}

	/* change the entry itself */
	rs->sr_err = bdb_id2entry_update( op->o_bd, lt2, &dummy );
	if ( rs->sr_err != 0 ) {
		Debug( LDAP_DEBUG_TRACE,
			LDAP_XSTRING(bdb_modify) ": id2entry update failed " "(%d)\n",
			rs->sr_err, 0, 0 );
		switch( rs->sr_err ) {
		case DB_LOCK_DEADLOCK:
		case DB_LOCK_NOTGRANTED:
			goto retry;
		}
		rs->sr_text = "entry update failed";
		goto return_results;
	}

	if ( TXN_COMMIT( lt2, 0 ) != 0 ) {
		rs->sr_err = LDAP_OTHER;
		rs->sr_text = "txn_commit(2) failed";
		goto return_results;
	}

	if( op->o_postread ) {
		if( postread_ctrl == NULL ) {
			postread_ctrl = &ctrls[num_ctrls++];
			ctrls[num_ctrls] = NULL;
		}
		if( slap_read_controls( op, rs, &dummy,
			&slap_post_read_bv, postread_ctrl ) )
		{
			Debug( LDAP_DEBUG_TRACE,
				"<=- " LDAP_XSTRING(bdb_modify)
				": post-read failed!\n", 0, 0, 0 );
			goto return_results;
		}
	}

	if( op->o_noop ) {
		if ( ( rs->sr_err = TXN_ABORT( ltid ) ) != 0 ) {
			rs->sr_text = "txn_abort (no-op) failed";
		} else {
			rs->sr_err = LDAP_X_NO_OPERATION;
			ltid = NULL;
			goto return_results;
		}
	} else {
		/* may have changed in bdb_modify_internal() */
		e->e_ocflags = dummy.e_ocflags;
		if ( fakeroot ) {
			e->e_private = NULL;
			entry_free( e );
		} else {
			rc = bdb_cache_modify( e, dummy.e_attrs, bdb->bi_dbenv, locker, &lock );
			switch( rc ) {
			case DB_LOCK_DEADLOCK:
			case DB_LOCK_NOTGRANTED:
				goto retry;
			}
		}
		dummy.e_attrs = NULL;

		rs->sr_err = TXN_COMMIT( ltid, 0 );
	}
	ltid = NULL;
	op->o_private = NULL;

	if( rs->sr_err != 0 ) {
		Debug( LDAP_DEBUG_TRACE,
			LDAP_XSTRING(bdb_modify) ": txn_%s failed: %s (%d)\n",
			op->o_noop ? "abort (no-op)" : "commit",
			db_strerror(rs->sr_err), rs->sr_err );
		rs->sr_err = LDAP_OTHER;
		rs->sr_text = "commit failed";

		goto return_results;
	}

	Debug( LDAP_DEBUG_TRACE,
		LDAP_XSTRING(bdb_modify) ": updated%s id=%08lx dn=\"%s\"\n",
		op->o_noop ? " (no-op)" : "",
		dummy.e_id, op->o_req_dn.bv_val );

	rs->sr_err = LDAP_SUCCESS;
	rs->sr_text = NULL;
	if( num_ctrls ) rs->sr_ctrls = ctrls;

return_results:
	if( dummy.e_attrs ) {
		attrs_free( dummy.e_attrs );
	}
	send_ldap_result( op, rs );
	if ( !SLAP_SHADOW( op->o_bd ))
		slap_graduate_commit_csn( op );

	if( rs->sr_err == LDAP_SUCCESS && bdb->bi_txn_cp ) {
		ldap_pvt_thread_yield();
		TXN_CHECKPOINT( bdb->bi_dbenv,
			bdb->bi_txn_cp_kbyte, bdb->bi_txn_cp_min, 0 );
	}

done:
	if( ltid != NULL ) {
		TXN_ABORT( ltid );
	}
	op->o_private = NULL;

	if( e != NULL ) {
		bdb_unlocked_cache_return_entry_w (&bdb->bi_cache, e);
	}

	if( preread_ctrl != NULL ) {
		slap_sl_free( (*preread_ctrl)->ldctl_value.bv_val, op->o_tmpmemctx );
		slap_sl_free( *preread_ctrl, op->o_tmpmemctx );
	}
	if( postread_ctrl != NULL ) {
		slap_sl_free( (*postread_ctrl)->ldctl_value.bv_val, op->o_tmpmemctx );
		slap_sl_free( *postread_ctrl, op->o_tmpmemctx );
	}
	return rs->sr_err;
}
