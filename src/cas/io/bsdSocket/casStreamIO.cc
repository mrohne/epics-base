/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/
//
// $Id$
//

#include "server.h"

//
// casStreamIO::casStreamIO()
//
casStreamIO::casStreamIO ( caServerI & cas, clientBufMemoryManager & bufMgr,
                          const ioArgsToNewStreamIO & args ) :
	casStrmClient ( cas, bufMgr ), sock ( args.sock ), addr (  args.addr), 
        blockingFlag ( xIsBlocking )
 {
	assert (sock>=0);
	int yes = true;
	int	status;

	/*
	 * see TCP(4P) this seems to make unsollicited single events much
	 * faster. I take care of queue up as load increases.
	 */
	status = setsockopt(
							this->sock,
							IPPROTO_TCP,
							TCP_NODELAY,
							(char *)&yes,
							sizeof(yes));
	if (status<0) {
		errlogPrintf(
			"CAS: %s TCP_NODELAY option set failed %s\n",
			__FILE__, SOCKERRSTR(SOCKERRNO));
		throw S_cas_internal;
	}

	/*
	 * turn on KEEPALIVE so if the client crashes
	 * this task will find out and exit
	 */
	status = setsockopt(
					sock,
					SOL_SOCKET,
					SO_KEEPALIVE,
					(char *)&yes,
					sizeof(yes));
	if (status<0) {
		errlogPrintf(
			"CAS: %s SO_KEEPALIVE option set failed %s\n",
			__FILE__, SOCKERRSTR(SOCKERRNO));
		throw S_cas_internal;
	}

	/*
	 * some concern that vxWorks will run out of mBuf's
	 * if this change is made
	 *
	 * joh 11-10-98
	 */
#if 0
	int i;

	/*
	 * set TCP buffer sizes to be synergistic
	 * with CA internal buffering
	 */
	i = MAX_MSG_SIZE;
	status = setsockopt(
					sock,
					SOL_SOCKET,
					SO_SNDBUF,
					(char *)&i,
					sizeof(i));
	if(status < 0){
			errlogPrintf("CAS: SO_SNDBUF set failed\n");
	    throw S_cas_internal;
	}
	i = MAX_MSG_SIZE;
	status = setsockopt(
					sock,
					SOL_SOCKET,
					SO_RCVBUF,
					(char *)&i,
					sizeof(i));
	if(status < 0){
		errlogPrintf("CAS: SO_RCVBUF set failed\n");
	    throw S_cas_internal;
	}
#endif

}

//
// casStreamIO::~casStreamIO()
//
casStreamIO::~casStreamIO()
{
	if (sock>=0) {
		socket_close(this->sock);
	}
}

//
// casStreamIO::osdSend()
//
outBufClient::flushCondition casStreamIO::osdSend ( const char *pInBuf, bufSizeT nBytesReq, 
                                 bufSizeT &nBytesActual )
{
    int	status;
    
    if ( nBytesReq == 0 ) {
        nBytesActual = 0;
        return outBufClient::flushNone;
    }
    
    status = send (this->sock, (char *) pInBuf, nBytesReq, 0);
    if (status == 0) {
        return outBufClient::flushDisconnect;
    }
    else if (status<0) {
        int anerrno = SOCKERRNO;
        
        if (anerrno != SOCK_EWOULDBLOCK) {
            int errnoCpy = SOCKERRNO;
            if (  
                errnoCpy != SOCK_ECONNABORTED &&
                errnoCpy != SOCK_ECONNRESET &&
                errnoCpy != SOCK_EPIPE &&
                errnoCpy != SOCK_ETIMEDOUT ) {

 			    char buf[64];
                ipAddrToA (&this->addr, buf, sizeof(buf));

			    errlogPrintf(
	"CAS: TCP socket send to \"%s\" failed because \"%s\"\n",
				    buf, SOCKERRSTR(errnoCpy));
            }
            return outBufClient::flushDisconnect;
        }
        else {
            return outBufClient::flushNone;
        }
    }
    nBytesActual = (bufSizeT) status;
    return outBufClient::flushProgress;
}

//
// casStreamIO::osdRecv()
//
inBufClient::fillCondition
casStreamIO::osdRecv ( char * pInBuf, bufSizeT nBytes, // X aCC 361
                      bufSizeT & nBytesActual )
{
    int nchars;
    
    nchars = recv (this->sock, pInBuf, nBytes, 0);
    if (nchars==0) {
        return casFillDisconnect;
    }
    else if (nchars<0) {
        int myerrno = SOCKERRNO;
        char buf[64];

        if (myerrno==SOCK_EWOULDBLOCK) {
            return casFillNone;
        }
        else  {
            if (
                myerrno != SOCK_ECONNABORTED &&
                myerrno != SOCK_ECONNRESET &&
                myerrno != SOCK_EPIPE &&
                myerrno != SOCK_ETIMEDOUT ) {
                ipAddrToA (&this->addr, buf, sizeof(buf));
                errlogPrintf(
		    "CAS: client %s disconnected because \"%s\"\n",
                    buf, SOCKERRSTR(myerrno));
            }
            return casFillDisconnect;
        }
    }
    else {
    	nBytesActual = (bufSizeT) nchars;
        return casFillProgress;
    }
}

//
// casStreamIO::show()
//
void casStreamIO::osdShow (unsigned level) const
{
	printf ( "casStreamIO at %p\n", 
        static_cast <const void *> ( this ) );
	if (level>1u) {
		char buf[64];
		ipAddrToA(&this->addr, buf, sizeof(buf));
		printf (
			"client=%s, port=%x\n",
			buf, ntohs(this->addr.sin_port));
	}
}

//
// casStreamIO::xSsetNonBlocking()
//
void casStreamIO::xSetNonBlocking()
{
	int status;
	osiSockIoctl_t yes = true;

	status = socket_ioctl(this->sock, FIONBIO, &yes); // X aCC 392
	if (status>=0) {
		this->blockingFlag = xIsntBlocking;
	}
	else {
		errlogPrintf("%s:CAS: TCP non blocking IO set fail because \"%s\"\n", 
				__FILE__, SOCKERRSTR(SOCKERRNO));
	    throw S_cas_internal;
	}
}

//
// casStreamIO::blockingState()
//
xBlockingStatus casStreamIO::blockingState() const
{
	return this->blockingFlag;
}

//
// casStreamIO::incomingBytesPresent()
//
bufSizeT casStreamIO::incomingBytesPresent () const // X aCC 361
{
    int status;
    osiSockIoctl_t nchars = 0;
    
    status = socket_ioctl ( this->sock, FIONREAD, &nchars ); // X aCC 392
    if ( status < 0 ) {
        int localError = SOCKERRNO;
        if (
            localError != SOCK_ECONNABORTED &&
            localError != SOCK_ECONNRESET &&
            localError != SOCK_EPIPE &&
            localError != SOCK_ETIMEDOUT ) 
        {
            char buf[64];
            int errnoCpy = SOCKERRNO;
            
            ipAddrToA ( &this->addr, buf, sizeof(buf) );
            errlogPrintf ("CAS: FIONREAD for %s failed because \"%s\"\n",
                buf, SOCKERRSTR ( errnoCpy ) );
        }
        return 0u;
    }
    else if ( nchars < 0 ) {
        return 0u;
    }
    else {
        return ( bufSizeT ) nchars;
    }
}

//
// casStreamIO::hostName()
//
void casStreamIO::hostName ( char *pInBuf, unsigned bufSizeIn ) const
{
	ipAddrToA ( &this->addr, pInBuf, bufSizeIn );
}

//
// casStreamIO:::optimumBufferSize()
//
bufSizeT casStreamIO::optimumBufferSize () 
{

#if 0
	int n;
	int size;
	int status;

	/* fetch the TCP send buffer size */
	n = sizeof(size);
	status = getsockopt(
			this->sock,
			SOL_SOCKET,
			SO_SNDBUF,
			(char *)&size,
			&n);
	if(status < 0 || n != sizeof(size)){
		size = 0x400;
	}

	if (size<=0) {
		size = 0x400;
	}
printf("the tcp buf size is %d\n", size);
	return (bufSizeT) size;
#else
// this needs to be MAX_TCP (until we fix the array problem)
	return (bufSizeT) MAX_TCP; // X aCC 392
#endif
}

//
// casStreamIO::getFD()
//
int casStreamIO::getFD() const
{
	return this->sock;
}
