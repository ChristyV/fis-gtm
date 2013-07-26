/****************************************************************
 *								*
 *	Copyright 2011 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"
#include "io.h"

void iomb_wtone(int v)
{
	mstr	temp;
	char 	p[1];

	p[0] = (char)v;
	temp.len = 1;
	temp.addr = p;
	iomb_write(&temp);
	return;
}
