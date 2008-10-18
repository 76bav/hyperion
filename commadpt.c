/*-------------------------------------------------------------------*/
/* Hercules Communication Line Driver                                */
/* (c) 1999-2006 Roger Bowler & Others                               */
/* Use of this program is governed by the QPL License                */
/* Original Author : Ivan Warren                                     */
/* Prime Maintainer : Ivan Warren                                    */
/*-------------------------------------------------------------------*/

// $Id$
//
// $Log$
// Revision 1.44  2008/03/04 01:10:29  ivan
// Add LEGACYSENSEID config statement to allow X'E4' Sense ID on devices
// that originally didn't support it. Defaults to off for compatibility reasons
//
// Revision 1.43  2008/02/15 22:51:39  rbowler
// Move Solaris specific definition of INADDR_NONE to hostopts.h
//
// Revision 1.42  2008/02/07 00:29:04  rbowler
// Solaris build support by Jeff Savit
//
// Revision 1.41  2007/12/14 17:48:52  rbowler
// Enable SENSE ID CCW for 2703,3410,3420
//
// Revision 1.40  2007/11/21 22:54:14  fish
// Use new BEGIN_DEVICE_CLASS_QUERY macro
//
// Revision 1.39  2006/12/08 09:43:18  jj
// Add CVS message log
//

#include "hstdinc.h"
#include "hercules.h"
#include "devtype.h"
#include "parser.h"
#include "commadpt.h"

#if defined(WIN32) && defined(OPTION_DYNAMIC_LOAD) && !defined(HDL_USE_LIBTOOL) && !defined(_MSVC_)
  SYSBLK *psysblk;
  #define sysblk (*psysblk)
#endif

 /*-------------------------------------------------------------------*/
 /* Ivan Warren 20040227                                              */
 /* This table is used by channel.c to determine if a CCW code is an  */
 /* immediate command or not                                          */
 /* The tape is addressed in the DEVHND structure as 'DEVIMM immed'   */
 /* 0 : Command is NOT an immediate command                           */
 /* 1 : Command is an immediate command                               */
 /* Note : An immediate command is defined as a command which returns */
 /* CE (channel end) during initialisation (that is, no data is       */
 /* actually transfered. In this case, IL is not indicated for a CCW  */
 /* Format 0 or for a CCW Format 1 when IL Suppression Mode is in     */
 /* effect                                                            */
 /*-------------------------------------------------------------------*/

static BYTE commadpt_immed_command[256]=
{ 0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

COMMADPT_PEND_TEXT;     /* Defined in commadpt.h                     */
                        /* Defines commadpt_pendccw_text array       */

/*---------------------------------------------------------------*/
/* PARSER TABLES                                                 */
/*---------------------------------------------------------------*/
static PARSER ptab[]={
    {"lport","%s"},
    {"lhost","%s"},
    {"rport","%s"},
    {"rhost","%s"},
    {"dial","%s"},
    {"rto","%s"},
    {"pto","%s"},
    {"eto","%s"},
    {"switched","%s"},
    {NULL,NULL}
};

enum {
    COMMADPT_KW_LPORT=1,
    COMMADPT_KW_LHOST,
    COMMADPT_KW_RPORT,
    COMMADPT_KW_RHOST,
    COMMADPT_KW_DIAL,
    COMMADPT_KW_READTO,
    COMMADPT_KW_POLLTO,
    COMMADPT_KW_ENABLETO,
    COMMADPT_KW_SWITCHED
} commadpt_kw;

static void logdump(char *txt,DEVBLK *dev,BYTE *bfr,size_t sz)
{
    size_t i;
    if(!dev->ccwtrace)
    {
        return;
    }
    logmsg(_("HHCCA300D %4.4X:%s : Status = TEXT=%s, TRANS=%s, TWS=%s\n"),
            dev->devnum,
            txt,
            dev->commadpt->in_textmode?"YES":"NO",
            dev->commadpt->in_xparmode?"YES":"NO",
            dev->commadpt->xparwwait?"YES":"NO");
    logmsg(_("HHCCA300D %4.4X:%s : Dump of %d (%x) byte(s)\n"),dev->devnum,txt,sz,sz);
    for(i=0;i<sz;i++)
    {
        if(i%16==0)
        {
            if(i!=0)
            {
                logmsg("\n");
            }
            logmsg(_("HHCCA300D %4.4X:%s : %4.4X:"),dev->devnum,txt,i);
        }
        if(i%4==0)
        {
            logmsg(" ");
        }
        logmsg("%2.2X",bfr[i]);
    }
    logmsg("\n");
}
/*-------------------------------------------------------------------*/
/* Handler utility routines                                          */
/*-------------------------------------------------------------------*/

/*-------------------------------------------------------------------*/
/* Buffer ring management                                            */
/*-------------------------------------------------------------------*/

/*-------------------------------------------------------------------*/
/* Buffer ring management : Init a buffer ring                       */
/*-------------------------------------------------------------------*/
void commadpt_ring_init(COMMADPT_RING *ring,size_t sz,int trace)
{
    ring->bfr=malloc(sz);
    ring->sz=sz;
    ring->hi=0;
    ring->lo=0;
    ring->havedata=0;
    ring->overflow=0;
    if(trace)
    {
        logmsg("HCCCA999D : Ring buffer for ring %p allocated at %p\n",
            ring,
            ring->bfr);
    }
}
/*-------------------------------------------------------------------*/
/* Buffer ring management : Free a buffer ring                       */
/*-------------------------------------------------------------------*/
static void commadpt_ring_terminate(COMMADPT_RING *ring,int trace)
{
    if(trace)
    {
        logmsg("HCCCA999D : Ring buffer for ring %p at %p freed\n",
            ring,
            ring->bfr);
    }
    if(ring->bfr!=NULL)
    {
        free(ring->bfr);
        ring->bfr=NULL;
    }
    ring->sz=0;
    ring->hi=0;
    ring->lo=0;
    ring->havedata=0;
    ring->overflow=0;
}
/*-------------------------------------------------------------------*/
/* Buffer ring management : Flush a buffer ring                      */
/*-------------------------------------------------------------------*/
static void commadpt_ring_flush(COMMADPT_RING *ring)
{
    ring->hi=0;
    ring->lo=0;
    ring->havedata=0;
    ring->overflow=0;
}
/*-------------------------------------------------------------------*/
/* Buffer ring management : Queue a byte in the ring                 */
/*-------------------------------------------------------------------*/
inline static void commadpt_ring_push(COMMADPT_RING *ring,BYTE b)
{
    ring->bfr[ring->hi++]=b;
    if(ring->hi>=ring->sz)
    {
        ring->hi=0;
    }
    if(ring->hi==ring->lo)
    {
        ring->overflow=1;
    }
    ring->havedata=1;
}
/*-------------------------------------------------------------------*/
/* Buffer ring management : Queue a byte array in the ring           */
/*-------------------------------------------------------------------*/
inline static void commadpt_ring_pushbfr(COMMADPT_RING *ring,BYTE *b,size_t sz)
{
    size_t i;
    for(i=0;i<sz;i++)
    {
        commadpt_ring_push(ring,b[i]);
    }
}
/*-------------------------------------------------------------------*/
/* Buffer ring management : Retrieve a byte from the ring            */
/*-------------------------------------------------------------------*/
inline static BYTE commadpt_ring_pop(COMMADPT_RING *ring)
{
    register BYTE b;
    b=ring->bfr[ring->lo++];
    if(ring->lo>=ring->sz)
    {
        ring->lo=0;
    }
    if(ring->hi==ring->lo)
    {
        ring->havedata=0;
    }
    return b;
}

/*-------------------------------------------------------------------*/
/* Buffer ring management : Retrive a byte array from the ring       */
/*-------------------------------------------------------------------*/
inline static size_t commadpt_ring_popbfr(COMMADPT_RING *ring,BYTE *b,size_t sz)
{
    size_t i;
    for(i=0;i<sz && ring->havedata;i++)
    {
        b[i]=commadpt_ring_pop(ring);
    }
    return i;
}
/*-------------------------------------------------------------------*/
/* Free all private structures and buffers                           */
/*-------------------------------------------------------------------*/
static void commadpt_clean_device(DEVBLK *dev)
{
    if(!dev)
    {
        /*
         * Shouldn't happen.. But during shutdown, some weird
         * things happen !
         */
        return;
    }
    if(dev->commadpt!=NULL)
    {
        commadpt_ring_terminate(&dev->commadpt->inbfr,dev->ccwtrace);
        commadpt_ring_terminate(&dev->commadpt->outbfr,dev->ccwtrace);
        commadpt_ring_terminate(&dev->commadpt->rdwrk,dev->ccwtrace);
        commadpt_ring_terminate(&dev->commadpt->pollbfr,dev->ccwtrace);
        /* release the CA lock */
        release_lock(&dev->commadpt->lock);
        free(dev->commadpt);
        dev->commadpt=NULL;
        if(dev->ccwtrace)
        {
                logmsg(_("HHCCA300D %4.4X:clean : Control block freed\n"),
                        dev->devnum);
        }
    }
    else
    {
        if(dev->ccwtrace)
        {
                logmsg(_("HHCCA300D %4.4X:clean : Control block not freed : not allocated\n"),dev->devnum);
        }
    }
    return;
}

/*-------------------------------------------------------------------*/
/* Allocate initial private structures                               */
/*-------------------------------------------------------------------*/
static int commadpt_alloc_device(DEVBLK *dev)
{
    dev->commadpt=malloc(sizeof(COMMADPT));
    if(dev->commadpt==NULL)
    {
        logmsg(_("HHCCA020E %4.4X:Memory allocation failure for main control block\n"),
                dev->devnum);
        return -1;
    }
    memset(dev->commadpt,0,sizeof(COMMADPT));
    commadpt_ring_init(&dev->commadpt->inbfr,4096,dev->ccwtrace);
    commadpt_ring_init(&dev->commadpt->outbfr,4096,dev->ccwtrace);
    commadpt_ring_init(&dev->commadpt->pollbfr,4096,dev->ccwtrace);
    commadpt_ring_init(&dev->commadpt->rdwrk,65536,dev->ccwtrace);
    dev->commadpt->dev=dev;
    return 0;
}
/*-------------------------------------------------------------------*/
/* Parsing utilities                                                 */
/*-------------------------------------------------------------------*/
/*-------------------------------------------------------------------*/
/* commadpt_getport : returns a port number or -1                    */
/*-------------------------------------------------------------------*/
static int commadpt_getport(char *txt)
{
    int pno;
    struct servent *se;
    pno=atoi(txt);
    if(pno==0)
    {
        se=getservbyname(txt,"tcp");
        if(se==NULL)
        {
            return -1;
        }
        pno=se->s_port;
    }
    return(pno);
}
/*-------------------------------------------------------------------*/
/* commadpt_getaddr : set an in_addr_t if ok, else return -1         */
/*-------------------------------------------------------------------*/
static int commadpt_getaddr(in_addr_t *ia,char *txt)
{
    struct hostent *he;
    he=gethostbyname(txt);
    if(he==NULL)
    {
        return(-1);
    }
    memcpy(ia,he->h_addr_list[0],4);
    return(0);
}
/*-------------------------------------------------------------------*/
/* commadpt_connout : make a tcp outgoing call                       */
/* return values : 0 -> call succeeded or initiated                  */
/*                <0 -> call failed                                  */
/*-------------------------------------------------------------------*/
static int commadpt_connout(COMMADPT *ca)
{
    int rc;
    char        wbfr[256];
    struct      sockaddr_in     sin;
    struct      in_addr intmp;
    sin.sin_family=AF_INET;
    sin.sin_addr.s_addr=ca->rhost;
    sin.sin_port=htons(ca->rport);
    if(socket_is_socket(ca->sfd))
    {
        close_socket(ca->sfd);
        ca->connect=0;
    }
    ca->sfd=socket(AF_INET,SOCK_STREAM,0);
    /* set socket to NON-blocking mode */
    socket_set_blocking_mode(ca->sfd,0);
    rc=connect(ca->sfd,(struct sockaddr *)&sin,sizeof(sin));
    if(rc<0)
    {
        if(HSO_errno==HSO_EINPROGRESS)
        {
            return(0);
        }
        else
        {
            strerror_r(HSO_errno,wbfr,256);
            intmp.s_addr=ca->rhost;
            logmsg(_("HHCCA001I %4.4X:Connect out to %s:%d failed during initial status : %s\n"),
                    ca->devnum,
                    inet_ntoa(intmp),
                    ca->rport,
                    wbfr);
            close_socket(ca->sfd);
            ca->connect=0;
            return(-1);
        }
    }
    ca->connect=1;
    return(0);
}
/*-------------------------------------------------------------------*/
/* commadpt_initiate_userdial : interpret DIAL data and initiate call*/
/* return values : 0 -> call succeeded or initiated                  */
/*                <0 -> call failed                                  */
/*-------------------------------------------------------------------*/
static int     commadpt_initiate_userdial(COMMADPT *ca)
{
    int dotcount;       /* Number of seps (the 4th is the port separator) */
    int i;              /* work                                           */
    int cur;            /* Current section                                */
    in_addr_t   destip; /* Destination IP address                         */
    U16 destport;       /* Destination TCP port                           */
    int incdata;        /* Incorrect dial data found                      */
    int goteon;         /* EON presence flag                              */

   /* See the DIAL CCW portion in execute_ccw for dial format information */

    incdata=0;
    goteon=0;
    dotcount=0;
    cur=0;
    destip=0;
    for(i=0;i<ca->dialcount;i++)
    {
        if(goteon)
        {
            /* EON MUST be last data byte */
            if(ca->dev->ccwtrace)
            {
                logmsg(_("HHCCA300D %4.4x : Found data beyond EON\n"),ca->devnum);
            }
            incdata=1;
            break;
        }
        switch(ca->dialdata[i]&0x0f)
        {
            case 0x0d:  /* SEP */
                if(dotcount<4)
                {
                    if(cur>255)
                    {
                        incdata=1;
                        if(ca->dev->ccwtrace)
                        {
                            logmsg(_("HHCCA300D %4.4x : Found incorrect IP address section at position %d\n"),ca->devnum,dotcount+1);
                            logmsg(_("HHCCA300D %4.4x : %d greater than 255\n"),ca->devnum,cur);
                        }
                        break;
                    }
                    destip<<=8;
                    destip+=cur;
                    cur=0;
                    dotcount++;
                }
                else
                {
                    incdata=1;
                    if(ca->dev->ccwtrace)
                    {
                        logmsg(_("HHCCA300D %4.4x : Too many separators in dial data\n"),ca->devnum);
                    }
                    break;
                }
                break;
            case 0x0c: /* EON */
                goteon=1;
                break;

                /* A,B,E,F not valid */
            case 0x0a:
            case 0x0b:
            case 0x0e:
            case 0x0f:
                incdata=1;
                if(ca->dev->ccwtrace)
                {
                    logmsg(_("HHCCA300D %4.4x : Incorrect dial data byte %2.2x\n"),ca->devnum,ca->dialdata[i]);
                }
                break;
            default:
                cur*=10;
                cur+=ca->dialdata[i]&0x0f;
                break;
        }
        if(incdata)
        {
            break;
        }
    }
    if(incdata)
    {
        return -1;
    }
    if(dotcount<4)
    {
        if(ca->dev->ccwtrace)
        {
            logmsg(_("HHCCA300D %4.4x : Not enough separators (only %d found) in dial data\n"),ca->devnum,dotcount);
        }
        return -1;
    }
    if(cur>65535)
    {
        if(ca->dev->ccwtrace)
        {
            logmsg(_("HHCCA300D %4.4x : Destination TCP port %d exceeds maximum of 65535\n"),ca->devnum,cur);
        }
        return -1;
    }
    destport=cur;
    /* Update RHOST/RPORT */
    ca->rport=destport;
    ca->rhost=destip;
    return(commadpt_connout(ca));
}
/*-------------------------------------------------------------------*/
/* Communication Thread - Read socket data (after POLL request       */
/*-------------------------------------------------------------------*/
static int commadpt_read_poll(COMMADPT *ca)
{
    BYTE b;
    int rc;
    while((rc=read_socket(ca->sfd,&b,1))>0)
    {
        if(b==0x32)
        {
            continue;
        }
        if(b==0x37)
        {
            return(1);
        }
    }
    if(rc>0)
    {
        /* Store POLL IX in bfr followed by byte */
        commadpt_ring_push(&ca->inbfr,ca->pollix);
        commadpt_ring_push(&ca->inbfr,b);
        return(2);
    }
    return(0);
}

/*-------------------------------------------------------------------*/
/* Communication Thread - Read socket data                           */
/*-------------------------------------------------------------------*/
static void commadpt_read(COMMADPT *ca)
{
    BYTE        bfr[256];
    int gotdata;
    int rc;
    gotdata=0;
    while((rc=read_socket(ca->sfd,bfr,256))>0)
    {
        logdump("RECV",ca->dev,bfr,rc);
        commadpt_ring_pushbfr(&ca->inbfr,bfr,(size_t)rc);
        gotdata=1;
    }
    if(!gotdata)
    {
        if(ca->connect)
        {
            ca->connect=0;
            close_socket(ca->sfd);
            ca->sfd=-1;
            if(ca->curpending!=COMMADPT_PEND_IDLE)
            {
                ca->curpending=COMMADPT_PEND_IDLE;
                signal_condition(&ca->ipc);
            }
        }
    }
}
/*-------------------------------------------------------------------*/
/* Communication Thread - Set TimeOut                                */
/*-------------------------------------------------------------------*/
static struct timeval *commadpt_setto(struct timeval *tv,int tmo)
{
    if(tmo!=0)
    {
        if(tmo<0)
        {
            tv->tv_sec=0;
            tv->tv_usec=1;
        }
        else
        {
            tv->tv_sec=tmo/1000;
            tv->tv_usec=(tmo%1000)*1000;
        }
        return(tv);
    }
    return(NULL);
}

/*-------------------------------------------------------------------*/
/* Communication Thread main loop                                    */
/*-------------------------------------------------------------------*/
static void *commadpt_thread(void *vca)
{
    COMMADPT    *ca;            /* Work CA Control Block Pointer     */
    int        sockopt;         /* Used for setsocketoption          */
    struct sockaddr_in sin;     /* bind socket address structure     */
    int devnum;                 /* device number copy for convenience*/
    int rc;                     /* return code from various rtns     */
    struct timeval tv;          /* select timeout structure          */
    struct timeval *seltv;      /* ptr to the timeout structure      */
    fd_set      rfd,wfd,xfd;    /* SELECT File Descriptor Sets       */
    BYTE        pipecom;        /* Byte read from IPC pipe           */
    int tempfd;                 /* Temporary FileDesc holder         */
    BYTE b;                     /* Work data byte                    */
    int writecont;              /* Write contention active           */
    int soerr;                  /* getsockopt SOERROR value          */
    socklen_t   soerrsz;        /* Size for getsockopt               */
    int maxfd;                  /* highest FD for select             */
    int ca_shutdown;            /* Thread shutdown internal flag     */
    int init_signaled;          /* Thread initialisation signaled    */
    int pollact;                /* A Poll Command is in progress     */
    int i;                      /* Ye Old Loop Counter               */
    int eintrcount=0;           /* Number of times EINTR occured in  */
                                /* a row.. Over 100 : Bail out !     */

    /*---------------------END OF DECLARES---------------------------*/

    /* fetch the commadpt structure */
    ca=(COMMADPT *)vca;

    /* Obtain the CA lock */
    obtain_lock(&ca->lock);

    /* get a work copy of devnum (for messages) */
    devnum=ca->devnum;

    /* reset shutdown flag */
    ca_shutdown=0;

    init_signaled=0;

    logmsg(_("HHCCA002I %4.4X:Line Communication thread "TIDPAT" started\n"),devnum,thread_id());

    pollact=0;  /* Initialise Poll activity flag */

    /* Determine if we should listen */
    /* if this is a DIAL=OUT only line, no listen is necessary */
    if(ca->dolisten)
    {
        /* Create the socket for a listen */
        ca->lfd=socket(AF_INET,SOCK_STREAM,0);
        if(!socket_is_socket(ca->lfd))
        {
            logmsg(_("HHCCA003E %4.4X:Cannot obtain socket for incoming calls : %s\n"),devnum,strerror(HSO_errno));
            ca->have_cthread=0;
            release_lock(&ca->lock);
            return NULL;
        }
        /* Turn blocking I/O off */
        /* set socket to NON-blocking mode */
        socket_set_blocking_mode(ca->lfd,0);

        /* Reuse the address regardless of any */
        /* spurious connection on that port    */
        sockopt=1;
        setsockopt(ca->lfd,SOL_SOCKET,SO_REUSEADDR,(GETSET_SOCKOPT_T*)&sockopt,sizeof(sockopt));

        /* Bind the socket */
        sin.sin_family=AF_INET;
        sin.sin_addr.s_addr=ca->lhost;
        sin.sin_port=htons(ca->lport);
        while(1)
        {
            rc=bind(ca->lfd,(struct sockaddr *)&sin,sizeof(sin));
            if(rc<0)
            {
                if(HSO_errno==HSO_EADDRINUSE)
                {
                    logmsg(_("HHCCA004W %4.4X:Waiting 5 seconds for port %d to become available\n"),devnum,ca->lport);
                    /*
                     * Check for a shutdown condition on entry
                     */
                    if(ca->curpending==COMMADPT_PEND_SHUTDOWN)
                    {
                        ca_shutdown=1;
                        ca->curpending=COMMADPT_PEND_IDLE;
                        signal_condition(&ca->ipc);
                        break;
                    }

                    /* Set to wait 5 seconds or input on the IPC pipe */
                    /* whichever comes 1st                            */
                    if(!init_signaled)
                    {
                        ca->curpending=COMMADPT_PEND_IDLE;
                        signal_condition(&ca->ipc);
                        init_signaled=1;
                    }

                    FD_ZERO(&rfd);
                    FD_ZERO(&wfd);
                    FD_ZERO(&xfd);
                    FD_SET(ca->pipe[1],&rfd);
                    tv.tv_sec=5;
                    tv.tv_usec=0;

                    release_lock(&ca->lock);
                    rc=select(ca->pipe[1]+1,&rfd,&wfd,&wfd,&tv);
                    obtain_lock(&ca->lock);
                    /*
                     * Check for a shutdown condition again after the sleep
                     */
                    if(ca->curpending==COMMADPT_PEND_SHUTDOWN)
                    {
                        ca_shutdown=1;
                        ca->curpending=COMMADPT_PEND_IDLE;
                        signal_condition(&ca->ipc);
                        break;
                    }
                    if(rc!=0)
                    {
                        /* Ignore any other command at this stage */
                        read_pipe(ca->pipe[1],&b,1);
                        ca->curpending=COMMADPT_PEND_IDLE;
                        signal_condition(&ca->ipc);
                    }
                }
                else
                {
                    logmsg(_("HHCCA018E %4.4X:Bind failed : %s\n"),devnum,strerror(HSO_errno));
                    ca_shutdown=1;
                    break;
                }
            }
            else
            {
                break;
            }
        }
        /* Start the listen */
        if(!ca_shutdown)
        {
            listen(ca->lfd,10);
            logmsg(_("HHCCA005I %4.4X:Listening on port %d for incoming TCP connections\n"),
                    devnum,
                    ca->lport);
            ca->listening=1;
        }
    }
    if(!init_signaled)
    {
        ca->curpending=COMMADPT_PEND_IDLE;
        signal_condition(&ca->ipc);
        init_signaled=1;
    }

    /* The MAIN select loop */
    /* It will listen on the following sockets : */
    /* ca->lfd : The listen socket */
    /* ca->sfd :
     *         read : When a read, prepare or DIAL command is in effect
     *        write : When a write contention occurs
     * ca->pipe[0] : Always
     *
     * A 3 Seconds timer is started for a read operation
     */

    while(!ca_shutdown)
    {
        FD_ZERO(&rfd);
        FD_ZERO(&wfd);
        FD_ZERO(&xfd);
        maxfd=0;
        if(ca->listening)
        {
                FD_SET(ca->lfd,&rfd);
                maxfd=maxfd<ca->lfd?ca->lfd:maxfd;
        }
        seltv=NULL;
        if(ca->dev->ccwtrace)
        {
                logmsg(_("HHCCA300D %4.4X:cthread - Entry - DevExec = %s\n"),devnum,commadpt_pendccw_text[ca->curpending]);
        }
        writecont=0;
        switch(ca->curpending)
        {
            case COMMADPT_PEND_SHUTDOWN:
                ca_shutdown=1;
                break;
            case COMMADPT_PEND_IDLE:
                break;
            case COMMADPT_PEND_READ:
                if(!ca->connect)
                {
                    ca->curpending=COMMADPT_PEND_IDLE;
                    signal_condition(&ca->ipc);
                    break;
                }
                if(ca->inbfr.havedata)
                {
                    ca->curpending=COMMADPT_PEND_IDLE;
                    signal_condition(&ca->ipc);
                    break;
                }
                seltv=commadpt_setto(&tv,ca->rto);
                FD_SET(ca->sfd,&rfd);
                maxfd=maxfd<ca->sfd?ca->sfd:maxfd;
                break;
            case COMMADPT_PEND_POLL:
                /* Poll active check - provision for write contention */
                /* pollact will be reset when NON syn data is received*/
                /* or when the read times out                         */
                /* Also prevents WRITE from exiting early             */
                if(!pollact && !writecont)
                {
                    int gotenq;

                    pollact=1;
                    gotenq=0;
                    /* Send SYN+SYN */
                    commadpt_ring_push(&ca->outbfr,0x32);
                    commadpt_ring_push(&ca->outbfr,0x32);
                    /* Fill the Output ring with POLL Data */
                    /* Up to 7 chars or ENQ                */
                    for(i=0;i<7;i++)
                    {
                        if(!ca->pollbfr.havedata)
                        {
                            break;
                        }
                        ca->pollused++;
                        b=commadpt_ring_pop(&ca->pollbfr);
                        if(b!=0x2D)
                        {
                            commadpt_ring_push(&ca->outbfr,b);
                        }
                        else
                        {
                            gotenq=1;
                            break;
                        }
                    }
                    if(!gotenq)
                    {
                        if(ca->dev->ccwtrace)
                        {
                            logmsg(_("HHCCA300D %4.4X:Poll Command abort - Poll address >7 Bytes\n"),devnum);
                        }
                        ca->badpoll=1;
                        ca->curpending=COMMADPT_PEND_IDLE;
                        signal_condition(&ca->ipc);
                        break;
                    }
                    b=commadpt_ring_pop(&ca->pollbfr);
                    ca->pollix=b;
                    seltv=commadpt_setto(&tv,ca->pto);
                }
                if(!writecont && ca->pto!=0)
                {
                    /* Set tv value (have been set earlier) */
                    seltv=&tv;
                    /* Set to read data still               */
                    FD_SET(ca->sfd,&rfd);
                    maxfd=maxfd<ca->sfd?ca->sfd:maxfd;
                }
                /* DO NOT BREAK - Continue with WRITE processing */
            case COMMADPT_PEND_WRITE:
                if(!writecont)
                {
                    while(ca->outbfr.havedata)
                    {
                        b=commadpt_ring_pop(&ca->outbfr);
                        if(ca->dev->ccwtrace)
                        {
                                logmsg(_("HHCCA300D %4.4X:Writing 1 byte in socket : %2.2X\n"),ca->devnum,b);
                        }
                        rc=write_socket(ca->sfd,&b,1);
                        if(rc!=1)
                        {
                            if(0
#ifndef WIN32
                                || EAGAIN == errno
#endif
                                || HSO_EWOULDBLOCK == HSO_errno
                            )
                            {
                                /* Contending for write */
                                writecont=1;
                                FD_SET(ca->sfd,&wfd);
                                maxfd=maxfd<ca->sfd?ca->sfd:maxfd;
                                break;
                            }
                            else
                            {
                                close_socket(ca->sfd);
                                ca->sfd=-1;
                                ca->connect=0;
                                ca->curpending=COMMADPT_PEND_IDLE;
                                signal_condition(&ca->ipc);
                                break;
                            }
                        }
                    }
                }
                else
                {
                        FD_SET(ca->sfd,&wfd);
                        maxfd=maxfd<ca->sfd?ca->sfd:maxfd;
                }
                if(!writecont && !pollact)
                {
                        ca->curpending=COMMADPT_PEND_IDLE;
                        signal_condition(&ca->ipc);
                        break;
                }
                break;
            case COMMADPT_PEND_DIAL:
                if(ca->connect)
                {
                    ca->curpending=COMMADPT_PEND_IDLE;
                    signal_condition(&ca->ipc);
                    break;
                }
                rc=commadpt_initiate_userdial(ca);
                if(rc!=0 || (rc==0 && ca->connect))
                {
                    ca->curpending=COMMADPT_PEND_IDLE;
                    signal_condition(&ca->ipc);
                    break;
                }
                FD_SET(ca->sfd,&wfd);
                maxfd=maxfd<ca->sfd?ca->sfd:maxfd;
                break;
            case COMMADPT_PEND_ENABLE:
                if(ca->connect)
                {
                    ca->curpending=COMMADPT_PEND_IDLE;
                    signal_condition(&ca->ipc);
                    break;
                }
                switch(ca->dialin+ca->dialout*2)
                {
                    case 0: /* DIAL=NO */
                        /* callissued is set here when the call */
                        /* actually failed. But we want to time */
                        /* a bit for program issuing ENABLES in */
                        /* a tight loop                         */
                        if(ca->callissued)
                        {
                            seltv=commadpt_setto(&tv,ca->eto);
                            break;
                        }
                        /* Issue a Connect out */
                        rc=commadpt_connout(ca);
                        if(rc==0)
                        {
                            /* Call issued */
                            if(ca->connect)
                            {
                                /* Call completed already */
                                ca->curpending=COMMADPT_PEND_IDLE;
                                signal_condition(&ca->ipc);
                            }
                            else
                            {
                                /* Call initiated - FD will be ready */
                                /* for writing when the connect ends */
                                /* getsockopt/SOERROR will tell if   */
                                /* the call was sucessfull or not    */
                                FD_SET(ca->sfd,&wfd);
                                maxfd=maxfd<ca->sfd?ca->sfd:maxfd;
                                ca->callissued=1;
                            }
                        }
                        /* Call did not succeed                                 */
                        /* Manual says : on a leased line, if DSR is not up     */
                        /* the terminate enable after a timeout.. That is       */
                        /* what the call just did (although the time out        */
                        /* was probably instantaneous)                          */
                        /* This is the equivalent of the comm equipment         */
                        /* being offline                                        */
                        /*       INITIATE A 3 SECOND TIMEOUT                    */
                        /* to prevent OSes from issuing a loop of ENABLES       */
                        else
                        {
                            seltv=commadpt_setto(&tv,ca->eto);
                        }
                        break;
                    default:
                    case 3: /* DIAL=INOUT */
                    case 1: /* DIAL=IN */
                        /* Wait forever */
                        break;
                    case 2: /* DIAL=OUT */
                        /* Makes no sense                               */
                        /* line must be enabled through a DIAL command  */
                        ca->curpending=COMMADPT_PEND_IDLE;
                        signal_condition(&ca->ipc);
                        break;
                }
                /* For cases not DIAL=OUT, the listen is already started */
                break;

                /* The CCW Executor says : DISABLE */
            case COMMADPT_PEND_DISABLE:
                if(ca->connect)
                {
                    close_socket(ca->sfd);
                    ca->sfd=-1;
                    ca->connect=0;
                }
                ca->curpending=COMMADPT_PEND_IDLE;
                signal_condition(&ca->ipc);
                break;

                /* A PREPARE has been issued */
            case COMMADPT_PEND_PREPARE:
                if(!ca->connect || ca->inbfr.havedata)
                {
                    ca->curpending=COMMADPT_PEND_IDLE;
                    signal_condition(&ca->ipc);
                    break;
                }
                FD_SET(ca->sfd,&rfd);
                maxfd=maxfd<ca->sfd?ca->sfd:maxfd;
                break;

                /* Don't know - shouldn't be here anyway */
            default:
                break;
        }

        /* If the CA is shutting down, exit the loop now */
        if(ca_shutdown)
        {
            ca->curpending=COMMADPT_PEND_IDLE;
            signal_condition(&ca->ipc);
            break;
        }

        /* Set the IPC pipe in the select */
        FD_SET(ca->pipe[0],&rfd);

        /* The the MAX File Desc for Arg 1 of SELECT */
        maxfd=maxfd<ca->pipe[0]?ca->pipe[0]:maxfd;
        maxfd++;

        /* Release the CA Lock before the select - all FDs addressed by the select are only */
        /* handled by the thread, and communication from CCW Executor/others to this thread */
        /* is via the pipe, which queues the info                                           */
        release_lock(&ca->lock);

        if(ca->dev->ccwtrace)
        {
                logmsg(_("HHCCA300D %4.4X:cthread - Select IN maxfd = %d / Devexec = %s\n"),devnum,maxfd,commadpt_pendccw_text[ca->curpending]);
        }
        rc=select(maxfd,&rfd,&wfd,&xfd,seltv);

        if(ca->dev->ccwtrace)
        {
                logmsg(_("HHCCA300D %4.4X:cthread - Select OUT rc=%d\n"),devnum,rc);
        }
        /* Get the CA lock back */
        obtain_lock(&ca->lock);

        if(rc==-1)
        {
            if(errno==EINTR)
            {
                eintrcount++;
                if(eintrcount>100)
                {
                    break;
                }
                continue;
            }
            logmsg(_("HHCCA006T %4.4X:Select failed : %s\n"),devnum,strerror(HSO_errno));
            break;
        }
        eintrcount=0;

        /* Select timed out */
        if(rc==0)
        {
            pollact=0;  /* Poll not active */
            if(ca->dev->ccwtrace)
            {
                logmsg(_("HHCCA300D %4.4X:cthread - Select TIME OUT\n"),devnum);
            }
            /* Reset Call issued flag */
            ca->callissued=0;

            /* timeout condition */
            signal_condition(&ca->ipc);
            ca->curpending=COMMADPT_PEND_IDLE;
            continue;
        }

        if(FD_ISSET(ca->pipe[0],&rfd))
        {
            rc=read_pipe(ca->pipe[0],&pipecom,1);
            if(rc==0)
            {
                if(ca->dev->ccwtrace)
                {
                        logmsg(_("HHCCA300D %4.4X:cthread - IPC Pipe closed\n"),devnum);
                }
                /* Pipe closed : terminate thread & release CA */
                ca_shutdown=1;
                break;
            }
            if(ca->dev->ccwtrace)
            {
                logmsg(_("HHCCA300D %4.4X:cthread - IPC Pipe Data ; code = %d\n"),devnum,pipecom);
            }
            switch(pipecom)
            {
                case 0: /* redrive select */
                        /* occurs when a new CCW is being executed */
                    break;
                case 1: /* Halt current I/O */
                    ca->callissued=0;
                    if(ca->curpending==COMMADPT_PEND_DIAL)
                    {
                        close_socket(ca->sfd);
                        ca->sfd=-1;
                    }
                    ca->curpending=COMMADPT_PEND_IDLE;
                    ca->haltpending=1;
                    signal_condition(&ca->ipc);
                    signal_condition(&ca->ipc_halt);    /* Tell the halt initiator too */
                    break;
                default:
                    break;
            }
            continue;
        }
        if(ca->connect)
        {
            if(FD_ISSET(ca->sfd,&rfd))
            {
                int dopoll;
                dopoll=0;
                if(ca->dev->ccwtrace)
                {
                        logmsg(_("HHCCA300D %4.4X:cthread - inbound socket data\n"),devnum);
                }
                if(pollact)
                {
                    switch(commadpt_read_poll(ca))
                    {
                        case 0: /* Only SYNs received */
                                /* Continue the timeout */
                            dopoll=1;
                            break;
                        case 1: /* EOT Received */
                            /* Send next poll sequence */
                            pollact=0;
                            dopoll=1;
                            break;
                        case 2: /* Something else received */
                            /* Index byte already stored in inbfr */
                            /* read the remaining data and return */
                            ca->pollsm=1;
                            dopoll=0;
                            break;
                        default:
                            /* Same as 0 */
                            dopoll=1;
                            break;
                    }
                }
                if(!dopoll)
                {
                    commadpt_read(ca);
                    ca->curpending=COMMADPT_PEND_IDLE;
                    signal_condition(&ca->ipc);
                    continue;
                }
            }
        }
        if(ca->sfd>=0)
        {
            if(FD_ISSET(ca->sfd,&wfd))
            {
                if(ca->dev->ccwtrace)
                {
                        logmsg(_("HHCCA300D %4.4X:cthread - socket write available\n"),devnum);
                }
                switch(ca->curpending)
                {
                    case COMMADPT_PEND_DIAL:
                    case COMMADPT_PEND_ENABLE:  /* Leased line enable call case */
                    soerrsz=sizeof(soerr);
                    getsockopt(ca->sfd,SOL_SOCKET,SO_ERROR,(GETSET_SOCKOPT_T*)&soerr,&soerrsz);
                    if(soerr==0)
                    {
                        ca->connect=1;
                    }
                    else
                    {
                        logmsg(_("HHCCA007W %4.4X:Outgoing call failed during %s command : %s\n"),devnum,commadpt_pendccw_text[ca->curpending],strerror(soerr));
                        if(ca->curpending==COMMADPT_PEND_ENABLE)
                        {
                            /* Ensure top of the loop doesn't restart a new call */
                            /* but starts a 3 second timer instead               */
                            ca->callissued=1;
                        }
                        ca->connect=0;
                        close_socket(ca->sfd);
                        ca->sfd=-1;
                    }
                    signal_condition(&ca->ipc);
                    ca->curpending=COMMADPT_PEND_IDLE;
                    break;

                    case COMMADPT_PEND_WRITE:
                    writecont=0;
                    break;

                    default:
                    break;
                }
                continue;
            }
        }
        /* Test for incoming call */
        if(ca->listening)
        {
            if(FD_ISSET(ca->lfd,&rfd))
            {
                logmsg(_("HHCCA008I %4.4X:cthread - Incoming Call\n"),devnum);
                tempfd=accept(ca->lfd,NULL,0);
                if(tempfd<0)
                {
                    continue;
                }
                /* If the line is already connected, just close */
                /* this call                                    */
                if(ca->connect)
                {
                    close_socket(tempfd);
                    continue;
                }
                /* Turn non-blocking I/O on */
                /* set socket to NON-blocking mode */
                socket_set_blocking_mode(tempfd,0);

                /* Check the line type & current operation */

                /* if DIAL=IN or DIAL=INOUT or DIAL=NO */
                if(ca->dialin || (ca->dialin+ca->dialout==0))
                {
                    /* check if ENABLE is in progress */
                    if(ca->curpending==COMMADPT_PEND_ENABLE)
                    {
                        /* Accept the call, indicate the line */
                        /* is connected and notify CCW exec   */
                        ca->curpending=COMMADPT_PEND_IDLE;
                        ca->connect=1;
                        ca->sfd=tempfd;
                        signal_condition(&ca->ipc);
                        continue;
                    }
                    /* if this is a leased line, accept the */
                    /* call anyway                          */
                    if(ca->dialin==0)
                    {
                        ca->connect=1;
                        ca->sfd=tempfd;
                        continue;
                    }
                }
                /* All other cases : just reject the call */
                close_socket(tempfd);
            }
        }
    }
    ca->curpending=COMMADPT_PEND_CLOSED;
    /* Check if we already signaled the init process  */
    if(!init_signaled)
    {
        signal_condition(&ca->ipc);
    }
    /* The CA is shutting down - terminate the thread */
    /* NOTE : the requestor was already notified upon */
    /*        detection of PEND_SHTDOWN. However      */
    /*        the requestor will only run when the    */
    /*        lock is released, because back          */
    /*        notification was made while holding     */
    /*        the lock                                */
    logmsg(_("HHCCA009I %4.4X:BSC utility thread terminated\n"),ca->devnum);
    release_lock(&ca->lock);
    return NULL;
}
/*-------------------------------------------------------------------*/
/* Wakeup the comm thread                                            */
/* Code : 0 -> Just wakeup the thread to redrive the select          */
/* Code : 1 -> Halt the current executing I/O                        */
/*-------------------------------------------------------------------*/
static void commadpt_wakeup(COMMADPT *ca,BYTE code)
{
    write_pipe(ca->pipe[1],&code,1);
}
/*-------------------------------------------------------------------*/
/* Wait for a copndition from the thread                             */
/* MUST HOLD the CA lock                                             */
/*-------------------------------------------------------------------*/
static void commadpt_wait(DEVBLK *dev)
{
    COMMADPT *ca;
    ca=dev->commadpt;
    wait_condition(&ca->ipc,&ca->lock);
}

/*-------------------------------------------------------------------*/
/* Halt currently executing I/O command                              */
/*-------------------------------------------------------------------*/
static void    commadpt_halt(DEVBLK *dev)
{
    if(!dev->busy)
    {
        return;
    }
    obtain_lock(&dev->commadpt->lock);
    commadpt_wakeup(dev->commadpt,1);
    /* Due to the mysteries of the host OS scheduling */
    /* the wait_condition may or may not exit after   */
    /* the CCW executor thread relinquishes control   */
    /* This however should not be of any concern      */
    /*                                                */
    /* but returning from the wait guarantees that    */
    /* the working thread will (or has) notified      */
    /* the CCW executor to terminate the current I/O  */
    wait_condition(&dev->commadpt->ipc_halt,&dev->commadpt->lock);
    release_lock(&dev->commadpt->lock);
}
/* The following 3 MSG functions ensure only 1 (one)  */
/* hardcoded instance exist for the same numbered msg */
/* that is issued on multiple situations              */
static void msg013e(DEVBLK *dev,char *kw,char *kv)
{
        logmsg(_("HHCCA013E %4.4X:Incorrect %s specification %s\n"),dev->devnum,kw,kv);
}
static void msg015e(DEVBLK *dev,char *dialt,char *kw)
{
        logmsg(_("HHCCA015E %4.4X:Missing parameter : DIAL=%s and %s not specified\n"),dev->devnum,dialt,kw);
}
static void msg016w017i(DEVBLK *dev,char *dialt,char *kw,char *kv)
{
        logmsg(_("HHCCA016W %4.4X:Conflicting parameter : DIAL=%s and %s=%s specified\n"),dev->devnum,dialt,kw,kv);
        logmsg(_("HHCCA017I %4.4X:RPORT parameter ignored\n"),dev->devnum);
}
/*-------------------------------------------------------------------*/
/* Device Initialisation                                             */
/*-------------------------------------------------------------------*/
static int commadpt_init_handler (DEVBLK *dev, int argc, char *argv[])
{
    char thread_name[32];
    int i;
    int rc;
    int pc; /* Parse code */
    int errcnt;
    struct in_addr in_temp;
    char    *dialt;
    char        fmtbfr[64];
    int etospec;        /* ETO= Specified */
    union {
        int num;
        char text[80];
    } res;
        dev->devtype=0x2703;
        if(dev->ccwtrace)
        {
                logmsg(_("HHCCA300D %4.4X:Initialisation starting\n"),dev->devnum);
        }

        rc=commadpt_alloc_device(dev);
        if(rc<0)
        {
                logmsg(_("HHCCA010I %4.4X:initialisation not performed\n"),
                        dev->devnum);
            return(-1);
        }
        if(dev->ccwtrace)
        {
                logmsg(_("HHCCA300D %4.4X:Initialisation : Control block allocated\n"),dev->devnum);
        }
        errcnt=0;
        /*
         * Initialise ports & hosts
        */
        dev->commadpt->sfd=-1;
        dev->commadpt->lport=0;
        dev->commadpt->rport=0;
        dev->commadpt->lhost=INADDR_ANY;
        dev->commadpt->rhost=INADDR_NONE;
        dev->commadpt->dialin=0;
        dev->commadpt->dialout=1;
        dev->commadpt->rto=3000;        /* Read Time-Out in milis */
        dev->commadpt->pto=3000;        /* Poll Time-out in milis */
        dev->commadpt->eto=10000;       /* Enable Time-out in milis */
        etospec=0;

        for(i=0;i<argc;i++)
        {
            pc=parser(ptab,argv[i],&res);
            if(pc<0)
            {
                logmsg(_("HHCCA011E %4.4X:Error parsing %s\n"),dev->devnum,argv[i]);
                errcnt++;
                continue;
            }
            if(pc==0)
            {
                logmsg(_("HHCCA012E %4.4X:Unrecognized parameter %s\n"),dev->devnum,argv[i]);
                errcnt++;
                continue;
            }
            switch(pc)
            {
                case COMMADPT_KW_LPORT:
                    rc=commadpt_getport(res.text);
                    if(rc<0)
                    {
                        errcnt++;
                        msg013e(dev,"LPORT",res.text);
                        break;
                    }
                    dev->commadpt->lport=rc;
                    break;
                case COMMADPT_KW_LHOST:
                    if(strcmp(res.text,"*")==0)
                    {
                        dev->commadpt->lhost=INADDR_ANY;
                        break;
                    }
                    rc=commadpt_getaddr(&dev->commadpt->lhost,res.text);
                    if(rc!=0)
                    {
                        msg013e(dev,"LHOST",res.text);
                        errcnt++;
                    }
                    break;
                case COMMADPT_KW_RPORT:
                    rc=commadpt_getport(res.text);
                    if(rc<0)
                    {
                        errcnt++;
                        msg013e(dev,"RPORT",res.text);
                        break;
                    }
                    dev->commadpt->rport=rc;
                    break;
                case COMMADPT_KW_RHOST:
                    if(strcmp(res.text,"*")==0)
                    {
                        dev->commadpt->rhost=INADDR_NONE;
                        break;
                    }
                    rc=commadpt_getaddr(&dev->commadpt->rhost,res.text);
                    if(rc!=0)
                    {
                        msg013e(dev,"RHOST",res.text);
                        errcnt++;
                    }
                    break;
                case COMMADPT_KW_READTO:
                    dev->commadpt->rto=atoi(res.text);
                    break;
                case COMMADPT_KW_POLLTO:
                    dev->commadpt->pto=atoi(res.text);
                    break;
                case COMMADPT_KW_ENABLETO:
                    dev->commadpt->eto=atoi(res.text);
                    etospec=1;
                    break;
                case COMMADPT_KW_SWITCHED:
                case COMMADPT_KW_DIAL:
                    if(strcasecmp(res.text,"yes")==0 || strcmp(res.text,"1")==0 || strcasecmp(res.text,"inout")==0)
                    {
                        dev->commadpt->dialin=1;
                        dev->commadpt->dialout=1;
                        break;
                    }
                    if(strcasecmp(res.text,"no")==0 || strcmp(res.text,"0")==0)
                    {
                        dev->commadpt->dialin=0;
                        dev->commadpt->dialout=0;
                        break;
                    }
                    if(strcasecmp(res.text,"in")==0)
                    {
                        dev->commadpt->dialin=1;
                        dev->commadpt->dialout=0;
                        break;
                    }
                    if(strcasecmp(res.text,"out")==0)
                    {
                        dev->commadpt->dialin=0;
                        dev->commadpt->dialout=1;
                        break;
                    }
                    logmsg(_("HHCCA014E %4.4X:Incorrect switched/dial specification %s; defaulting to DIAL=OUT\n"),dev->devnum,res.text);
                    dev->commadpt->dialin=0;
                    dev->commadpt->dialout=0;
                    break;
                default:
                    break;
            }
        }
        /*
         * Check parameters consistency
         * when DIAL=NO :
         *     lport must not be 0
         *     lhost may be anything
         *     rport must not be 0
         *     rhost must not be INADDR_NONE
         * when DIAL=IN or DIAL=INOUT
         *     lport must NOT be 0
         *     lhost may be anything
         *     rport MUST be 0
         *     rhost MUST be INADDR_NONE
         * when DIAL=OUT
         *     lport MUST be 0
         *     lhost MUST be INADDR_ANY
         *     rport MUST be 0
         *     rhost MUST be INADDR_NONE
        */
        switch(dev->commadpt->dialin+dev->commadpt->dialout*2)
        {
                case 0:
                    dialt="NO";
                    break;
                case 1:
                    dialt="IN";
                    break;
                case 2:
                    dialt="OUT";
                    break;
                case 3:
                    dialt="INOUT";
                    break;
                default:
                    dialt="*ERR*";
                    break;
        }
        switch(dev->commadpt->dialin+dev->commadpt->dialout*2)
        {
            case 0: /* DIAL = NO */
                dev->commadpt->eto=0;
                if(dev->commadpt->lport==0)
                {
                    msg015e(dev,dialt,"LPORT");
                    errcnt++;
                }
                if(dev->commadpt->rport==0)
                {
                    msg015e(dev,dialt,"RPORT");
                    errcnt++;
                }
                if(dev->commadpt->rhost==INADDR_NONE)
                {
                    msg015e(dev,dialt,"RHOST");
                    errcnt++;
                }
                if(etospec)
                {
                    snprintf(fmtbfr,sizeof(fmtbfr),"%d",dev->commadpt->eto);
                    msg016w017i(dev,dialt,"ETO",fmtbfr);
                    errcnt++;
                }
                dev->commadpt->eto=0;
                break;
            case 1: /* DIAL = IN */
            case 3: /* DIAL = INOUT */
                if(dev->commadpt->lport==0)
                {
                    msg015e(dev,dialt,"LPORT");
                    errcnt++;
                }
                if(dev->commadpt->rport!=0)
                {
                    snprintf(fmtbfr,sizeof(fmtbfr),"%d",dev->commadpt->rport);
                    msg016w017i(dev,dialt,"RPORT",fmtbfr);
                }
                if(dev->commadpt->rhost!=INADDR_NONE)
                {
                    in_temp.s_addr=dev->commadpt->rhost;
                    msg016w017i(dev,dialt,"RHOST",inet_ntoa(in_temp));
                    dev->commadpt->rhost=INADDR_NONE;
                }
                break;
            case 2: /* DIAL = OUT */
                if(dev->commadpt->lport!=0)
                {
                    snprintf(fmtbfr,sizeof(fmtbfr),"%d",dev->commadpt->lport);
                    msg016w017i(dev,dialt,"LPORT",fmtbfr);
                    dev->commadpt->lport=0;
                }
                if(dev->commadpt->rport!=0)
                {
                    snprintf(fmtbfr,sizeof(fmtbfr),"%d",dev->commadpt->rport);
                    msg016w017i(dev,dialt,"RPORT",fmtbfr);
                    dev->commadpt->rport=0;
                }
                if(dev->commadpt->lhost!=INADDR_ANY)    /* Actually it's more like INADDR_NONE */
                {
                    in_temp.s_addr=dev->commadpt->lhost;
                    msg016w017i(dev,dialt,"LHOST",inet_ntoa(in_temp));
                    dev->commadpt->lhost=INADDR_ANY;
                }
                if(dev->commadpt->rhost!=INADDR_NONE)
                {
                    in_temp.s_addr=dev->commadpt->rhost;
                    msg016w017i(dev,dialt,"RHOST",inet_ntoa(in_temp));
                    dev->commadpt->rhost=INADDR_NONE;
                }
                break;
        }
        if(errcnt>0)
        {
            logmsg(_("HHCCA021I %4.4X:Initialisation failed due to previous errors\n"),dev->devnum);
            return -1;
        }
        in_temp.s_addr=dev->commadpt->lhost;
        in_temp.s_addr=dev->commadpt->rhost;
        dev->bufsize=256;
        dev->numsense=2;
        memset(dev->sense,0,sizeof(dev->sense));

        /* Initialise various flags & statuses */
        dev->commadpt->enabled=0;
        dev->commadpt->connect=0;
        dev->commadpt->lnctl=COMMADPT_LNCTL_BSC;
        dev->fd=100;    /* Ensures 'close' function called */
        dev->commadpt->devnum=dev->devnum;

        /* Initialize the device identifier bytes */
        dev->numdevid = sysblk.legacysenseid ? 7 : 0;
        dev->devid[0] = 0xFF;
        dev->devid[1] = dev->devtype >> 8;
        dev->devid[2] = dev->devtype & 0xFF;
        dev->devid[3] = 0x00;
        dev->devid[4] = dev->devtype >> 8;
        dev->devid[5] = dev->devtype & 0xFF;
        dev->devid[6] = 0x00;

        /* Initialize the CA lock */
        initialize_lock(&dev->commadpt->lock);

        /* Initialise thread->I/O & halt initiation EVB */
        initialize_condition(&dev->commadpt->ipc);
        initialize_condition(&dev->commadpt->ipc_halt);

        /* Allocate I/O -> Thread signaling pipe */
        create_pipe(dev->commadpt->pipe);

        /* Point to the halt routine for HDV/HIO/HSCH handling */
        dev->halt_device=commadpt_halt;

        /* Obtain the CA lock */
        obtain_lock(&dev->commadpt->lock);

        /* Indicate listen required if DIAL!=OUT */
        if(dev->commadpt->dialin ||
                (!dev->commadpt->dialin && !dev->commadpt->dialout))
        {
            dev->commadpt->dolisten=1;
        }
        else
        {
            dev->commadpt->dolisten=0;
        }

        /* Start the async worker thread */

    /* Set thread-name for debugging purposes */
        snprintf(thread_name,sizeof(thread_name),
                 "commadpt %4.4X thread",dev->devnum);
        thread_name[sizeof(thread_name)-1]=0;

        dev->commadpt->curpending=COMMADPT_PEND_TINIT;
        if(create_thread(&dev->commadpt->cthread,DETACHED,commadpt_thread,dev->commadpt,thread_name))
        {
            logmsg(D_("HHCCA022E create_thread: %s\n"),strerror(errno));
            release_lock(&dev->commadpt->lock);
            return -1;
        }
        commadpt_wait(dev);
        if(dev->commadpt->curpending!=COMMADPT_PEND_IDLE)
        {
            logmsg(_("HHCCA019E %4.4x : BSC comm thread did not initialise\n"),dev->devnum);
            /* Release the CA lock */
            release_lock(&dev->commadpt->lock);
            return -1;
        }
        dev->commadpt->have_cthread=1;

        /* Release the CA lock */
        release_lock(&dev->commadpt->lock);
        /* Indicate succesfull completion */
        return 0;
}

static char *commadpt_lnctl_names[]={
    "NONE",
    "BSC",
    "TELE2"
};

/*-------------------------------------------------------------------*/
/* Query the device definition                                       */
/*-------------------------------------------------------------------*/
static void commadpt_query_device (DEVBLK *dev, char **class,
                int buflen, char *buffer)
{
    BEGIN_DEVICE_CLASS_QUERY( "LINE", dev, class, buflen, buffer );

    snprintf(buffer,buflen,"%s STA=%s CN=%s, EIB=%s OP=%s",
            commadpt_lnctl_names[dev->commadpt->lnctl],
            dev->commadpt->enabled?"ENA":"DISA",
            dev->commadpt->connect?"YES":"NO",
            dev->commadpt->eibmode?"YES":"NO",
            commadpt_pendccw_text[dev->commadpt->curpending]);
}

/*-------------------------------------------------------------------*/
/* Close the device                                                  */
/* Invoked by HERCULES shutdown & DEVINIT processing                 */
/*-------------------------------------------------------------------*/
static int commadpt_close_device ( DEVBLK *dev )
{
    if(dev->ccwtrace)
    {
        logmsg(_("HHCCA300D %4.4X:Closing down\n"),dev->devnum);
    }

    /* Terminate current I/O thread if necessary */
    if(dev->busy)
    {
        commadpt_halt(dev);
    }

    /* Obtain the CA lock */
    obtain_lock(&dev->commadpt->lock);

    /* Terminate worker thread if it is still up */
    if(dev->commadpt->have_cthread)
    {
        dev->commadpt->curpending=COMMADPT_PEND_SHUTDOWN;
        commadpt_wakeup(dev->commadpt,0);
        commadpt_wait(dev);
        dev->commadpt->cthread=(TID)-1;
        dev->commadpt->have_cthread=0;
    }


    /* Free all work storage */
    /* The CA lock will be released by the cleanup routine */
    commadpt_clean_device(dev);

    /* Indicate to hercules the device is no longer opened */
    dev->fd=-1;

    if(dev->ccwtrace)
    {
        logmsg(_("HHCCA300D %4.4X:Closed down\n"),dev->devnum);
    }
    return 0;
}


/*-------------------------------------------------------------------*/
/* Execute a Channel Command Word                                    */
/*-------------------------------------------------------------------*/
static void commadpt_execute_ccw (DEVBLK *dev, BYTE code, BYTE flags,
        BYTE chained, U16 count, BYTE prevcode, int ccwseq,
        BYTE *iobuf, BYTE *more, BYTE *unitstat, U16 *residual)
{
U32 num;                        /* Work : Actual CCW transfer count                   */
BYTE    b;                      /* Input processing work variable : Current character */
BYTE    setux;                  /* EOT kludge */
BYTE    turnxpar;               /* Write contains turn to transparent mode */
int     i;                      /* work */
BYTE    gotdle;                 /* Write routine DLE marker */
    UNREFERENCED(flags);
    UNREFERENCED(chained);
    UNREFERENCED(prevcode);
    UNREFERENCED(ccwseq);
    *residual = 0;
    /*
     * Obtain the COMMADPT lock
     */
    if(dev->ccwtrace)
    {
        logmsg(_("HHCCA300D %4.4X:CCW Exec - Entry code = %x\n"),dev->devnum,code);
    }
    obtain_lock(&dev->commadpt->lock);
    switch (code) {
        /*---------------------------------------------------------------*/
        /* CONTROL NO-OP                                                 */
        /*---------------------------------------------------------------*/
        case 0x03:
                *residual=0;
                *unitstat=CSW_CE|CSW_DE;
                break;

        /*---------------------------------------------------------------*/
        /* BASIC SENSE                                                   */
        /*---------------------------------------------------------------*/
        case 0x04:
                num=count<dev->numsense?count:dev->numsense;
                *more=count<dev->numsense?1:0;
                memcpy(iobuf,dev->sense,num);
                *residual=count-num;
                *unitstat=CSW_CE|CSW_DE;
                break;

        /*---------------------------------------------------------------*/
        /* SENSE ID                                                      */
        /*---------------------------------------------------------------*/
        case 0xE4:
                /* Calculate residual byte count */
                num = (count < dev->numdevid) ? count : dev->numdevid;
                *residual = count - num;
                *more = count < dev->numdevid ? 1 : 0;

                /* Copy device identifier bytes to channel I/O Buffer */
                memcpy (iobuf, dev->devid, num);

                /* Return unit status */
                *unitstat = CSW_CE | CSW_DE;
                break;

        /*---------------------------------------------------------------*/
        /* ENABLE                                                        */
        /*---------------------------------------------------------------*/
        case 0x27:
                if(dev->commadpt->dialin+dev->commadpt->dialout*2==2)
                {
                    /* Enable makes no sense on a dial out only line */
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_IR;
                    dev->sense[1]=0x2E; /* Simulate Failed Call In */
                    break;
                }
                if(dev->commadpt->connect)
                {
                    /* Already connected */
                    dev->commadpt->enabled=1;
                    *unitstat=CSW_CE|CSW_DE;
                    break;
                }
                dev->commadpt->curpending=COMMADPT_PEND_ENABLE;
                commadpt_wakeup(dev->commadpt,0);
                commadpt_wait(dev);
                /* If the line is not connected now, then ENABLE failed */
                if(dev->commadpt->connect)
                {
                    *unitstat=CSW_CE|CSW_DE;
                    dev->commadpt->enabled=1;
                    /* Clean the input buffer */
                    commadpt_ring_flush(&dev->commadpt->inbfr);
                    break;
                }
                if(dev->commadpt->haltpending)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UX;
                    dev->commadpt->haltpending=0;
                    break;
                }
                if(dev->commadpt->dialin)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_IR;
                    dev->sense[1]=0x2e;
                }
                else
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_IR;
                    dev->sense[1]=0x21;
                }
                break;

        /*---------------------------------------------------------------*/
        /* DISABLE                                                       */
        /*---------------------------------------------------------------*/
        case 0x2F:
                /* Reset some flags */
                dev->commadpt->xparwwait=0;
                commadpt_ring_flush(&dev->commadpt->inbfr);      /* Flush buffers */
                commadpt_ring_flush(&dev->commadpt->outbfr);      /* Flush buffers */

                if((!dev->commadpt->dialin && !dev->commadpt->dialout) || !dev->commadpt->connect)
                {
                    *unitstat=CSW_CE|CSW_DE;
                    dev->commadpt->enabled=0;
                    break;
                }
                dev->commadpt->curpending=COMMADPT_PEND_DISABLE;
                commadpt_wakeup(dev->commadpt,0);
                commadpt_wait(dev);
                dev->commadpt->enabled=0;
                *unitstat=CSW_CE|CSW_DE;
                break;
        /*---------------------------------------------------------------*/
        /* SET MODE                                                      */
        /*---------------------------------------------------------------*/
        case 0x23:
                /* Transparent Write Wait State test */
                if(dev->commadpt->xparwwait)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_CR;
                    return;
                }
                num=1;
                *residual=count-num;
                *unitstat=CSW_CE|CSW_DE;
                if(dev->ccwtrace)
                {
                        logmsg(_("HHCCA300D %4.4X Set Mode : %s\n"),dev->devnum,iobuf[0]&0x40 ? "EIB":"NO EIB");
                }
                dev->commadpt->eibmode=(iobuf[0]&0x40)?1:0;
                break;
        /*---------------------------------------------------------------*/
        /* POLL Command                                                  */
        /*---------------------------------------------------------------*/
        case 0x09:
                /* Transparent Write Wait State test */
                if(dev->commadpt->xparwwait)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_CR;
                    return;
                }
                /* Save POLL data */
                commadpt_ring_flush(&dev->commadpt->pollbfr);
                commadpt_ring_pushbfr(&dev->commadpt->pollbfr,iobuf,count);
                /* Set some utility variables */
                dev->commadpt->pollused=0;
                dev->commadpt->badpoll=0;
                /* Tell thread */
                dev->commadpt->curpending=COMMADPT_PEND_POLL;
                commadpt_wakeup(dev->commadpt,0);
                commadpt_wait(dev);
                /* Flush the output & poll rings */
                commadpt_ring_flush(&dev->commadpt->outbfr);
                commadpt_ring_flush(&dev->commadpt->pollbfr);
                /* Check for HALT */
                if(dev->commadpt->haltpending)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UX;
                    dev->commadpt->haltpending=0;
                    break;
                }
                /* Check for bad poll data */
                if(dev->commadpt->badpoll)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=0x08;
                    dev->sense[1]=0x84;
                    break;
                }
                /* Determine remaining length */
                *residual=count-dev->commadpt->pollused;
                /* Determine if SM should be set (succesfull or unsucessfull POLLs) */
                /* exhausting poll data when all stations reported NO data          */
                /* does not set Status Modifier                                     */
                *unitstat=CSW_CE|CSW_DE|(dev->commadpt->pollsm?CSW_SM:0);
                /* NOTE : The index byte (and rest) are in the Input Ring */
                break;

        /*---------------------------------------------------------------*/
        /* DIAL                                                          */
        /* Info on DIAL DATA :                                           */
        /* Dial character formats :                                      */
        /*                        x x x x 0 0 0 0 : 0                    */
        /*                            ........                           */
        /*                        x x x x 1 0 0 1 : 9                    */
        /*                        x x x x 1 1 0 0 : SEP                  */
        /*                        x x x x 1 1 0 1 : EON                  */
        /* EON is ignored                                                */
        /* format is : AAA/SEP/BBB/SEP/CCC/SEP/DDD/SEP/PPPP              */
        /*          where A,B,C,D,P are numbers from 0 to 9              */
        /* This perfoms an outgoing call to AAA.BBB.CCC.DDD port PPPP    */
        /*---------------------------------------------------------------*/
        case 0x29:
                /* The line must have dial-out capability */
                if(!dev->commadpt->dialout)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_CR;
                    dev->sense[1]=0x04;
                    break;
                }
                /* The line must be disabled */
                if(dev->commadpt->enabled)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_CR;
                    dev->sense[1]=0x05;
                    break;
                }
                num=count>sizeof(dev->commadpt->dialdata) ? sizeof(dev->commadpt->dialdata) : count;
                memcpy(dev->commadpt->dialdata,iobuf,num);
                dev->commadpt->curpending=COMMADPT_PEND_DIAL;
                commadpt_wakeup(dev->commadpt,0);
                commadpt_wait(dev);
                *residual=count-num;
                if(dev->commadpt->haltpending)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UX;
                    dev->commadpt->haltpending=0;
                    break;
                }
                if(!dev->commadpt->connect)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_IR;
                    dev->commadpt->enabled=0;
                }
                else
                {
                    *unitstat=CSW_CE|CSW_DE;
                    dev->commadpt->enabled=1;
                }
                break;

        /*---------------------------------------------------------------*/
        /* READ                                                          */
        /*---------------------------------------------------------------*/
        case 0x02:
                setux=0;
                /* Check the line is enabled */
                if(!dev->commadpt->enabled)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_CR;
                    dev->sense[1]=0x06;
                    break;
                }
                /* Transparent Write Wait State test */
                if(dev->commadpt->xparwwait)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_CR;
                    break;
                }
                /* Check for any remaining data in read work buffer */
                if(dev->commadpt->readcomp)
                {
                    if(dev->commadpt->rdwrk.havedata)
                    {
                        num=commadpt_ring_popbfr(&dev->commadpt->rdwrk,iobuf,count);
                        if(dev->commadpt->rdwrk.havedata)
                        {
                            *more=1;
                        }
                        *residual=count-num;
                        *unitstat=CSW_CE|CSW_DE;
                        break;
                    }
                }
                if(dev->commadpt->datalostcond)
                {
                        dev->commadpt->datalostcond=0;
                        commadpt_ring_flush(&dev->commadpt->inbfr);
                        *residual=count;
                        *unitstat=CSW_CE|CSW_DE;
                        break;
                }
                dev->commadpt->readcomp=0;
                *unitstat=0;
                num=0;
                /* The following is the BIG READ ROUTINE MESS */
                /* the manual's indications on when to exit   */
                /* a read and what to transfer to the main    */
                /* storage is fuzzy (at best)                 */
                /*                                            */
                /* The line input can be in 3 possible        */
                /* conditions :                               */
                /*     Transparent Text Mode                  */
                /*     Text Mode                              */
                /*     none of the above (initial status)     */
                /* transition from one mode to the other is   */
                /* also not very well documented              */
                /* so the following code is based on          */
                /* empirical knowledge and some interpretation*/
                /* also... the logic should probably be       */
                /* rewritten                                  */

                /* We will remain in READ state with the thread */
                /* as long as we haven't met a read ending condition */
                while(1)
                {
                    /* READ state */
                    dev->commadpt->curpending=COMMADPT_PEND_READ;
                    /* Tell worker thread */
                    commadpt_wakeup(dev->commadpt,0);
                    /* Wait for some data */
                    commadpt_wait(dev);

                    /* If we are not connected, the read fails */
                    if(!dev->commadpt->connect)
                    {
                        *unitstat=CSW_DE|CSW_CE|CSW_UC;
                        dev->sense[0]=SENSE_IR;
                        break;
                    }

                    /* If the I/O was halted - indicate Unit Check */
                    if(dev->commadpt->haltpending)
                    {
                        *unitstat=CSW_CE|CSW_DE|CSW_UX;
                        dev->commadpt->haltpending=0;
                        break;
                    }

                    /* If no data is present - 3 seconds have passed without */
                    /* receiving data (or a SYNC)                            */
                    if(!dev->commadpt->inbfr.havedata)
                    {
                        *unitstat=CSW_DE|CSW_CE|CSW_UC;
                        dev->sense[0]=0x01;
                        dev->sense[1]=0xe3;
                        break;
                    }
                    /* Start processing data flow here */
                    /* Pop bytes until we run out of data or */
                    /* until the processing indicates the read */
                    /* should now terminate */
                    while(
                            dev->commadpt->inbfr.havedata
                            && !dev->commadpt->readcomp)
                    {
                        /* fetch 1 byte from the input ring */
                        b=commadpt_ring_pop(&dev->commadpt->inbfr);
                        if(!dev->commadpt->gotdle)
                        {
                            if(b==0x10)
                            {
                                dev->commadpt->gotdle=1;
                                continue;
                            }
                        }
                        if(dev->commadpt->in_textmode)
                        {
                            if(dev->commadpt->in_xparmode)
                            {
                                        /* TRANSPARENT MODE READ */
                                if(dev->commadpt->gotdle)
                                {
                                    switch(b)
                                    {
                                        case 0x10:
                                            commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                            break;
                                        case 0x32:
                                            break;
                                        case 0x1F: /* ITB - Exit xparent, set EIB - do NOT exit read yet */
                                            dev->commadpt->in_xparmode=0;
                                            commadpt_ring_push(&dev->commadpt->rdwrk,0x10);
                                            commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                            if(dev->commadpt->eibmode)
                                            {
                                                commadpt_ring_push(&dev->commadpt->rdwrk,0);
                                            }
                                            break;
                                        case 0x26: /* ETB - Same as ITB but DO exit read now */
                                            dev->commadpt->in_xparmode=0;
                                            commadpt_ring_push(&dev->commadpt->rdwrk,0x10);
                                            commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                            if(dev->commadpt->eibmode)
                                            {
                                                commadpt_ring_push(&dev->commadpt->rdwrk,0);
                                            }
                                            dev->commadpt->readcomp=1;
                                            break;
                                        case 0x03: /* ETX - Same as ETB */
                                            dev->commadpt->in_xparmode=0;
                                            commadpt_ring_push(&dev->commadpt->rdwrk,0x10);
                                            commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                            if(dev->commadpt->eibmode)
                                            {
                                                commadpt_ring_push(&dev->commadpt->rdwrk,0);
                                            }
                                            dev->commadpt->readcomp=1;
                                            break;
                                        case 0x2D: /* ENQ */
                                            dev->commadpt->in_xparmode=0;
                                            dev->commadpt->in_textmode=0;
                                            commadpt_ring_push(&dev->commadpt->rdwrk,0x10);
                                            commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                            dev->commadpt->readcomp=1;
                                            break;
                                        default:
                                            commadpt_ring_push(&dev->commadpt->rdwrk,0x10);
                                            commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                            break;
                                    }
                                }
                                else
                                {
                                    commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                }
                            }
                            else
                            {
                                if(b!=0x32)
                                {
                                     /* TEXT MODE READ */
                                    if(dev->commadpt->gotdle)
                                    {
                                        switch(b)
                                        {
                                            case 0x02: /* STX */
                                            dev->commadpt->in_xparmode=1;
                                            break;
                                            case 0x2D: /* ENQ */
                                            dev->commadpt->readcomp=1;
                                            break;
                                            default:
                                                if((b&0xf0)==0x60 || (b&0xf0)==0x70)
                                                {
                                                    dev->commadpt->readcomp=1;
                                                }
                                                break;
                                        }
                                        commadpt_ring_push(&dev->commadpt->rdwrk,0x10);
                                        commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                    }
                                    else
                                    {
                                        switch(b)
                                        {
                                            case 0x2D:      /* ENQ */
                                                dev->commadpt->readcomp=1;
                                                dev->commadpt->in_textmode=0;
                                                commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                                break;
                                            case 0x3D:      /* NAK */
                                                dev->commadpt->readcomp=1;
                                                commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                                break;
                                            case 0x26:      /* ETB */
                                            case 0x03:      /* ETX */
                                                dev->commadpt->readcomp=1;
                                                dev->commadpt->in_textmode=0;
                                                commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                                if(dev->commadpt->eibmode)
                                                {
                                                    commadpt_ring_push(&dev->commadpt->rdwrk,0);
                                                }
                                                break;
                                            case 0x1F:      /* ITB */
                                                commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                                if(dev->commadpt->eibmode)
                                                {
                                                    commadpt_ring_push(&dev->commadpt->rdwrk,0);
                                                }
                                                break;
                                            default:
                                                commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                                break;
                                        }
                                    }
                                }
                            }
                        }
                        else
                        {
                            if(b!=0x32)
                            {
                                if(dev->commadpt->gotdle)
                                {
                                    if((b & 0xf0) == 0x60 || (b&0xf0)==0x70)
                                    {
                                        commadpt_ring_push(&dev->commadpt->rdwrk,0x10);
                                        commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                        dev->commadpt->readcomp=1;
                                    }
                                    else
                                    {
                                        if(b==0x02)
                                        {
                                            commadpt_ring_push(&dev->commadpt->rdwrk,0x10);
                                            commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                            dev->commadpt->in_textmode=1;
                                            dev->commadpt->in_xparmode=1;
                                        }
                                    }
                                }
                                else
                                {
                                    switch(b)
                                    {
                                        case 0x37:  /* EOT */
                                            setux=1;
                                            dev->commadpt->readcomp=1;
                                            break;
                                        case 0x01:
                                        case 0x02:
                                            dev->commadpt->in_textmode=1;
                                            break;
                                        case 0x2D: /* ENQ */
                                            dev->commadpt->readcomp=1;
                                            break;
                                        case 0x3D: /* NAK */
                                            dev->commadpt->readcomp=1;
                                            break;
                                        default:
                                            break;
                                    }
                                    commadpt_ring_push(&dev->commadpt->rdwrk,b);
                                }
                            }
                        }
                        dev->commadpt->gotdle=0;
                    } /* END WHILE - READ FROM DATA BUFFER */
                    /* If readcomp is set, then we may exit the read loop */
                    if(dev->commadpt->readcomp)
                    {
                        if(dev->commadpt->rdwrk.havedata)
                        {
                            num=commadpt_ring_popbfr(&dev->commadpt->rdwrk,iobuf,count);
                            if(dev->commadpt->rdwrk.havedata)
                            {
                                *more=1;
                            }
                            *residual=count-num;
                            *unitstat=CSW_CE|CSW_DE|(setux?CSW_UX:0);
                            logdump("Read",dev,iobuf,num);
                            break;
                        }
                    }
                } /* END WHILE - READ FROM THREAD */
                break;

        /*---------------------------------------------------------------*/
        /* WRITE                                                         */
        /*---------------------------------------------------------------*/
        case 0x01:
                logdump("Writ",dev,iobuf,count);
                *residual=count;

                /* Check if we have an opened path */
                if(!dev->commadpt->connect)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_IR;
                    break;
                }

                /* Check if the line has been enabled */
                if(!dev->commadpt->enabled)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_CR;
                    break;
                }

                /* read 1 byte to check for pending input */
                i=read_socket(dev->commadpt->sfd,&b,1);
                if(i>0)
                {
                    /* Push it in the communication input buffer ring */
                    commadpt_ring_push(&dev->commadpt->inbfr,b);
                }
                /* Set UX on write if line has pending inbound data */
                if(dev->commadpt->inbfr.havedata)
                {
                    dev->commadpt->datalostcond=1;
                    *unitstat=CSW_CE|CSW_DE|CSW_UX;
                    break;
                }
                /*
                 * Fill in the Write Buffer
                 */

                /* To start : not transparent mode, no DLE received yet */
                turnxpar=0;
                gotdle=0;

                /* Scan the I/O buffer */
                for(i=0;i<count;i++)
                {
                    /* Get 1 byte */
                    b=iobuf[i];

                    /* If we are in transparent mode, we must double the DLEs */
                    if(turnxpar)
                    {
                        /* Check for a DLE */
                        if(b==0x10)
                        {
                                /* put another one in the output buffer */
                                commadpt_ring_push(&dev->commadpt->outbfr,0x10);
                        }
                    }
                    else        /* non transparent mode */
                    {
                        if(b==0x10)
                        {
                            gotdle=1;   /* Indicate we have a DLE for next pass */
                        }
                        else
                        {
                            /* If there was a DLE on previous pass */
                            if(gotdle)
                            {
                                /* check for DLE/ETX */
                                if(b==0x02)
                                {
                                    /* Indicate transparent mode on */
                                    turnxpar=1;
                                }
                            }
                        }
                    }
                    /* Put the current byte on the output ring */
                    commadpt_ring_push(&dev->commadpt->outbfr,b);
                }
                /* If we had a DLE/STX, the line is now in Transparent Write Wait state */
                /* meaning that no CCW codes except Write, No-Op, Sense are allowed     */
                /* (that's what the manual says.. I doubt DISABLE is disallowed)        */
                /* Anyway.. The program will have an opportunity to turn XPARENT mode   */
                /* off on the next CCW.                                                 */
                /* CAVEAT : The manual doesn't say if the line remains in transparent   */
                /*          Write Wait state if the next CCW doesn't start with DLE/ETX */
                /*          or DLE/ITB                                                  */
                if(turnxpar)
                {
                    dev->commadpt->xparwwait=1;
                }
                else
                {
                    dev->commadpt->xparwwait=0;
                }
                /* Indicate to the worker thread the current operation is OUTPUT */
                dev->commadpt->curpending=COMMADPT_PEND_WRITE;

                /* All bytes written out - residual = 0 */
                *residual=0;

                /* Wake-up the worker thread */
                commadpt_wakeup(dev->commadpt,0);

                /* Wait for operation completion */
                commadpt_wait(dev);

                /* Check if the line is still connected */
                if(!dev->commadpt->connect)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_IR;
                    break;
                }

                /* Check if the I/O was interrupted */
                if(dev->commadpt->haltpending)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UX;
                    break;
                }
                *unitstat=CSW_CE|CSW_DE;
                break;

        /*---------------------------------------------------------------*/
        /* PREPARE                                                       */
        /* NOTE : DO NOT SET RESIDUAL to 0 : Otherwise, channel.c        */
        /*        will reflect a channel prot check - residual           */
        /*        should indicate NO data was transfered for this        */
        /*        pseudo-read operation                                  */
        /*---------------------------------------------------------------*/
        case 0x06:
                *residual=count;
                /* PREPARE not allowed unless line is enabled */
                if(!dev->commadpt->enabled)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_CR;
                    dev->sense[1]=0x06;
                    break;
                }

                /* Transparent Write Wait State test */
                if(dev->commadpt->xparwwait)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_CR;
                    return;
                }

                /* If data is present, prepare ends immediatly */
                if(dev->commadpt->inbfr.havedata)
                {
                    *unitstat=CSW_CE|CSW_DE;
                    break;
                }

                /* Indicate to the worker thread to notify us when data arrives */
                dev->commadpt->curpending=COMMADPT_PEND_PREPARE;

                /* Wakeup worker thread */
                commadpt_wakeup(dev->commadpt,0);

                /* Wait for completion */
                commadpt_wait(dev);

                /* If I/O was halted (this one happens often) */
                if(dev->commadpt->haltpending)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UX;
                    dev->commadpt->haltpending=0;
                    break;
                }

                /* Check if the line is still connected */
                if(!dev->commadpt->connect)
                {
                    *unitstat=CSW_CE|CSW_DE|CSW_UC;
                    dev->sense[0]=SENSE_IR;
                    break;
                }

                /* Normal Prepare exit condition - data is present in the input buffer */
                *unitstat=CSW_CE|CSW_DE;
                break;

        default:
        /*---------------------------------------------------------------*/
        /* INVALID OPERATION                                             */
        /*---------------------------------------------------------------*/
            /* Set command reject sense byte, and unit check status */
            *unitstat=CSW_CE+CSW_DE+CSW_UC;
            dev->sense[0]=SENSE_CR;
            break;

    }
    release_lock(&dev->commadpt->lock);
}


/*---------------------------------------------------------------*/
/* DEVICE FUNCTION POINTERS                                      */
/*---------------------------------------------------------------*/

#if defined(OPTION_DYNAMIC_LOAD)
static
#endif
DEVHND comadpt_device_hndinfo = {
        &commadpt_init_handler,        /* Device Initialisation      */
        &commadpt_execute_ccw,         /* Device CCW execute         */
        &commadpt_close_device,        /* Device Close               */
        &commadpt_query_device,        /* Device Query               */
        NULL,                          /* Device Start channel pgm   */
        NULL,                          /* Device End channel pgm     */
        NULL,                          /* Device Resume channel pgm  */
        NULL,                          /* Device Suspend channel pgm */
        NULL,                          /* Device Read                */
        NULL,                          /* Device Write               */
        NULL,                          /* Device Query used          */
        NULL,                          /* Device Reserve             */
        NULL,                          /* Device Release             */
        NULL,                          /* Device Attention           */
        commadpt_immed_command,        /* Immediate CCW Codes        */
        NULL,                          /* Signal Adapter Input       */
        NULL,                          /* Signal Adapter Output      */
        NULL,                          /* Hercules suspend           */
        NULL                           /* Hercules resume            */
};


/* Libtool static name colision resolution */
/* note : lt_dlopen will look for symbol & modulename_LTX_symbol */
#if !defined(HDL_BUILD_SHARED) && defined(HDL_USE_LIBTOOL)
#define hdl_ddev hdt2703_LTX_hdl_ddev
#define hdl_depc hdt2703_LTX_hdl_depc
#define hdl_reso hdt2703_LTX_hdl_reso
#define hdl_init hdt2703_LTX_hdl_init
#define hdl_fini hdt2703_LTX_hdl_fini
#endif


#if defined(OPTION_DYNAMIC_LOAD)
HDL_DEPENDENCY_SECTION;
{
     HDL_DEPENDENCY(HERCULES);
     HDL_DEPENDENCY(DEVBLK);
     HDL_DEPENDENCY(SYSBLK);
}
END_DEPENDENCY_SECTION


#if defined(WIN32) && !defined(HDL_USE_LIBTOOL) && !defined(_MSVC_)
  #undef sysblk
  HDL_RESOLVER_SECTION;
  {
    HDL_RESOLVE_PTRVAR( psysblk, sysblk );
  }
  END_RESOLVER_SECTION
#endif


HDL_DEVICE_SECTION;
{
    HDL_DEVICE(2703, comadpt_device_hndinfo );
}
END_DEVICE_SECTION
#endif
