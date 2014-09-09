/* 
 * Copyright: Stanford University / SLAC National Laboratory.
 *
 * EPICS BASE is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution. 
 *
 * Author: Till Straumann <strauman@slac.stanford.edu>, 2011
 */ 

#include <Windows.h>
#include <stdio.h>

#define epicsExportSharedSymbols
#include "epicsStackTracePvt.h"

int epicsBackTrace(void **buf, int buf_sz)
{
	/* Docs say that (for some windows versions) the sum of
	 * skipped + captured frames must be less than 63
	 */
	if ( buf_sz >= 63 )
		buf_sz = 62;
	return CaptureStackBackTrace(0, buf_sz, buf, 0);
}
