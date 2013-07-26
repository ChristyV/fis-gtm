/****************************************************************
 *								*
 *	Copyright 2001, 2009 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

/* iosocket_flush.c */

#include "mdef.h"

#include "gtm_socket.h"
#include "gtm_inet.h"
#include <errno.h>
#include <netinet/tcp.h>
#include "gtm_stdio.h"
#include "gtm_string.h"

#include "io.h"
#include "iotcpdef.h"
#include "iotcproutine.h"
#include "gt_timer.h"
#include "iosocketdef.h"

GBLREF tcp_library_struct       tcp_routines;

void iosocket_flush(io_desc *iod)
{
#ifdef C9A06001531
	/* pending change request C9A06001531 */

	d_socket_struct	*dsocketptr;
	socket_struct	*socketptr;
	int             on = 1, off = 0;
        char            *errptr;
        int4            errlen;

        error_def(ERR_SOCKWRITE);
        error_def(ERR_TEXT);
	error_def(ERR_CURRSOCKOFR);

	assert(gtmsocket == iod->type);

	dsocketptr = (d_socket_struct *)iod->dev_sp;
	socketptr = dsocketptr->socket[dsocketptr->current_socket];

	if (dsocketptr->current_socket >= dsocketptr->n_socket)
	{
		rts_error(VARLSTCNT(4) ERR_CURRSOCKOFR, 2, dsocketptr->current_socket, dsocketptr->n_socket);
		return;
	}
        memcpy(dsocketptr->dollar_device, "0", SIZEOF("0"));
        if( -1 == tcp_routines.aa_setsockopt(socketptr->sd, SOL_SOCKET, TCP_NODELAY, &on, SIZEOF(on)) ||
		(-1 == tcp_routines.aa_setsockopt(socketptr->sd, SOL_SOCKET, TCP_NODELAY, &off, SIZEOF(off))))
        {
		errptr = (char *)STRERROR(errno);
                errlen = strlen(errptr);
                iod->dollar.za = 9;
		MEMCPY_LIT(dsocketptr->dollar_device, "1,");
                memcpy(&dsocketptr->dollar_device[SIZEOF("1,") - 1], errptr, errlen + 1);	/* we want the null */
		if (socketptr->ioerror)
			rts_error(VARLSTCNT(6) ERR_SOCKWRITE, 0, ERR_TEXT, 2, errlen, errptr);
		return;
        }

#endif
	return;
}
