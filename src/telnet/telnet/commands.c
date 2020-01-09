/*
 * Copyright (c) 1988, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* based on @(#)commands.c	8.1 (Berkeley) 6/6/93 */

#include <autoconf.h>

#if	defined(unix)
#include <sys/param.h>
#if	defined(CRAY) || defined(sysV88)
#include <sys/types.h>
#endif
#include <sys/file.h>
#else
#include <sys/types.h>
#endif	/* defined(unix) */
#include <sys/socket.h>
#include <netinet/in.h>
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif /* HAVE_ARPA_INET_H */
#ifdef	CRAY
#include <fcntl.h>
#endif	/* CRAY */
#include <sys/wait.h>

#include <stdio.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <signal.h>
#include <netdb.h>
#include <ctype.h>
#include <pwd.h>
#include <stdarg.h>
#include <errno.h>
#ifdef HAVE_VFORK_H
#include <vfork.h>
#endif

#include <arpa/telnet.h>

#include "general.h"

#include "ring.h"

#include "externs.h"
#include "defines.h"
#include "types.h"

#if defined(AUTHENTICATION) || defined(FORWARD)
#include <libtelnet/auth.h>
#endif

#ifdef	ENCRYPTION
#include <libtelnet/encrypt.h>
#endif

#if	defined(AUTHENTICATION) || defined(ENCRYPTION)
#include <libtelnet/misc-proto.h>
#endif

#if !defined(CRAY) && !defined(sysV88)
#include <netinet/in_systm.h>
# if (defined(vax) || defined(tahoe) || defined(hp300)) && !defined(ultrix)
# include <machine/endian.h>
# endif /* vax */
#endif /* !defined(CRAY) && !defined(sysV88) */
#include <netinet/ip.h>


#if HAVE_ARPA_NAMESER_H
#include <arpa/nameser.h>
#endif
#include <netdb.h>

#ifndef MAXDNAME
#define MAXDNAME 256 /*per the rfc*/
#endif
#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif

#if	defined(IPPROTO_IP) && defined(IP_TOS)
int tos = -1;
static	unsigned long sourceroute(char *, char **, int *);
#endif	/* defined(IPPROTO_IP) && defined(IP_TOS) */

#include "fake-addrinfo.h"

#include <k5-platform.h>

char	*hostname;
static char _hostname[MAXDNAME];
static char hostaddrstring[NI_MAXHOST];

extern char *getenv();

extern int isprefix();
extern char **genget();
extern int Ambiguous();

typedef int (*intrtn_t)();
static int call (intrtn_t, ...);
void cmdrc (char *, char *);
static    int
send_tncmd (void (*func)(), char *, char *);
static int help(int, char **);

typedef struct {
	char	*name;		/* command name */
	char	*help;		/* help string (NULL for no help) */
	int	(*handler)	/* routine which executes command */
                        (int, char *[]);
	int	needconnect;	/* Do we need to be connected to execute? */
} Command;

static char line[256];
static char saveline[256];
static int margc;
static char *margv[20];

    static void
makeargv()
{
    register char *cp, *cp2, c;
    register char **argp = margv;

    margc = 0;
    cp = line;
    if (*cp == '!') {		/* Special case shell escape */
	strncpy(saveline, line, sizeof(saveline) - 1);
				/* save for shell command */
	saveline[sizeof(saveline)  - 1] = '\0';
	*argp++ = "!";		/* No room in string to get this */
	margc++;
	cp++;
    }
    while ((c = *cp)) {
	register int inquote = 0;
	while (isspace((int) c))
	    c = *++cp;
	if (c == '\0')
	    break;
	*argp++ = cp;
	margc += 1;
	for (cp2 = cp; c != '\0'; c = *++cp) {
	    if (inquote) {
		if (c == inquote) {
		    inquote = 0;
		    continue;
		}
	    } else {
		if (c == '\\') {
		    if ((c = *++cp) == '\0')
			break;
		} else if (c == '"') {
		    inquote = '"';
		    continue;
		} else if (c == '\'') {
		    inquote = '\'';
		    continue;
		} else if (isspace((int) c))
		    break;
	    }
	    *cp2++ = c;
	}
	*cp2 = '\0';
	if (c == '\0')
	    break;
	cp++;
    }
    *argp++ = 0;
}

/*
 * Make a character string into a number.
 *
 * Todo:  1.  Could take random integers (12, 0x12, 012, 0b1).
 */

	static int
special(s)
	register char *s;
{
	register char c;
	char b;

	switch (*s) {
	case '^':
		b = *++s;
		if (b == '?') {
		    c = b | 0x40;		/* DEL */
		} else {
		    c = b & 0x1f;
		}
		break;
	default:
		c = *s;
		break;
	}
	return c;
}

/*
 * Construct a control character sequence
 * for a special character.
 */
	static char *
control(c)
	register cc_t c;
{
	static char buf[5];
	/*
	 * The only way I could get the Sun 3.5 compiler
	 * to shut up about
	 *	if ((unsigned int)c >= 0x80)
	 * was to assign "c" to an unsigned int variable...
	 * Arggg....
	 */
	register unsigned int uic = (unsigned int)c;

	if (uic == 0x7f)
		return ("^?");
	if (c == (cc_t)_POSIX_VDISABLE) {
		return "off";
	}
	if (uic >= 0x80) {
		buf[0] = '\\';
		buf[1] = ((c>>6)&07) + '0';
		buf[2] = ((c>>3)&07) + '0';
		buf[3] = (c&07) + '0';
		buf[4] = 0;
	} else if (uic >= 0x20) {
		buf[0] = c;
		buf[1] = 0;
	} else {
		buf[0] = '^';
		buf[1] = '@'+c;
		buf[2] = 0;
	}
	return (buf);
}



/*
 *	The following are data structures and routines for
 *	the "send" command.
 *
 */
 
struct sendlist {
    char	*name;		/* How user refers to it (case independent) */
    char	*help;		/* Help information (0 ==> no help) */
    int		needconnect;	/* Need to be connected */
    int		narg;		/* Number of arguments */
    int		(*handler)	/* Routine to perform (for special ops) */
			(char *);
    int		nbyte;		/* Number of bytes to send this command */
    int		what;		/* Character to be sent (<0 ==> special) */
};


static int
	send_esc (char *),
	send_help (char *),
	send_docmd (char *),
	send_dontcmd (char *),
	send_willcmd (char *),
	send_wontcmd (char *);

static struct sendlist Sendlist[] = {
    { "ao",	"Send Telnet Abort output",		1, 0, 0, 2, AO },
    { "ayt",	"Send Telnet 'Are You There'",		1, 0, 0, 2, AYT },
    { "brk",	"Send Telnet Break",			1, 0, 0, 2, BREAK },
    { "break",	0,					1, 0, 0, 2, BREAK },
    { "ec",	"Send Telnet Erase Character",		1, 0, 0, 2, EC },
    { "el",	"Send Telnet Erase Line",		1, 0, 0, 2, EL },
    { "escape",	"Send current escape character",	1, 0, send_esc, 1, 0 },
    { "ga",	"Send Telnet 'Go Ahead' sequence",	1, 0, 0, 2, GA },
    { "ip",	"Send Telnet Interrupt Process",	1, 0, 0, 2, IP },
    { "intp",	0,					1, 0, 0, 2, IP },
    { "interrupt", 0,					1, 0, 0, 2, IP },
    { "intr",	0,					1, 0, 0, 2, IP },
    { "nop",	"Send Telnet 'No operation'",		1, 0, 0, 2, NOP },
    { "eor",	"Send Telnet 'End of Record'",		1, 0, 0, 2, EOR },
    { "abort",	"Send Telnet 'Abort Process'",		1, 0, 0, 2, ABORT },
    { "susp",	"Send Telnet 'Suspend Process'",	1, 0, 0, 2, SUSP },
    { "eof",	"Send Telnet End of File Character",	1, 0, 0, 2, xEOF },
    { "synch",	"Perform Telnet 'Synch operation'",	1, 0, dosynch, 2, 0 },
    { "getstatus", "Send request for STATUS",		1, 0, get_status, 6, 0 },
    { "?",	"Display send options",			0, 0, send_help, 0, 0 },
    { "help",	0,					0, 0, send_help, 0, 0 },
    { "do",	0,					0, 1, send_docmd, 3, 0 },
    { "dont",	0,					0, 1, send_dontcmd, 3, 0 },
    { "will",	0,					0, 1, send_willcmd, 3, 0 },
    { "wont",	0,					0, 1, send_wontcmd, 3, 0 },
    { 0 }
};

#define	GETSEND(name) ((struct sendlist *) genget(name, (char **) Sendlist, \
				sizeof(struct sendlist)))

    static int
sendcmd(argc, argv)
    int  argc;
    char **argv;
{
    int count;		/* how many bytes we are going to need to send */
    int i;
    struct sendlist *s;	/* pointer to current command */
    int success = 0;
    int needconnect = 0;

    if (argc < 2) {
	printf("need at least one argument for 'send' command\r\n");
	printf("'send ?' for help\n");
	return 0;
    }
    /*
     * First, validate all the send arguments.
     * In addition, we see how much space we are going to need, and
     * whether or not we will be doing a "SYNCH" operation (which
     * flushes the network queue).
     */
    count = 0;
    for (i = 1; i < argc; i++) {
	s = GETSEND(argv[i]);
	if (s == 0) {
	    printf("Unknown send argument '%s'\n'send ?' for help.\r\n",
			argv[i]);
	    return 0;
	} else if (Ambiguous(s)) {
	    printf("Ambiguous send argument '%s'\n'send ?' for help.\r\n",
			argv[i]);
	    return 0;
	}
	if (i + s->narg >= argc) {
	    fprintf(stderr,
	    "Need %d argument%s to 'send %s' command.  'send %s ?' for help.\n",
		s->narg, s->narg == 1 ? "" : "s", s->name, s->name);
	    return 0;
	}
	count += s->nbyte;
	if (s->handler == send_help) {
	    send_help(NULL);
	    return 0;
	}

	i += s->narg;
	needconnect += s->needconnect;
    }
    if (!connected && needconnect) {
	printf("?Need to be connected first.\r\n");
	printf("'send ?' for help\r\n");
	return 0;
    }
    /* Now, do we have enough room? */
    if (NETROOM() < count) {
	printf("There is not enough room in the buffer TO the network\r\n");
	printf("to process your request.  Nothing will be done.\r\n");
	printf("('send synch' will throw away most data in the network\r\n");
	printf("buffer, if this might help.)\r\n");
	return 0;
    }
    /* OK, they are all OK, now go through again and actually send */
    count = 0;
    for (i = 1; i < argc; i++) {
	if ((s = GETSEND(argv[i])) == 0) {
	    fprintf(stderr, "Telnet 'send' error - argument disappeared!\n");
	    (void) quit(0, NULL);
	    /*NOTREACHED*/
	}
	if (s->handler) {
	    count++;
	    success += (*s->handler)(argv[i+1]);
	    i += s->narg;
	} else {
	    NET2ADD(IAC, s->what);
	    printoption("SENT", IAC, s->what);
	}
    }
    return (count == success);
}

    static int
send_esc(s)
    char *s;
{
    NETADD(escape);
    return 1;
}

    static int
send_docmd(name)
    char *name;
{
    return(send_tncmd(send_do, "do", name));
}

    static int
send_dontcmd(name)
    char *name;
{
    return(send_tncmd(send_dont, "dont", name));
}
    static int
send_willcmd(name)
    char *name;
{
    return(send_tncmd(send_will, "will", name));
}
    static int
send_wontcmd(name)
    char *name;
{
    return(send_tncmd(send_wont, "wont", name));
}

static    int
send_tncmd(func, cmd, name)
    void	(*func)();
    char	*cmd, *name;
{
    char **cpp;
    extern char *telopts[];
    register int val = 0;

    if (isprefix(name, "help") || isprefix(name, "?")) {
	register int col, len;

	printf("Usage: send %s <value|option>\r\n", cmd);
	printf("\"value\" must be from 0 to 255\r\n");
	printf("Valid options are:\r\n\t");

	col = 8;
	for (cpp = telopts; *cpp; cpp++) {
	    len = strlen(*cpp) + 3;
	    if (col + len > 65) {
		printf("\r\n\t");
		col = 8;
	    }
	    printf(" \"%s\"", *cpp);
	    col += len;
	}
	printf("\r\n");
	return 0;
    }
    cpp = (char **)genget(name, telopts, sizeof(char *));
    if (Ambiguous(cpp)) {
	fprintf(stderr,"'%s': ambiguous argument ('send %s ?' for help).\n",
					name, cmd);
	return 0;
    }
    if (cpp) {
	val = cpp - telopts;
    } else {
	register char *cp = name;

	while (*cp >= '0' && *cp <= '9') {
	    val *= 10;
	    val += *cp - '0';
	    cp++;
	}
	if (*cp != 0) {
	    fprintf(stderr, "'%s': unknown argument ('send %s ?' for help).\n",
					name, cmd);
	    return 0;
	} else if (val < 0 || val > 255) {
	    fprintf(stderr, "'%s': bad value ('send %s ?' for help).\n",
					name, cmd);
	    return 0;
	}
    }
    if (!connected) {
	printf("?Need to be connected first.\r\n");
	return 0;
    }
    (*func)(val, 1);
    return 1;
}

    static int
send_help(n)
     char *n;
{
    struct sendlist *s;	/* pointer to current command */
    for (s = Sendlist; s->name; s++) {
	if (s->help)
	    printf("%-15s %s\r\n", s->name, s->help);
    }
    return(0);
}

/*
 * The following are the routines and data structures referred
 * to by the arguments to the "toggle" command.
 */

    static int
lclchars(s)
     int s;
{
    donelclchars = 1;
    return 1;
}

    static int
togdebug(s)
     int s;
{
#ifndef	NOT43
    if (net > 0 &&
	(SetSockOpt(net, SOL_SOCKET, SO_DEBUG, debug)) < 0) {
	    perror("setsockopt (SO_DEBUG)");
    }
#else	/* NOT43 */
    if (debug) {
	if (net > 0 && SetSockOpt(net, SOL_SOCKET, SO_DEBUG, 0, 0) < 0)
	    perror("setsockopt (SO_DEBUG)");
    } else
	printf("Cannot turn off socket debugging\r\n");
#endif	/* NOT43 */
    return 1;
}


    static int
togcrlf(s)
     int s;
{
    if (crlf) {
	printf("Will send carriage returns as telnet <CR><LF>.\r\n");
    } else {
	printf("Will send carriage returns as telnet <CR><NUL>.\r\n");
    }
    return 1;
}

int binmode;

    static int
togbinary(val)
    int val;
{
    donebinarytoggle = 1;

    if (val >= 0) {
	binmode = val;
    } else {
	if (my_want_state_is_will(TELOPT_BINARY) &&
				my_want_state_is_do(TELOPT_BINARY)) {
	    binmode = 1;
	} else if (my_want_state_is_wont(TELOPT_BINARY) &&
				my_want_state_is_dont(TELOPT_BINARY)) {
	    binmode = 0;
	}
	val = binmode ? 0 : 1;
    }

    if (val == 1) {
	if (my_want_state_is_will(TELOPT_BINARY) &&
					my_want_state_is_do(TELOPT_BINARY)) {
	    printf("Already operating in binary mode with remote host.\r\n");
	} else {
	    printf("Negotiating binary mode with remote host.\r\n");
	    tel_enter_binary(3);
	}
    } else {
	if (my_want_state_is_wont(TELOPT_BINARY) &&
					my_want_state_is_dont(TELOPT_BINARY)) {
	    printf("Already in network ascii mode with remote host.\r\n");
	} else {
	    printf("Negotiating network ascii mode with remote host.\r\n");
	    tel_leave_binary(3);
	}
    }
    return 1;
}

    static int
togrbinary(val)
    int val;
{
    donebinarytoggle = 1;

    if (val == -1)
	val = my_want_state_is_do(TELOPT_BINARY) ? 0 : 1;

    if (val == 1) {
	if (my_want_state_is_do(TELOPT_BINARY)) {
	    printf("Already receiving in binary mode.\r\n");
	} else {
	    printf("Negotiating binary mode on input.\r\n");
	    tel_enter_binary(1);
	}
    } else {
	if (my_want_state_is_dont(TELOPT_BINARY)) {
	    printf("Already receiving in network ascii mode.\r\n");
	} else {
	    printf("Negotiating network ascii mode on input.\r\n");
	    tel_leave_binary(1);
	}
    }
    return 1;
}

    static int
togxbinary(val)
    int val;
{
    donebinarytoggle = 1;

    if (val == -1)
	val = my_want_state_is_will(TELOPT_BINARY) ? 0 : 1;

    if (val == 1) {
	if (my_want_state_is_will(TELOPT_BINARY)) {
	    printf("Already transmitting in binary mode.\r\n");
	} else {
	    printf("Negotiating binary mode on output.\r\n");
	    tel_enter_binary(2);
	}
    } else {
	if (my_want_state_is_wont(TELOPT_BINARY)) {
	    printf("Already transmitting in network ascii mode.\r\n");
	} else {
	    printf("Negotiating network ascii mode on output.\r\n");
	    tel_leave_binary(2);
	}
    }
    return 1;
}


static int togglehelp (int);
#if	defined(AUTHENTICATION)
extern int auth_togdebug (int);
#endif

struct togglelist {
    char	*name;		/* name of toggle */
    char	*help;		/* help message */
    int		(*handler)	/* routine to do actual setting */
			(int);
    int		*variable;
    char	*actionexplanation;
};

static struct togglelist Togglelist[] = {
    { "autoflush",
	"flushing of output when sending interrupt characters",
	    0,
		&autoflush,
		    "flush output when sending interrupt characters" },
    { "autosynch",
	"automatic sending of interrupt characters in urgent mode",
	    0,
		&autosynch,
		    "send interrupt characters in urgent mode" },
#if	defined(AUTHENTICATION)
    { "autologin",
	"automatic sending of login and/or authentication info",
	    0,
		&autologin,
		    "send login name and/or authentication information" },
    { "authdebug",
	"Toggle authentication debugging",
	    auth_togdebug,
		0,
		     "print authentication debugging information" },
#endif
#ifdef	ENCRYPTION
    { "autoencrypt",
	"automatic encryption of data stream",
	    EncryptAutoEnc,
		0,
		    "automatically encrypt output" },
    { "autodecrypt",
	"automatic decryption of data stream",
	    EncryptAutoDec,
		0,
		    "automatically decrypt input" },
    { "verbose_encrypt",
	"Toggle verbose encryption output",
	    EncryptVerbose,
		0,
		    "print verbose encryption output" },
    { "encdebug",
	"Toggle encryption debugging",
	    EncryptDebug,
		0,
		    "print encryption debugging information" },
#endif	/* ENCRYPTION */
    { "skiprc",
	"don't read ~/.telnetrc file",
	    0,
		&skiprc,
		    "skip reading of ~/.telnetrc file" },
    { "binary",
	"sending and receiving of binary data",
	    togbinary,
		0,
		    0 },
    { "inbinary",
	"receiving of binary data",
	    togrbinary,
		0,
		    0 },
    { "outbinary",
	"sending of binary data",
	    togxbinary,
		0,
		    0 },
    { "crlf",
	"sending carriage returns as telnet <CR><LF>",
	    togcrlf,
		&crlf,
		    0 },
    { "crmod",
	"mapping of received carriage returns",
	    0,
		&crmod,
		    "map carriage return on output" },
    { "localchars",
	"local recognition of certain control characters",
	    lclchars,
		&localchars,
		    "recognize certain control characters" },
    { " ", "", 0 },		/* empty line */
#if	defined(unix) && defined(TN3270)
    { "apitrace",
	"(debugging) toggle tracing of API transactions",
	    0,
		&apitrace,
		    "trace API transactions" },
    { "cursesdata",
	"(debugging) toggle printing of hexadecimal curses data",
	    0,
		&cursesdata,
		    "print hexadecimal representation of curses data" },
#endif	/* defined(unix) && defined(TN3270) */
    { "debug",
	"debugging",
	    togdebug,
		&debug,
		    "turn on socket level debugging" },
    { "netdata",
	"printing of hexadecimal network data (debugging)",
	    0,
		&netdata,
		    "print hexadecimal representation of network traffic" },
    { "prettydump",
	"output of \"netdata\" to user readable format (debugging)",
	    0,
		&prettydump,
		    "print user readable output for \"netdata\"" },
    { "options",
	"viewing of options processing (debugging)",
	    0,
		&showoptions,
		    "show option processing" },
    { "termdata",
	"(debugging) toggle printing of hexadecimal terminal data",
	    0,
		&termdata,
		    "print hexadecimal representation of terminal traffic" },
    { "?",
	0,
	    togglehelp },
    { "help",
	0,
	    togglehelp },
    { 0 }
};

    static int
togglehelp(n)
    int n;
{
    struct togglelist *c;

    for (c = Togglelist; c->name; c++) {
	if (c->help) {
	    if (*c->help)
		printf("%-15s toggle %s\r\n", c->name, c->help);
	    else
		printf("\r\n");
	}
    }
    printf("\r\n");
    printf("%-15s %s\r\n", "?", "display help information");
    return 0;
}

    static void
settogglehelp(set)
    int set;
{
    struct togglelist *c;

    for (c = Togglelist; c->name; c++) {
	if (c->help) {
	    if (*c->help)
		printf("%-15s %s %s\r\n", c->name, set ? "enable" : "disable",
						c->help);
	    else
		printf("\r\n");
	}
    }
}

#define	GETTOGGLE(name) (struct togglelist *) \
		genget(name, (char **) Togglelist, sizeof(struct togglelist))

    static int
toggle(argc, argv)
    int  argc;
    char *argv[];
{
    int retval = 1;
    char *name;
    struct togglelist *c;

    if (argc < 2) {
	fprintf(stderr,
	    "Need an argument to 'toggle' command.  'toggle ?' for help.\n");
	return 0;
    }
    argc--;
    argv++;
    while (argc--) {
	name = *argv++;
	c = GETTOGGLE(name);
	if (Ambiguous(c)) {
	    fprintf(stderr, "'%s': ambiguous argument ('toggle ?' for help).\n",
					name);
	    return 0;
	} else if (c == 0) {
	    fprintf(stderr, "'%s': unknown argument ('toggle ?' for help).\n",
					name);
	    return 0;
	} else {
	    if (c->variable) {
		*c->variable = !*c->variable;		/* invert it */
		if (c->actionexplanation) {
		    printf("%s %s.\r\n", *c->variable? "Will" : "Won't",
							c->actionexplanation);
		}
	    }
	    if (c->handler) {
		retval &= (*c->handler)(-1);
	    }
	}
    }
    return retval;
}

/*
 * The following perform the "set" command.
 */

#ifdef	USE_TERMIO
struct termio new_tc = { 0 };
#endif

struct setlist {
    char *name;				/* name */
    char *help;				/* help information */
    void (*handler)();
    cc_t *charp;			/* where it is located at */
};

static struct setlist Setlist[] = {
#ifdef	KLUDGELINEMODE
    { "echo", 	"character to toggle local echoing on/off", 0, &echoc },
#endif
    { "escape",	"character to escape back to telnet command mode", 0, &escape },
    { "rlogin", "rlogin escape character", 0, &rlogin },
    { "tracefile", "file to write trace information to", SetNetTrace, (cc_t *)NetTraceFile},
    { " ", "" },
    { " ", "The following need 'localchars' to be toggled true", 0, 0 },
    { "flushoutput", "character to cause an Abort Output", 0, termFlushCharp },
    { "interrupt", "character to cause an Interrupt Process", 0, termIntCharp },
    { "quit",	"character to cause an Abort process", 0, termQuitCharp },
    { "eof",	"character to cause an EOF ", 0, termEofCharp },
    { " ", "" },
    { " ", "The following are for local editing in linemode", 0, 0 },
    { "erase",	"character to use to erase a character", 0, termEraseCharp },
    { "kill",	"character to use to erase a line", 0, termKillCharp },
    { "lnext",	"character to use for literal next", 0, termLiteralNextCharp },
    { "susp",	"character to cause a Suspend Process", 0, termSuspCharp },
    { "reprint", "character to use for line reprint", 0, termRprntCharp },
    { "worderase", "character to use to erase a word", 0, termWerasCharp },
    { "start",	"character to use for XON", 0, termStartCharp },
    { "stop",	"character to use for XOFF", 0, termStopCharp },
    { "forw1",	"alternate end of line character", 0, termForw1Charp },
    { "forw2",	"alternate end of line character", 0, termForw2Charp },
    { "ayt",	"alternate AYT character", 0, termAytCharp },
    { 0 }
};

#if	defined(CRAY) && !defined(__STDC__)
/* Work around compiler bug in pcc 4.1.5 */
    void
_setlist_init()
{
#ifndef	KLUDGELINEMODE
#define	N 5
#else
#define	N 6
#endif
	Setlist[N+0].charp = &termFlushChar;
	Setlist[N+1].charp = &termIntChar;
	Setlist[N+2].charp = &termQuitChar;
	Setlist[N+3].charp = &termEofChar;
	Setlist[N+6].charp = &termEraseChar;
	Setlist[N+7].charp = &termKillChar;
	Setlist[N+8].charp = &termLiteralNextChar;
	Setlist[N+9].charp = &termSuspChar;
	Setlist[N+10].charp = &termRprntChar;
	Setlist[N+11].charp = &termWerasChar;
	Setlist[N+12].charp = &termStartChar;
	Setlist[N+13].charp = &termStopChar;
	Setlist[N+14].charp = &termForw1Char;
	Setlist[N+15].charp = &termForw2Char;
	Setlist[N+16].charp = &termAytChar;
#undef	N
}
#endif	/* defined(CRAY) && !defined(__STDC__) */

    static struct setlist *
getset(name)
    char *name;
{
    return (struct setlist *)
		genget(name, (char **) Setlist, sizeof(struct setlist));
}

    void
set_escape_char(s)
    char *s;
{
	if (rlogin != _POSIX_VDISABLE) {
		rlogin = (s && *s) ? special(s) : _POSIX_VDISABLE;
		printf("Telnet rlogin escape character is '%s'.\r\n",
					control(rlogin));
	} else {
		escape = (s && *s) ? special(s) : _POSIX_VDISABLE;
		printf("Telnet escape character is '%s'.\r\n", control(escape));
	}
}

    static int
setcmd(argc, argv)
    int  argc;
    char *argv[];
{
    int value;
    struct setlist *ct;
    struct togglelist *c;

    if (argc < 2 || argc > 3) {
	printf("Format is 'set Name Value'\n'set ?' for help.\r\n");
	return 0;
    }
    if ((argc == 2) && (isprefix(argv[1], "?") || isprefix(argv[1], "help"))) {
	for (ct = Setlist; ct->name; ct++)
	    printf("%-15s %s\r\n", ct->name, ct->help);
	printf("\r\n");
	settogglehelp(1);
	printf("%-15s %s\r\n", "?", "display help information");
	return 0;
    }

    ct = getset(argv[1]);
    if (ct == 0) {
	c = GETTOGGLE(argv[1]);
	if (c == 0) {
	    fprintf(stderr, "'%s': unknown argument ('set ?' for help).\n",
			argv[1]);
	    return 0;
	} else if (Ambiguous(c)) {
	    fprintf(stderr, "'%s': ambiguous argument ('set ?' for help).\n",
			argv[1]);
	    return 0;
	}
	if (c->variable) {
	    if ((argc == 2) || (strcmp("on", argv[2]) == 0))
		*c->variable = 1;
	    else if (strcmp("off", argv[2]) == 0)
		*c->variable = 0;
	    else {
		printf("Format is 'set togglename [on|off]'\n'set ?' for help.\r\n");
		return 0;
	    }
	    if (c->actionexplanation) {
		printf("%s %s.\r\n", *c->variable? "Will" : "Won't",
							c->actionexplanation);
	    }
	}
	if (c->handler)
	    (*c->handler)(1);
    } else if (argc != 3) {
	printf("Format is 'set Name Value'\n'set ?' for help.\r\n");
	return 0;
    } else if (Ambiguous(ct)) {
	fprintf(stderr, "'%s': ambiguous argument ('set ?' for help).\n",
			argv[1]);
	return 0;
    } else if (ct->handler) {
	(*ct->handler)(argv[2]);
	printf("%s set to \"%s\".\r\n", ct->name, (char *)ct->charp);
    } else {
	if (strcmp("off", argv[2])) {
	    value = special(argv[2]);
	} else {
	    value = _POSIX_VDISABLE;
	}
	*(ct->charp) = (cc_t)value;
	printf("%s character is '%s'.\r\n", ct->name, control(*(ct->charp)));
    }
    slc_check();
    return 1;
}

    static int
unsetcmd(argc, argv)
    int  argc;
    char *argv[];
{
    struct setlist *ct;
    struct togglelist *c;
    register char *name;

    if (argc < 2) {
	fprintf(stderr,
	    "Need an argument to 'unset' command.  'unset ?' for help.\n");
	return 0;
    }
    if (isprefix(argv[1], "?") || isprefix(argv[1], "help")) {
	for (ct = Setlist; ct->name; ct++)
	    printf("%-15s %s\r\n", ct->name, ct->help);
	printf("\r\n");
	settogglehelp(0);
	printf("%-15s %s\r\n", "?", "display help information");
	return 0;
    }

    argc--;
    argv++;
    while (argc--) {
	name = *argv++;
	ct = getset(name);
	if (ct == 0) {
	    c = GETTOGGLE(name);
	    if (c == 0) {
		fprintf(stderr, "'%s': unknown argument ('unset ?' for help).\n",
			name);
		return 0;
	    } else if (Ambiguous(c)) {
		fprintf(stderr, "'%s': ambiguous argument ('unset ?' for help).\n",
			name);
		return 0;
	    }
	    if (c->variable) {
		*c->variable = 0;
		if (c->actionexplanation) {
		    printf("%s %s.\r\n", *c->variable? "Will" : "Won't",
							c->actionexplanation);
		}
	    }
	    if (c->handler)
		(*c->handler)(0);
	} else if (Ambiguous(ct)) {
	    fprintf(stderr, "'%s': ambiguous argument ('unset ?' for help).\n",
			name);
	    return 0;
	} else if (ct->handler) {
	    (*ct->handler)(0);
	    printf("%s reset to \"%s\".\r\n", ct->name, (char *)ct->charp);
	} else {
	    *(ct->charp) = _POSIX_VDISABLE;
	    printf("%s character is '%s'.\r\n", ct->name, control(*(ct->charp)));
	}
    }
    return 1;
}

/*
 * The following are the data structures and routines for the
 * 'mode' command.
 */
#ifdef	KLUDGELINEMODE
extern int kludgelinemode;

    static int
dokludgemode()
{
    kludgelinemode = 1;
    send_wont(TELOPT_LINEMODE, 1);
    send_dont(TELOPT_SGA, 1);
    send_dont(TELOPT_ECHO, 1);
    return 1;			/* I'm guessing here -- eichin -- XXX */
}
#endif

    static int
dolinemode()
{
#ifdef	KLUDGELINEMODE
    if (kludgelinemode)
	send_dont(TELOPT_SGA, 1);
#endif
    send_will(TELOPT_LINEMODE, 1);
    send_dont(TELOPT_ECHO, 1);
    return 1;
}

    static int
docharmode()
{
#ifdef	KLUDGELINEMODE
    if (kludgelinemode)
	send_do(TELOPT_SGA, 1);
    else
#endif
    send_wont(TELOPT_LINEMODE, 1);
    send_do(TELOPT_ECHO, 1);
    return 1;
}

    static int
dolmmode(bit, on)
    int bit, on;
{
    unsigned char c;
    extern int linemode;

    if (my_want_state_is_wont(TELOPT_LINEMODE)) {
	printf("?Need to have LINEMODE option enabled first.\r\n");
	printf("'mode ?' for help.\r\n");
	return 0;
    }

    if (on)
	c = (linemode | bit);
    else
	c = (linemode & ~bit);
    lm_mode(&c, 1, 1);
    return 1;
}

static int
tel_setmode(bit)
    int bit;
{
    return dolmmode(bit, 1);
}

static int
tel_clearmode(bit)
    int bit;
{
    return dolmmode(bit, 0);
}

struct modelist {
	char	*name;		/* command name */
	char	*help;		/* help string */
	int	(*handler)();	/* routine which executes command */
	int	needconnect;	/* Do we need to be connected to execute? */
	int	arg1;
};

static int modehelp(void);

static struct modelist ModeList[] = {
    { "character", "Disable LINEMODE option",	docharmode, 1 },
#ifdef	KLUDGELINEMODE
    { "",	"(or disable obsolete line-by-line mode)", 0 },
#endif
    { "line",	"Enable LINEMODE option",	dolinemode, 1 },
#ifdef	KLUDGELINEMODE
    { "",	"(or enable obsolete line-by-line mode)", 0 },
#endif
    { "", "", 0 },
    { "",	"These require the LINEMODE option to be enabled", 0 },
    { "isig",	"Enable signal trapping",	tel_setmode, 1, MODE_TRAPSIG },
    { "+isig",	0,				tel_setmode, 1, MODE_TRAPSIG },
    { "-isig",	"Disable signal trapping",	tel_clearmode, 1, MODE_TRAPSIG },
    { "edit",	"Enable character editing",	tel_setmode, 1, MODE_EDIT },
    { "+edit",	0,				tel_setmode, 1, MODE_EDIT },
    { "-edit",	"Disable character editing",	tel_clearmode, 1, MODE_EDIT },
    { "softtabs", "Enable tab expansion",	tel_setmode, 1, MODE_SOFT_TAB },
    { "+softtabs", 0,				tel_setmode, 1, MODE_SOFT_TAB },
    { "-softtabs", "Disable character editing",	tel_clearmode, 1, MODE_SOFT_TAB },
    { "litecho", "Enable literal character echo", tel_setmode, 1, MODE_LIT_ECHO },
    { "+litecho", 0,				tel_setmode, 1, MODE_LIT_ECHO },
    { "-litecho", "Disable literal character echo", tel_clearmode, 1, MODE_LIT_ECHO },
    { "help",	0,				modehelp, 0 },
#ifdef	KLUDGELINEMODE
    { "kludgeline", 0,				dokludgemode, 1 },
#endif
    { "", "", 0 },
    { "?",	"Print help information",	modehelp, 0 },
    { 0 },
};


static int
modehelp()
{
    struct modelist *mt;

    printf("format is:  'mode Mode', where 'Mode' is one of:\r\n\r\n");
    for (mt = ModeList; mt->name; mt++) {
	if (mt->help) {
	    if (*mt->help)
		printf("%-15s %s\r\n", mt->name, mt->help);
	    else
		printf("\r\n");
	}
    }
    return 0;
}

#define	GETMODECMD(name) (struct modelist *) \
		genget(name, (char **) ModeList, sizeof(struct modelist))

    static int
modecmd(argc, argv)
    int  argc;
    char *argv[];
{
    struct modelist *mt;

    if (argc != 2) {
	printf("'mode' command requires an argument\r\n");
	printf("'mode ?' for help.\r\n");
    } else if ((mt = GETMODECMD(argv[1])) == 0) {
	fprintf(stderr, "Unknown mode '%s' ('mode ?' for help).\n", argv[1]);
    } else if (Ambiguous(mt)) {
	fprintf(stderr, "Ambiguous mode '%s' ('mode ?' for help).\n", argv[1]);
    } else if (mt->needconnect && !connected) {
	printf("?Need to be connected first.\r\n");
	printf("'mode ?' for help.\r\n");
    } else if (mt->handler) {
	return (*mt->handler)(mt->arg1);
    }
    return 0;
}

/*
 * The following data structures and routines implement the
 * "display" command.
 */

    static int
display(argc, argv)
    int  argc;
    char *argv[];
{
    struct togglelist *tl;
    struct setlist *sl;

#define	dotog(tl)	if (tl->variable && tl->actionexplanation) { \
			    if (*tl->variable) { \
				printf("will"); \
			    } else { \
				printf("won't"); \
			    } \
			    printf(" %s.\r\n", tl->actionexplanation); \
			}

#define	doset(sl)   if (sl->name && *sl->name != ' ') { \
			if (sl->handler == 0) \
			    printf("%-15s [%s]\r\n", sl->name, control(*sl->charp)); \
			else \
			    printf("%-15s \"%s\"\r\n", sl->name, (char *)sl->charp); \
		    }

    if (argc == 1) {
	for (tl = Togglelist; tl->name; tl++) {
	    dotog(tl);
	}
	printf("\r\n");
	for (sl = Setlist; sl->name; sl++) {
	    doset(sl);
	}
    } else {
	int i;

	for (i = 1; i < argc; i++) {
	    sl = getset(argv[i]);
	    tl = GETTOGGLE(argv[i]);
	    if (Ambiguous(sl) || Ambiguous(tl)) {
		printf("?Ambiguous argument '%s'.\r\n", argv[i]);
		return 0;
	    } else if (!sl && !tl) {
		printf("?Unknown argument '%s'.\r\n", argv[i]);
		return 0;
	    } else {
		if (tl) {
		    dotog(tl);
		}
		if (sl) {
		    doset(sl);
		}
	    }
	}
    }
    /*@*/optionstatus();
#ifdef	ENCRYPTION
    EncryptStatus();
#endif	/* ENCRYPTION */
    return 1;
#undef	doset
#undef	dotog
}

/*
 * The following are the data structures, and many of the routines,
 * relating to command processing.
 */

/*
 * Set the escape character.
 */
	static int
setescape(argc, argv)
	int argc;
	char *argv[];
{
	register char *arg;
	char buf[50];

	printf(
	    "Deprecated usage - please use 'set escape%s%s' in the future.\r\n",
				(argc > 2)? " ":"", (argc > 2)? argv[1]: "");
	if (argc > 2)
		arg = argv[1];
	else {
		printf("new escape character: ");
		(void) fgets(buf, sizeof(buf), stdin);
		arg = buf;
	}
	if (arg[0] != '\0')
		escape = arg[0];
	if (!In3270) {
		printf("Escape character is '%s'.\r\n", control(escape));
	}
	(void) fflush(stdout);
	return 1;
}

    /*VARARGS*/
    static int
togcrmod(argc, argv)
     int argc;
     char **argv;
{
    crmod = !crmod;
    printf("Deprecated usage - please use 'toggle crmod' in the future.\r\n");
    printf("%s map carriage return on output.\r\n", crmod ? "Will" : "Won't");
    (void) fflush(stdout);
    return 1;
}

    /*VARARGS*/
static int
suspend(argc, argv)
     int argc;
     char **argv;
{
#ifdef	SIGTSTP
    setcommandmode();
    {
	long oldrows, oldcols, newrows, newcols, err;

	err = (TerminalWindowSize(&oldrows, &oldcols) == 0) ? 1 : 0;
	(void) kill(0, SIGTSTP);
	/*
	 * If we didn't get the window size before the SUSPEND, but we
	 * can get them now (???), then send the NAWS to make sure that
	 * we are set up for the right window size.
	 */
	if (TerminalWindowSize(&newrows, &newcols) && connected &&
	    (err || ((oldrows != newrows) || (oldcols != newcols)))) {
		sendnaws();
	}
    }
    /* reget parameters in case they were changed */
    TerminalSaveState();
    setconnmode(0);
#else
    printf("Suspend is not supported.  Try the '!' command instead\r\n");
#endif
    return 1;
}

#if	!defined(TN3270)
    /*ARGSUSED*/
static int
shell(argc, argv)
    int argc;
    char *argv[];
{
    long oldrows, oldcols, newrows, newcols, err;

    setcommandmode();

    err = (TerminalWindowSize(&oldrows, &oldcols) == 0) ? 1 : 0;
    switch(vfork()) {
    case -1:
	perror("Fork failed");
	break;

    case 0:
	{
	    /*
	     * Fire up the shell in the child.
	     */
	    register char *shellp, *shellname;

	    shellp = getenv("SHELL");
	    if (shellp == NULL)
		shellp = "/bin/sh";
	    if ((shellname = strrchr(shellp, '/')) == 0)
		shellname = shellp;
	    else
		shellname++;
	    if (argc > 1)
		execl(shellp, shellname, "-c", &saveline[1], (char *)NULL);
	    else
		execl(shellp, shellname, (char *)NULL);
	    perror("Execl");
	    _exit(1);
	}
    default:
	    (void)wait((int *)0);	/* Wait for the shell to complete */

	    if (TerminalWindowSize(&newrows, &newcols) && connected &&
		(err || ((oldrows != newrows) || (oldcols != newcols)))) {
		    sendnaws();
	    }
	    break;
    }
    return 1;
}
#else	/* !defined(TN3270) */
extern int shell();
#endif	/* !defined(TN3270) */

/*VARARGS*/
static int
bye(argc, argv)
    int  argc;		/* Number of arguments */
    char *argv[];	/* arguments */
{
    extern int resettermname;

    if (connected) {
	(void) shutdown(net, 2);
	printf("Connection closed.\r\n");
	(void) NetClose(net);
	connected = 0;
	resettermname = 1;
#if	defined(AUTHENTICATION) || defined(ENCRYPTION)
	auth_encrypt_connect(connected);
#endif	/* defined(AUTHENTICATION) || defined(ENCRYPTION) */
	/* reset options */
	tninit();
#if	defined(TN3270)
	SetIn3270();		/* Get out of 3270 mode */
#endif	/* defined(TN3270) */
    }
    if ((argc != 2) || (strcmp(argv[1], "fromquit") != 0)) {
	longjmp(toplevel, 1);
	/* NOTREACHED */
    }
    return 1;			/* Keep lint, etc., happy */
}

/*VARARGS*/
int
quit(argc, argv)
	int argc;
	char *argv[];
{
	(void) call(bye, "bye", "fromquit", 0);
	Exit(0);
	/*NOTREACHED*/
	return 0;
}

/*VARARGS*/
static int
logout(argc, argv)
     int argc;
     char **argv;
{
	send_do(TELOPT_LOGOUT, 1);
	(void) netflush();
	return 1;
}


/*
 * The SLC command.
 */

struct slclist {
	char	*name;
	char	*help;
	void	(*handler)();
	int	arg;
};

static void slc_help(void);

struct slclist SlcList[] = {
    { "export",	"Use local special character definitions",
						slc_mode_export,	0 },
    { "import",	"Use remote special character definitions",
						slc_mode_import,	1 },
    { "check",	"Verify remote special character definitions",
						slc_mode_import,	0 },
    { "help",	0,				slc_help,		0 },
    { "?",	"Print help information",	slc_help,		0 },
    { 0 },
};

    static void
slc_help()
{
    struct slclist *c;

    for (c = SlcList; c->name; c++) {
	if (c->help) {
	    if (*c->help)
		printf("%-15s %s\r\n", c->name, c->help);
	    else
		printf("\r\n");
	}
    }
}

static struct slclist *
getslc(name)
    char *name;
{
    return (struct slclist *)
		genget(name, (char **) SlcList, sizeof(struct slclist));
}

static int
slccmd(argc, argv)
    int  argc;
    char *argv[];
{
    struct slclist *c;

    if (argc != 2) {
	fprintf(stderr,
	    "Need an argument to 'slc' command.  'slc ?' for help.\n");
	return 0;
    }
    c = getslc(argv[1]);
    if (c == 0) {
        fprintf(stderr, "'%s': unknown argument ('slc ?' for help).\n",
    				argv[1]);
        return 0;
    }
    if (Ambiguous(c)) {
        fprintf(stderr, "'%s': ambiguous argument ('slc ?' for help).\n",
    				argv[1]);
        return 0;
    }
    (*c->handler)(c->arg);
    slcstate();
    return 1;
}

/*
 * The ENVIRON command.
 */

struct envlist {
	char	*name;
	char	*help;
	void	(*handler)();
	int	narg;
};

extern struct env_lst *
	env_define (unsigned char *, unsigned char *);
extern void
	env_undefine (unsigned char *),
	env_export (unsigned char *),
	env_unexport (unsigned char *),
	env_send (unsigned char *),
#if defined(OLD_ENVIRON) && defined(ENV_HACK)
	env_varval (unsigned char *),
#endif
	env_list (void);
static void
	env_help (void);

struct envlist EnvList[] = {
    { "define",	"Define an environment variable",
						(void (*)())env_define,	2 },
    { "undefine", "Undefine an environment variable",
						env_undefine,	1 },
    { "export",	"Mark an environment variable for automatic export",
						env_export,	1 },
    { "unexport", "Don't mark an environment variable for automatic export",
						env_unexport,	1 },
    { "send",	"Send an environment variable", env_send,	1 },
    { "list",	"List the current environment variables",
						env_list,	0 },
#if defined(OLD_ENVIRON) && defined(ENV_HACK)
    { "varval", "Reverse VAR and VALUE (auto, right, wrong, status)",
						env_varval,    1 },
#endif
    { "help",	0,				env_help,		0 },
    { "?",	"Print help information",	env_help,		0 },
    { 0 },
};

    static void
env_help()
{
    struct envlist *c;

    for (c = EnvList; c->name; c++) {
	if (c->help) {
	    if (*c->help)
		printf("%-15s %s\r\n", c->name, c->help);
	    else
		printf("\r\n");
	}
    }
}

    static struct envlist *
getenvcmd(name)
    char *name;
{
    return (struct envlist *)
		genget(name, (char **) EnvList, sizeof(struct envlist));
}

static int
env_cmd(argc, argv)
    int  argc;
    char *argv[];
{
    struct envlist *c;

    if (argc < 2) {
	fprintf(stderr,
	    "Need an argument to 'environ' command.  'environ ?' for help.\n");
	return 0;
    }
    c = getenvcmd(argv[1]);
    if (c == 0) {
        fprintf(stderr, "'%s': unknown argument ('environ ?' for help).\n",
    				argv[1]);
        return 0;
    }
    if (Ambiguous(c)) {
        fprintf(stderr, "'%s': ambiguous argument ('environ ?' for help).\n",
    				argv[1]);
        return 0;
    }
    if (c->narg + 2 != argc) {
	fprintf(stderr,
	    "Need %s%d argument%s to 'environ %s' command.  'environ ?' for help.\n",
		c->narg < argc - 2 ? "only " : "",
		c->narg, c->narg == 1 ? "" : "s", c->name);
	return 0;
    }
    (*c->handler)(argv[2], argv[3]);
    return 1;
}

struct env_lst {
	struct env_lst *next;	/* pointer to next structure */
	struct env_lst *prev;	/* pointer to previous structure */
	unsigned char *var;	/* pointer to variable name */
	unsigned char *value;	/* pointer to variable value */
	int export;		/* 1 -> export with default list of variables */
	int welldefined;	/* A well defined variable */
};

struct env_lst envlisthead;

static	struct env_lst *
env_find(var)
	unsigned char *var;
{
	register struct env_lst *ep;

	for (ep = envlisthead.next; ep; ep = ep->next) {
		if (strcmp((char *)ep->var, (char *)var) == 0)
			return(ep);
	}
	return(NULL);
}

	void
env_init()
{
	extern char **environ;
	char **epp, *cp;
	struct env_lst *ep;

	for (epp = environ; *epp; epp++) {
		if ((cp = strchr(*epp, '='))) {
			*cp = '\0';
			ep = env_define((unsigned char *)*epp,
					(unsigned char *)cp+1);
			ep->export = 0;
			*cp = '=';
		}
	}
	/*
	 * Special case for DISPLAY variable.  If it is ":0.0" or
	 * "unix:0.0", we have to get rid of "unix" and insert our
	 * hostname.
	 */
	if ((ep = env_find("DISPLAY"))
	    && ((*ep->value == ':')
	        || (strncmp((char *)ep->value, "unix:", 5) == 0))) {
		char hbuf[256+1];
		char *cp2 = strchr((char *)ep->value, ':');

		gethostname(hbuf, 256);
		hbuf[256] = '\0';
		asprintf(&cp, "%s%s", hbuf, cp2);
		free(ep->value);
		ep->value = (unsigned char *)cp;
	}
	/*
	 * If USER is not defined, but LOGNAME is, then add
	 * USER with the value from LOGNAME.  By default, we
	 * don't export the USER variable.
	 */
	if ((env_find("USER") == NULL) && (ep = env_find("LOGNAME"))) {
		env_define((unsigned char *)"USER", ep->value);
		env_unexport((unsigned char *)"USER");
	}
	env_export((unsigned char *)"DISPLAY");
	env_export((unsigned char *)"PRINTER");
}

	struct env_lst *
env_define(var, value)
	unsigned char *var, *value;
{
	register struct env_lst *ep;

	if ((ep = env_find(var))) {
		if (ep->var)
			free(ep->var);
		if (ep->value)
			free(ep->value);
	} else {
		ep = (struct env_lst *)malloc(sizeof(struct env_lst));
		ep->next = envlisthead.next;
		envlisthead.next = ep;
		ep->prev = &envlisthead;
		if (ep->next)
			ep->next->prev = ep;
	}
	ep->welldefined = opt_welldefined((char *)var);
	ep->export = 1;
	ep->var = (unsigned char *)strdup((char *)var);
	ep->value = (unsigned char *)strdup((char *)value);
	return(ep);
}

	void
env_undefine(var)
	unsigned char *var;
{
	register struct env_lst *ep;

	if ((ep = env_find(var))) {
		ep->prev->next = ep->next;
		if (ep->next)
			ep->next->prev = ep->prev;
		if (ep->var)
			free(ep->var);
		if (ep->value)
			free(ep->value);
		free(ep);
	}
}

	void
env_export(var)
	unsigned char *var;
{
	register struct env_lst *ep;

	if ((ep = env_find(var)))
		ep->export = 1;
}

	void
env_unexport(var)
	unsigned char *var;
{
	register struct env_lst *ep;

	if ((ep = env_find(var)))
		ep->export = 0;
}

	void
env_send(var)
	unsigned char *var;
{
	register struct env_lst *ep;

        if (my_state_is_wont(TELOPT_NEW_ENVIRON)
#ifdef	OLD_ENVIRON
	    && my_state_is_wont(TELOPT_OLD_ENVIRON)
#endif
		) {
		fprintf(stderr,
		    "Cannot send '%s': Telnet ENVIRON option not enabled\n",
									var);
		return;
	}
	ep = env_find(var);
	if (ep == 0) {
		fprintf(stderr, "Cannot send '%s': variable not defined\n",
									var);
		return;
	}
	env_opt_start_info();
	env_opt_add(ep->var);
	env_opt_end(0);
}

	void
env_list()
{
	register struct env_lst *ep;

	for (ep = envlisthead.next; ep; ep = ep->next) {
		printf("%c %-20s %s\r\n", ep->export ? '*' : ' ',
					ep->var, ep->value);
	}
}

	unsigned char *
env_default(init, welldefined)
	int init;
{
	static struct env_lst *nep = NULL;

	if (init) {
		nep = &envlisthead;
		return NULL;	/* guessing here too -- eichin -- XXX */
	}
	if (nep) {
		while ((nep = nep->next)) {
			if (nep->export && (nep->welldefined == welldefined))
				return(nep->var);
		}
	}
	return(NULL);
}

	unsigned char *
env_getvalue(var)
	unsigned char *var;
{
	register struct env_lst *ep;

	if ((ep = env_find(var)))
		return(ep->value);
	return(NULL);
}

	int
env_is_exported(var)
	unsigned char *var;
{
	register struct env_lst *ep;

	if ((ep = env_find(var)))
		return ep->export;
	return 0;
}
	    
#if defined(OLD_ENVIRON) && defined(ENV_HACK)
	void
env_varval(what)
	unsigned char *what;
{
	extern int old_env_var, old_env_value, env_auto;
	unsigned int len = strlen((char *)what);

	if (len == 0)
		goto unknown;

	if (strncasecmp((char *)what, "status", len) == 0) {
		if (env_auto)
			printf("%s%s", "VAR and VALUE are/will be ",
					"determined automatically\r\n");
		if (old_env_var == OLD_ENV_VAR)
			printf("VAR and VALUE set to correct definitions\r\n");
		else
			printf("VAR and VALUE definitions are reversed\r\n");
	} else if (strncasecmp((char *)what, "auto", len) == 0) {
		env_auto = 1;
		old_env_var = OLD_ENV_VALUE;
		old_env_value = OLD_ENV_VAR;
	} else if (strncasecmp((char *)what, "right", len) == 0) {
		env_auto = 0;
		old_env_var = OLD_ENV_VAR;
		old_env_value = OLD_ENV_VALUE;
	} else if (strncasecmp((char *)what, "wrong", len) == 0) {
		env_auto = 0;
		old_env_var = OLD_ENV_VALUE;
		old_env_value = OLD_ENV_VAR;
	} else {
unknown:
		printf("Unknown \"varval\" command. (\"auto\", \"right\", \"wrong\", \"status\")\r\n");
	}
}
#endif

#if	defined(AUTHENTICATION)
/*
 * The AUTHENTICATE command.
 */

struct authlist {
	char	*name;
	char	*help;
	int	(*handler)();
	int	narg;
};

extern int
	auth_enable (char *),
	auth_disable (char *),
	auth_status (void);
static int
	auth_help (void);

struct authlist AuthList[] = {
    { "status",	"Display current status of authentication information",
						auth_status,	0 },
    { "disable", "Disable an authentication type ('auth disable ?' for more)",
						auth_disable,	1 },
    { "enable", "Enable an authentication type ('auth enable ?' for more)",
						auth_enable,	1 },
    { "help",	0,				auth_help,		0 },
    { "?",	"Print help information",	auth_help,		0 },
    { 0 },
};

    static int
auth_help()
{
    struct authlist *c;

    for (c = AuthList; c->name; c++) {
	if (c->help) {
	    if (*c->help)
		printf("%-15s %s\r\n", c->name, c->help);
	    else
		printf("\r\n");
	}
    }
    return 0;
}

int
auth_cmd(argc, argv)
    int  argc;
    char *argv[];
{
    struct authlist *c;

    if (argc < 2) {
      fprintf(stderr,
          "Need an argument to 'auth' command.  'auth ?' for help.\n");
      return 0;
    }

    c = (struct authlist *)
		genget(argv[1], (char **) AuthList, sizeof(struct authlist));
    if (c == 0) {
        fprintf(stderr, "'%s': unknown argument ('auth ?' for help).\n",
    				argv[1]);
        return 0;
    }
    if (Ambiguous(c)) {
        fprintf(stderr, "'%s': ambiguous argument ('auth ?' for help).\n",
    				argv[1]);
        return 0;
    }
    if (c->narg + 2 != argc) {
	fprintf(stderr,
	    "Need %s%d argument%s to 'auth %s' command.  'auth ?' for help.\n",
		c->narg < argc + 2 ? "only " : "",
		c->narg, c->narg == 1 ? "" : "s", c->name);
	return 0;
    }
    return((*c->handler)(argv[2], argv[3]));
}
#endif

#ifdef	ENCRYPTION
/*
 * The ENCRYPT command.
 */

struct encryptlist {
	char	*name;
	char	*help;
	int	(*handler)();
	int	needconnect;
	int	minarg;
	int	maxarg;
};

extern int
	EncryptEnable (char *, char *),
	EncryptDisable (char *, char *),
	EncryptType (char *, char *),
	EncryptStart (char *),
	EncryptStartInput (void),
	EncryptStartOutput (void),
	EncryptStop (char *),
	EncryptStopInput (void),
	EncryptStopOutput (void),
	EncryptStatus (void);
static int
	EncryptHelp (void);

struct encryptlist EncryptList[] = {
    { "enable", "Enable encryption. ('encrypt enable ?' for more)",
						EncryptEnable, 1, 1, 2 },
    { "disable", "Disable encryption. ('encrypt enable ?' for more)",
						EncryptDisable, 0, 1, 2 },
    { "type", "Set encryption type. ('encrypt type ?' for more)",
						EncryptType, 0, 1, 1 },
    { "start", "Start encryption. ('encrypt start ?' for more)",
						EncryptStart, 1, 0, 1 },
    { "stop", "Stop encryption. ('encrypt stop ?' for more)",
						EncryptStop, 1, 0, 1 },
    { "input", "Start encrypting the input stream",
						EncryptStartInput, 1, 0, 0 },
    { "-input", "Stop encrypting the input stream",
						EncryptStopInput, 1, 0, 0 },
    { "output", "Start encrypting the output stream",
						EncryptStartOutput, 1, 0, 0 },
    { "-output", "Stop encrypting the output stream",
						EncryptStopOutput, 1, 0, 0 },

    { "status",	"Display current status of authentication information",
						EncryptStatus,	0, 0, 0 },
    { "help",	0,				EncryptHelp,	0, 0, 0 },
    { "?",	"Print help information",	EncryptHelp,	0, 0, 0 },
    { 0 },
};

    static int
EncryptHelp()
{
    struct encryptlist *c;

    for (c = EncryptList; c->name; c++) {
	if (c->help) {
	    if (*c->help)
		printf("%-15s %s\r\n", c->name, c->help);
	    else
		printf("\r\n");
	}
    }
    return 0;
}

int
encrypt_cmd(argc, argv)
    int  argc;
    char *argv[];
{
    struct encryptlist *c;

    if (argc < 2) {
	fprintf(stderr,
	    "Need an argument to 'encrypt' command.  'encrypt ?' for help.\n");
	return 0;
    }

    c = (struct encryptlist *)
		genget(argv[1], (char **) EncryptList, sizeof(struct encryptlist));
    if (c == 0) {
        fprintf(stderr, "'%s': unknown argument ('encrypt ?' for help).\n",
    				argv[1]);
        return 0;
    }
    if (Ambiguous(c)) {
        fprintf(stderr, "'%s': ambiguous argument ('encrypt ?' for help).\n",
    				argv[1]);
        return 0;
    }
    argc -= 2;
    if (argc < c->minarg || argc > c->maxarg) {
	if (c->minarg == c->maxarg) {
	    fprintf(stderr, "Need %s%d argument%s ",
		c->minarg < argc ? "only " : "", c->minarg,
		c->minarg == 1 ? "" : "s");
	} else {
	    fprintf(stderr, "Need %s%d-%d arguments ",
		c->maxarg < argc ? "only " : "", c->minarg, c->maxarg);
	}
	fprintf(stderr, "to 'encrypt %s' command.  'encrypt ?' for help.\n",
		c->name);
	return 0;
    }
    if (c->needconnect && !connected) {
	if (!(argc && (isprefix(argv[2], "help") || isprefix(argv[2], "?")))) {
	    printf("?Need to be connected first.\r\n");
	    return 0;
	}
    }
    return ((*c->handler)(argc > 0 ? argv[2] : 0,
			argc > 1 ? argv[3] : 0,
			argc > 2 ? argv[4] : 0));
}
#endif	/* ENCRYPTION */

#if	defined(FORWARD)

/*
 * The FORWARD command.
 */


extern int forward_flags;

struct forwlist {
	char	*name;
	char	*help;
	int	(*handler)();
	int	f_flags;
};

static int
	forw_status (void),
	forw_set (int),
	forw_help (void);

struct forwlist ForwList[] = {
    { "status",	"Display current status of credential forwarding",
						forw_status,	0 },
    { "disable", "Disable credential forwarding",
						forw_set,	0 },
    { "enable", "Enable credential forwarding",
						forw_set,
						OPTS_FORWARD_CREDS },
    { "forwardable", "Enable credential forwarding of forwardable credentials",
						forw_set,
						OPTS_FORWARD_CREDS |
						OPTS_FORWARDABLE_CREDS },
    { "help",	0,				forw_help,		0 },
    { "?",	"Print help information",	forw_help,		0 },
    { 0 },
};

    static int
forw_status()
{
    if (forward_flags & OPTS_FORWARD_CREDS) {
	if (forward_flags & OPTS_FORWARDABLE_CREDS) {
	    printf("Credential forwarding of forwardable credentials enabled\n");
	} else {
	    printf("Credential forwarding enabled\n");
	}
    } else {
	printf("Credential forwarding disabled\n");
    }
    return(0);
}

int
forw_set(f_flags)
     int f_flags;
{
    forward_flags = f_flags;
    return(0);
}

static int
forw_help()
{
    struct forwlist *c;

    for (c = ForwList; c->name; c++) {
	if (c->help) {
	    if (*c->help)
		printf("%-15s %s\n", c->name, c->help);
	    else
		printf("\n");
	}
    }
    return 0;
}

static int
forw_cmd(argc, argv)
    int  argc;
    char *argv[];
{
    struct forwlist *c;

    if (argc < 2) {
      fprintf(stderr,
          "Need an argument to 'forward' command.  'forward ?' for help.\n");
      return 0;
    }

    c = (struct forwlist *)
		genget(argv[1], (char **) ForwList, sizeof(struct forwlist));
    if (c == 0) {
        fprintf(stderr, "'%s': unknown argument ('forw ?' for help).\n",
    				argv[1]);
        return 0;
    }
    if (Ambiguous(c)) {
        fprintf(stderr, "'%s': ambiguous argument ('forw ?' for help).\n",
    				argv[1]);
        return 0;
    }
    if (argc != 2) {
	fprintf(stderr,
       "No arguments needed to 'forward %s' command.  'forward ?' for help.\n",
		c->name);
	return 0;
    }
    return((*c->handler)(c->f_flags));
}
#endif

#if	defined(unix) && defined(TN3270)
    static void
filestuff(fd)
    int fd;
{
    int res;

#ifdef	F_GETOWN
    setconnmode(0);
    res = fcntl(fd, F_GETOWN, 0);
    setcommandmode();

    if (res == -1) {
	perror("fcntl");
	return;
    }
    printf("\tOwner is %d.\r\n", res);
#endif

    setconnmode(0);
    res = fcntl(fd, F_GETFL, 0);
    setcommandmode();

    if (res == -1) {
	perror("fcntl");
	return;
    }
#ifdef notdef
    printf("\tFlags are 0x%x: %s\r\n", res, decodeflags(res));
#endif
}
#endif /* defined(unix) && defined(TN3270) */

/*
 * Print status about the connection.
 */
    /*ARGSUSED*/
static int
status(argc, argv)
    int	 argc;
    char *argv[];
{
    if (connected) {
	printf("Connected to %s (%s).\r\n", hostname, hostaddrstring);
	if ((argc < 2) || strcmp(argv[1], "notmuch")) {
	    int mode = getconnmode();

	    if (my_want_state_is_will(TELOPT_LINEMODE)) {
		printf("Operating with LINEMODE option\r\n");
		printf("%s line editing\r\n", (mode&MODE_EDIT) ? "Local" : "No");
		printf("%s catching of signals\r\n",
					(mode&MODE_TRAPSIG) ? "Local" : "No");
		slcstate();
#ifdef	KLUDGELINEMODE
	    } else if (kludgelinemode && my_want_state_is_dont(TELOPT_SGA)) {
		printf("Operating in obsolete linemode\r\n");
#endif
	    } else {
		printf("Operating in single character mode\r\n");
		if (localchars)
		    printf("Catching signals locally\r\n");
	    }
	    printf("%s character echo\r\n", (mode&MODE_ECHO) ? "Local" : "Remote");
	    if (my_want_state_is_will(TELOPT_LFLOW))
		printf("%s flow control\r\n", (mode&MODE_FLOW) ? "Local" : "No");
#ifdef	ENCRYPTION
	    encrypt_display();
#endif	/* ENCRYPTION */
	}
    } else {
	printf("No connection.\r\n");
    }
#   if !defined(TN3270)
    printf("Escape character is '%s'.\r\n", control(escape));
    (void) fflush(stdout);
#   else /* !defined(TN3270) */
    if ((!In3270) && ((argc < 2) || strcmp(argv[1], "notmuch"))) {
	printf("Escape character is '%s'.\r\n", control(escape));
    }
#   if defined(unix)
    if ((argc >= 2) && !strcmp(argv[1], "everything")) {
	printf("SIGIO received %d time%s.\r\n",
				sigiocount, (sigiocount == 1)? "":"s");
	if (In3270) {
	    printf("Process ID %d, process group %d.\r\n",
					    getpid(), getpgrp(getpid()));
	    printf("Terminal input:\r\n");
	    filestuff(tin);
	    printf("Terminal output:\r\n");
	    filestuff(tout);
	    printf("Network socket:\r\n");
	    filestuff(net);
	}
    }
    if (In3270 && transcom) {
       printf("Transparent mode command is '%s'.\r\n", transcom);
    }
#   endif /* defined(unix) */
    (void) fflush(stdout);
    if (In3270) {
	return 0;
    }
#   endif /* defined(TN3270) */
    return 1;
}

#ifdef	SIGINFO
/*
 * Function that gets called when SIGINFO is received.
 */
#if	defined(CRAY) || (defined(USE_TERMIO) && !defined(SYSV_TERMIO))
void 
ayt_status()
{
    (void) call(status, "status", "notmuch", 0);
}
#else
int
ayt_status()
{
    (void) call(status, "status", "notmuch", 0);
    return 0;
}
#endif
#endif

    int
tn(argc, argv)
    int argc;
    char *argv[];
{
#if	defined(IP_OPTIONS) && defined(IPPROTO_IP)
    char *srp = 0;
    int srlen = 0;
#endif
    char *cmd, *hostp = 0, *portp = 0, *volatile user = 0;
    struct addrinfo *addrs = 0, *addrp;
    struct addrinfo hints;
    int error;

    if (connected) {
	printf("?Already connected to %s\r\n", hostname);
	return 0;
    }
    if (argc < 2) {
	(void) strlcpy(line, "open ", sizeof(line));
	printf("(to) ");
	(void) fgets(&line[strlen(line)], (int) (sizeof(line) - strlen(line)),
		     stdin);
	makeargv();
	argc = margc;
	argv = margv;
    }
    cmd = *argv;
    --argc; ++argv;
    while (argc) {
	if (isprefix(*argv, "?"))
	    goto usage;
	if (strcmp(*argv, "-l") == 0) {
	    --argc; ++argv;
	    if (argc == 0)
		goto usage;
	    user = *argv++;
	    --argc;
	    continue;
	}
	if (strcmp(*argv, "-a") == 0) {
	    --argc; ++argv;
	    autologin = 1;
	    continue;
	}
	if (hostp == 0) {
	    hostp = *argv++;
	    --argc;
	    continue;
	}
	if (portp == 0) {
	    portp = *argv++;
	    --argc;
	    continue;
	}
    usage:
	printf("usage: %s [-l user] [-a] host-name [port]\r\n", cmd);
	return 0;
    }
    if (hostp == 0)
	goto usage;

    if (portp) {
	if (*portp == '-') {
	    portp++;
	    telnetport = 1;
	} else
	    telnetport = 0;
    } else {
	portp = "telnet";
	telnetport = 1;
    }

#if	defined(IP_OPTIONS) && defined(IPPROTO_IP)
    if (hostp[0] == '@' || hostp[0] == '!') {
	static struct sockaddr_in sr_sin4;
	static struct addrinfo sr_addr;
	unsigned long temp;
	if ((hostname = strrchr(hostp, ':')) == NULL)
	    hostname = strrchr(hostp, '@');
	hostname++;
	srp = 0;
	temp = sourceroute(hostp, &srp, &srlen);
	if (temp == 0) {
	    herror(srp);
	    return 0;
	} else if (temp == (unsigned long) -1) {
	    printf("Bad source route option: %s\r\n", hostp);
	    return 0;
	} else {
	    sr_sin4.sin_addr.s_addr = temp;
	    sr_sin4.sin_family = AF_INET;
#ifdef HAVE_SA_LEN
	    sr_sin4.sin_len = sizeof (sr_sin4);
#endif
	    sr_addr.ai_family = AF_INET;
	    sr_addr.ai_addrlen = sizeof (sr_sin4);
	    sr_addr.ai_addr = (struct sockaddr *) &sr_sin4;
	    sr_addr.ai_next = 0;
	    sr_addr.ai_canonname = hostname;
	    addrs = &sr_addr;
	}
    } else {
#endif
	memset (&hints, 0, sizeof (hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = PF_UNSPEC;


	/* The GNU Libc (Red Hat Linux 6.1, on x86, which MIT is using
	   at this time) implementation seems to completely ignore
	   AI_NUMERICHOST, and contacts DNS anyways.  But other
	   versions will not, and we do want to treat the two cases a
	   little differently.  */
#ifdef AF_INET6
#define IS_NUMERIC_ADDR(P) \
	('\0' == (P)[strspn((P), (strchr((P),':') ? "abcdefABCDEF:0123456789." : "0123456789."))])
#else
#define IS_NUMERIC_ADDR(P) \
	('\0' == (P)[strspn((P), "0123456789.")])
#endif
	if (! IS_NUMERIC_ADDR (hostp))
	    goto not_numeric;


	hints.ai_flags = AI_NUMERICHOST;
	error = getaddrinfo (hostp, portp, &hints, &addrs);
	if (error == 0) {
	    if (getnameinfo (addrs->ai_addr, addrs->ai_addrlen,
			     _hostname, sizeof(_hostname), 0, 0, NI_NAMEREQD) != 0)
		strncpy(_hostname, hostp, sizeof (_hostname));
	    hostname = _hostname;
	} else {
	not_numeric:
	    hints.ai_flags = AI_CANONNAME;
	    error = getaddrinfo (hostp, portp, &hints, &addrs);
	    if (error == 0) {

		/* Stupid glibc lossage again.  */
		if (! IS_NUMERIC_ADDR (addrs->ai_canonname)) {
		    strncpy(_hostname, addrs->ai_canonname, sizeof(_hostname));
		} else {
		    fprintf (stderr,
			     "telnet: system library bug? getaddrinfo returns numeric address\n"
			     "\tas canonical name of %s\n",
			     hostp);
		    strncpy(_hostname, hostp, sizeof (_hostname));
		}

	    } else {
		strncpy(_hostname, hostp, sizeof (_hostname));
	    }
	    hostname = _hostname;
	}
	if (error) {
	    fprintf (stderr, "%s/%s: %s\n", hostp, portp, gai_strerror (error));
	    return 0;
	}
#if	defined(IP_OPTIONS) && defined(IPPROTO_IP)
    }
#endif
    for (addrp = addrs; addrp && !connected; addrp = addrp->ai_next) {
	error = getnameinfo (addrp->ai_addr, addrp->ai_addrlen,
			     hostaddrstring, sizeof (hostaddrstring),
			     (char *) NULL, 0, NI_NUMERICHOST);
	if (error) {
	    fprintf (stderr, "getnameinfo() error printing address: %s\n",
		     gai_strerror (error));
	    strlcpy (hostaddrstring, "[address unprintable]",
		     sizeof(hostaddrstring));
	}
	printf("Trying %s...\r\n", hostaddrstring);
#if	defined(IP_OPTIONS) && defined(IPPROTO_IP)
	if (srp && addrp->ai_family != AF_INET) {
	    printf ("source routing not supported (yet) for address family,"
		    " trying another address\n");
	    continue;
	}
#endif
	net = socket(addrp->ai_family, SOCK_STREAM, 0);
	if (net < 0) {
	    perror("telnet: socket");
	    continue;
	}
#if	defined(IP_OPTIONS) && defined(IPPROTO_IP)
	if (srp) {
	    if (addrp->ai_family != AF_INET)
		printf ("source routing not supported (yet)"
			" for address family\n");
	    else if (setsockopt(net, IPPROTO_IP, IP_OPTIONS,
				(char *)srp, srlen) < 0)
		perror("setsockopt (IP_OPTIONS)");
	}
#endif
#if	defined(IPPROTO_IP) && defined(IP_TOS)
	if (addrp->ai_family == AF_INET) {
# if	defined(HAVE_GETTOSBYNAME)
	    struct tosent *tp;
	    if (tos < 0 && (tp = gettosbyname("telnet", "tcp")))
		tos = tp->t_tos;
# endif
	    if (tos < 0)
		tos = 020;	/* Low Delay bit */
	    if (tos
		&& (setsockopt(net, IPPROTO_IP, IP_TOS,
		    (char *)&tos, sizeof(int)) < 0)
		&& (errno != ENOPROTOOPT))
		    perror("telnet: setsockopt (IP_TOS) (ignored)");
	}
#endif	/* defined(IPPROTO_IP) && defined(IP_TOS) */

	if (debug && SetSockOpt(net, SOL_SOCKET, SO_DEBUG, 1) < 0) {
		perror("setsockopt (SO_DEBUG)");
	}

	if (connect(net, addrp->ai_addr, addrp->ai_addrlen) < 0) {
	    if (hostaddrstring[0]) {
		fprintf(stderr, "telnet: connect to address %s: %s\n",
			hostaddrstring, strerror (errno));
		(void) NetClose(net);
		continue;
	    }
	}
	connected++;

#if	defined(AUTHENTICATION) || defined(ENCRYPTION)
	auth_encrypt_connect(connected);
#endif	/* defined(AUTHENTICATION) || defined(ENCRYPTION) */
    }
    if (!connected) {
	perror("telnet: Unable to connect to remote host");
	return 0;
    }
    if (user)
      user = strdup(user);
    if (hostp)
      hostp = strdup(hostp);
    cmdrc(hostp, hostname);
    if (hostp)
      free(hostp);
    if (autologin && user == NULL) {
	struct passwd *pw;

	user = getenv("USER");
	if (user == NULL ||
	    ((pw = getpwnam(user)) && pw->pw_uid != getuid())) {
	        pw = getpwuid(getuid());
		if (pw)
			user = pw->pw_name;
		else
			user = NULL;
	}
	if (user)
	  user = strdup(user);
    }
    if (user) {
	env_define((unsigned char *)"USER", (unsigned char *)user);
	env_export((unsigned char *)"USER");
    }
    (void) call(status, "status", "notmuch", 0);
    if (setjmp(peerdied) == 0)
	telnet(user);
    if (user)
      free(user);
    (void) NetClose(net);
    ExitString("Connection closed by foreign host.\r\n",1);
    /*NOTREACHED*/
    return 0;
}

#define HELPINDENT ((int) sizeof ("connect"))

static char
	openhelp[] =	"connect to a site",
	closehelp[] =	"close current connection",
	logouthelp[] =	"forcibly logout remote user and close the connection",
	quithelp[] =	"exit telnet",
	statushelp[] =	"print status information",
	helphelp[] =	"print help information",
	sendhelp[] =	"transmit special characters ('send ?' for more)",
	sethelp[] = 	"set operating parameters ('set ?' for more)",
	unsethelp[] = 	"unset operating parameters ('unset ?' for more)",
	togglestring[] ="toggle operating parameters ('toggle ?' for more)",
	slchelp[] =	"change state of special charaters ('slc ?' for more)",
	displayhelp[] =	"display operating parameters",
#if	defined(TN3270) && defined(unix)
	transcomhelp[] = "specify Unix command for transparent mode pipe",
#endif	/* defined(TN3270) && defined(unix) */
#if	defined(AUTHENTICATION)
	authhelp[] =	"turn on (off) authentication ('auth ?' for more)",
#endif
#ifdef	ENCRYPTION
	encrypthelp[] =	"turn on (off) encryption ('encrypt ?' for more)",
#endif	/* ENCRYPTION */
#ifdef  FORWARD
	forwardhelp[] = "turn on (off) credential forwarding ('forward ?' for more)",
#endif
#if	defined(unix)
	zhelp[] =	"suspend telnet",
#endif	/* defined(unix) */
	shellhelp[] =	"invoke a subshell",
	envhelp[] =	"change environment variables ('environ ?' for more)",
	modestring[] = "try to enter line or character mode ('mode ?' for more)";

static int	help();

static Command cmdtab[] = {
	{ "close",	closehelp,	bye,		1 },
	{ "logout",	logouthelp,	logout,		1 },
	{ "display",	displayhelp,	display,	0 },
	{ "mode",	modestring,	modecmd,	0 },
	{ "open",	openhelp,	tn,		0 },
	{ "quit",	quithelp,	quit,		0 },
	{ "send",	sendhelp,	sendcmd,	0 },
	{ "set",	sethelp,	setcmd,		0 },
	{ "unset",	unsethelp,	unsetcmd,	0 },
	{ "status",	statushelp,	status,		0 },
	{ "toggle",	togglestring,	toggle,		0 },
	{ "slc",	slchelp,	slccmd,		0 },
#if	defined(TN3270) && defined(unix)
	{ "transcom",	transcomhelp,	settranscom,	0 },
#endif	/* defined(TN3270) && defined(unix) */
#if	defined(AUTHENTICATION)
	{ "auth",	authhelp,	auth_cmd,	0 },
#endif
#ifdef	ENCRYPTION
	{ "encrypt",	encrypthelp,	encrypt_cmd,	0 },
#endif	/* ENCRYPTION */
#ifdef  FORWARD
	{ "forward",    forwardhelp,    forw_cmd,       0 },
#endif
#if	defined(unix)
	{ "z",		zhelp,		suspend,	0 },
#endif	/* defined(unix) */
#if	defined(TN3270)
	{ "!",		shellhelp,	shell,		1 },
#else
	{ "!",		shellhelp,	shell,		0 },
#endif
	{ "environ",	envhelp,	env_cmd,	0 },
	{ "?",		helphelp,	help,		0 },
	{  0,           0,              0,              0 }
};

static char	crmodhelp[] =	"deprecated command -- use 'toggle crmod' instead";
static char	escapehelp[] =	"deprecated command -- use 'set escape' instead";

static Command cmdtab2[] = {
	{ "help",	0,		help,		0 },
	{ "escape",	escapehelp,	setescape,	0 },
	{ "crmod",	crmodhelp,	togcrmod,	0 },
	{  0,           0,              0,              0 }
};


/*
 * Call routine with argc, argv set from args (terminated by 0).
 */

    /*VARARGS1*/
static int
#ifdef HAVE_STDARG_H
call(intrtn_t routine, ...)
#else
call(routine, va_alist)
    intrtn_t routine;
    va_dcl
#endif
{
    va_list ap;
    char *args[100];
    int argno = 0;

#ifdef HAVE_STDARG_H
    va_start(ap, routine);
#else
    va_start(ap);
#endif

    while ((args[argno++] = va_arg(ap, char *)) != 0) {
	;
    }
    va_end(ap);
    return (*routine)(argno-1, args);
}


    static Command *
getcmd(name)
    char *name;
{
    Command *cm;

    if ((cm = (Command *) genget(name, (char **) cmdtab, sizeof(Command))))
	return cm;
    return (Command *) genget(name, (char **) cmdtab2, sizeof(Command));
}

    void
command(top, tbuf, cnt)
    int top;
    char *tbuf;
    int cnt;
{
    register Command *c;

    setcommandmode();
    if (!top) {
	putchar('\n');
#if	defined(unix)
    } else {
	(void) signal(SIGINT, SIG_DFL);
	(void) signal(SIGQUIT, SIG_DFL);
#endif	/* defined(unix) */
    }
    for (;;) {
	if (rlogin == _POSIX_VDISABLE)
		printf("%s> ", prompt);
	if (tbuf) {
	    register char *cp;
	    cp = line;
	    while (cnt > 0 && (*cp++ = *tbuf++) != '\n')
		cnt--;
	    tbuf = 0;
	    if (cp == line || *--cp != '\n' || cp == line)
		goto getline;
	    *cp = '\0';
	    if (rlogin == _POSIX_VDISABLE)
	        printf("%s\r\n", line);
	} else {
	getline:
	    if (rlogin != _POSIX_VDISABLE)
		printf("%s> ", prompt);
	    if (fgets(line, sizeof(line), stdin) == NULL) {
		if (feof(stdin) || ferror(stdin)) {
		    (void) quit(0, NULL);
		    /*NOTREACHED*/
		}
		break;
	    }
	}
	if (line[0] == 0)
	    break;
	makeargv();
	if (margv[0] == 0) {
	    break;
	}
	c = getcmd(margv[0]);
	if (Ambiguous(c)) {
	    printf("?Ambiguous command\r\n");
	    continue;
	}
	if (c == 0) {
	    printf("?Invalid command\r\n");
	    continue;
	}
	if (c->needconnect && !connected) {
	    printf("?Need to be connected first.\r\n");
	    continue;
	}
	if ((*c->handler)(margc, margv)) {
	    break;
	}
    }
    if (!top) {
	if (!connected) {
	    longjmp(toplevel, 1);
	    /*NOTREACHED*/
	}
#if	defined(TN3270)
	if (shell_active == 0) {
	    setconnmode(0);
	}
#else	/* defined(TN3270) */
	setconnmode(0);
#endif	/* defined(TN3270) */
    }
}

/*
 * Help command.
 */
static int
help(argc, argv)
	int argc;
	char *argv[];
{
	register Command *c;

	if (argc == 1) {
		printf("Commands may be abbreviated.  Commands are:\r\n\r\n");
		for (c = cmdtab; c->name; c++)
			if (c->help) {
				printf("%-*s\t%s\r\n", HELPINDENT, c->name,
								    c->help);
			}
		return 0;
	}
	while (--argc > 0) {
		register char *arg;
		arg = *++argv;
		c = getcmd(arg);
		if (Ambiguous(c))
			printf("?Ambiguous help command %s\r\n", arg);
		else if (c == (Command *)0)
			printf("?Invalid help command %s\r\n", arg);
		else
			printf("%s\r\n", c->help);
	}
	return 0;
}

static char *rcname = 0;
static char rcbuf[128];

void
cmdrc(m1, m2)
	char *m1, *m2;
{
    register Command *c;
    FILE *rcfile;
    int gotmachine = 0;
    unsigned int l1 = strlen(m1);
    unsigned int l2 = strlen(m2);
    char m1save[64];

    if (skiprc)
	return;

    strncpy(m1save, m1, sizeof(m1save) - 1);
    m1save[sizeof(m1save) - 1] = '\0';
    m1 = m1save;

    if (rcname == 0) {
	rcname = getenv("HOME");
	if (rcname)
	    strncpy(rcbuf, rcname, sizeof(rcbuf) - 1);
	else
	    rcbuf[0] = '\0';
	rcbuf[sizeof(rcbuf) - 1] = '\0';
	strncat(rcbuf, "/.telnetrc", sizeof(rcbuf) - 1 - strlen(rcbuf));
	rcname = rcbuf;
    }

    if ((rcfile = fopen(rcname, "r")) == 0) {
	return;
    }

    for (;;) {
	if (fgets(line, sizeof(line), rcfile) == NULL)
	    break;
	if (line[0] == 0)
	    break;
	if (line[0] == '#')
	    continue;
	if (gotmachine) {
	    if (!isspace((int) line[0]))
		gotmachine = 0;
	}
	if (gotmachine == 0) {
	    if (isspace((int) line[0]))
		continue;
	    if (strncasecmp(line, m1, l1) == 0)
		strncpy(line, &line[l1], sizeof(line) - l1);
	    else if (strncasecmp(line, m2, l2) == 0)
		strncpy(line, &line[l2], sizeof(line) - l2);
	    else if (strncasecmp(line, "DEFAULT", 7) == 0)
		strncpy(line, &line[7], sizeof(line) - 7);
	    else
		continue;
	    if (line[0] != ' ' && line[0] != '\t' && line[0] != '\n')
		continue;
	    gotmachine = 1;
	}
	makeargv();
	if (margv[0] == 0)
	    continue;
	c = getcmd(margv[0]);
	if (Ambiguous(c)) {
	    printf("?Ambiguous command: %s\r\n", margv[0]);
	    continue;
	}
	if (c == 0) {
	    printf("?Invalid command: %s\r\n", margv[0]);
	    continue;
	}
	/*
	 * This should never happen...
	 */
	if (c->needconnect && !connected) {
	    printf("?Need to be connected first for %s.\r\n", margv[0]);
	    continue;
	}
	(*c->handler)(margc, margv);
    }
    fclose(rcfile);
}

#if	defined(IP_OPTIONS) && defined(IPPROTO_IP)

/*
 * Source route is handed in as
 *	[!]@hop1@hop2...[@|:]dst
 * If the leading ! is present, it is a
 * strict source route, otherwise it is
 * assmed to be a loose source route.
 *
 * We fill in the source route option as
 *	hop1,hop2,hop3...dest
 * and return a pointer to hop1, which will
 * be the address to connect() to.
 *
 * Arguments:
 *	arg:	pointer to route list to decipher
 *
 *	cpp: 	If *cpp is not equal to NULL, this is a
 *		pointer to a pointer to a character array
 *		that should be filled in with the option.
 *
 *	lenp:	pointer to an integer that contains the
 *		length of *cpp if *cpp != NULL.
 *
 * Return values:
 *
 *	Returns the address of the host to connect to.  If the
 *	return value is -1, there was a syntax error in the
 *	option, either unknown characters, or too many hosts.
 *	If the return value is 0, one of the hostnames in the
 *	path is unknown, and *cpp is set to point to the bad
 *	hostname.
 *
 *	*cpp:	If *cpp was equal to NULL, it will be filled
 *		in with a pointer to our static area that has
 *		the option filled in.  This will be 32bit aligned.
 * 
 *	*lenp:	This will be filled in with how long the option
 *		pointed to by *cpp is.
 *	
 */
static	unsigned long
sourceroute(arg, cpp, lenp)
	char	*arg;
	char	**cpp;
	int	*lenp;
{
	static char lsr[44];
#ifdef	sysV88
	static IOPTN ipopt;
#endif
	char *cp, *cp2, *lsrp, *lsrep;
	register int tmp;
	struct in_addr sin_addr;
	register struct hostent *host = 0;
	register char c;

	/*
	 * Verify the arguments, and make sure we have
	 * at least 7 bytes for the option.
	 */
	if (cpp == NULL || lenp == NULL)
		return((unsigned long)-1);
	if (*cpp != NULL && *lenp < 7)
		return((unsigned long)-1);
	/*
	 * Decide whether we have a buffer passed to us,
	 * or if we need to use our own static buffer.
	 */
	if (*cpp) {
		lsrp = *cpp;
		lsrep = lsrp + *lenp;
	} else {
		*cpp = lsrp = lsr;
		lsrep = lsrp + 44;
	}

	cp = arg;

	/*
	 * Next, decide whether we have a loose source
	 * route or a strict source route, and fill in
	 * the begining of the option.
	 */
#ifndef	sysV88
	if (*cp == '!') {
		cp++;
		*lsrp++ = IPOPT_SSRR;
	} else
		*lsrp++ = IPOPT_LSRR;
#else
	if (*cp == '!') {
		cp++;
		ipopt.io_type = IPOPT_SSRR;
	} else
		ipopt.io_type = IPOPT_LSRR;
#endif

	if (*cp != '@')
		return((unsigned long)-1);

#ifndef	sysV88
	lsrp++;		/* skip over length, we'll fill it in later */
	*lsrp++ = 4;
#endif

	cp++;

	sin_addr.s_addr = 0;

	for (c = 0;;) {
		if (c == ':')
			cp2 = 0;
		else for (cp2 = cp; (c = *cp2); cp2++) {
			if (c == ',') {
				*cp2++ = '\0';
				if (*cp2 == '@')
					cp2++;
			} else if (c == '@') {
				*cp2++ = '\0';
			} else if (c == ':') {
				*cp2++ = '\0';
			} else
				continue;
			break;
		}
		if (!c)
			cp2 = 0;

		if ((tmp = inet_addr(cp)) != -1) {
			sin_addr.s_addr = tmp;
		} else if ((host = gethostbyname(cp))) {
#if	defined(h_addr)
			memcpy(&sin_addr,
			        host->h_addr_list[0], sizeof(sin_addr));
#else
			memcpy(&sin_addr, host->h_addr, sizeof(sin_addr));
#endif
		} else {
			*cpp = cp;
			return(0);
		}
		memcpy(lsrp, &sin_addr, 4);
		lsrp += 4;
		if (cp2)
			cp = cp2;
		else
			break;
		/*
		 * Check to make sure there is space for next address
		 */
		if (lsrp + 4 > lsrep)
			return((unsigned long)-1);
	}
#ifndef	sysV88
	if ((*(*cpp+IPOPT_OLEN) = lsrp - *cpp) <= 7) {
		*cpp = 0;
		*lenp = 0;
		return((unsigned long)-1);
	}
	*lsrp++ = IPOPT_NOP; /* 32 bit word align it */
	*lenp = lsrp - *cpp;
#else
	ipopt.io_len = lsrp - *cpp;
	if (ipopt.io_len <= 5) {		/* Is 3 better ? */
		*cpp = 0;
		*lenp = 0;
		return((unsigned long)-1);
	}
	*lenp = sizeof(ipopt);
	*cpp = (char *) &ipopt;
#endif
	return(sin_addr.s_addr);
}
#endif
