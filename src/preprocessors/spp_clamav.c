/* $Id: ClamAV-2.3.3-1.diff,v 1.1.1.1 2005/05/06 21:19:36 jonkman Exp $ */
/* Snort Preprocessor for Antivirus Checking with ClamAV */

/*
** Copyright (C) 1998-2002 Martin Roesch <roesch@sourcefire.com>
** Copyright (C) 2003 Sourcefire, Inc.
** Copyright (C) 2004 William Metcalf <William_Metcalf@kcmo.org> and
**                    Victor Julien <victor@nk.nl>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/* spp_clamav.c
 *
 * Purpose: Sends packet p to ClamAV for Antivirus checking.
 *
 * Arguments: None
 *
 * Effect: Who needs virus.rules??? :-)
 *
 * Comments:
 *
 *
 * TODO:
 * - documentation
 * - are the defaultports in ParseClamAVArgs ok?
 * - options structure like s4data in Stream4 for cl_root, VirusScanPorts, drop/reject/alert, defs dirlocation **IN PROGRESS**
 * - maybe more protocol specific support for less false negatives?
 *
 *
 * Changes:
 *
 * 2004/11/10: added code for the automatic reloading of the virusdefs
 *             added support for ClamAV 0.80
 * 2006/02/03: added check for http traffic so we better handle http downloads
 *             removed cl_scanbuf since it was broken anyway
 *             cleanups and comments added.
 *             added 8080 to the default ports
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef DEBUG
   #ifndef INLINE
       #define INLINE inline
   #endif
#else
   #ifdef INLINE
       #undef INLINE
   #endif
   #define INLINE
#endif /* DEBUG */

#include <sys/types.h>
#include <stdlib.h>
#include <ctype.h>
#include <rpc/types.h>
#include <errno.h>
#include "generators.h"
#include "event_wrapper.h"
#include "util.h"
#include "plugbase.h"
#include "parser.h"
#include "decode.h"
#include "debug.h"
#include "mstring.h"
#include "log.h"
#include "spp_clamav.h"
#include "stream_api.h"
#ifdef GIDS
#include "inline.h"
#endif

#include "snort.h"
#include <clamav.h>

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

/* we have cl_build() */
#define CLAMAV_HAVE_CL_BUILD 1

/* we need this to stringify the CLAMAV_DEFDIR which is supplied at compiletime see:
  http://gcc.gnu.org/onlinedocs/gcc-3.4.1/cpp/Stringification.html#Stringification */
#define xstr(s) str(s)
#define str(s) #s

/* the config struct */
struct ClamAVConfig
{
   /* scan limitations */
   char toclientonly; /* if set to 1 scan only traffic to the client */
   char toserveronly; /* if set to 1 scan only traffic to the server */
   char VirusScanPorts[65536/8]; /* array containing info about which ports we care about */

   /* actions */
   char drop;
   char reset;
   char rboth;

   /* virdef dir */
   char dbdir[255];

   /* temp dir for file descriptors */
   char desctmpdir[255];

   /* reload time in seconds */
   u_int16_t reloadtime;
   u_int32_t next_reload_time;

    /* what to do when clam returns an error
     * 0: nothing, just alert 
     * 1: alert and block */
    char block_failed_scans;

} clamcnf;

/* pointer to ClamAV's in-memory virusdatabase */
struct cl_node *cl_root;
/* scanner limits */
struct cl_limits clam_limits;
static void ClamAVInit(char *);
extern void SetupClamAV();
static int VirusInPacket(Packet *);
int check_4_http_headers(u_int8_t *, int);
static void VirusChecker(Packet *, void *);
extern u_int32_t event_id;

/* db reloading */
struct cl_stat dbstat;


/*
 * Function: SetupClamAV()
 *
 * Purpose: Registers the preprocessor.
 *
 * Arguments: None.
 *
 * Returns: void function
 *
 */
void SetupClamAV()
{
   RegisterPreprocessor("ClamAV", ClamAVInit);
}


/*
 * Function: ProcessPorts(u_char *)
 *
 * Purpose: Sets the port limits
 *
 * Arguments: pointer to string with portlist.
 *
 * Returns: void function
 *
 */
static void ProcessPorts(u_char *portlist)
{
   int j = 0;
   int i = 0;
   char **ports;
   int num_ports;
   char *port;
   u_int32_t portnum;

   /* reset the ports array */
   bzero(&clamcnf.VirusScanPorts, sizeof(clamcnf.VirusScanPorts));

   ports = mSplit(portlist, " ", 40, &num_ports, 0);

   /* run through the ports */
   for(j = 0; j < num_ports; j++)
   {
       port = ports[j];

       /* we need to set this port */
       if(isdigit((int)port[0]))
       {
           portnum = atoi(port);
           if(portnum > 65535)
           {
               FatalError("%s(%d) => Bad port list to scan: "
                   "port '%d' out of range\n", portnum, file_name, file_line);
           }

           /* mark this port as being interesting using some portscan2-type voodoo,
              and also add it to the port list string while we're at it so we can
              later print out all the ports with a single LogMessage() */
           clamcnf.VirusScanPorts[(portnum/8)] |= 1<<(portnum%8);
       }
       /* we need to unset this port */
       else if(port[0] == '!')
       {
           for(i = 0; i < strlen(port) && port[i+1] != '\0'; i++)
           {
               port[i] = port[i+1];
           }
           port[i] = '\0';

           if(isdigit((int)port[0]))
           {
               portnum = atoi(port);
               if(portnum > 65535)
               {
                   FatalError("%s(%d) => Bad port list to scan: "
                       "port '%d' out of range\n", portnum, file_name, file_line);
               }

               /* clear the bit - this removes the port from the array */
               clamcnf.VirusScanPorts[(portnum/8)] &= ~(1<<(portnum%8));
           }
           else
           {
               FatalError("%s(%d) => Bad port list to scan: "
                          "bad port\n", file_name, file_line);
           }
       }
       /* we need to set all ports */
       else if(!strncasecmp(port, "all", 3))
       {
           /* enable all ports */
           for(portnum = 0; portnum <= 65535; portnum++)
               clamcnf.VirusScanPorts[(portnum/8)] |= 1<<(portnum%8);
       }
       else if(!strncasecmp(port, "ports", 5));
       else
       {
           FatalError("%s(%d) => Bad port list to scan: "
                      "bad port\n", file_name, file_line);
       }
   }

   mSplitFree(&ports, num_ports);

   /* some pretty printing */
   if(!pv.quiet_flag)
   {
       /* print the portlist */
       LogMessage("    Ports: ");

       for(portnum = 0, j = 0; portnum <= 65535; portnum++)
       {
           if((clamcnf.VirusScanPorts[(portnum/8)] & (1<<(portnum%8))))
           {
               LogMessage("%d ", portnum);
               j++;
           }

           if(j > 20)
           {
               LogMessage("...\n");
               return;
           }
       }
   }
}


 /*
 * Function: ParseClamAVArgs(u_char *)
 *
 * Purpose: reads the options and sets the defaults.
 *
 * Arguments: pointer to string with options
 *
 * Returns: void function
 */
void ParseClamAVArgs(u_char *args)
{
   char **toks;
   int num_toks;
   int i = 0;
   char *index;
   int ports_done = 0;
   char **dbdirtoks;
   int num_dbdirtoks = 0;
   char **dbtimetoks;
   int num_dbtimetoks = 0;
   char **desctmptoks;
   int num_desctmptoks = 0;


   /* ftp, smtp, http, pop3, nntp, samba (2x), imap */
   u_char *default_ports = "21 25 80 81 110 119 139 445 143 8080";

#ifdef GIDS
   clamcnf.drop = 0;
   clamcnf.reset = 0;
   clamcnf.rboth = 0;
   clamcnf.block_failed_scans = 0; /* default we pass failed scans */
#endif /* GIDS */
   clamcnf.toclientonly = 0;
   clamcnf.toserveronly = 0;

   /* default tmp dir */
   if(strlcpy(clamcnf.desctmpdir, "/tmp", sizeof(clamcnf.desctmpdir)) >= sizeof(clamcnf.desctmpdir))
   {
       FatalError("The tempdir supplied at compile time is too long\n");
   }

#ifdef CLAMAV_DEFDIR
   /* copy the default that was set at compile time, if any */
   if(strlcpy(clamcnf.dbdir, xstr(CLAMAV_DEFDIR), sizeof(clamcnf.dbdir)) >= sizeof(clamcnf.dbdir))
#else
   /* otherwise a buildin default */
   if(strlcpy(clamcnf.dbdir, "/var/lib/clamav/", sizeof(clamcnf.dbdir)) >= sizeof(clamcnf.dbdir))
#endif
   {
       FatalError("The defdir supplied at compile time is too long\n");
   }


   /* reload time default to 10 minutes */
   clamcnf.reloadtime = 600;


   if(!pv.quiet_flag)
   {
       LogMessage("ClamAV config:\n");
   }


   /* if no args, load the default config */
   if(args == NULL)
   {
       if(!pv.quiet_flag)
       {
           LogMessage("    no options, using defaults.\n");
       }
   }
   /* process the args */
   else
   {
       toks = mSplit(args, ",", 12, &num_toks, 0);

       for(i = 0; i < num_toks; i++)
       {
           index = toks[i];
           while(isspace((int)*index)) index++;

           if(!strncasecmp(index, "ports", 5))
           {
               ProcessPorts(toks[i]);
               ports_done = 1;
           }
#ifdef GIDS
           else if(!strncasecmp(index, "action-reset", 12))
           {
              clamcnf.reset = 1;
           }
           else if(!strncasecmp(index, "action-rboth", 12))
           {
              clamcnf.rboth = 1;
           }
           else if(!strncasecmp(index, "action-drop", 11))
           {
              clamcnf.drop = 1;
           }
           else if(!strncasecmp(index, "block-failed-scan", 17))
           {
              clamcnf.block_failed_scans = 1;
           }
#endif /* GIDS */
           else if(!strncasecmp(index, "toclientonly", 12))
           {
              clamcnf.toclientonly = 1;
           }
           else if(!strncasecmp(index, "toserveronly", 12))
           {
              clamcnf.toserveronly = 1;
           }
           else if(!strncasecmp(index, "dbdir", 5))
           {
               /* get the argument for the option */
               dbdirtoks = mSplit(index, " ", 1, &num_dbdirtoks, 0);

               /* copy it to the clamcnf */
               if(strlcpy(clamcnf.dbdir, dbdirtoks[1], sizeof(clamcnf.dbdir)) >= sizeof(clamcnf.dbdir))
               {
                   FatalError("The defdir supplied in the config is too long\n");
               }
             mSplitFree(&dbdirtoks, num_dbdirtoks);
           }
           else if(!strncasecmp(index, "dbreload-time", 13))
           {
               /* get the argument for the option */
               dbtimetoks = mSplit(index, " ", 1, &num_dbtimetoks, 0);

               if(isdigit((int)dbtimetoks[1][0]))
               {
                   clamcnf.reloadtime  = atoi(dbtimetoks[1]);
               }
               else
               {
                   FatalError("We need an integer in seconds for dbreload interval\n");
               }
             mSplitFree(&dbtimetoks, num_dbtimetoks);
           }
           else if((!strncasecmp(index, "descriptor-temp-dir", 19)))
           {
               /* get the argument for the option */
               desctmptoks = mSplit(index, " ", 1, &num_desctmptoks, 0);

               /* copy it to the clamcnf */
               if(strlcpy(clamcnf.desctmpdir, desctmptoks[1], sizeof(clamcnf.desctmpdir)) >= sizeof(clamcnf.desctmpdir))
               {
                   FatalError("The tmpdir supplied in the config is too long\n");
               }
             mSplitFree(&desctmptoks, num_desctmptoks);
           }
           else
           {
               FatalError("%s(%d) => Bad ClamAV option specified: "
                          "\"%s\"\n", file_name, file_line, toks[i]);
           }
       }

       mSplitFree(&toks, num_toks);
   }

#ifdef GIDS
   /* sanety checks */
   if(clamcnf.drop && clamcnf.reset)
   {
       FatalError("Can't set action-drop and action-reset together!\n");
   }
#endif /* GIDS */
   if(clamcnf.toclientonly && clamcnf.toserveronly)
   {
       FatalError("Can't set toclientonly and toserveronly together!\n");
   }


   /* if at this stage the ports are not yet done, load the default ports */
   if(!ports_done)
       ProcessPorts(default_ports);


   /* some pretty printing */
   if(!pv.quiet_flag)
   {
       /* action */
#ifdef GIDS
       if(clamcnf.drop == 1)
           LogMessage("    Virus found action: DROP\n");
       else if(clamcnf.reset == 1)
           LogMessage("    Virus found action: RESET\n");
       else if(clamcnf.rboth == 1)
           LogMessage("    Virus found action: RESET-BOTH\n");
       else
           LogMessage("    Virus found action: ALERT\n");

       if(clamcnf.block_failed_scans == 1)
           LogMessage("    Scanning Failed action: BLOCK (uses Virus Found action)\n");
       else
           LogMessage("    Scanning Failed action: ALERT\n");
#endif /* GIDS */
       /* dbdir */
       LogMessage("    Virus definitions dir: '%s'\n", clamcnf.dbdir);
       /* limits */
       LogMessage("    Virus DB reload time: '%i'\n", clamcnf.reloadtime);
       if(clamcnf.toclientonly == 1)
           LogMessage("    Scan only traffic to the client\n");
       else if(clamcnf.toserveronly == 1)
           LogMessage("    Scan only traffic to the server\n");
       LogMessage("    Directory for tempfiles (file descriptor mode): '%s'\n",
                           clamcnf.desctmpdir);
   }
}


/*
 * Function: ClamAVInit(u_char *)
 *
 * All it does right now is register the plugin, eventually we will take port args
 */
void ClamAVInit(char *args)
{
   int ret = 0;
   int n = 0;
   struct timeval t;
   cl_root = NULL;

   /* first parse the commandline args */
   ParseClamAVArgs(args);


   /* get the current time for the reloading */
   memset(&t, 0, sizeof(struct timeval));
   gettimeofday(&t, NULL);
   clamcnf.next_reload_time = t.tv_sec + clamcnf.reloadtime;

   /* setup for check */
   memset(&dbstat, 0, sizeof(struct cl_stat));
   cl_statinidir(clamcnf.dbdir, &dbstat);

   /* open the defs dir */
   ret = cl_load(clamcnf.dbdir, &cl_root, &n, CL_DB_STDOPT);
   if(ret != 0)
   {
       FatalError("ClamAV: cl_load() %s\n", cl_strerror(ret));
   }

   /* run buildtrie */
#ifdef CLAMAV_HAVE_CL_BUILD
   if((ret = cl_build(cl_root)))
#else
   if((ret = cl_buildtrie(cl_root)))
#endif /* CLAMAV_HAVE_CL_BUILD */
   {
       FatalError("ClamAV: cl_build() %s\n", cl_strerror(ret));
   }

   /* set the limits */
   memset(&clam_limits, 0, sizeof(clam_limits));
   /* maximal global size */
   clam_limits.maxscansize = 20 * 1048576; /* 20 MB */
   /* maximal archived file size */
   clam_limits.maxfilesize = 10 * 1048576; /* 10 MB */
   /* maximal recursion level */
   clam_limits.maxreclevel = 5;
   /* maximal number of files in archive */;
   clam_limits.maxfiles = 1000;
   /* disable memory limit for bzip2 scanner */
   clam_limits.archivememlim = 0;

   /* register the VirusChecker */
   AddFuncToPreprocList(VirusChecker, PRIORITY_SCANNER, PP_CLAMAV);
}


/*
 * Function:  ClamAVReloadDB(void)
 *
 * Purpose:   Determines if the virus definition files need
 *            to be reloaded. If so, reload them.
 *
 * Arguments: pointer to Packet *p, for the time only
 *
 * Returns:   none, void function
 *
 */
static void ClamAVReloadDB(Packet *p)
{
   int ret = 0;
   int n = 0;

   DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"going to Reload ClamAV Database\n"););
   clamcnf.next_reload_time = p->pkth->ts.tv_sec + clamcnf.reloadtime;

   /* now really check */
   if(cl_statchkdir(&dbstat) == 1)
   {
       /* okay, we are going to reload the db */
#ifdef CLAMAV_HAVE_CL_BUILD
       cl_free(cl_root);
#else
       cl_freetrie(cl_root);
#endif /* CLAMAV_HAVE_CL_BUILD */
       cl_root = NULL;

       /* open the defs dir */
       ret = cl_load(clamcnf.dbdir, &cl_root, &n, CL_DB_STDOPT);
       if(ret != 0)
       {
           FatalError("ClamAV: cl_load() %s\n", cl_strerror(ret));
       }

       /* run buildtrie */
#ifdef CLAMAV_HAVE_CL_BUILD
       if((ret = cl_build(cl_root)))
#else
       if((ret = cl_buildtrie(cl_root)))
#endif /* CLAMAV_HAVE_CL_BUILD */
       {
           FatalError("ClamAV: cl_build() %s\n", cl_strerror(ret));
       }

       DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"New ClamAV Database loaded\n"););

       /* re-init the dbstat */
       cl_statfree(&dbstat);

       memset(&dbstat, 0, sizeof(struct cl_stat));
       cl_statinidir(clamcnf.dbdir, &dbstat);
   }
   else
   {
       DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"Database is up2date\n"););
   }

   return;
}


/*
 * Function:  check_for_http(Packet *p, u_int8_t **data, int *dsize)
 *
 * Purpose:   Determines if the packet is a HTTP response. We are interested
 *            in those because we want to cut off the header from the
 *            response, so ClamAV has less trouble detecting virusses.
 *
 *            It doesn't actually modify any buffer, it only returns (through
 *            the **data argument) the start of the buffer after the header.
 *            *dsize is the size of the buffer without the header.
 *
 *            The check for HTTP should be very cheap, so we do it on all
 *            traffic. Later we can change this if there is a need to.
 *
 * Arguments: pointer to the packet, pointer to a data pointer, pointer to
 *            any int.
 *
 * Returns:   nothing, void function
 */
int
strip_http_headers_p(Packet *p, u_int8_t **data, int *dsize)
{
       u_int8_t *needle = NULL;
       int offset = 0;
       int tmpsize = 0;
       int chunked = 0;
       u_int8_t *newbuf = NULL;
  
       /* set needle to past the HTTP keyword */
       needle = (u_int8_t *)p->data + 5;

       /* search the http headers for Transfer-Encoding */
       needle = (u_int8_t *)strstr(needle, "Transfer-Encoding: chunked");

       if(needle != NULL)
       {
           /* chunked encoding used */
           chunked = 1;
       }
       else
       { 
           /* needle is NULL set the pointer back */
           needle = p->data + 5;
       }
       /* search for the end of the http header string */
       needle = strstr(needle, "\r\n\r\n");
       if(needle == NULL)
       {
           return 0;
       }
       /* set newbuf past the http headers */
       newbuf = needle + 4;
       /* size of packet minus headers */ 
       offset = newbuf - p->data;

       /* point the new buf to the beginning of the payload */
       if(chunked)
       {
         /* chunked encoding used put needle past */
         needle = needle + 4; 
         needle = strstr(needle, "\r\n");
         if(needle == NULL)
         {
               /* chunked but couldn't return after chunksize */
               return 0;
         }
         /* just get us past the first chunksize */
         newbuf = needle + 2;
         
         /* size of packet minus headers and chunksize */
         offset = newbuf - p->data; 
       }

       DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"p->data %p, needle %p, newbuf %p, offset %d, p->dsize %u\n",
               p->data, needle, newbuf, offset, p->dsize););

       /* some sanity checks */
       if(offset >= p->dsize)
               return 0;
       if(offset < 0)
               return 0;

       tmpsize = p->dsize - offset;

       while(check_4_http_headers(newbuf,tmpsize))
       {
          needle = newbuf + 5;
          /* stripping http headers from buffer */
          needle = strstr(needle, "\r\n\r\n");
          if(needle == NULL)
          {
               return 0;
          }

          /* point the new buf to the beginning of the payload */
          newbuf = needle + 4;
          offset = newbuf - p->data;
          tmpsize = p->dsize - offset;
       }

       /* some sanity checks */
       if(offset >= p->dsize)
               return 0;
       if(offset < 0)
               return 0;

       /* finally pass the new buf ptr and size back to the caller */
       *data = newbuf;
       *dsize = p->dsize - offset;
       return 1;
}


int
check_4_http_headers(u_int8_t *buf, int dsize)
{
    if((dsize > 5) &&
        (buf[0] == 'H' || buf[1] == 'T' ||
         buf[2] == 'T' || buf[3] == 'P'))
    {
       /* http headers found */ 
       return 1;
    }

    //printf("no http headers found\n");
    return 0;
}


/*
 * Function:  ScanPort(Packet *p)
 *
 * Purpose:   Determines if a packet needs to be scanned based
 *            on the source and destination ports, and for tcp
 *            packets also if we want to scan only toclient or
 *            toserver packets.
 *
 * Arguments: pointer to the packet
 *
 * Returns:   returns 1 if the packet needs to be scanned,
 *                    0 otherwise.
 */
static INLINE int ScanPort(Packet *p)
{
   /* tcp packet */
   if(p->tcph)
   {
       if(p->packet_flags & PKT_FROM_SERVER && !clamcnf.toserveronly)
       {
           /* server to client packet: check sp */
           if(!(clamcnf.VirusScanPorts[(p->sp/8)] & (1<<(p->sp%8))))
               return 0;
           else
               return 1;
       }
       else if(p->packet_flags & PKT_FROM_CLIENT && !clamcnf.toclientonly)
       {
           /* client to server packet: check dp */
           if(!(clamcnf.VirusScanPorts[(p->dp/8)] & (1<<(p->dp%8))))
               return 0;
           else
               return 1;
       }
       else
       {
           /* if we get here the packet was FROM_SERVER while in toserveronly mode
              or vice-versa. So we don't scan. */
           return 0;
       }
   }
   /* if the packet is not tcp we have no idea about the direction of the
    * packet. So we check both the dp and the sp. */
   else if(!(clamcnf.VirusScanPorts[(p->dp/8)] & (1<<(p->dp%8))) && /* destination ports */
           !(clamcnf.VirusScanPorts[(p->sp/8)] & (1<<(p->sp%8))))   /* source ports */
   {
       return 0;
   }
   else
   {
       return 1;
   }
}


static ssize_t writepacket(int fd, const void *pbuffer, size_t psize)
{
   ssize_t pdwritten = 0, pd;

   do {
       DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"we have written %i of %i bytes in writepacket loop\n",pdwritten,psize););

       if((pd = write(fd, &((const char *)pbuffer)[pdwritten], psize - pdwritten)) == -1)
       {
           if(errno == EINTR)
           {
               DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"we wrote %i of %i bytes to disk and got a signal interrupt looping\n",pdwritten,psize););
           }
           else
           {
               return -1;
           }
       }
       pdwritten += pd;

       DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"we have written %i of %i bytes in writepacket loop\n",pdwritten,psize););
   } while (pdwritten < psize);

   return pdwritten;
}


/*      returns:
       1: virus or error, so not scanned
       0: ok
*/
static INLINE int StoreAndScan(Packet *p)
{
   int fd = 0;
   char tempfilename[256]="";
   size_t size = 0;
   int ret = 0, retval = 0;
   Event event;
   char outstring[255];
   const char *cl_virusname = NULL;
   u_int8_t *buf = (u_int8_t *)p->data;
   int dsize = p->dsize;

   /* create a secure temp file */
   size = snprintf(tempfilename, sizeof(tempfilename), "%s/snort_inline-clamav-XXXXXX", clamcnf.desctmpdir);
   DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"tempfilename is %s\n",tempfilename););
   if(size >= sizeof(tempfilename))
   {
       FatalError("buffer too small");
   }
   fd = mkstemp(tempfilename);
   if(fd == -1)
   {
       DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"mkstemp error: %s.\n", strerror(errno)););
       return 1;
   }

   /* check if this is a HTTP response, if it is, the buffer
      and buffer size will be adjusted */
   if(check_4_http_headers(buf,dsize))
   {
      if(strip_http_headers_p(p, &buf, &dsize))
      {
         //printf("headers stripped\n");
      }
   }
   
   /* writing the data to the tempfile */
   ret = writepacket(fd, buf, dsize);
   if(ret < dsize)
   {
       DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"tmpfile too small\n"););
   }

   /* scan the descriptor */
   ret = cl_scandesc(fd, &cl_virusname, NULL, cl_root, &clam_limits, CL_SCAN_STDOPT);
   if(ret == CL_CLEAN)
   {
       DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"Packet is clean\n"););
   }
   else if(ret == CL_VIRUS)
   {
       snprintf(outstring, sizeof(outstring), "%s %s", CLAMAV_VIRUSFOUND_STR, cl_virusname);

       DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"tempfile and or packet is infected: '%s'.\n", outstring););

       /* alert! */
       SetEvent(&event, GENERATOR_SPP_CLAMAV, CLAMAV_VIRUSFOUND, 1, 0, 0, 0);
       CallAlertFuncs(p, outstring, NULL, &event);
       CallLogFuncs(p, outstring, NULL, &event);
       retval = 1;
   }
   else
   {
       snprintf(outstring, sizeof(outstring), "%s (%d) %s", CLAMAV_SCANFAILED_STR, ret, cl_strerror(ret));

       DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"scanning the tempfile failed: '%s'.\n", outstring););

       /* alert! */
       SetEvent(&event, GENERATOR_SPP_CLAMAV, CLAMAV_SCANFAILED, 1, 0, 0, 0);
       CallAlertFuncs(p, outstring, NULL, &event);
       CallLogFuncs(p, outstring, NULL, &event);

       /* only return that we found an error when the
        * config says so */
       if (clamcnf.block_failed_scans == 1)
           retval = 1;
	
   }

   /* close the fd */
   close(fd);
   /* remove it */
   unlink(tempfilename);

   return retval;
}


/*
 * Function: VirusInPacket(Packet *)
 *
 * Purpose: Perform Virusscanning on the payload of a packet.
 *
 * Arguments: p => pointer to the current packet data struct
 *
 * Returns: 1: is a virus was found
 *          0: if the packet is clean
 *
 */
static int VirusInPacket(Packet *p)
{
   /* check if we need to reload the virus db */
   if(p->pkth->ts.tv_sec >= clamcnf.next_reload_time)
   {
       ClamAVReloadDB(p);
   }


   /* virus scanning requires data, so check if we have any */
   if(p->dsize == 0)
   {
       return 0;
   }


   /* check the port list to see if we need to scan this port */
   if(!ScanPort(p))
   {
       return 0;
   }


   /* if cl_root is NULL we return */
   if(!cl_root)
   {
       return 0;
   }


   /* do the actual scanning */
   return(StoreAndScan(p));
}


/*
 * Function: PreprocFunction(Packet *)
 *
 * Purpose: Perform the preprocessor's intended function.  This can be
 *          simple (statistics collection) or complex (IP defragmentation)
 *          as you like.  Try not to destroy the performance of the whole
 *          system by trying to do too much....
 *
 * Arguments: p => pointer to the current packet data struct
 *
 * Returns: void function
 *
 */
static void VirusChecker(Packet *p, void *context)
{
   if(VirusInPacket(p))
   {
#ifdef GIDS
       if(clamcnf.reset)
       {
           DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"ClamAV virus found sending Reject\n"););
           InlineReject(p);
       }
       else if(clamcnf.rboth)
       {
           DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"ClamAV virus found sending Reject\n"););
           InlineReject(p);
       }
       else if(clamcnf.drop)
       {
           DEBUG_WRAP(DebugMessage(DEBUG_CLAMAV,"ClamAV virus found sending Drop\n"););
           InlineDrop(p);
       }
       else
       {
            /* we only call AlertFlushStream when we are in alert mode
	       because if the packet stays in our reassembled buffer
	       (in stream4inline mode) we might keep alerting on it. */
            stream_api->alert_flush_stream(p);
            /* we also disable detection because if snort alerts it will
	       try to access the reassebled session, which is already gone.
	       This will cause us to SEGV. */
            DisableDetect(p);
       }
#endif /* GIDS */
   }
   return;
}

