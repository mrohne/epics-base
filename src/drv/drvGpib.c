/* drvGpib.c */
/* share/src/drv/drvGpib.c $Id$ */

/*      Author: John Winans
 *      Date:   09-10-91
 *      GPIB driver for the NI-1014 and NI-1014D VME cards.
 *
 *      Experimental Physics and Industrial Control System (EPICS)
 *
 *      Copyright 1991, the Regents of the University of California,
 *      and the University of Chicago Board of Governors.
 *
 *      This software was produced under  U.S. Government contracts:
 *      (W-7405-ENG-36) at the Los Alamos National Laboratory,
 *      and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *      Initial development by:
 *              The Controls and Automation Group (AT-8)
 *              Ground Test Accelerator
 *              Accelerator Technology Division
 *              Los Alamos National Laboratory
 *
 *      Co-developed with
 *              The Controls and Computing Group
 *              Accelerator Systems Division
 *              Advanced Photon Source
 *              Argonne National Laboratory
 *
 * Modification Log:
 * -----------------
 * .01  09-13-91        jrw     Written on Friday the 13th :-(
 *				Much of the code in physIo() was stolen from
 *				a gpib driver that came from Los Alamos.  It
 *                              referenced little more than National 
 *                              Instruments and Bob Daly (from ANL) in its
 *                              credits.
 ******************************************************************************
 *
 * Notes:
 *  If 1014D cards are used, make sure that the W7 switch is set to LMR.
 *  The internals of the 1014D are such that the DMAC can NEVER be hard-reset
 *  unless the SYSRESET* vme line is asserted.  The LMR mode allows the
 *  initGpib() function to reset the DMAC properly.
 */

/******************************************************************************
 *
 * The following defines should be in module_types.h or derived
 * from a support functions.
 *
 ******************************************************************************/
#define GPIB_SHORT_OFF	0x5000	/* The first address of link 0's region */
				/* Each link uses 0x0200 bytes */
#define GPIB_NUM_LINKS  4	/* Max number of NI GPIB ports allowed */
#define GPIB_IVEC_BASE  100     /* Vectored interrupts (2 used for each link) */
#define GPIB_IRQ_LEVEL  5       /* IRQ level */
/**************** end of stuff that does not belong here **********************/



#include <vxWorks.h>
#include <types.h>
#include <iosLib.h>
#include <taskLib.h>
#include <semLib.h>
#include <memLib.h>
#include <rngLib.h>
#include <wdLib.h>
#include <lstLib.h>
#include <vme.h>
#include <sysLib.h>

#include <drvSup.h>
#include <dbDefs.h>
#include <link.h>

#include <drvGpibInterface.h>
#include "drvGpib.h"

long	reportGpib();
long	initGpib();
int	niIrq();
int	niIrqError();
int	niTmoHandler();
int	ibLinkTask();
int	srqIntEnable();
int	srqIntDisable();

int	qGpibReq();
int	registerSrqCallback();
int	writeIb();
int	readIb();
int	writeIbCmd();
int	ioctlIb();
int	srqPollInhibit();

int	ibDebug = 0;		/* Turns on debug messages from this driver */
int	ibSrqDebug = 0;		/* Turns on ONLY srq related debug messages */

/******************************************************************************
 *
 * Generic driver block.  Epics uses it to call the init routine
 * when an iocInit() is done.
 *
 ******************************************************************************/
struct drvGpibSet drvGpib={
  9,
  reportGpib,
  initGpib,
  qGpibReq,
  registerSrqCallback,
  writeIb,
  readIb,
  writeIbCmd,
  ioctlIb,
  srqPollInhibit
};

/******************************************************************************
 *
 * This structure is used to build array-chained DMA operations.  See the
 * physIo() function and the National Instruments docs for more info.
 *
 ******************************************************************************/
struct    cc_ary
{
    caddr_t         cc_ccb;
    short           cc_ONE;
    caddr_t         cc_n_1addr;
    short           cc_TWO;
};

/******************************************************************************
 *
 * This structure is used to hold the hardware-specific information for a
 * single GPIB link.  There is one for each link constructed in initGpib().
 *
 ******************************************************************************/
struct	niLink {
  char		tmoFlag;	/* timeout has occurred */
  int		tmoLimit;	/* how long to wait before issuing timeout */
  SEM_ID	ioSem;		/* DMA I/O operation complete or WD timeout */
  WDOG_ID	watchDogId;	/* watchdog for timeouts */
  struct	ibregs	*ibregs;/* pointer to board registers */

  char		cc_byte;
  struct	cc_ary	cc_array;

  char		r_isr1;
  char		r_isr2;
  int		first_read;
};

static	int	defaultTimeout = 60;	/* in 60ths, for GPIB timeouts */

static	char	init_called = 0;	/* To insure that init is done first */
static	char	*short_base;		/* Base of short address space */
static	char	*ram_base;		/* Base of the ram on the CPU board */

static	struct	ibLink	*pIbLink[GPIB_NUM_LINKS];	/* NULL if link not present */
static	struct	niLink	*pNiLink[GPIB_NUM_LINKS];	/* NULL if link not present */
static	int	pollInhibit[GPIB_NUM_LINKS][IBAPERLINK];	
		/* 0=pollable, 1=user inhibited, 2=no device found */

static int timeoutSquelch = 0;	/* used to quiet timeout errors during polling */

/******************************************************************************
 *
 * This function prints a message indicating the status of each possible GPIB
 * card found in the system.
 *
 ******************************************************************************/
long
reportGpib()
{
  int	i;

  if (init_called)
  {
    for (i=0; i< GPIB_NUM_LINKS; i++)
    {
      if (pIbLink[i])
      {
        logMsg("Link %d (address 0x%08.8X) present and initialized.\n", i, pNiLink[i]->ibregs);
      }
      else
      {
        logMsg("Link %d not installed.\n", i);
      }
    }
  }
  else
  {
    logMsg("Gpib driver has not yet been initialized.\n");
  }
  return(OK);
}


/******************************************************************************
 *
 * Called by the iocInit processing.
 * initGpib, probes the gpib card addresses and if one is present, it
 * is initialized for use.  It should only be called one time.
 *
 * The loops in this function are logically 1 large one.  They were seperated
 * so that the 1014D cards could be initialized properly.  [Both ports must
 * have some of their registers set at the same time and then not later
 * altered... for example the LMR reset bit.]
 *
 ******************************************************************************/
/* BUG -- this should be static */
/*static*/ long
initGpib()
{
  int	i;
  int	j;
  int	probeValue;
  struct ibregs	*pibregs;
  char	s;

  if (init_called)
  {
    logMsg("initGpib(): Gpib devices already initialized!\n");
    return(ERROR);
  }

  /* figure out where the short address space is */
  sysBusToLocalAdrs(VME_AM_SUP_SHORT_IO , 0, &short_base);

  /* figure out where the CPU memory is (when viewed from the backplane) */
  sysLocalToBusAdrs(VME_AM_STD_SUP_DATA, &ram_base, &ram_base);
  ram_base = (char *)((ram_base - (char *)&ram_base) & 0x00FFFFFF);

  if (ibDebug)
  {
    logMsg("Gpib driver package initializing\n");
    logMsg("short_base            0x%08.8X\n", short_base);
    logMsg("ram_base              0x%08.8X\n", ram_base);
    logMsg("GPIB_SHORT_OFF        0x%08.8X\n", GPIB_SHORT_OFF);
    logMsg("GPIB_NUM_LINKS        0x%08.8X\n", GPIB_NUM_LINKS);
  }

  /* When probing, send out a reset signal to reset the DMAC and the TLC */
  probeValue = D_LMR | D_SFL;

  pibregs = (struct ibregs *)((unsigned int)short_base + GPIB_SHORT_OFF);
  /* Gotta do all the probing first because the 1014D's LMRs are shared :-( */
  for (i=0; i<GPIB_NUM_LINKS; i++)
  {
    if (vxMemProbe(&(pibregs->cfg2), WRITE, 1, &probeValue) < OK)
    { /* no GPIB board present here */
      pIbLink[i] = (struct ibLink *) NULL;
      pNiLink[i] = (struct niLink *) NULL;

      if (ibDebug)
	logMsg("Probing of address 0x%08.8X failed\n", pibregs);

    }
    else
    { /* GPIB board found... reserve space for structures & reset the thing */
      if (ibDebug)
	logMsg("GPIB card found at address 0x%08.8X\n", pibregs);

      if ((pIbLink[i] = (struct ibLink *) malloc(sizeof(struct ibLink))) == NULL)
      { /* This better never happen! */
        /* errMsg( BUG -- figure out how to use this thing ); */
	logMsg("Can't malloc memory for link data structures!\n");
        return(ERROR);
      }

      /* Allocate and init the sems and linked lists */
      pIbLink[i]->linkType = GPIB_IO;	/* spec'd in link.h */
      pIbLink[i]->linkId = i;		/* link number */
      pIbLink[i]->bug = -1;		/* this is not a bug link */
      pIbLink[i]->srqIntFlag = 0;	/* no srq ints set now */
      pIbLink[i]->linkEventSem = semCreate();
      lstInit(&(pIbLink[i]->hiPriList)); /* init the list as empty */
      pIbLink[i]->hiPriSem = semCreate();
      semGive(pIbLink[i]->hiPriSem);
      lstInit(&(pIbLink[i]->loPriList));
      pIbLink[i]->loPriSem = semCreate();
      semGive(pIbLink[i]->loPriSem);
      pIbLink[i]->srqRing = rngCreate(SRQRINGSIZE * sizeof(struct srqStatus));
      for (j=0; j<IBAPERLINK; j++)
      {
        pIbLink[i]->srqHandler[j] = NULL;	/* no handler is registered */
	pIbLink[i]->deviceStatus[j] = IDLE;	/* assume device is IDLE */
      }

      pibregs->cfg2 = D_SFL;	/* can't set all bits at same time */
      pibregs->cfg2 = D_SFL | D_SPAC | D_SC;	/* put board in operating mode */

      if ((pNiLink[i] = (struct niLink *) malloc(sizeof(struct niLink))) == NULL)
      {
        /* errMsg( BUG -- figure out how to use this thing ); */
        logMsg("Can't malloc memory for link data structures!\n");
        return(ERROR);
      }

      pNiLink[i]->ibregs = pibregs;
      pNiLink[i]->ioSem = semCreate();
      pNiLink[i]->watchDogId = wdCreate();
      pNiLink[i]->tmoLimit = defaultTimeout;
      pNiLink[i]->tmoFlag = 0;

      pNiLink[i]->cc_array.cc_ccb = 0;	/* DMAC array chained structure */
      pNiLink[i]->cc_array.cc_ONE = 1;
      pNiLink[i]->cc_array.cc_n_1addr = 0;
      pNiLink[i]->cc_array.cc_TWO = 2;

      pNiLink[i]->first_read = 1;	/* used in physIo() */
    }
    pibregs++;		/* ready for next board window */
  }

  /* Bring up the cards (has to be done last because the 1014D has to have */
  /* both ports reset before either one is initialized.                    */

  for (i=0; i<GPIB_NUM_LINKS; i++)
  {
    if (pNiLink[i] != NULL)
    {
      /* 7210 TLC setup */
  
      /* clear status regs by reading them */
      s = pNiLink[i]->ibregs->cptr;
      pNiLink[i]->r_isr1 = pNiLink[i]->ibregs->isr1;
      pNiLink[i]->r_isr2 = pNiLink[i]->ibregs->isr2;
  
      /* disable all interrupts from the 7210 */
      pNiLink[i]->ibregs->imr1 = 0;	/* DMA and ERROR IRQ mask reg */
      pNiLink[i]->ibregs->imr2 = 0;	/* SRQ IRQ mask reg */
      pNiLink[i]->ibregs->spmr = 0;	/* serial poll mode register */

      pNiLink[i]->ibregs->adr = 0;	/* device address = 0 */
      pNiLink[i]->ibregs->adr = HR_ARS|HR_DT|HR_DL; /* no secondary addressing */
      pNiLink[i]->ibregs->admr = HR_TRM1|HR_TRM0|HR_ADM0;
      pNiLink[i]->ibregs->eosr = 0;	/* end of string value */
      pNiLink[i]->ibregs->auxmr = ICR|8;	/* internal counter = 8 */
      pNiLink[i]->ibregs->auxmr = PPR|HR_PPU; /* paralell poll unconfigure */
      pNiLink[i]->ibregs->auxmr = AUXRA|0;
      pNiLink[i]->ibregs->auxmr = AUXRB|0;
      pNiLink[i]->ibregs->auxmr = AUXRE|0;

      /* DMAC setup */

      pNiLink[i]->ibregs->cfg1 = (GPIB_IRQ_LEVEL << 5)|D_BRG3|D_DBM;
      pNiLink[i]->ibregs->ch1.niv = GPIB_IVEC_BASE + i*2;	/* normal IRQ vector */
      pNiLink[i]->ibregs->ch1.eiv = GPIB_IVEC_BASE+1+i*2;	/* error IRQ vector */
      pNiLink[i]->ibregs->ch0.niv = GPIB_IVEC_BASE + i*2;	/* normal IRQ vector */
      pNiLink[i]->ibregs->ch0.eiv = GPIB_IVEC_BASE+1+i*2;   /* error IRQ vector */
      pNiLink[i]->ibregs->ch1.ccr = D_EINT;	/* stop operation, allow ints */
      pNiLink[i]->ibregs->ch0.ccr = 0;		/* stop all channel operation */
      pNiLink[i]->ibregs->ch0.cpr = 3;		/* highest priority */
      pNiLink[i]->ibregs->ch1.cpr = 3;		/* highest priority */
      pNiLink[i]->ibregs->ch1.dcr = D_CS|D_IACK|D_IPCL;
      pNiLink[i]->ibregs->ch0.dcr = D_CS|D_IACK|D_IPCL;
      pNiLink[i]->ibregs->ch1.scr = 0;		/* no counting during DMA */
      pNiLink[i]->ibregs->ch1.mfc = D_SUP|D_S24;
      pNiLink[i]->ibregs->ch1.bfc = D_SUP|D_S24;
      pNiLink[i]->ibregs->ch0.scr = D_MCU;	/* count up during DMA cycles */
      pNiLink[i]->ibregs->ch0.mfc = D_SUP|D_S24;
  
      /* attach the interrupt handler routines */
      intConnect((GPIB_IVEC_BASE + i*2) * 4, niIrq, i);
      intConnect((GPIB_IVEC_BASE + 1 + (i*2)) * 4, niIrqError, i);


    }
  }

  /* should have interrups running before I do any I/O */
  sysIntEnable(GPIB_IRQ_LEVEL);

  /* Fire up the TLCs and nudge all the addresses on the GPIB bus */
  /* by doing a serial poll on all of them.  If someone did a */
  /* srqPollInhibit() on a specific link, then skip it and continue. */

  for (i=0; i<GPIB_NUM_LINKS; i++)
  {
    if (pNiLink[i] != NULL)
    {
      pNiLink[i]->ibregs->auxmr = AUX_PON;	/* release pon state */

      niGpibIoctl(i, IBIFC);		/* fire out an interface clear */
      niGpibIoctl(i, IBREN, 1);		/* turn on the REN line */

      if (niGpibCmd(i, "?_", 2) == ERROR)/* send out a UNL and UNT */
      {
	/* errMsg() */
	logMsg("GPIB init failed for link %d, disableing.\n", i);
	pNiLink[i] = NULL;		/* prevent flood-o-errors */
	pIbLink[i] = NULL;
      }

      /* poll all available adresses to see if will respond */
      speIb(pIbLink[i]);
      for (j=1; j<31; j++)		/* poll 1 thru 31 (no 0 or 32) */
      {
	if (pollInhibit[i][j] != 1);	/* if user did not block it out */
        {
	  niGpibIoctl(i, IBTMO, 4);
	  if (pollIb(pIbLink[i], j, 0) == ERROR)
	    pollInhibit[i][j] = 2;	/* address is not pollable */
        }
      }
      spdIb(pIbLink[i]);

      pIbLink[i]->srqIntFlag = 0;	/* In case was set by above polling */

      /* Start a task to manage the link */
      if (taskSpawn("gpibLink", 46, VX_FP_TASK|VX_STDIO, 2000, ibLinkTask, i) == ERROR)
      {
        logMsg("initGpib(): failed to start link task for link %d\n", i);
        /*errMsg()*/
      }
    }
  }

  init_called = 1;		/* let reportGpib() know init occurred */
  return(OK);
}



/******************************************************************************
 *
 * Interrupt handler for all normal DMAC interrupts.
 *
 * This is invoked at the termination of a DMA operation or if the TLC
 * requests an un-masked interrupt (typically SRQ from the GPIB bus.)
 *
 * Keep in mind that channel0's interrupts are related to the SRQs and that
 * the ints from channel1 are related to the DMA operations completing.
 *
 * Note:
 *  The isr2 status should always be read first since reading isr1 can reset
 *  some of the isr2 status.
 *
 ******************************************************************************/
static int
niIrq(link)
int	link;
{

  if (ibDebug)
    logMsg("GPIB interrupt from link %d\n", link);

  if (GPIB_IRQ_LEVEL == 4)          /* gotta ack ourselves on HK boards */
    sysBusIntAck(GPIB_IRQ_LEVEL);

  /* Check the DMA error status bits first */
  if (pNiLink[link]->ibregs->ch0.csr & D_ERR || pNiLink[link]->ibregs->ch1.csr & D_ERR)
  {
    logMsg("GPIB error during DMA from link %d\n", link);

    /* read the status regs to clear any int status from the TLC */
    pNiLink[link]->r_isr2 |= pNiLink[link]->ibregs->isr2;
    pNiLink[link]->r_isr1 |= pNiLink[link]->ibregs->isr1;

    logMsg("  ch0.csr = %02.2X, ch1.csr = %02.2X\n", pNiLink[link]->ibregs->ch0.csr, pNiLink[link]->ibregs->ch1.csr);
    logMsg("  ch0.cer = %02.2X, ch1.cer = %02.2X\n", pNiLink[link]->ibregs->ch0.cer, pNiLink[link]->ibregs->ch1.cer);
    logMsg("  r_isr1 = %02.2X, r_isr2 = %02.2X\n", pNiLink[link]->r_isr1, pNiLink[link]->r_isr2);

    pNiLink[link]->ibregs->ch0.csr = ~D_PCLT;	/* Keep srq int status */
    pNiLink[link]->ibregs->ch1.csr = D_CLEAR;
    pNiLink[link]->ibregs->imr1 = 0;
    pNiLink[link]->ibregs->imr2 = 0;

    /* No semaphores are given in here because we don't know why we got */
    /* here.  It is best to let I/O time out if any was going on. */
    return(ERROR);
  }

  /* channel 0 PCL status is the SRQ line for the link */

  if ((pNiLink[link]->ibregs->ch0.csr) & D_PCLT)
  {
    pNiLink[link]->ibregs->ch0.csr = D_PCLT;	/* Reset srq status */
    pIbLink[link]->srqIntFlag = 1;

    if (ibDebug|| ibSrqDebug)
      logMsg("GPIB SRQ interrupt on link %d\n", link);

    semGive(pIbLink[link]->linkEventSem);

/* BUG - should I not check both channels? */
    return(0);
  } 

/* BUG -- perhaps set a flag so the WD system knows I proceeded here */

  /* if there was a watch-dog timer tie, let the timeout win. */
  if (pNiLink[link]->tmoFlag  == FALSE)
  {
    if (pNiLink[link]->ibregs->ch1.csr & D_PCLT)
    {
      if (ibDebug) 
	logMsg("GPIB DMA completion interrupt from link %d\n", link);
/* BUG -- how to deal with the r_isr values???????????????? */
      /* read the status regs to clear any int status from the TLC */
      /* changed these to = from |= because they never got cleared! */
      pNiLink[link]->r_isr2 = pNiLink[link]->ibregs->isr2;
      pNiLink[link]->r_isr1 = pNiLink[link]->ibregs->isr1;

      if (pNiLink[link]->ibregs->ch1.csr & D_COC)
      {
	/* this should not be set because we ALWAYS ask for 1 too */
	/* many bytes to be transfered.  See 1014 docs on ints */
	logMsg("GPIB COC bit set after DMA on channel 1 link %d\n", link);
      }
      /* DMA complete via sync detect */
      pNiLink[link]->ibregs->imr1 = 0;
      pNiLink[link]->ibregs->imr2 = 0;
      pNiLink[link]->ibregs->ch1.csr = D_CLEAR;
      /* Leave Channel 0's ints alone since it did not generate the interrupt */
      semGive(pNiLink[link]->ioSem);

      return(0);
    }
  }
  else
  {
    /* The DMAC should get reset by the watch-dog handling code if I get here */
    if (ibDebug)
      logMsg("GPIB DMA completion interrupt but wd expired already on link %d\n", link);
  }
  return(0);
}


/******************************************************************************
 *
 * An interrupt handler that catches the DMAC error interrupts.  These should
 * never occurr.
 *
 ******************************************************************************/
static int
niIrqError(link)
int	link;
{
  /*errMsg()*/
  logMsg("GPIB error interrupt generated on link %d\n", link);
  logMsg(" ch0 status = %02.2X %02.2X %02.2X\n", 
	pNiLink[link]->ibregs->ch0.csr & 0xff, 
	pNiLink[link]->ibregs->ch0.cer & 0xff, 
	pNiLink[link]->ibregs->ch0.ccr);
  logMsg("ch1 status = %02.2X %02.2X %02.2X\n", 
	pNiLink[link]->ibregs->ch1.csr & 0xff,
	pNiLink[link]->ibregs->ch1.cer & 0xff,
	pNiLink[link]->ibregs->ch1.ccr);
  pNiLink[link]->ibregs->ch0.ccr = D_SAB;
  pNiLink[link]->ibregs->ch1.ccr = D_SAB;
  return(0);
}


/******************************************************************************
 *
 * This function is used to queue an I/O transaction requests for the Ni-based
 * links.
 *
 ******************************************************************************/
static int
niQGpibReq(link, pdpvt, prio)
int	link;
struct	dpvtGpibHead *pdpvt;
int	prio;
{
  switch (prio) {
  case IB_Q_LOW:		/* low priority transaction request */
    semTake(pIbLink[link]->loPriSem);
    lstAdd(&(pIbLink[link]->loPriList), pdpvt);
    semGive(pIbLink[link]->loPriSem);
    semGive(pIbLink[link]->linkEventSem);
    break;
  case IB_Q_HIGH:		/* high priority transaction request */
    semTake(pIbLink[link]->hiPriSem);
    lstAdd(&(pIbLink[link]->hiPriList), pdpvt);
    semGive(pIbLink[link]->hiPriSem);
    semGive(pIbLink[link]->linkEventSem);
    break;
  default:		/* invalid priority */
    logMsg("invalid priority requested in call to qgpibreq(%d, %08.8X, %d)\n", link, pdpvt, prio);
    return(ERROR);
  }
  if (ibDebug)
    logMsg("qgpibreq(%d, 0x%08.8X, %d): transaction queued\n", link, pdpvt, prio);

  return(OK);
}


/******************************************************************************
 *
 * niGpibCmd()
 *
 * This function is used to output a command string to the GPIB bus.
 *
 * The controller is placed in the active state prior to the outputting of
 * the first command.
 *
 * Before calling niGpibCmd() the first time, an niGpibIoctl(IBIFC) call must
 * be made to init the bus and enable the interface card.
 *
 ******************************************************************************/
static int
niGpibCmd(link, buffer, length)
int     link;
char    *buffer;
int     length;
{
  int	iDelay;		/* how long to spin before doing a taskWait */
  int	tooLong;	/* how long should I tolerate waiting */
  int	lenCtr;

  lenCtr = length;

  if (ibDebug)
    logMsg("niGpibCmd(%d, 0x%08.8X, %d): command string >%s<\n", link, buffer, length, buffer);

  tooLong = 60;		/* limit to wait for ctrlr's command buffer */
  pNiLink[link]->ibregs->auxmr = AUX_TCA;	/* take control of the bus */

  while (lenCtr)
  {
    pNiLink[link]->r_isr2 &= ~HR_CO;
    iDelay = 100;			/* wait till the ctlr is ready */
    while (iDelay && (((pNiLink[link]->r_isr2 |= pNiLink[link]->ibregs->isr2) & HR_CO) == 0))
      iDelay--;

    if (iDelay)
    {
      pNiLink[link]->ibregs->cdor = *buffer++;	/* output a byte */
      lenCtr--;
      tooLong = 60;	/* reset the limit again */
    }
    else
    {
      if (!(tooLong--))
      {
	/* errMsg() */
	logMsg("niGpibCmd(%d, 0x%08.8X, %d): Timeout writing command >%s<\n", link, buffer, length, buffer);
	pNiLink[link]->ibregs->auxmr = AUX_GTS;
	return(ERROR);
      }
      taskDelay(1);			/* ctlr is taking too long */
    }
  }
  tooLong = 60;
  while(tooLong--)
  {
    pNiLink[link]->r_isr2 &= ~HR_CO;
    iDelay = 100;			/* wait till the ctlr is ready */
    while (iDelay && (((pNiLink[link]->r_isr2 |= pNiLink[link]->ibregs->isr2) & HR_CO) == 0))
      iDelay--;
  
    if(iDelay)
    {
      pNiLink[link]->ibregs->auxmr = AUX_GTS;
      return(length);
    }
    else
    {
      taskDelay(1);
    }
  }
  /* errMsg() */
  logMsg("niGpibCmd(%d, 0x%08.8X, %d): Timeout after writing command >%s<\n", link, buffer, length, buffer);
  pNiLink[link]->ibregs->auxmr = AUX_GTS;
  return(ERROR);
}


/******************************************************************************
 *
 * Read a buffer via Ni-based link.
 *
 ******************************************************************************/
static int
niGpibRead(link, buffer, length)
int	link;
char	*buffer;
int	length;
{
  int	err;

  if(ibDebug)
    logMsg("niGpibRead(%d, 0x%08.8X, %d)\n",link, buffer, length);

  if (niCheckLink(link) == ERROR)
  {
    /* bad link number */
    return(ERROR);
  }

  err = niPhysIo(READ, link, buffer, length);
  pNiLink[link]->r_isr1 &= ~HR_END;

  return(err ? err : length - niGpibResid(link));
}


/******************************************************************************
 *
 * Write a buffer out an Ni-based link.
 *
 ******************************************************************************/
static int
niGpibWrite(link, buffer, length)
int	link;
char	*buffer;
int	length;
{
  int	err;

  if(ibDebug)
    logMsg("niGpibWrite(%d, 0x%08.8X, %d)\n",link, buffer, length);

  if (niCheckLink(link) == ERROR)
  {
    /* bad link number */
    return(ERROR);
  }

  err = niPhysIo(WRITE, link, buffer, length);

  return(err ? err : length - niGpibResid(link));
}


/******************************************************************************
 *
 * This function is used to figure out the difference in the transfer-length
 * requested in a read or write request, and that actually transfered.
 *
 ******************************************************************************/
static int
niGpibResid(link)
int	link;
{
  register int    cnt;

  cnt = pNiLink[link]->ibregs->ch0.mtc;
  if (pNiLink[link]->ibregs->ch1.mtc == 2 || cnt) /* add one if carry-cycle */
    cnt++;					/* never started */

  return(cnt);
}


/******************************************************************************
 *
 * This function provides access to the GPIB protocol operations on the NI
 * interface board.
 *
 ******************************************************************************/
static int
niGpibIoctl(link, cmd, v)
int	link;
int	cmd;
int	v;
{
  int stat = OK;

  if(ibDebug)
    logMsg("niGpibIoctl(%d, %d, %d)\n",link, cmd, v);

  if (niCheckLink(link) == ERROR)
  {
    /* bad link number */
    return(ERROR);
  }

  switch (cmd) {
  case IBTMO:		/* set the timeout value for the next transaction */
    pNiLink[link]->tmoLimit = v;
    break;
  case IBIFC:		/* fire out an Interface Clear pulse */
    pNiLink[link]->ibregs->auxmr = AUX_SIFC;	/* assert the line */
    taskDelay(10);			/* wait a little while */
    pNiLink[link]->ibregs->auxmr = AUX_CIFC;	/* clear the line */
    taskDelay(10);			/* wait a little while */
    break;
  case IBREN:		/* turn on or off the REN line */
    pNiLink[link]->ibregs->auxmr = (v ? AUX_SREN : AUX_CREN);
    break;
  case IBGTS:		/* go to standby (ATN off etc...) */
    pNiLink[link]->ibregs->auxmr = AUX_GTS;
    break;
  case IBGTA:		/* go to active (ATN on etc...) (IBIFC must also be called */
    pNiLink[link]->ibregs->auxmr = AUX_TCA;
    break;
  case IBNILNK:		/* returns the max number of NI links possible */
    stat = GPIB_NUM_LINKS;
    break;
  default:
    return(ERROR);
  }
  return(stat);
}

/******************************************************************************
 *
 * This function is used to validate all non-BitBus -> GPIB link numbers that
 * are passed in from user requests.
 *
 ******************************************************************************/
static int
niCheckLink(link)
int	link;
{
  if (link<0 || link >= GPIB_NUM_LINKS)
  {
    /* link number out of range */
    return(ERROR);
  }
  if (pIbLink[link] == NULL)
  {
    /* link number has no card installed */
    return(ERROR);
  }
  return(OK);
}


/******************************************************************************
 *
 * This routine does DMA based I/O with the GPIB bus.  It sets up the NI board's
 * DMA registers, initiates the transfer and waits for it to complete.  It uses
 * a watchdog timer in case the transfer dies.  It returns OK, or ERROR
 * depending on if the transfer succeeds or not.
 *
 ******************************************************************************/
static int
niPhysIo(dir, link, buffer, length)
int	dir;		/* direction (READ or WRITE) */
int	link;		/* link number to do the I/O with */
char	*buffer;	/* data to transfer */
int	length;		/* number of bytes to transfer */
{
  int	status;
  short	cnt;
  register struct ibregs *b;
  char	w_imr2;
  int	temp_addr;
  int	tmoTmp;


  b = pNiLink[link]->ibregs;
  cnt = length;

  b->auxmr = AUX_GTS;	/* go to standby mode */
  b->ch1.ccr = D_SAB;	/* halt channel activity */
  b->ch0.ccr = D_SAB;	/* halt channel activity */

  b->ch1.csr = D_CLEAR;
  b->ch0.csr = D_CLEAR & ~D_PCLT;

  b->imr2 = 0;		/* set these bits last */
  status = OK;

  if (dir == READ)
  {
    if (pNiLink[link]->first_read == 0)
      b->auxmr = AUX_FH;		/* finish handshake */
    else
      pNiLink[link]->first_read = 0;

    b->auxmr = AUXRA | HR_HLDE;		/* hold off on end */
    
    if (cnt != 1)
      pNiLink[link]->cc_byte = AUXRA | HR_HLDA;	/* (cc) holdoff on all */
    else
      pNiLink[link]->cc_byte = b->auxmr = AUXRA | HR_HLDA; /* last byte, do now */
    b->ch0.ocr = D_DTM | D_XRQ;
    /* make sure I only alter the 1014D port-specific fields here! */
    b->cfg1 = D_ECC | D_IN | (GPIB_IRQ_LEVEL << 5) | D_BRG3 | D_DBM;
    b->ch1.ocr = D_DTM | D_ACH | D_XRQ;
    b->ch1.ocr = D_DTM | D_ACH | D_XRQ;

    /* enable interrupts and dma */
    b->imr1 = HR_ENDIE;
    w_imr2 = HR_DMAI;
  }
  else
  {
    if (cnt != 1)
      pNiLink[link]->cc_byte = AUX_SEOI;	/* send EOI with last byte */
    else
      b->auxmr = AUX_SEOI;			/* last byte, do it now */

    b->ch0.ocr = D_MTD | D_XRQ;
    /* make sure I only alter the 1014D port-specific fields here! */
    b->cfg1 = D_ECC | D_OUT | (GPIB_IRQ_LEVEL << 5) | D_BRG3 | D_DBM;
    b->ch1.ocr = D_MTD | D_ACH | D_XRQ;

    /* enable interrupts and dma */
    b->imr1 = 0;
    w_imr2 = HR_DMAO;
  }
  /* setup channel 1 (carry cycle) */
  pNiLink[link]->cc_array.cc_ccb = &(pNiLink[link]->cc_byte) + (long) ram_base;
  pNiLink[link]->cc_array.cc_n_1addr = &buffer[cnt - 1] + (long)ram_base;
  cnt--;
  temp_addr = (long) (&(pNiLink[link]->cc_array)) + (long)ram_base;
  niWrLong(&b->ch1.bar, temp_addr);
  b->ch1.btc = 2;

  /* setup channel 0 (main transfer) */
  b->ch0.mtc = cnt ? cnt : 1;
  temp_addr = (long) (buffer) + (long) ram_base;
  niWrLong(&b->ch0.mar, temp_addr);

  /* setup GPIB response timeout handler */
  if (pNiLink[link]->tmoLimit == 0)
    pNiLink[link]->tmoLimit = defaultTimeout;	/* 0 = take the default */
  pNiLink[link]->tmoFlag = FALSE;		/* assume no timeout */
  wdStart(pNiLink[link]->watchDogId, pNiLink[link]->tmoLimit, niTmoHandler, link);
  pNiLink[link]->tmoLimit = 0;			/* reset for next time */
  /* start dma (ch1 first) */
  if (cnt)
    b->ch1.ccr = D_EINT | D_SRT;	/* enable interrupts */
  else
    b->ch1.ccr = D_EINT;

  b->ch0.ccr = D_SRT;
  b->imr2 = w_imr2;				/* this must be done last */

  /* check for error in DMAC initialization */
  if ((b->ch0.csr & D_ERR) || (b->ch1.csr & D_ERR))
  {
    /* errMsg() */
    logMsg("DMAC error initialization on link %d.\n", link);
    return (ERROR);
  }
  if (cnt)
  {
    if (ibDebug == 1)
      logMsg("Link %d waiting for DMA int or WD timeout.\n", link);
    semTake(pNiLink[link]->ioSem);		/* timeout or DMA finish */
  }
  else 
    if (b->ch0.mtc)
    {
      if (ibDebug == 1)
	logMsg("wd cnt =0 wait\n");
      tmoTmp = 0;
      while (b->ch0.mtc)
      {
	taskDelay(1);
	if (++tmoTmp == pNiLink[link]->tmoLimit)
	{
	  pNiLink[link]->tmoFlag = TRUE;
	  break;
	}
      }
    }
  if (pNiLink[link]->tmoFlag == TRUE)
  {
    status = ERROR;
    /* reset */
    pNiLink[link]->r_isr2 |= pNiLink[link]->ibregs->isr2;
    pNiLink[link]->r_isr1 |= pNiLink[link]->ibregs->isr1;
    pNiLink[link]->ibregs->imr1 = 0;
    pNiLink[link]->ibregs->imr2 = 0;
    pNiLink[link]->ibregs->ch1.csr = D_CLEAR;
    /* errMsg() */
    if (!timeoutSquelch)
      logMsg("TIMEOUT GPIB DEVICE on link %d\n", link);
  }
  else
  {
    wdCancel(pNiLink[link]->watchDogId);
    status = OK;
    if (b->ch0.csr & D_ERR)
    {
      logMsg("DMAC error on link %d, channel 0 = %x\n", link, b->ch0.cer);
      status = ERROR;
    }
    if (b->ch1.csr & D_ERR)
    {
      logMsg("DMAC error on link %d, channel 1 = %x\n", link, b->ch1.cer);
      status = ERROR;
    }
  }
  /*
   * DMA transfer complete reset as per instructions in GPIB
   * 'Programming Considerations' 5-14 note: ISR2 + ISR1 are both read
   * in ibintr() interrupt handler IMR1 + IMR2 are both read in
   * ibwait() only when invoked, so we always do a read here
   */
  b->ch0.ccr = D_SAB;			/* halt channel activity */
  b->ch0.csr = D_CLEAR & ~D_PCLT;
  b->ch1.ccr = D_SAB;
  b->ch1.csr = D_CLEAR;

  b->imr2 = 0;
  /* make sure I only alter the 1014D port-specific fields here! */
  b->cfg1 = (GPIB_IRQ_LEVEL << 5) | D_BRG3 | D_DBM;

  return (status);
}


/******************************************************************************
 *
 * This function is called by the watch-dog timer if it expires while waiting
 * for a GPIB transaction to complete.
 *
 ******************************************************************************/
static int
niTmoHandler(link)
int	link;
{
  pNiLink[link]->tmoFlag = TRUE;	/* indicate that timeout occurred */
  semGive(pNiLink[link]->ioSem);	/* wake up the phys I/O routine */
  return(0);
}


/******************************************************************************
 *
 * Sometimes we have to make sure that regs on the GPIB board are accessed as
 * 16-bit values.  This function writes out a 32-bit value in 2 16-bit pieces.
 *
 ******************************************************************************/
static int
niWrLong(loc, val)
short          *loc;
int             val;
{
  register short *ptr = loc;

  *ptr = val >> 16;
  *(ptr + 1) = val & 0xffff;
}


/******************************************************************************
 *
 * At the time this function is started as its own task, the linked list
 * structures will have been created and initialized.
 *
 ******************************************************************************/
static int 
ibLinkTask(link)
int	link;
{
  struct  ibLink 	*plink; /* a reference to the link structures covered */
  struct dpvtGpibHead  *pnode;
  struct srqStatus ringData;
  int   pollAddress;
  int	pollActive;
  int	working;

  if (ibDebug)
    logMsg("ibLinkTask started for link %d\n", link);

  plink = pIbLink[link];

  working = 1;	/* check queues for work the first time */
  while (1)
  {
    if (!working)
    {
      /* Enable SRQ interrupts */
      srqIntEnable(link);
      semTake(plink->linkEventSem);
  
      /* Disable SRQ interrupts */
      srqIntDisable(link);

      if (ibDebug)
      {
        logMsg("ibLinkTask(%d): got an event\n", link);
      }
    }
    working = 0;	/* Assume will do nothing */

    /* Check if an SRQ interrupt has occurred recently */

    /* If link is doing DMA, this function will be performing the work */
    /* function from either the High or Low priority queue and will not */
    /* try to poll devices while DMA is under way :-) */

    if (plink->srqIntFlag)
    {
      if (ibDebug || ibSrqDebug)
	logMsg("ibLinkTask(%d): srqIntFlag set.\n", link);

      plink->srqIntFlag = 0;
      pollActive = 0;

      pollAddress = 1;          /* skip 0 and 31, poll 1-30 */
      while (pollAddress < 31)
      {
        if (!(pollInhibit[link][pollAddress])) /* zero if allowed */
        {
          if (!pollActive)
          { /* set the serial poll enable mode if not done so yet */
            pollActive = 1;
            speIb(plink);
          }
	  if (ibDebug || ibSrqDebug)
            logMsg("ibLinkTask(%d): poling device %d\n", link, pollAddress);
          if ((ringData.status = pollIb(plink, pollAddress, 1)) & 0x40)
          {
            ringData.device = pollAddress;
	    if (ibDebug || ibSrqDebug)
	      logMsg("ibLinkTask(%d): device %d srq status = 0x%02.2X\n", link, pollAddress, ringData.status);
            if (plink->srqHandler[ringData.device] != NULL)
            { /* there is a registered SRQ handler for this device */
              rngBufPut(plink->srqRing, &ringData, sizeof(ringData));
            }
	    else
	      if (ibDebug || ibSrqDebug)
		logMsg("ibLinkTask(%d): got an srq from device %d... ignored\n", link, pollAddress);
          }
        }
	pollAddress++;
      }
      if (pollActive)
      { /* unset serial poll mode if it got set above */
        pollActive = 0;
        spdIb(plink);
      }
      else
      {
	logMsg("ibLinkTask(%d): got an SRQ, but have no pollable devices!\n", link);
      }
      /* The srqIntFlag should still be set if the SRQ line is again/still */
      /* active, otherwise we have a possible srq interrupt processing */
      /* deadlock! */
    }

    /* See if there is a need to process an SRQ solicited transaction. */
    /* Do all of them before going on to other transactions. */
    while (rngBufGet(plink->srqRing, &ringData, sizeof(ringData)))
    {
      if (ibDebug || ibSrqDebug)
	logMsg("ibLinkTask(%d): dispatching srq handler for device %d\n", link, ringData.device);
      plink->deviceStatus[ringData.device] = (*(plink->srqHandler)[ringData.device])(plink->srqParm[ringData.device], ringData.status);
      working=1;
    }

    /* see if the Hi priority queue has anything in it */
    semTake(plink->hiPriSem);

    if ((pnode = (struct dpvtGpibHead *)lstFirst(&(plink->hiPriList))) != NULL)
    {
      while (plink->deviceStatus[pnode->device] == BUSY)
        if ((pnode = (struct dpvtGpibHead *)lstNext(&(plink->hiPriList))) == NULL)
          break;
    }
    if (pnode != NULL)
      lstDelete(&(plink->hiPriList), pnode);

    semGive(plink->hiPriSem);

    if (pnode != NULL)
    {
      if (ibDebug)
        logMsg("ibLinkTask(%d): got Hi Pri xact, pnode= 0x%08.8X\n", link, pnode);

      plink->deviceStatus[pnode->device] = (*(pnode->workStart))(pnode);
      working=1;
    }
    else
    {
      semTake(plink->loPriSem);
      if ((pnode = (struct dpvtGpibHead *)lstFirst(&(plink->loPriList))) != NULL)
      {
        while (plink->deviceStatus[pnode->device] == BUSY)
          if ((pnode = (struct dpvtGpibHead *)lstNext(&(plink->loPriList))) == NULL)
            break;
      }
      if (pnode != NULL)
        lstDelete(&(plink->loPriList), pnode);

      semGive(plink->loPriSem);

      if (pnode != NULL)
      {
        if(ibDebug)
          logMsg("ibLinkTask(%d): got Lo Pri xact, pnode= 0x%08.8X\n", link, pnode);
        plink->deviceStatus[pnode->device] = (*(pnode->workStart))(pnode);
        working=1;
      }
    }
  }
}


/******************************************************************************
 *
 * The following are functions used to take care of serial polling.  They
 * are called from the ibLinkTask.
 *
 ******************************************************************************/
/******************************************************************************
 *
 * Pollib sends out an SRQ poll and returns the poll response.
 * If there is an error during polling (timeout), the value -1 is returned.
 *
 ******************************************************************************/
/* BUG -- keep the polling during init time quiet if possible */
static int
pollIb(plink, gpibAddr, verbose)
struct ibLink     *plink;
int             gpibAddr;
int		verbose;	/* set to 1 if should log any errors */
{
  char  pollCmd[4];
  unsigned char  pollResult[3];
  int	status;

  if(verbose && (ibDebug || ibSrqDebug))
    logMsg("pollIb(0x%08.8X, %d, %d)\n", plink, gpibAddr, verbose);

    timeoutSquelch = !verbose;	/* keep the I/O routines quiet if desired */

  /* raw-read back the response from the instrument */
  if (readIb(plink->linkType, plink->linkId, plink->bug, gpibAddr, pollResult, sizeof(pollResult)) == ERROR)
  {
    if(verbose)
      logMsg("pollIb(%d, %d): data read error\n", plink->linkId, gpibAddr);
    status = ERROR;
  }
  else
  {
    status = pollResult[0];
    if (ibDebug || ibSrqDebug)
    {
      logMsg("pollIb(%d, %d): poll status = 0x%02.2X\n", plink->linkId, gpibAddr, status);
    }
  }

  timeoutSquelch = 0;	/* return I/O error logging to normal */
  return(status);
}


/******************************************************************************
 *
 * speIb is used to send out a Serial Poll Enable command on the GPIB
 * bus.
 *
 ******************************************************************************/
static int
speIb(plink)
struct ibLink     *plink;
{
  /* write out the Serial Poll Enable command */
  writeIbCmd(plink->linkType, plink->linkId, plink->bug, "\030", 1);

  return(0);
}


/******************************************************************************
 *
 * spdIb is used to send out a Serial Poll Disable command on the GPIB
 * bus.
 * 
 ******************************************************************************/
static int
spdIb(plink)
struct ibLink     *plink;
{
  /* write out the Serial Poll Disable command */
  writeIbCmd(plink->linkType, plink->linkId, plink->bug, "\031", 1);

  return(0);
}

/******************************************************************************
 *
 * Functions used to enable and disable SRQ interrupts.  These only make
 * sense on a Ni based link, so they are ignored in the BitBus case.
 *
 * The interrupts referred to here are the actual VME bus interrupts that are
 * generated by the Ni card when it sees the SRQ line go high.
 *
 ******************************************************************************/
static int
srqIntEnable(link)
int	link;
{
  int	lockKey;


  if(ibDebug || ibSrqDebug)
    logMsg("srqIntEnable(%d): ch0.csr = 0x%02.2X, gsr=0x%02.2X\n", link, pNiLink[link]->ibregs->ch0.csr, pNiLink[link]->ibregs->gsr);

  lockKey = intLock();	/* lock out ints because the DMAC likes to glitch */

  if (!((pNiLink[link]->ibregs->ch0.csr) & D_NSRQ))
  { /* SRQ line is CURRENTLY active, just give the event sem and return */
    semGive(pIbLink[link]->linkEventSem);
    pIbLink[link]->srqIntFlag = 1;

    if(ibDebug || ibSrqDebug)
      logMsg("srqIntEnable(%d): found SRQ active, setting srqIntFlag\n", link);

    /* Clear the PCLT status if is already set to prevent unneeded int later */
    pNiLink[link]->ibregs->ch0.csr = D_PCLT;
  }
  else
    pNiLink[link]->ibregs->ch0.ccr = D_EINT;	/* Allow SRQ ints */

  intUnlock(lockKey);
  return(0);
}

static int
srqIntDisable(link)
int	link;
{
  int   lockKey;

  if(ibDebug || ibSrqDebug)
    logMsg("srqIntDisable(%d): ch0.csr = 0x%02.2X, gsr=0x%02.2X\n", link, pNiLink[link]->ibregs->ch0.csr, pNiLink[link]->ibregs->gsr);

  lockKey = intLock();  /* lock out ints because the DMAC likes to glitch */
  pNiLink[link]->ibregs->ch0.ccr = 0;		/* Don't allow SRQ ints */
  intUnlock(lockKey);

  return(0);
}

/****************************************************************************
 *
 * The following routines are the user-callable entry points to the GPIB
 * driver.
 *
 ****************************************************************************/
/******************************************************************************
 *
 * A device support module may call this function to request that the GPIB
 * driver NEVER poll a given device.
 *
 * Devices are polled when an SRQ event is present on the GPIB link.  Some
 * devices are too dumb to deal with being polled.
 *
 * This is NOT a static function, because it must be invoked from the startup
 * script BEFORE iocInit is called.
 *
 * BUG --
 * This could change if we decide to poll them during the second call to init()
 * when epics 3.2 is available.
 *
 ******************************************************************************/
/* static */ int 
srqPollInhibit(linkType, linkId, bug, gpibAddr)
int	linkType;	/* link type (defined in link.h) */
int     linkId;         /* the link number the handler is related to */
int     bug;            /* the bug node address if on a bitbus link */
int     gpibAddr;       /* the device address the handler is for */
{
  if (ibDebug || ibSrqDebug)
    logMsg("srqPollInhibit(%d, %d, %d, %d): called -- not yet implemented!\n", linkType, linkId, bug, gpibAddr);

  if (linkType == GPIB_IO)
  {
    if (niCheckLink(linkId) == ERROR)
    {
      logMsg("drvGpib: srqPollInhibit(): invalid link number specified\n");
      return(ERROR);
    }
    pollInhibit[linkId][gpibAddr] = 1;	/* mark it as inhibited */
    return(OK);
  }
  if (linkType == BBGPIB_IO)
  {
    logMsg("drvGpib: srqPollInhibit(): not supported on a bitbus link\n");
    return(ERROR);
  }
  logMsg("drvGpib: srqPollInhibit(): invalid link type specified\n");
  return(ERROR);
}

/******************************************************************************
 *
 * This allows a device support module to register an SRQ event handler.
 *
 * It is used to specify a function to call when an SRQ event is detected 
 * on the specified link and device.  When the SRQ handler is called, it is
 * passed the requested parm and the poll-status from the gpib device.
 *
 ******************************************************************************/
static int 
registerSrqCallback(linkType, linkId, bug, gpibAddr, handler, parm)
int     linkType;       /* link type (defined in link.h) */
int     linkId;         /* the link number the handler is related to */
int	bug;		/* the bug node address if on a bitbus link */
int     gpibAddr;       /* the device address the handler is for */
int     (*handler)();   /* handler function to invoke upon SRQ detection */
caddr_t	parm;		/* so caller can have dpvt passed back */
{
  if(ibDebug || ibSrqDebug)
    logMsg("registerSrqCallback(%d, %d, %d, %d, 0x%08.8X, %d)\n", linkType, linkId, bug, gpibAddr, handler, parm);

  if (linkType == GPIB_IO)
  {
    if (niCheckLink(linkId) == ERROR)
    {
      logMsg("drvGpib: registerSrqCallback(): invalid link number specified\n");
      return(ERROR);
    }
    pIbLink[linkId]->srqHandler[gpibAddr] = handler;
    pIbLink[linkId]->srqParm[gpibAddr] = parm;
  }
  else if (linkType == BBGPIB_IO)
  {
    logMsg("drvGpib: registerSrqCallback(): not supported on bitbus links\n");
    return(ERROR);
  }
  else
  {
    logMsg("drvGpib: registerSrqCallback(): invalid link type specified\n");
    return(ERROR);
  }
  return(OK);
}


/******************************************************************************
 *
 * Allow users to operate the internal functions of the driver.
 *
 * This can be fatal to the driver... make sure you know what you are doing!
 *
 ******************************************************************************/
static int
ioctlIb(linkType, link, bug, cmd, v)
int     linkType;       /* link type (defined in link.h) */
int     link;           /* the link number to use */
int	bug;		/* node number if is a bitbus -> gpib link */
int	cmd;
int	v;
{
  int	stat;

  if (linkType == GPIB_IO)
    stat = niGpibIoctl(link, cmd, v);
  else
  {
    printf("ioctlIb(%d, %d, %d, %d, %v): bitbus->gpib not implemented yet\n");
    stat = ERROR;
  }
  return(stat);
}

/******************************************************************************
 *
 * This function allows a user program to queue a GPIB work request for
 * future execution.  It is the ONLY way a user function can initiate
 * a GPIB message transaction.
 *
 * A work request represents a function that the ibLinkTask is to call (when
 * ready) to allow the user program access to the readIb, writeIb, and
 * writeIbCmd functions.  The user programs should never call these functions
 * at any other times.
 *
 * Returns OK, or ERROR.
 *
 ******************************************************************************/
static int
qGpibReq(linkType, link, pdpvt, prio)
int	linkType;	/* link type (defined in link.h) */
int	link;		/* the link number to use */
struct	dpvtGpibHead *pdpvt; /* pointer to the device private structure */
int	prio;		/* 1 for high priority or 2 for low */
{
  int	stat;

  if (linkType == GPIB_IO)
    stat = niQGpibReq(link, pdpvt, prio);
  else  /* is a BBGPB_IO link */
    stat = bbQGpibReq(link, pdpvt, prio);

  return(stat);
}


/******************************************************************************
 *
 * The following functions are defined for use by device support modules.
 * They may ONLY be called by the linkTask.
 *
 ******************************************************************************/
/******************************************************************************
 *
 * A device support callable entry point used to write data to GPIB devices.
 *
 * This function returns the number of bytes written out.
 *
 ******************************************************************************/
static int
writeIb(linkType, linkId, bug, gpibAddr, data, length)
int	linkType;	/* link type (defined in link.h) */
int	linkId;		/* the link number to use */
int	gpibAddr;	/* the device number to write the data to */
char	*data;		/* the data buffer to write out */
int	length;		/* number of bytes to write out from the data buffer */
{
  char	attnCmd[5];
  int	stat;

  attnCmd[0] = '?';			/* global unlisten */
  attnCmd[1] = '_';			/* global untalk */
  attnCmd[2] = gpibAddr+LADBASE;	/* lad = gpibAddr */
  attnCmd[3] = 0+TADBASE;		/* mta = 0 */
  attnCmd[4] = '\0';			/* in case debugging prints it */

  if(ibDebug)
    logMsg("writeIb(%d, %d, %d, %d, 0x%08.8X, %d)\n", linkType, linkId, bug, gpibAddr, data, length);

  if (writeIbCmd(linkType, linkId, bug, attnCmd, 4) != 4)
    return(ERROR);

  if (linkType == GPIB_IO)
    stat = niGpibWrite(linkId, data, length);
  else	/* is a BBGPB_IO link */
    stat = bbGpibWrite(linkId, bug, data, length);

  if (writeIbCmd(linkType, linkId, bug, attnCmd, 2) != 2)
    return(ERROR);


  return(stat);
}


/******************************************************************************
 *
 * A device support callable entry point used to read data from GPIB devices.
 *
 * This function returns the number of bytes read from the device, or ERROR
 * if the read operation failed.
 *
 ******************************************************************************/
static int
readIb(linkType, linkId, bug, gpibAddr, data, length)
int	linkType;	/* link type (defined in link.h) */
int	linkId;		/* the link number to use */
int	gpibAddr;	/* the device number to read the data from */
char	*data;		/* the buffer to place the data into */
int	length;		/* max number of bytes to place into the buffer */
{
  char  attnCmd[5];
  int   stat;

  if(ibDebug)
    logMsg("readIb(%d, %d, %d, %d, 0x%08.8X, %d)\n", linkType, linkId, bug, gpibAddr, data, length);

  attnCmd[0] = '_';                     /* global untalk */
  attnCmd[1] = '?';                     /* global unlisten */
  attnCmd[2] = gpibAddr+TADBASE;        /* tad = gpibAddr */
  attnCmd[3] = 0+LADBASE;		/* mta = 0 */
  attnCmd[4] = '\0';

  if (writeIbCmd(linkType, linkId, bug, attnCmd, 4) != 4)
    return(ERROR);

  if (linkType == GPIB_IO)
    stat = niGpibRead(linkId, data, length);
  else  /* is a BB->IB link */
    stat = bbGpibRead(linkId, bug, data, length);

  if (writeIbCmd(linkType, linkId, bug, attnCmd, 2) != 2)
    return(ERROR);

  return(stat);
}


/******************************************************************************
 *
 * A device support callable entry point that is used to write commands
 * to GPIB devices.  (this is the same as a regular write except that the
 * ATN line is held high during the write.
 *
 * This function returns the number of bytes written out.
 *
 ******************************************************************************/
static int
writeIbCmd(linkType, linkId, bug, data, length)
int	linkType;	/* link type (defined in link.h) */
int     linkId;   	/* the link number to use */
char    *data;  	/* the data buffer to write out */
int     length; 	/* number of bytes to write out from the data buffer */
{
  int	stat;

  if(ibDebug)
    logMsg("writeIbCmd(%d, %d, %d, 0x%08.8X, %d)\n", linkType, linkId, bug, data, length);

  if (linkType == GPIB_IO)
  {
    /* raw-write the data */
    stat = niGpibCmd(linkId, data, length);
  }
  else  /* is a BBGPIB_IO link */
  {
    /* raw-write the data */
    stat = bbGpibCmd(linkId, bug, data, length);
  }
  return(stat);
}

/***************************************************************
/* These will be fleshed out to work with the bitbus driver when 
/* it is written some day.
/***************************************************************/
int
bbGpibRead(link, buffer, length)
int     link;
char    *buffer;
int     length;
{
  logMsg("Read attempted to a bitbus->gpib link\n");
  return(ERROR);
}

int
bbGpibWrite(link, buffer, length)
int     link;
char    *buffer;
int     length;
{
  logMsg("Write attempted to a bitbus->gpib link\n");
  return(ERROR);
}

int
bbGpibIoctl(link, cmd, v)
int     link;
int     cmd;
int     v;
{
  logMsg("Ioctl attempted to a bitbus->gpib link\n");
  return(ERROR);
}

int
bbGpibCmd(link, buffer, length)
int     link;
char    *buffer;
int     length;
{
  logMsg("Command write sent to a bitbus->gpib link\n");
  return(ERROR);
}

/******************************************************************************
 *
 * Queue a request for the ibLinkTask associated with the proper BitBus link
 * and node.  If there is no ibLinkTask started for that specific link and
 * node, then start one first.  The time used to start the task will only
 * happen the first time that I/O is done to the link & node so the overhead
 * probably not worth worrying about.
 *
 ******************************************************************************/
bbQGpibReq(link, pdpvt, prio)
int	link;
struct	dpvtGpibHead *pdpvt;
int	prio;
{
  logMsg("bbQGpibReq called for a bitbug->gpib link\n");
  return(ERROR);
}
