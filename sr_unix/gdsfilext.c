/****************************************************************
 *								*
 *	Copyright 2001, 2013 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gtm_string.h"
#include "gtm_time.h"
#include <sys/mman.h>
#include <errno.h>
#include "gtm_unistd.h"
#include <signal.h>

#include "buddy_list.h"
#include "gdsroot.h"
#include "gdskill.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbml.h"
#include "gdsbt.h"
#include "gdsfhead.h"
#include "gdscc.h"
#include "filestruct.h"
#include "gtmio.h"
#include "iosp.h"
#include "jnl.h"
#include "hashtab_int4.h"     /* needed for tp.h */
#include "tp.h"
#include "eintr_wrappers.h"
#include "send_msg.h"
#include "gt_timer.h"
#include "mmseg.h"
#include "gdsblk.h"		/* needed for gds_blk_downgrade.h */
#include "gds_blk_downgrade.h"	/* for IS_GDS_BLK_DOWNGRADE_NEEDED macro */
#include "wbox_test_init.h"
#include "anticipatory_freeze.h"
/* Include prototypes */
#include "bit_set.h"
#include "disk_block_available.h"
#include "gds_map_moved.h"
#include "gtmmsg.h"
#include "gdsfilext.h"
#include "bm_getfree.h"
#include "gtmimagename.h"
#include "gtmdbglvl.h"
#include "min_max.h"
#include "repl_msg.h"
#include "gtmsource.h"
#include "error.h"

#define	GDSFILEXT_CLNUP						\
{								\
	if (!was_crit)						\
		rel_crit(gv_cur_region);			\
}

#define ISSUE_WAITDSKSPACE(TO_WAIT, WAIT_PERIOD, MECHANISM)									\
{																\
	uint4		seconds;												\
																\
	seconds = TO_WAIT + (CDB_STAGNATE - t_tries) * WAIT_PERIOD;								\
	MECHANISM(CSA_ARG(cs_addrs) VARLSTCNT(11) ERR_WAITDSKSPACE, 4, process_id, seconds, DB_LEN_STR(gv_cur_region), ERR_TEXT,\
			2, LEN_AND_LIT("Please make more disk space available or shutdown GT.M to avoid data loss"), ENOSPC);	\
}

#define SUSPICIOUS_EXTEND 	(2 * (dollar_tlevel ? sgm_info_ptr->cw_set_depth : cw_set_depth) < cs_addrs->ti->free_blocks)

GBLREF	sigset_t	blockalrm;
GBLREF	sgmnt_addrs	*cs_addrs;
GBLREF	sgmnt_data_ptr_t cs_data;
GBLREF	unsigned char	cw_set_depth;
GBLREF	uint4		dollar_tlevel;
GBLREF	gd_region	*gv_cur_region;
GBLREF	inctn_opcode_t	inctn_opcode;
GBLREF	boolean_t	mu_reorg_process;
GBLREF	uint4		process_id;
GBLREF	sgm_info	*sgm_info_ptr;
GBLREF	unsigned int	t_tries;
GBLREF	jnl_gbls_t	jgbl;
GBLREF	inctn_detail_t	inctn_detail;			/* holds detail to fill in to inctn jnl record */
GBLREF	boolean_t	gtm_dbfilext_syslog_disable;	/* control whether db file extension message is logged or not */
GBLREF	uint4		gtmDebugLevel;
GBLREF	jnlpool_addrs	jnlpool;

error_def(ERR_DBFILERR);
error_def(ERR_DBFILEXT);
error_def(ERR_DSKSPACEFLOW);
error_def(ERR_JNLFLUSH);
error_def(ERR_NOSPACEEXT);
error_def(ERR_SYSCALL);
error_def(ERR_TEXT);
error_def(ERR_TOTALBLKMAX);
error_def(ERR_WAITDSKSPACE);

OS_PAGE_SIZE_DECLARE

uint4	 gdsfilext(uint4 blocks, uint4 filesize, boolean_t trans_in_prog)
{
	sm_uc_ptr_t		old_base[2], mmap_retaddr;
	boolean_t		was_crit, is_mm;
	char			buff[DISK_BLOCK_SIZE];
	int			result, save_errno, status;
	uint4			new_bit_maps, bplmap, map, new_blocks, new_total, max_tot_blks;
	uint4			jnl_status, to_wait, to_msg, wait_period;
	gtm_uint64_t		avail_blocks, mmap_sz;
	off_t			new_eof;
	trans_num		curr_tn;
	unix_db_info		*udi;
	inctn_opcode_t		save_inctn_opcode;
	int4			prev_extend_blks_to_upgrd;
	jnl_private_control	*jpc;
	jnl_buffer_ptr_t	jbp;
	DCL_THREADGBL_ACCESS;

	assert(!IS_DSE_IMAGE);
	assert((cs_addrs->nl == NULL) || (process_id != cs_addrs->nl->trunc_pid)); /* mu_truncate shouldn't extend file... */
	assert(!process_exiting);
	DEBUG_ONLY(old_base[0] = old_base[1] = NULL);
	assert(!gv_cur_region->read_only);
	udi = FILE_INFO(gv_cur_region);
	is_mm = (dba_mm == cs_addrs->hdr->acc_meth);
#	if !defined(MM_FILE_EXT_OK)
	if (!udi->grabbed_access_sem && is_mm)
		return (uint4)(NO_FREE_SPACE); /* should this be changed to show extension not allowed ? */
#	endif
	/* Both blocks and total blocks are unsigned ints so make sure we aren't asking for huge numbers that will
	   overflow and end up doing silly things.
	*/
	assert((blocks <= (MAXTOTALBLKS(cs_data) - cs_data->trans_hist.total_blks)) || WBTEST_ENABLED(WBTEST_FILE_EXTEND_ERROR));
	if (!blocks)
		return (uint4)(NO_FREE_SPACE); /* should this be changed to show extension not enabled ? */
	bplmap = cs_data->bplmap;
	/* New total of non-bitmap blocks will be number of current, non-bitmap blocks, plus new blocks desired
	 * There are (bplmap - 1) non-bitmap blocks per bitmap, so add (bplmap - 2) to number of non-bitmap blocks
	 *      and divide by (bplmap - 1) to get total number of bitmaps for expanded database. (must round up in this
	 *      manner as every non-bitmap block must have an associated bitmap)
	 * Current number of bitmaps is (total number of current blocks + bplmap - 1) / bplmap.
	 * Subtract current number of bitmaps from number needed for expanded database to get number of new bitmaps needed.
	 */
	new_bit_maps = DIVIDE_ROUND_UP(cs_data->trans_hist.total_blks
			- DIVIDE_ROUND_UP(cs_data->trans_hist.total_blks, bplmap) + blocks, bplmap - 1)
			- DIVIDE_ROUND_UP(cs_data->trans_hist.total_blks, bplmap);
	new_blocks = blocks + new_bit_maps;
	assert(0 < (int)new_blocks);
	if (new_blocks + cs_data->trans_hist.total_blks > MAXTOTALBLKS(cs_data))
	{
		assert(FALSE);
		send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(1) ERR_TOTALBLKMAX);
		return (uint4)(NO_FREE_SPACE);
	}
	if (0 != (save_errno = disk_block_available(udi->fd, &avail_blocks, FALSE)))
	{
		send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(5) ERR_DBFILERR, 2, DB_LEN_STR(gv_cur_region), save_errno);
		rts_error_csa(CSA_ARG(cs_addrs) VARLSTCNT(5) ERR_DBFILERR, 2, DB_LEN_STR(gv_cur_region), save_errno);
	} else
	{
		if (!(gtmDebugLevel & GDL_IgnoreAvailSpace))
		{	/* Bypass this space check if debug flag above is on. Allows us to create a large sparce DB
			 * in space it could never fit it if wasn't sparse. Needed for some tests.
			 */
			avail_blocks = avail_blocks / (cs_data->blk_size / DISK_BLOCK_SIZE);
			if ((blocks * EXTEND_WARNING_FACTOR) > avail_blocks)
			{
				if (blocks > (uint4)avail_blocks)
				{
					SETUP_THREADGBL_ACCESS;
					if (!ANTICIPATORY_FREEZE_ENABLED(cs_addrs))
						return (uint4)(NO_FREE_SPACE);
					else
						send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(6) MAKE_MSG_WARNING(ERR_NOSPACEEXT), 4,
							DB_LEN_STR(gv_cur_region), new_blocks, (uint4)avail_blocks);
				} else
					send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(5) ERR_DSKSPACEFLOW, 3, DB_LEN_STR(gv_cur_region),
						 (uint4)(avail_blocks - ((new_blocks <= avail_blocks) ? new_blocks : 0)));
			}
		}
	}
	/* From here on, we need to use GDSFILEXT_CLNUP before returning to the caller */
	was_crit = cs_addrs->now_crit;
	assert(!cs_addrs->hold_onto_crit || was_crit);
	/* If we are coming from mupip_extend (which gets crit itself) we better have waited for any unfreezes to occur.
	 * If we are coming from online rollback (when that feature is available), we will come in holding crit and in
	 * 	the final retry. In that case too, we expect to have waited for unfreezes to occur in the caller itself.
	 * Therefore if we are coming in holding crit from MUPIP, we expect the db to be unfrozen so no need to wait for
	 * freeze.
	 * If we are coming from GT.M and final retry (in which case we come in holding crit) we expect to have waited
	 * 	for any unfreezes (by invoking tp_crit_all_regions) to occur (TP or non-TP) before coming into this
	 *	function. However, there is one exception. In the final retry, if tp_crit_all_regions notices that
	 *	at least one of the participating regions did ONLY READs, it will not wait for any freeze on THAT region
	 *	to complete before grabbing crit. Later, in the final retry, if THAT region did an update which caused
	 *	op_tcommit to invoke bm_getfree->gdsfilext, then we would have come here with a frozen region on which
	 *	we hold crit.
	 */
	assert(!was_crit || !cs_data->freeze || (dollar_tlevel && (CDB_STAGNATE <= t_tries)));
	/*
	 * If we are in the final retry and already hold crit, it is possible that csa->nl->wc_blocked is also set to TRUE
	 * (by a concurrent process in phase2 which encountered an error in the midst of commit and secshr_db_clnup
	 * finished the job for it). In this case we do NOT want to invoke wcs_recover as that will update the "bt"
	 * transaction numbers without correspondingly updating the history transaction numbers (effectively causing
	 * a cdb_sc_blkmod type of restart). Therefore do NOT call grab_crit (which unconditionally invokes wcs_recover)
	 * if we already hold crit.
	 */
	if (!was_crit)
	{
		for ( ; ; )
		{
			grab_crit(gv_cur_region);
			if (!cs_data->freeze && !IS_REPL_INST_FROZEN)
				break;
			rel_crit(gv_cur_region);
			while (cs_data->freeze || IS_REPL_INST_FROZEN)
				hiber_start(1000);
		}
	} else if ((cs_data->freeze || IS_REPL_INST_FROZEN) && dollar_tlevel)
	{	/* We don't want to continue with file extension as explained above. Hence return with an error code which
		 * op_tcommit will recognize (as a cdb_sc_needcrit type of restart) and restart accordingly.
		 */
		assert(CDB_STAGNATE <= t_tries);
		GDSFILEXT_CLNUP;
		return (uint4)(FINAL_RETRY_FREEZE_PROG);
	}
	assert(cs_addrs->ti->total_blks == cs_data->trans_hist.total_blks);
	if (cs_data->trans_hist.total_blks != filesize)
	{	/* Somebody else has already extended it, since we are in crit, this is trust-worthy. However, in case of MM,
		 * we still need to remap the database
		 */
		assert((cs_data->trans_hist.total_blks > filesize) GTM_TRUNCATE_ONLY( || !is_mm));
		/* For BG, someone else could have truncated or extended - we have no idea */
		GDSFILEXT_CLNUP;
		return (SS_NORMAL);
	}
	if (trans_in_prog && SUSPICIOUS_EXTEND)
	{
		if (!was_crit)
		{
			GDSFILEXT_CLNUP;
			return (uint4)(EXTEND_SUSPECT);
		}
		/* If free_blocks counter is not ok, then correct it. Do the check again. If still fails, then it means we held
		 * crit through bm_getfree into gdsfilext and still didn't get it right.
		 */
		assertpro(!is_free_blks_ctr_ok() && !SUSPICIOUS_EXTEND);
	}
	if (JNL_ENABLED(cs_data))
	{
		if (!jgbl.dont_reset_gbl_jrec_time)
			SET_GBL_JREC_TIME;	/* needed before jnl_ensure_open as that can write jnl records */
		jpc = cs_addrs->jnl;
		jbp = jpc->jnl_buff;
		/* Before writing to jnlfile, adjust jgbl.gbl_jrec_time if needed to maintain time order
		 * of jnl records. This needs to be done BEFORE the jnl_ensure_open as that could write
		 * journal records (if it decides to switch to a new journal file).
		 */
		ADJUST_GBL_JREC_TIME(jgbl, jbp);
		jnl_status = jnl_ensure_open();
		if (jnl_status)
		{
			GDSFILEXT_CLNUP;
			send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(6) jnl_status, 4, JNL_LEN_STR(cs_data), DB_LEN_STR(gv_cur_region));
			return (uint4)(NO_FREE_SPACE);	/* should have better return status */
		}
	}
	if (is_mm)
	{
		cs_addrs->nl->mm_extender_pid = process_id;
		status = wcs_wtstart(gv_cur_region, 0);
		cs_addrs->nl->mm_extender_pid = 0;
		assertpro(SS_NORMAL == status);
		old_base[0] = cs_addrs->db_addrs[0];
		old_base[1] = cs_addrs->db_addrs[1];
		cs_addrs->db_addrs[0] = NULL; /* don't rely on it until the mmap below */
		status = munmap((caddr_t)old_base[0], (size_t)(old_base[1] - old_base[0]));
		if (0 != status)
		{
			save_errno = errno;
			GDSFILEXT_CLNUP;
			send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(12) ERR_DBFILERR, 2, DB_LEN_STR(gv_cur_region),
					ERR_SYSCALL, 5, LEN_AND_LIT("munmap()"), CALLFROM, save_errno);
			return (uint4)(NO_FREE_SPACE);
		}
	}
	CHECK_TN(cs_addrs, cs_data, cs_data->trans_hist.curr_tn);	/* can issue rts_error TNTOOLARGE */
	new_total = cs_data->trans_hist.total_blks + new_blocks;
	new_eof = ((off_t)(cs_data->start_vbn - 1) * DISK_BLOCK_SIZE) + ((off_t)new_total * cs_data->blk_size);
	DB_LSEEKWRITE(cs_addrs, udi->fn, udi->fd, new_eof, buff, DISK_BLOCK_SIZE, save_errno);
	if ((ENOSPC == save_errno) && IS_GTM_IMAGE)
	{
		/* Attempt to write every second, and send message to operator every 1/20 of cs_data->wait_disk_space */
		wait_period = to_wait = DIVIDE_ROUND_UP(cs_data->wait_disk_space, CDB_STAGNATE + 1);
		to_msg = (to_wait / 8) ? (to_wait / 8) : 1;		/* send around 8 messages during 1 wait_period */
		while ((to_wait > 0) && (ENOSPC == save_errno))
		{
			if ((to_wait == cs_data->wait_disk_space) || (to_wait % to_msg == 0))
				ISSUE_WAITDSKSPACE(to_wait, wait_period, send_msg_csa);
			hiber_start(1000);
			to_wait--;
			LSEEKWRITE(udi->fd, new_eof, buff, DISK_BLOCK_SIZE, save_errno);
		}
	}
	if (0 != save_errno)
	{
		GDSFILEXT_CLNUP;
		if (ENOSPC != save_errno)
			send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(5) ERR_DBFILERR, 2, DB_LEN_STR(gv_cur_region), save_errno);
		return (uint4)(NO_FREE_SPACE);
	}
	if (WBTEST_ENABLED(WBTEST_FILE_EXTEND_INTERRUPT_1))
	{
		LONG_SLEEP(600);
		assert(FALSE);
	}
	/* Ensure the EOF and metadata get to disk BEFORE any bitmap writes. Otherwise, the file size could no longer reflect
	 * a proper extent and subsequent invocations of gdsfilext could corrupt the database.
	 */
	GTM_DB_FSYNC(cs_addrs, udi->fd, status);
	assert(0 == status);
	if (0 != status)
	{
		GDSFILEXT_CLNUP;
		send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(8) ERR_DBFILERR, 5, RTS_ERROR_LITERAL("fsync1()"), CALLFROM, status);
		return (uint4)(NO_FREE_SPACE);
	}
	if (WBTEST_ENABLED(WBTEST_FILE_EXTEND_INTERRUPT_2))
	{
		LONG_SLEEP(600);
		assert(FALSE); /* Should be killed before that */
	}
	DEBUG_ONLY(prev_extend_blks_to_upgrd = cs_data->blks_to_upgrd;)
	/* inctn_detail.blks_to_upgrd_delta holds the increase in "csd->blks_to_upgrd" due to the file extension */
	inctn_detail.blks2upgrd_struct.blks_to_upgrd_delta =
			(IS_GDS_BLK_DOWNGRADE_NEEDED(cs_data->desired_db_format) ? new_bit_maps : 0);
	if (JNL_ENABLED(cs_data))
	{
		save_inctn_opcode = inctn_opcode;
		if (mu_reorg_process)
			inctn_opcode = inctn_gdsfilext_mu_reorg;
		else
			inctn_opcode = inctn_gdsfilext_gtm;
		if (0 == jpc->pini_addr)
			jnl_put_jrt_pini(cs_addrs);
		jnl_write_inctn_rec(cs_addrs);
		inctn_opcode = save_inctn_opcode;
		/* Harden INCTN to disk before updating/flushing database. This will ensure that any positive adjustment to the
		 * blks_to_upgrd counter (stored in the inctn record) is seen by recovery before a V4 format bitmap block is.
		 */
		jnl_status = jnl_flush(gv_cur_region);
		if (SS_NORMAL != jnl_status)
		{
			send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(9) ERR_JNLFLUSH, 2, JNL_LEN_STR(cs_data),
				ERR_TEXT, 2, RTS_ERROR_TEXT("Error with journal flush during gdsfilext"),
				jnl_status);
			assert(NOJNL == jpc->channel); /* jnl file lost has been triggered */
			/* In this routine, all code that follows from here on does not assume anything about the
			 * journaling characteristics of this database so it is safe to continue execution even though
			 * journaling got closed in the middle. Let the caller deal with this situation.
			 */
		}
	}
	if (new_bit_maps)
	{
		for (map = ROUND_UP(cs_data->trans_hist.total_blks, bplmap); map < new_total; map += bplmap)
		{
			DEBUG_ONLY(new_bit_maps--;)
			if (SS_NORMAL != (status = bml_init(map)))
			{
				GDSFILEXT_CLNUP;
				send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(5) ERR_DBFILERR, 2, DB_LEN_STR(gv_cur_region), status);
				return (uint4)(NO_FREE_SPACE);
			}
		}
		assert(0 == new_bit_maps);
	}
	/* Ensures that if at all the file header write goes to disk before the crash, the bitmap blocks are all
	 * guaranteed to be initialized and synced to disk as well.
	 */
	if (WBTEST_ENABLED(WBTEST_FILE_EXTEND_INTERRUPT_3))
	{
		LONG_SLEEP(600);
		assert(FALSE); /* Should be killed before that */
	}
	GTM_DB_FSYNC(cs_addrs, udi->fd, status);
	assert(0 == status);
	if (0 != status)
	{
		GDSFILEXT_CLNUP;
		send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(8) ERR_DBFILERR, 5, RTS_ERROR_LITERAL("fsync2()"), CALLFROM, status);
		return (uint4)(NO_FREE_SPACE);
	}
	if (WBTEST_ENABLED(WBTEST_FILE_EXTEND_INTERRUPT_4))
	{
		LONG_SLEEP(600);
		assert(FALSE); /* Should be killed before that */
	}
	assert(cs_data->blks_to_upgrd == (inctn_detail.blks2upgrd_struct.blks_to_upgrd_delta + prev_extend_blks_to_upgrd));
	assert(0 < (int)blocks);
	assert(0 < (int)(cs_addrs->ti->free_blocks + blocks));
	cs_addrs->ti->free_blocks += blocks;
	blocks = cs_data->trans_hist.total_blks;
	cs_addrs->total_blks = cs_addrs->ti->total_blks = new_total;
	if (blocks / bplmap * bplmap != blocks)
	{
		bit_set(blocks / bplmap, MM_ADDR(cs_data)); /* Mark old last local map as having space */
		if ((int4)blocks > cs_addrs->nl->highest_lbm_blk_changed)
			cs_addrs->nl->highest_lbm_blk_changed = blocks;
	}
	curr_tn = cs_addrs->ti->curr_tn;
	assert(cs_addrs->ti->early_tn == cs_addrs->ti->curr_tn);
	/* do not increment transaction number for forward recovery */
	if (!jgbl.forw_phase_recovery || JNL_ENABLED(cs_data))
	{
		cs_data->trans_hist.early_tn = cs_data->trans_hist.curr_tn + 1;
		INCREMENT_CURR_TN(cs_data);
	}
	/* white box test for interrupted file extension */
	if (WBTEST_ENABLED(WBTEST_FILE_EXTEND_INTERRUPT_5))
	{
		LONG_SLEEP(600);
		assert(FALSE); /* Should be killed before that */
	}
	fileheader_sync(gv_cur_region);
	/* white box test for interrupted file extension */
	if (WBTEST_ENABLED(WBTEST_FILE_EXTEND_INTERRUPT_6))
	{
		LONG_SLEEP(600);
		assert(FALSE); /* Should be killed before that */
	}
	if (is_mm)
	{
		assert((NULL != old_base[0]) && (NULL != old_base[1]));
		mmap_sz = new_eof - BLK_ZERO_OFF(cs_data);	/* Don't mmap the file header and master map */
		CHECK_LARGEFILE_MMAP(gv_cur_region, mmap_sz);   /* can issue rts_error MMFILETOOLARGE */
		status = (sm_long_t)(mmap_retaddr = (sm_uc_ptr_t)MMAP_FD(udi->fd, mmap_sz, BLK_ZERO_OFF(cs_data), FALSE));
		GTM_WHITE_BOX_TEST(WBTEST_MMAP_SYSCALL_FAIL, status, -1);
		if (-1 == status)
		{
			save_errno = errno;
			WBTEST_ASSIGN_ONLY(WBTEST_MMAP_SYSCALL_FAIL, save_errno, ENOMEM);
			GDSFILEXT_CLNUP;
			send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(12) ERR_DBFILERR, 2, DB_LEN_STR(gv_cur_region),
					ERR_SYSCALL, 5, LEN_AND_LIT("mmap()"), CALLFROM, save_errno);
			return (uint4)(NO_FREE_SPACE);
		}
		/* In addition to updating the internal map values, gds_map_moved sets cs_data to point to the remapped file */
		gds_map_moved(mmap_retaddr, old_base[0], old_base[1], mmap_sz); /* updates cs_addrs->db_addrs[1] */
		cs_addrs->db_addrs[0] = mmap_retaddr;
		assert(cs_addrs->db_addrs[0] < cs_addrs->db_addrs[1]);
 	}
	GDSFILEXT_CLNUP;
	INCR_GVSTATS_COUNTER(cs_addrs, cs_addrs->nl, n_db_extends, 1);
	if (!gtm_dbfilext_syslog_disable)
		send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(7) ERR_DBFILEXT, 5, DB_LEN_STR(gv_cur_region), blocks, new_total,
				&curr_tn);
	return (SS_NORMAL);
}
