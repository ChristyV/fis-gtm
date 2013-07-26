/****************************************************************
 *								*
 *	Copyright 2012, 2013 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include <errno.h>	/* for ENOSPC */

#include "anticipatory_freeze.h"
#include "wait_for_disk_space.h"
#include "gtmio.h"
#include "tp_grab_crit.h"
#include "have_crit.h"
#include "filestruct.h"
#include "jnl.h"
#include "error.h"
#include "gtmmsg.h"

#ifdef DEBUG
GBLDEF	uint4			lseekwrite_target;
#endif

GBLREF	jnlpool_addrs		jnlpool;
GBLREF	volatile int4		exit_state;
GBLREF	int4			exi_condition;
GBLREF	int4			forced_exit_err;

error_def(ERR_DSKNOSPCAVAIL);
error_def(ERR_DSKNOSPCBLOCKED);
error_def(ERR_DSKSPCAVAILABLE);
error_def(ERR_ENOSPCQIODEFER);

/* In case of ENOSPC, if anticipatory freeze scheme is in effect and this process has attached to the
 * journal pool, trigger an instance freeze in this case and wait for the disk space to be available
 * at which point unfreeze the instance.
 */
void wait_for_disk_space(sgmnt_addrs *csa, char *fn, int fd, off_t offset, char *buf, size_t count, int *save_errno)
{
	boolean_t	was_crit;
	gd_region	*reg;
	int		fn_len, tmp_errno;
	boolean_t	freeze_cleared;
	char		wait_comment[MAX_FREEZE_COMMENT_LEN];
#	ifdef DEBUG
		uint4	lcl_lseekwrite_target;
#	endif
	DCL_THREADGBL_ACCESS;	/* needed by ANTICIPATORY_FREEZE_AVAILABLE macro */

	SETUP_THREADGBL_ACCESS;	/* needed by ANTICIPATORY_FREEZE_AVAILABLE macro */
#	ifdef DEBUG
		/* Reset global to safe state after noting it down in a local (just in case there are errors in this function) */
		lcl_lseekwrite_target = lseekwrite_target; lseekwrite_target = LSEEKWRITE_IS_TO_NONE;
#	endif
	/* If anticipatory freeze scheme is not in effect, or if this database does not care about it,
	 * or DSKNOSPCAVAIL is not configured as a custom error, return right away.
	 */
	if (!ANTICIPATORY_FREEZE_ENABLED(csa) || (NULL == is_anticipatory_freeze_needed_fnptr)
			|| !(*is_anticipatory_freeze_needed_fnptr)(csa, ERR_DSKNOSPCAVAIL))
		return;
	fn_len = STRLEN(fn);
	was_crit = csa->now_crit;
	reg = csa->region;
	/* Let us take the case this process has opened the database but does not hold crit on it. If we come in to this
	 * function while trying to flush either to the db or jnl, setting the instance freeze would require a "grab_lock"
	 * which could hang due to another process holding that and in turn waiting for the exact same db or jnl write
	 * to succeed. This has the potential of creating a deadlock so we avoid that by returning right away. Since we
	 * dont hold crit, this call to do the jnl or db write is not critical in the sense no process will be affected
	 * because this write did not happen. Therefore it is okay to return right away. On the other hand if this
	 * process holds crit, then it is not possible that the other process holds the jnlpool lock
	 * and is waiting for the db or jnl qio (since that flow usually happens in t_end and tp_tend
	 * where db crit is first obtained before jnlpool lock is). Therefore it is safe to do a grab_lock
	 * in that case without worrying about potential deadlocks.
	 * Update *save_errno to indicate this is not a ENOSPC condition (since we have chosen to defer
	 * the ENOSPC condition to some other process that encounters it while holding crit).
	 *
	 * There is a possibility that if the caller is jnl_wait we will retry this logic indefinitely without ever
	 * setting instance freeze because we dont hold crit. To avoid that, do tp_grab_crit to see if it is available.
	 * If so, go ahead with freezing the instance. If not issue QIODEFER message and return. It is still possible
	 * the same process issues multiple QIODEFER messages before the instance gets frozen. But it should be rare.
	 */
	if (!was_crit)
		tp_grab_crit(reg);
	if (!csa->now_crit)
	{
		send_msg_csa(CSA_ARG(NULL) VARLSTCNT(4) ERR_ENOSPCQIODEFER, 2, fn_len, fn);
		*save_errno = ERR_ENOSPCQIODEFER;
		return;
	}
	/* We either came into this function holding crit or "tp_grab_crit" succeeded */
	assert(NULL != jnlpool.jnlpool_ctl);
	assert(NULL != fn);	/* if "csa" is non-NULL, fn better be non-NULL as well */
	/* The "send_msg" of DSKNOSPCAVAIL done below will set instance freeze (the configuration file includes it). After that, we
	 * will keep retrying the IO waiting for disk space to become available. If yes, we will clear the freeze. Until that is
	 * done, we should not allow ourselves to be interrupted as otherwise interrupt code can try to write to the db/jnl (as
	 * part of DB_LSEEKWRITE) and the first step there would be to wait for the freeze to be lifted off. Since we were the ones
	 * who set the freeze in the first place, the auto-clearing of freeze (on disk space freeup) will no longer work in that
	 * case. Hence the reason not to allow interrupts.
	 */
	DEFER_INTERRUPTS(INTRPT_IN_WAIT_FOR_DISK_SPACE);
	send_msg_csa(CSA_ARG(csa) VARLSTCNT(4) ERR_DSKNOSPCAVAIL, 2, fn_len, fn); /* this should set the instance freeze */
	/* Make a copy of the freeze comment which would be set by the previous message. */
	GENERATE_INST_FROZEN_COMMENT(wait_comment, MAX_FREEZE_COMMENT_LEN, ERR_DSKNOSPCAVAIL);
	tmp_errno = *save_errno;
	assert(ENOSPC == tmp_errno);
	/* Hang/retry waiting for the disk space situation to be cleared. */
	for ( ; ENOSPC == tmp_errno; )
	{
		if (!IS_REPL_INST_FROZEN)
		{	/* Some other process cleared the instance freeze. But we still dont have our disk
			 * space issue resolved so set the freeze flag again until space is available for us.
			 */
			send_msg_csa(CSA_ARG(csa) VARLSTCNT(4) ERR_DSKNOSPCAVAIL, 2, fn_len, fn);
		} else if (exit_state != 0)
		{
			send_msg_csa(CSA_ARG(NULL) VARLSTCNT(1) forced_exit_err);
			gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(1) forced_exit_err);
			exit(-exi_condition);
		}
		/* Sleep for a while before retrying the write. Do not use "hiber_start" as that
		 * uses timers and if we are already in a timer handler now, nested timers wont work.
		 */
		SHORT_SLEEP(SLEEP_IORETRYWAIT);
		DEBUG_ONLY(CLEAR_FAKE_ENOSPC_IF_MASTER_DEAD);
		/* If some other process froze the instance and changed the comment, a retry of the
		 * LSEEKWRITE may not be appropriate, so just loop waiting for the freeze to be lifted.
		 */
		if (IS_REPL_INST_FROZEN && (STRCMP(wait_comment, jnlpool.jnlpool_ctl->freeze_comment) != 0))
		{
			send_msg_csa(CSA_ARG(NULL) VARLSTCNT(4) ERR_DSKNOSPCBLOCKED, 2, fn_len, fn);
			WAIT_FOR_REPL_INST_UNFREEZE(csa)
		}
		LSEEKWRITE(fd, offset, buf, count, tmp_errno);
#		ifdef DEBUG
		if (LSEEKWRITE_IS_TO_DB == lcl_lseekwrite_target)
			FAKE_ENOSPC(csa, fake_db_enospc, lcl_lseekwrite_target, tmp_errno)
		else if (LSEEKWRITE_IS_TO_JNL == lcl_lseekwrite_target)
			FAKE_ENOSPC(csa, fake_jnl_enospc, lcl_lseekwrite_target, tmp_errno)
#		endif
	}
	/* Report that we were able to continue whether we are still frozen or not. */
	send_msg_csa(CSA_ARG(NULL) VARLSTCNT(4) ERR_DSKSPCAVAILABLE, 2, fn_len, fn);
	/* Only report if we were the process to set the current freeze comment; otherwise someone else reported it. */
	if (STRCMP(wait_comment, jnlpool.jnlpool_ctl->freeze_comment) == 0)
	{
		CLEAR_ANTICIPATORY_FREEZE(freeze_cleared);			/* sets freeze_cleared */
		REPORT_INSTANCE_UNFROZEN(freeze_cleared);
	}
	*save_errno = tmp_errno;
	ENABLE_INTERRUPTS(INTRPT_IN_WAIT_FOR_DISK_SPACE);
	if (!was_crit)
		rel_crit(reg);
	return;
}
