/*
 *  pgetopt.c - Portable implementation of getopt() command line args parser, 
 *              originally made available by IBM and the authors listed below.
 *     
 *  Created on 8/8/08.
 *  Portions of this document are Copyright (C) 2008, PlexFX, 
 *											               All Rights Reserved.
 *
 *  History: 
 *  Original Date Unknown
 *              This code is quite old, but it was originally called GETOPT.C 
 *              in the comments, along with a GETOPT.H thin header, and used the 
 *              same namespace as the getopt() implementation on my UNIX variant 
 *              platforms.  The original date has been lost.  It may date back 
 *              to even pre-ANSI C.  The development team at PlexFX has been 
 *              using it (primarily for Windows command line tools, but also on
 *              other platforms for many years.  A search for historical dates
 *              via web search engines found it widely used, but no date stamps
 *              on its original form seem to have been preserved.
 *              It can be found in various forms in open source packages, such 
 *              as using a search engine on one or both of the author strings 
 *              shown in the original comment block below.  For example, as of 
 *              the creation date on this file, a slightly modified verion of 
 *              it was used in library code found in the CVS tree for 
 *              OpenSolaris.
 *
 *              It was also included on at least some of the MSDN Library discs
 *              Around the early 2001-2003 time frame.
 *
 *  2008-08-08  This version is a modified version of the original IBM code, but
 *              the filename and namespace used has been altered along with some
 *              calling convention changes.  As such, it can be used as a drop-
 *              in replacement for getopt() even on UNIX or Linux systems that 
 *              have their own getopt() implementations in libc without naming
 *              collisions.  This means it can be used portably on any OS with
 *              a conforming C compiler.  It does *not* attempt to implement the
 *              more long-winded getopt_long() interface.  Naming of APIs, 
 *              headers and the optarg/optind externs have been prefixed with
 *              'p' to accomplish this.  Examples: pgetopt(), poptarg, poptind,
 *              pgetopt.c, pgetopt.h.
 *              Note: This interface keeps external state (to match original
 *                    calling conventions).  As such, it is not thread safe,
 *                    and should be called in only one thread (use from main()
 *							 before additional threads are started).  As the command
 *                    line should never change, this should not be an issue.
 *
 */

/* Original IBM "AS IS" license follows */

/*****************************************************************************
 *
 *  MODULE NAME : GETOPT.C
 *
 *  COPYRIGHTS:
 *             This module contains code made available by IBM
 *             Corporation on an AS IS basis.  Any one receiving the
 *             module is considered to be licensed under IBM copyrights
 *             to use the IBM-provided source code in any way he or she
 *             deems fit, including copying it, compiling it, modifying
 *             it, and redistributing it, with or without
 *             modifications.  No license under any IBM patents or
 *             patent applications is to be implied from this copyright
 *             license.
 *
 *             A user of the module should understand that IBM cannot
 *             provide technical support for the module and will not be
 *             responsible for any consequences of use of the program.
 *
 *             Any notices, including this one, are not to be removed
 *             from the module without the prior written consent of
 *             IBM.
 *
 *  AUTHOR:   Original author:
 *                 G. R. Blair (BOBBLAIR at AUSVM1)
 *                 Internet: bobblair@bobblair.austin.ibm.com
 *
 *            Extensively revised by:
 *                 John Q. Walker II, Ph.D. (JOHHQ at RALVM6)
 *                 Internet: johnq@ralvm6.vnet.ibm.com
 *
 *****************************************************************************/

/******************************************************************************
 * pgetopt()
 *
 * The pgetopt() function is a command line parser.  It returns the next
 * option character in argv that matches an option character in optstring.
 *
 * The argv argument points to an array of argc+1 elements containing argc
 * pointers to character strings followed by a null pointer.
 *
 * The optstring argument points to a string of option characters; if an
 * option character is followed by a colon, the option is expected to have
 * an argument that may or may not be separated from it by white space.
 * The external variable poptarg is set to point to the start of the option
 * argument on return from pgetopt().
 *
 * The pgetopt() function places in poptind the argv index of the next argument
 * to be processed.  The system initializes the external variable poptind to
 * 1 before the first call to pgetopt().
 *
 * When all options have been processed (that is, up to the first nonoption
 * argument), pgetopt() returns EOF.  The special option "--" may be used to
 * delimit the end of the options; EOF will be returned, and "--" will be
 * skipped.
 *
 * The pgetopt() function returns a question mark (?) when it encounters an
 * option character not included in optstring.  This error message can be
 * disabled by setting popterr to zero.  Otherwise, it returns the option
 * character that was detected.
 *
 * If the special option "--" is detected, or all options have been
 * processed, EOF is returned.
 *
 * Options are marked by either a minus sign (-) or a slash (/).
 *
 * No other errors are defined.
 *****************************************************************************/

#include <stdio.h>                  /* for EOF */
#include <string.h>                 /* for strchr() */
#include "pgetopt.h"						/* pgetopt() interface and example code */

/* global variables that are specified as exported by pgetopt() */
char *poptarg = NULL;    /* pointer to the start of the option argument  */
int   poptind = 1;       /* number of the next argv[] to be evaluated    */
int   popterr = 1;       /* non-zero if a question mark should be returned
                          * when a non-valid option character is detected */

/* handle possible future character set concerns by putting this in a macro */
#define _next_char(string)  (char)(*(string+1))

int 
pgetopt(int argc, char *argv[], char *optstring)
{
	static char *IndexPosition = NULL; /* place inside current argv string */
	char *ArgString = NULL;        /* where to start from next */
	char *OptString;               /* the string in our program */
	
	
   if (IndexPosition != NULL) {
		/* we last left off inside an argv string */
		if (*(++IndexPosition)) {
			/* there is more to come in the most recent argv */
			ArgString = IndexPosition;
		}
	}
	
	if (ArgString == NULL) {
		/* we didn't leave off in the middle of an argv string */
		if (poptind >= argc) {
			/* more command-line arguments than the argument count */
			IndexPosition = NULL;  /* not in the middle of anything */
			return EOF;             /* used up all command-line arguments */
		}
		
		/*---------------------------------------------------------------------
		 * If the next argv[] is not an option, there can be no more options.
		 *-------------------------------------------------------------------*/
		ArgString = argv[poptind++]; /* set this to the next argument ptr */
		
		if (('/' != *ArgString) && /* doesn't start with a slash or a dash? */
			 ('-' != *ArgString)) {
			--poptind;               /* point to current arg once we're done */
			poptarg = NULL;          /* no argument follows the option */
			IndexPosition = NULL;  /* not in the middle of anything */
			return EOF;             /* used up all the command-line flags */
		}
		
		/* check for special end-of-flags markers */
		if ((strcmp(ArgString, "-") == 0) ||
			 (strcmp(ArgString, "--") == 0)) {
			poptarg = NULL;          /* no argument follows the option */
			IndexPosition = NULL;  /* not in the middle of anything */
			return EOF;             /* encountered the special flag */
		}
		
		ArgString++;               /* look past the / or - */
	}
	
	if (':' == *ArgString) {       /* is it a colon? */
		/*---------------------------------------------------------------------
		 * Rare case: if opterr is non-zero, return a question mark;
		 * otherwise, just return the colon we're on.
		 *-------------------------------------------------------------------*/
		return (popterr ? (int)'?' : (int)':');
	}
	else if ((OptString = strchr(optstring, *ArgString)) == 0) {
		/*---------------------------------------------------------------------
		 * The letter on the command-line wasn't any good.
		 *-------------------------------------------------------------------*/
		poptarg = NULL;              /* no argument follows the option */
		IndexPosition = NULL;      /* not in the middle of anything */
		return (popterr ? (int)'?' : (int)*ArgString);
	}
	else {
		/*---------------------------------------------------------------------
		 * The letter on the command-line matches one we expect to see
		 *-------------------------------------------------------------------*/
		if (':' == _next_char(OptString)) { /* is the next letter a colon? */
			/* It is a colon.  Look for an argument string. */
			if ('\0' != _next_char(ArgString)) {  /* argument in this argv? */
				poptarg = &ArgString[1];   /* Yes, it is */
			}
			else {
				/*-------------------------------------------------------------
				 * The argument string must be in the next argv.
				 * But, what if there is none (bad input from the user)?
				 * In that case, return the letter, and poptarg as NULL.
				 *-----------------------------------------------------------*/
				if (poptind < argc)
					poptarg = argv[poptind++];
				else {
					poptarg = NULL;
					return (popterr ? (int)'?' : (int)*ArgString);
				}
			}
			IndexPosition = NULL;  /* not in the middle of anything */
		}
		else {
			/* it's not a colon, so just return the letter */
			poptarg = NULL;          /* no argument follows the option */
			IndexPosition = ArgString;    /* point to the letter we're on */
		}
		return (int)*ArgString;    /* return the letter that matched */
	}
}


