/******************************************************************************
  Copyright (c) 1995, 1996 Xerox Corporation.  All rights reserved.
  Portions of this code were written by Stephen White, aka ghond.
  Use and copying of this software and preparation of derivative works based
  upon this software are permitted.  Any distribution of this software or
  derivative works must comply with all applicable United States export
  control laws.  This software is made available AS IS, and Xerox Corporation
  makes no warranty about the software, its performance or its conformity to
  any specification.  Any person obtaining a copy of this software is requested
  to send their name and post office or electronic mail address to:
    Pavel Curtis
    Xerox PARC
    3333 Coyote Hill Rd.
    Palo Alto, CA 94304
    Pavel@Xerox.Com
 *****************************************************************************/

/* Experimenting

 * This module contains an example of how to add functionality to the
 * MOO server using interfaces exported by various other modules.  The
 * example itself is commented out, since it's really not all that
 * useful in general; it was written primarily to test out the
 * interfaces it uses.
 *
 * The uncommented parts of this module provide one possible skeleton
 * for a module that implements new MOO built-in functions.  Replacing
 * the commented-out parts is a quick way to try out your own function
 * definitions.  In future releases, this file will be kept as a
 * convenient place to experiment with new server functionality; it
 * will not contain any live code that the server actually depends on,
 * or, rather, nothing beyond the register_experiments() function with
 * its #ifdef-ed out body that *does*, in fact, get called on every
 * server startup even though it does nothing by default (i.e.,
 * assuming you leave it unmodified).
 */

/*\ Random explanatory meta-comments that do not need to appear
 *| in any Actual File will look like this.  Enjoy.  --wrog
 */

/*\ (Historical note:  This file *was* originally named extensions.c,
 *|  but has since been renamed to better reflect its role as a
 *|  playground now that (25 years later) there is a somewhat more
 *|  developed extension framework and with a bit more thought put
 *|  into how extensions should be defined, behave, and interact with
 *|  each other in the server context.  See Extensions.md for what you
 *|  need to know once you are ready to migrate code *out* of this
 *|  file and into an actual extension.)
 */

/*\ 1st block of includes for a .c file answers this question:
 *| What interfaces are we exporting or contributing to?
 *|
 *| As per usual C practice, declare anything of use to other modules
 *| in experiments.h.  And usually there will just be one file here.
 *|
 *| However, because *this* file also defines a registration function
 *| (register_experiments()), and, as a matter of convention and unlike
 *| all other interfaces, the registration functions are all "owned" by
 *| the bf_register module (even though they do not actually live in
 *| bf_register.c; see explanation in bf_register.h), there will, in
 *| fact be a 2nd file to mention here:
 */
#include "experiments.h"
#include "bf_register.h"

/*\ The following is something that only *this* file does because
 *| we are blocking out everything that actually does anything.
 */
#define EXAMPLE 0
#if EXAMPLE

/*\ 2nd block of includes for a .c file:  config.h and options.h
 *|
 *| Yes the headers would have included them if they needed them, but
 *| maybe they didn't.  Headers tend to be conservative about this.
 *| It is, however, pretty much guaranteed you'll eventually reference
 *| something from there, so you may as well put these in now and get
 *| it over with:
 */
#include "config.h"
#include "options.h"

/*\ 3rd block of includes for a .c file:  System-corrective headers.
 *| Many of these are explained in the various stanzas of config.h.in
 */
#include "my-unistd.h"

/*\ 4th block of includes for a .c file:
 *| Actual headers needed from other parts of the server.
 *| Alphbetical order is fine (see experiments.h comment).
 */
#include "exceptions.h"
#include "functions.h"
#include "log.h"
#include "network.h"
#include "net_multi.h"
#include "storage.h"
#include "tasks.h"

/*\ Unless you're doing something funky (like Pavel did with the networking .h files)
 *| do NOT bury any #includes further down the file where they
 *|   (1) will NOT be seen, and
 *|   (2) might possibly make code in front of them behave differently
 *|       because they haven't been #included yet,
 *|       which will be super-mysterious to debug,
 *|       and whoever gets stuck doing that will hate you forever.
 */

/*\ Begin actual content of the file:
 */
typedef struct stdin_waiter {
    struct stdin_waiter *next;
    vm the_vm;
} stdin_waiter;

static stdin_waiter *waiters = 0;

static task_enum_action
stdin_enumerator(task_closure closure, void *data)
{
    stdin_waiter **ww;

    for (ww = &waiters; *ww; ww = &((*ww)->next)) {
	stdin_waiter *w = *ww;
	const char *status = (w->the_vm->task_id & 1
			      ? "stdin-waiting"
			      : "stdin-weighting");
	task_enum_action tea = (*closure) (w->the_vm, status, data);

	if (tea == TEA_KILL) {
	    *ww = w->next;
	    myfree(w, M_TASK);
	    if (!waiters)
		network_unregister_fd(0);
	}
	if (tea != TEA_CONTINUE)
	    return tea;
    }

    return TEA_CONTINUE;
}

static void
stdin_readable(int fd UNUSED_, void *data)
{
    char buffer[1000];
    int n;
    Var v;
    stdin_waiter *w;

    if (data != &waiters)
	panic("STDIN_READABLE: Bad data!");

    if (!waiters) {
	errlog("STDIN_READABLE: Nobody cares!\n");
	return;
    }
    n = read(0, buffer, sizeof(buffer));
    buffer[n] = '\0';
    while (n)
	if (buffer[--n] == '\n')
	    buffer[n] = 'X';

    if (buffer[0] == 'a') {
	v.type = TYPE_ERR;
	v.v.err = E_NACC;
    } else {
	v.type = TYPE_STR;
	v.v.str = str_dup(buffer);
    }

    resume_task(waiters->the_vm, v);
    w = waiters->next;
    myfree(waiters, M_TASK);
    waiters = w;
    if (!waiters)
	network_unregister_fd(0);
}

static enum error
stdin_suspender(vm the_vm, void *data)
{
    stdin_waiter *w = data;

    if (!waiters)
	network_register_fd(0, stdin_readable, 0, &waiters);

    w->the_vm = the_vm;
    w->next = waiters;
    waiters = w;

    return E_NONE;
}

static void
free_stdin_waiter(void *data)
{
    myfree(data, M_TASK);
}

static package
bf_read_stdin(Var arglist UNUSED_, Byte next UNUSED_, void *vdata UNUSED_, Objid progr UNUSED_)
{
    stdin_waiter *w = mymalloc(sizeof(stdin_waiter), M_TASK);

    return make_suspend_pack_with_cancel(stdin_suspender, w, free_stdin_waiter);
}

/*\ Again, something that only *this* file does because we are
 *| do not want to have actual content.  Also never forget to comment
 *| your #endifs if the corresponding #ifs are more than 1 screen above.
 */
#endif				/* EXAMPLE */

/*\ If the file defines a registration function, it should be the last
 *| thing, or, at least, that's where people will expect to find it.
 *| Also, a file will typically only need one of these, though if
 *| there are weird #ifdef situations, then fine; do what you need to.
 *| Alsoalso, notice how experiments.h does not have this declaration.
 */
void
register_experiments(void)
{
#if EXAMPLE
    register_task_queue(stdin_enumerator);
    register_function("read_stdin", 0, 0, bf_read_stdin);
#endif
}


/*\ Finally, if when writing your function you went back in time to,
 *| say, 1996, then maybe you'd have CVS log entries showing up here, too.
 *| Otherwise, you won't.
 */

/*
 * $Log$
 * Revision 2.1  1996/02/08  07:03:47  pavel
 * Renamed err/logf() to errlog/oklog().  Updated copyright notice for 1996.
 * Release 1.8.0beta1.
 *
 * Revision 2.0  1995/11/30  04:26:34  pavel
 * New baseline version, corresponding to release 1.8.0alpha1.
 *
 * Revision 1.1  1995/11/30  04:26:21  pavel
 * Initial revision
 */
