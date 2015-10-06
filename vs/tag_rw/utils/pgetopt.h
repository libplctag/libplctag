/*
 *  pgetopt.h - Portable implementation of getopt() command line args parser, 
 *              originally made available by IBM and the authors listed below.
 *     
 *  Created on 8/8/08.
 *  Portions of this document are Copyright (C) 2008, PlexFX, 
 *											               All Rights Reserved.
 *
 *  History: 
 *  Original Date Unknown
 *              This code is quite old, but it was originally called GETOPT.H 
 *              in the comments, along with a GETOPT.C source file, and used the 
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
 *              pgetopt.c, pgetopt.h.  Also added some comments to clarify usage
 *              for the popterr extern.
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
 *  MODULE NAME : GETOPT.H
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

#ifndef H_PGETOPT
#define H_PGETOPT 1

extern char * poptarg;  /* carries the optional argument when a command line
                         * arg is specified with a ':' after it in the optstring
                         * and is usually handled by the caller in a switch()
                         * block. */
extern int    poptind;	/* The caller should not need to adjust this normally */
extern int    popterr;  /* The pgetopt() function returns a question mark (?) 
                         * when it encounters an option character not included in 
								 * optstring.  This error message can be disabled by 
                         * setting popterr to zero.  Otherwise, it returns the 
                         * option character that was detected. */

int pgetopt(int argc, char *argv[], char *optstring);

/* Example code by PlexFX to demonstrate calling of and parsing optional extra
 * args.  This is a sample code fragment, untested, minimal or non-existent 
 * error handling, and some headers variable declarations are omitted.
 */

#if 0 /* remove, for example purposes only */

/* Note the ':' shown below in the pattern string, to specify additional arg */
char opt_pattern[] = "s:V?";

while ((c = pgetopt(argc, argv, opt_pattern)) != -1) 
{
	switch(c) 
	{
      case 's':      /* specify a /s option with a numeric size parameter 
							 * which is provided in poptarg, a char *
		                * Example: $ someprogram /s 100
		                */
			
			some_size = atoi(poptarg);   /* should use strtol() in new code */
         break;
		case 'V':		/* specify a /V version option, with no parameter */
			puts("someprogram: Version 1.0");
			break;
		case '?':		/* explicit allows of -? or /? */
		default:			/* and give usage any time invalid arguments are given */
			PrintUsage();						/* call some function to show usage */
			break;
	}
}
#endif /* 0 */

#endif /* ! H_GETOPT */

