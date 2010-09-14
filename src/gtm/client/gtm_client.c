/*-------------------------------------------------------------------------
 *
 * gtm-client.c
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010 Nippon Telegraph and Telephone Corporation
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
/* Time in seconds to wait for a response from GTM */
/* We should consider making this a GUC */
#define CLIENT_GTM_TIMEOUT 20

#include <time.h>

#include "gtm/gtm_c.h"

#include "gtm/libpq-fe.h"
#include "gtm/libpq-int.h"

#include "gtm/gtm_client.h"
#include "gtm/gtm_msg.h"
#include "gtm/assert.h"

void GTM_FreeResult(GTM_Result *result, bool is_proxy);

/*
 * Connection Management API
 */
GTM_Conn *
connect_gtm(const char *connect_string)
{
	return PQconnectGTM(connect_string);
}

void
disconnect_gtm(GTM_Conn *conn)
{
	GTMPQfinish(conn);
}

/*
 * Transaction Management API
 */
GlobalTransactionId
begin_transaction(GTM_Conn *conn, GTM_IsolationLevel isolevel, GTM_Timestamp *timestamp)
{
	bool txn_read_only = false;
	GTM_Result *res = NULL;
	time_t finish_time;

	 /* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_TXN_BEGIN_GETGXID, sizeof (GTM_MessageType), conn) ||
		gtmpqPutInt(isolevel, sizeof (GTM_IsolationLevel), conn) ||
		gtmpqPutc(txn_read_only, conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	if (res->gr_status == 0)
	{
		if (timestamp)
			*timestamp = res->gr_resdata.grd_gxid_tp.timestamp;

		return res->gr_resdata.grd_gxid_tp.gxid;
	}
	else
		return InvalidGlobalTransactionId;

receive_failed:
send_failed:
	return InvalidGlobalTransactionId;
}

/*
 * Transaction Management API
 * Begin a transaction for an autovacuum worker process
 */
GlobalTransactionId
begin_transaction_autovacuum(GTM_Conn *conn, GTM_IsolationLevel isolevel)
{
	bool txn_read_only = false;
	GTM_Result *res = NULL;
	time_t finish_time;

	 /* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_TXN_BEGIN_GETGXID_AUTOVACUUM, sizeof (GTM_MessageType), conn) ||
		gtmpqPutInt(isolevel, sizeof (GTM_IsolationLevel), conn) ||
		gtmpqPutc(txn_read_only, conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	if (res->gr_status == 0)
		return res->gr_resdata.grd_gxid;
	else
		return InvalidGlobalTransactionId;

receive_failed:
send_failed:
	return InvalidGlobalTransactionId;
}

int
commit_transaction(GTM_Conn *conn, GlobalTransactionId gxid)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	 /* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_TXN_COMMIT, sizeof (GTM_MessageType), conn) ||
		gtmpqPutc(true, conn) ||
		gtmpqPutnchar((char *)&gxid, sizeof (GlobalTransactionId), conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	if (res->gr_status == 0)
	{
		Assert(res->gr_type == TXN_COMMIT_RESULT);
		Assert(res->gr_resdata.grd_gxid == gxid);
	}

	return res->gr_status;

receive_failed:
send_failed:
	return -1;
}

int
commit_prepared_transaction(GTM_Conn *conn, GlobalTransactionId gxid, GlobalTransactionId prepared_gxid)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	/* Start the message */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_TXN_COMMIT_PREPARED, sizeof (GTM_MessageType), conn) ||
		gtmpqPutc(true, conn) ||
		gtmpqPutnchar((char *)&gxid, sizeof (GlobalTransactionId), conn) ||
		gtmpqPutc(true, conn) ||
		gtmpqPutnchar((char *)&prepared_gxid, sizeof (GlobalTransactionId), conn))
		goto send_failed;

	/* Finish the message */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backends gets it */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	if (res->gr_status == 0)
	{
		Assert(res->gr_type == TXN_COMMIT_PREPARED_RESULT);
		Assert(res->gr_resdata.grd_gxid == gxid);
	}

send_failed:
receive_failed:
	return -1;
}

int 
abort_transaction(GTM_Conn *conn, GlobalTransactionId gxid)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	 /* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_TXN_ROLLBACK, sizeof (GTM_MessageType), conn) ||
		gtmpqPutc(true, conn) ||
		gtmpqPutnchar((char *)&gxid, sizeof (GlobalTransactionId), conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	if (res->gr_status == 0)
	{
		Assert(res->gr_type == TXN_ROLLBACK_RESULT);
		Assert(res->gr_resdata.grd_gxid == gxid);
	}

	return res->gr_status;

receive_failed:
send_failed:
	return -1;

}

int
being_prepared_transaction(GTM_Conn *conn, GlobalTransactionId gxid, char *gid,
					int datanodecnt, PGXC_NodeId datanodes[], int coordcnt,
					PGXC_NodeId coordinators[])
{
	GTM_Result *res = NULL;
	time_t finish_time;

	 /* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_TXN_BEING_PREPARED, sizeof (GTM_MessageType), conn) ||
		gtmpqPutc(true, conn) ||
		gtmpqPutnchar((char *)&gxid, sizeof (GlobalTransactionId), conn) ||
		/* Send also GID for an explicit prepared transaction */
		gtmpqPutInt(strlen(gid), sizeof (GTM_GIDLen), conn) ||
		gtmpqPutnchar((char *) gid, strlen(gid), conn) ||
		gtmpqPutInt(datanodecnt, sizeof (int), conn) ||
		gtmpqPutnchar((char *)datanodes, sizeof (PGXC_NodeId) * datanodecnt, conn) ||
		gtmpqPutInt(coordcnt, sizeof (int), conn))
		goto send_failed;

	/* Coordinator connections are not always involved in a transaction */
	if (coordcnt != 0 && gtmpqPutnchar((char *)coordinators, sizeof (PGXC_NodeId) * coordcnt, conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	if (res->gr_status == 0)
	{
		Assert(res->gr_type == TXN_BEING_PREPARED_RESULT);
		Assert(res->gr_resdata.grd_gxid == gxid);
	}

	return res->gr_status;

receive_failed:
send_failed:
	return -1;
}


int
prepare_transaction(GTM_Conn *conn, GlobalTransactionId gxid)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	 /* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_TXN_PREPARE, sizeof (GTM_MessageType), conn) ||
		gtmpqPutc(true, conn) ||
		gtmpqPutnchar((char *)&gxid, sizeof (GlobalTransactionId), conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	if (res->gr_status == 0)
	{
		Assert(res->gr_type == TXN_PREPARE_RESULT);
		Assert(res->gr_resdata.grd_gxid == gxid);
	}

	return res->gr_status;

receive_failed:
send_failed:
	return -1;
}

int
get_gid_data(GTM_Conn *conn,
			 GTM_IsolationLevel isolevel,
			 char *gid,
			 GlobalTransactionId *gxid,
			 GlobalTransactionId *prepared_gxid,
			 int *datanodecnt,
			 PGXC_NodeId **datanodes,
			 int *coordcnt,
			 PGXC_NodeId **coordinators)
{
	bool txn_read_only = false;
	GTM_Result *res = NULL;
	time_t finish_time;

	/* Start the message */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_TXN_GET_GID_DATA, sizeof (GTM_MessageType), conn) ||
		gtmpqPutInt(isolevel, sizeof (GTM_IsolationLevel), conn) ||
		gtmpqPutc(txn_read_only, conn) ||
		/* Send also GID for an explicit prepared transaction */
		gtmpqPutInt(strlen(gid), sizeof (GTM_GIDLen), conn) ||
		gtmpqPutnchar((char *) gid, strlen(gid), conn))
		goto send_failed;

	/* Finish the message */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	if (res->gr_status == 0)
	{
		*gxid = res->gr_resdata.grd_txn_get_gid_data.gxid;
		*prepared_gxid = res->gr_resdata.grd_txn_get_gid_data.prepared_gxid;
		*datanodes = res->gr_resdata.grd_txn_get_gid_data.datanodes;
		*coordinators = res->gr_resdata.grd_txn_get_gid_data.coordinators;
		*datanodecnt = res->gr_resdata.grd_txn_get_gid_data.datanodecnt;
		*coordcnt = res->gr_resdata.grd_txn_get_gid_data.coordcnt;
	}

	return res->gr_status;

receive_failed:
send_failed:
	return -1;
}

/*
 * Snapshot Management API
 */
GTM_SnapshotData *
get_snapshot(GTM_Conn *conn, GlobalTransactionId gxid, bool canbe_grouped)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	 /* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_SNAPSHOT_GET, sizeof (GTM_MessageType), conn) ||
		gtmpqPutc(canbe_grouped, conn) ||
		gtmpqPutc(true, conn) ||
		gtmpqPutnchar((char *)&gxid, sizeof (GlobalTransactionId), conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	if (res->gr_status == 0)
	{
		Assert(res->gr_type == SNAPSHOT_GET_RESULT);
		Assert(res->gr_resdata.grd_txn_snap_multi.gxid == gxid);
		return &(res->gr_snapshot);
	}
	else
		return NULL;


receive_failed:
send_failed:
	return NULL;
}

/*
 * Sequence Management API
 */
int
open_sequence(GTM_Conn *conn, GTM_SequenceKey key, GTM_Sequence increment,
			  GTM_Sequence minval, GTM_Sequence maxval,
			  GTM_Sequence startval, bool cycle)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	 /* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_SEQUENCE_INIT, sizeof (GTM_MessageType), conn) ||
		gtmpqPutInt(key->gsk_keylen, 4, conn) ||
		gtmpqPutnchar(key->gsk_key, key->gsk_keylen, conn) ||
		gtmpqPutnchar((char *)&increment, sizeof (GTM_Sequence), conn) ||
		gtmpqPutnchar((char *)&minval, sizeof (GTM_Sequence), conn) ||
		gtmpqPutnchar((char *)&maxval, sizeof (GTM_Sequence), conn) ||
		gtmpqPutnchar((char *)&startval, sizeof (GTM_Sequence), conn) ||
		gtmpqPutc(cycle, conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	return res->gr_status;

receive_failed:
send_failed:
	return -1;
}

int
alter_sequence(GTM_Conn *conn, GTM_SequenceKey key, GTM_Sequence increment,
			   GTM_Sequence minval, GTM_Sequence maxval,
			   GTM_Sequence startval, GTM_Sequence lastval, bool cycle, bool is_restart)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	/* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_SEQUENCE_ALTER, sizeof (GTM_MessageType), conn) ||
		gtmpqPutInt(key->gsk_keylen, 4, conn) ||
		gtmpqPutnchar(key->gsk_key, key->gsk_keylen, conn) ||
		gtmpqPutnchar((char *)&increment, sizeof (GTM_Sequence), conn) ||
		gtmpqPutnchar((char *)&minval, sizeof (GTM_Sequence), conn) ||
		gtmpqPutnchar((char *)&maxval, sizeof (GTM_Sequence), conn) ||
		gtmpqPutnchar((char *)&startval, sizeof (GTM_Sequence), conn) ||
		gtmpqPutnchar((char *)&lastval, sizeof (GTM_Sequence), conn) ||
		gtmpqPutc(cycle, conn) ||
		gtmpqPutc(is_restart, conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	return res->gr_status;

receive_failed:
send_failed:
 return -1;
}

int
close_sequence(GTM_Conn *conn, GTM_SequenceKey key)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	 /* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_SEQUENCE_CLOSE, sizeof (GTM_MessageType), conn) ||
		gtmpqPutInt(key->gsk_keylen, 4, conn) ||
		gtmpqPutnchar(key->gsk_key, key->gsk_keylen, conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	return res->gr_status;

receive_failed:
send_failed:
	return -1;
}

int
rename_sequence(GTM_Conn *conn, GTM_SequenceKey key, GTM_SequenceKey newkey)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	/* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_SEQUENCE_RENAME, sizeof (GTM_MessageType), conn) ||
		gtmpqPutInt(key->gsk_keylen, 4, conn) ||
		gtmpqPutnchar(key->gsk_key, key->gsk_keylen, conn)||
		gtmpqPutInt(newkey->gsk_keylen, 4, conn) ||
		gtmpqPutnchar(newkey->gsk_key, newkey->gsk_keylen, conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	return res->gr_status;

	receive_failed:
	send_failed:
	return -1;
}

GTM_Sequence
get_current(GTM_Conn *conn, GTM_SequenceKey key)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	 /* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_SEQUENCE_GET_CURRENT, sizeof (GTM_MessageType), conn) ||
		gtmpqPutInt(key->gsk_keylen, 4, conn) ||
		gtmpqPutnchar(key->gsk_key, key->gsk_keylen, conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	if (res->gr_status == 0)
		return res->gr_resdata.grd_seq.seqval;
	else
		return InvalidSequenceValue;

receive_failed:
send_failed:
	return -1;
}

int
set_val(GTM_Conn *conn, GTM_SequenceKey key, GTM_Sequence nextval, bool iscalled)
{
	GTM_Result *res = NULL;
    time_t finish_time;

	/* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_SEQUENCE_SET_VAL, sizeof (GTM_MessageType), conn) ||
		gtmpqPutInt(key->gsk_keylen, 4, conn) ||
		gtmpqPutnchar(key->gsk_key, key->gsk_keylen, conn) ||
		gtmpqPutnchar((char *)&nextval, sizeof (GTM_Sequence), conn) ||
		gtmpqPutc(iscalled, conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	return res->gr_status;

receive_failed:
send_failed:
	return -1;
}

GTM_Sequence
get_next(GTM_Conn *conn, GTM_SequenceKey key)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	/* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_SEQUENCE_GET_NEXT, sizeof (GTM_MessageType), conn) ||
		gtmpqPutInt(key->gsk_keylen, 4, conn) ||
		gtmpqPutnchar(key->gsk_key, key->gsk_keylen, conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	if (res->gr_status == 0)
		return res->gr_resdata.grd_seq.seqval;
	else
		return InvalidSequenceValue;

receive_failed:
send_failed:
	return -1;
}

int
reset_sequence(GTM_Conn *conn, GTM_SequenceKey key)
{
	GTM_Result *res = NULL;
	time_t finish_time;

	 /* Start the message. */
	if (gtmpqPutMsgStart('C', true, conn) ||
		gtmpqPutInt(MSG_SEQUENCE_RESET, sizeof (GTM_MessageType), conn) ||
		gtmpqPutInt(key->gsk_keylen, 4, conn) ||
		gtmpqPutnchar(key->gsk_key, key->gsk_keylen, conn))
		goto send_failed;

	/* Finish the message. */
	if (gtmpqPutMsgEnd(conn))
		goto send_failed;

	/* Flush to ensure backend gets it. */
	if (gtmpqFlush(conn))
		goto send_failed;

	finish_time = time(NULL) + CLIENT_GTM_TIMEOUT;
	if (gtmpqWaitTimed(true, false, conn, finish_time) ||
		gtmpqReadData(conn) < 0)
		goto receive_failed;

	if ((res = GTMPQgetResult(conn)) == NULL)
		goto receive_failed;

	return res->gr_status;

receive_failed:
send_failed:
	return -1;
}

void
GTM_FreeResult(GTM_Result *result, bool is_proxy)
{
	if (result == NULL)
		return;
	gtmpqFreeResultData(result, is_proxy);
	free(result);
}