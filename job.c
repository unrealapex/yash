/* Yash: yet another shell */
/* job.c: job control */
/* (C) 2007-2008 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "common.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#if HAVE_GETTEXT
# include <libintl.h>
#endif
#include "option.h"
#include "util.h"
#include "strbuf.h"
#include "plist.h"
#include "sig.h"
#include "job.h"


static inline job_T *get_job(size_t jobnumber)
    __attribute__((pure));
static inline void free_job(job_T *job);
static void trim_joblist(void);
static void set_current_jobnumber(size_t no);
static size_t find_next_job(size_t numlimit);
static int calc_status(int status)
    __attribute__((const));
static wchar_t *get_job_name(const job_T *job)
    __attribute__((nonnull,warn_unused_result));
static char *get_process_status_string(const process_T *p, bool *needfree)
    __attribute__((nonnull,malloc,warn_unused_result));
static char *get_job_status_string(const job_T *job, bool *needfree)
    __attribute__((nonnull,malloc,warn_unused_result));


/* The list of jobs.
 * `joblist.contents[ACTIVE_JOBNO]' is a special job, which is called "active
 * job": the job that is being executed. */
static plist_T joblist;

/* number of the current/previous jobs. 0 if none. */
static size_t current_jobnumber, previous_jobnumber;

/* Initializes the job list. */
void init_job(void)
{
    static bool initialized = false;
    if (!initialized) {
	initialized = true;
	pl_init(&joblist);
	pl_add(&joblist, NULL);
    }
}

/* Sets the active job. */
void set_active_job(job_T *job)
{
    assert(joblist.contents[ACTIVE_JOBNO] == NULL);
    joblist.contents[ACTIVE_JOBNO] = job;
}

/* Moves the active job into the job list.
 * If `current' is true or there is no current job, the job will be the current
 * job. */
void add_job(bool current)
{
    job_T *job = joblist.contents[ACTIVE_JOBNO];

    assert(job != NULL);
    joblist.contents[ACTIVE_JOBNO] = NULL;

    /* if there is an empty element in the list, use it */
    for (size_t i = 1; i < joblist.length; i++) {
	if (joblist.contents[i] == NULL) {
	    joblist.contents[i] = job;
	    if (current || current_jobnumber == 0)
		set_current_jobnumber(i);
	    else if (previous_jobnumber == 0)
		previous_jobnumber = i;
	    return;
	}
    }

    /* if there is no empty, append at the end of the list */
    pl_add(&joblist, job);
    if (current || current_jobnumber == 0)
	set_current_jobnumber(joblist.length - 1);
    else if (previous_jobnumber == 0)
	previous_jobnumber = joblist.length - 1;
}

/* Returns the job of the specified number or NULL if not found. */
job_T *get_job(size_t jobnumber)
{
    return jobnumber < joblist.length ? joblist.contents[jobnumber] : NULL;
}

/* Removes the job of the specified number.
 * If the job is the current/previous job, the current/previous job is reset
 * (another job is assigned to it). */
void remove_job(size_t jobnumber)
{
    job_T *job = get_job(jobnumber);
    joblist.contents[jobnumber] = NULL;
    free_job(job);
    trim_joblist();

    if (jobnumber == current_jobnumber) {
	current_jobnumber = previous_jobnumber;
	previous_jobnumber = find_next_job(current_jobnumber);
    } else if (jobnumber == previous_jobnumber) {
	previous_jobnumber = find_next_job(current_jobnumber);
    }
}

/* Removes all jobs unconditionally. */
void remove_all_jobs(void)
{
    for (size_t i = 0; i < joblist.length; i++)
	remove_job(i);
    trim_joblist();
    current_jobnumber = previous_jobnumber = 0;
}

/* Frees a job. */
void free_job(job_T *job)
{
    if (job) {
	for (size_t i = 0; i < job->j_pcount; i++)
	    free(job->j_procs[i].pr_name);
	free(job);
    }
}

/* Removes unused elements in `joblist'. */
void trim_joblist(void)
{
    size_t tail = joblist.length;

    while (tail > 0 && joblist.contents[--tail] == NULL);
    tail++;
    if (joblist.maxlength > 20 && joblist.maxlength / 2 > joblist.length)
	pl_setmax(&joblist, tail);
    else
	pl_remove(&joblist, tail, SIZE_MAX);
}

/* - When the current job changes, the last current job will be the next
 *   previous job.
 *   - The "fg" command changes the current job.
 *   - The `add_job' function may change the current job.
 * - When the current job finishes, the previous job becomes the current job.
 * - Restarting the current or previous job by the "bg" command resets the
 *   current and previous jobs.
 * - The "wait" command doesn't change the current and previous jobs. */

/* Sets the current job number to the specified one and resets the previous job
 * number. If `jobnumber' is 0, the previous job becomes the current job.
 * Otherwise `jobnumber' must be a valid job number. */
void set_current_jobnumber(size_t jobnumber)
{
    assert(jobnumber == 0 || get_job(jobnumber) != NULL);

    previous_jobnumber = current_jobnumber;
    if (jobnumber == 0) {
	jobnumber = previous_jobnumber;
	if (jobnumber == 0 || get_job(jobnumber) == NULL)
	    jobnumber = find_next_job(0);
    }
    current_jobnumber = jobnumber;

    if (previous_jobnumber == 0
	    || previous_jobnumber == current_jobnumber)
	previous_jobnumber = find_next_job(current_jobnumber);
}

/* Returns an arbitrary job number except the specified.
 * The returned number is suitable for the next current/previous jobs.
 * If there is no job to pick out, 0 is returned.
 * Stopped jobs are preferred to running/finished jobs.
 * If there are more than one stopped jobs, larger job number is preferred. */
size_t find_next_job(size_t excl)
{
    size_t jobnumber = joblist.length;
    while (--jobnumber > 0) {
	if (jobnumber != excl) {
	    job_T *job = get_job(jobnumber);
	    if (job != NULL && job->j_status == JS_STOPPED)
		return jobnumber;
	}
    }
    jobnumber = joblist.length;
    while (--jobnumber > 0) {
	if (jobnumber != excl) {
	    job_T *job = get_job(jobnumber);
	    if (job != NULL)
		return jobnumber;
	}
    }
    return 0;
}

/* Counts the number of jobs in the job list. */
size_t job_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < joblist.length; i++)
	if (joblist.contents[i])
	    count++;
    return count;
}

/* Counts the number of stopped jobs in the job list. */
size_t stopped_job_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < joblist.length; i++) {
	job_T *job = joblist.contents[i];
	if (job && job->j_status == JS_STOPPED)
	    count++;
    }
    return count;
}


/* Updates the info about the jobs in the job list by calling `waitpid'.
 * This function doesn't block. */
void do_wait(void)
{
    pid_t pid;
    int status;
#ifdef WIFCONTINUED
    static int waitopts = WUNTRACED | WCONTINUED | WNOHANG;
#else
    static int waitopts = WUNTRACED | WNOHANG;
#endif

start:
    pid = waitpid(-1, &status, waitopts);
    if (pid < 0) {
	switch (errno) {
	    case EINTR:
		goto start;  /* try again */
	    case ECHILD:
		return;      /* there are no child processes */
	    case EINVAL:
#ifdef WIFCONTINUED
		/* According to the Bash source:
		 *     WCONTINUED may be rejected by waitpid as invalid even
		 *     when defined
		 * -> retry without WCONTINUED. */
		if (waitopts & WCONTINUED) {
		    waitopts = WUNTRACED | WNOHANG;
		    goto start;
		}
#endif
		/* falls thru! */
	    default:
		xerror(errno, "waitpid");
		return;
	}
    } else if (pid == 0) {
	/* no more jobs to be updated */
	return;
    }

    size_t jobnumber, pnumber;
    job_T *job;
    process_T *pr;

    /* determine `jobnumber', `job' and `pr' from `pid' */
    for (jobnumber = 0; jobnumber < joblist.length; jobnumber++)
	if ((job = joblist.contents[jobnumber]))
	    for (pnumber = 0; pnumber < job->j_pcount; pnumber++)
		if ((pr = &job->j_procs[pnumber])->pr_pid == pid)
		    goto found;

    /* If `pid' is not found in the job list, we simply ignore it. This may
     * happen on some occasions: e.g. the job is "disown"ed. */
    goto start;

found:
    pr->pr_statuscode = status;
    if (WIFEXITED(status) || WIFSIGNALED(status))
	pr->pr_status = JS_DONE;
    else if (WIFSTOPPED(status))
	pr->pr_status = JS_STOPPED;
#ifdef WIFCONTINUED
    else if (WIFCONTINUED(status))
	pr->pr_status = JS_RUNNING;
#endif

    /* decide the job status from the process status:
     * - JS_RUNNING if any of the processes is running.
     * - JS_STOPPED if no processes are running but some are stopped.
     * - JS_DONE if all the processes are finished. */
    jobstatus_T oldstatus = job->j_status;
    bool anyrunning = false, anystopped = false;
    /* check if there are running/stopped processes */
    for (size_t i = 0; i < job->j_pcount; i++) {
	switch (job->j_procs[i].pr_status) {
	    case JS_RUNNING:  anyrunning = true;  goto out_of_loop;
	    case JS_STOPPED:  anystopped = true;  break;
	    default:                              break;
	}
    }
out_of_loop:
    job->j_status = anyrunning ? JS_RUNNING : anystopped ? JS_STOPPED : JS_DONE;
    if (job->j_status != oldstatus)
	job->j_statuschanged = true;

    goto start;
}

/* Waits for a job to finish (or stop).
 * `jobnumber' must be a valid job number.
 * If `return_on_stop' is false, waits for the job to finish.
 * Otherwise, waits for the job to finish or stop.
 * This function returns immediately if the job is already finished/stopped. */
void wait_for_job(size_t jobnumber, bool return_on_stop)
{
    job_T *job = joblist.contents[jobnumber];

    block_sigchld_and_sighup();
    for (;;) {
	if (job->j_status == JS_DONE)
	    break;
	if (return_on_stop && job->j_status == JS_STOPPED)
	    break;
	wait_for_sigchld();
    }
    unblock_sigchld_and_sighup();
}

/* Computes the exit status from the status code returned by `waitpid'. */
int calc_status(int status)
{
    if (WIFEXITED(status))
	return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
	return WTERMSIG(status) + TERMSIGOFFSET;
    if (WIFSTOPPED(status))
	return WSTOPSIG(status) + TERMSIGOFFSET;
#ifdef WIFCONTINUED
    if (WIFCONTINUED(status))
	return 0;
#endif
    assert(false);
}

/* Computes the exit status of the specified job.
 * The job must be JS_DONE or JS_STOPPED. */
int calc_status_of_job(const job_T *job)
{
    switch (job->j_status) {
    case JS_DONE:
	if (job->j_procs[job->j_pcount - 1].pr_pid)
	    return calc_status(job->j_procs[job->j_pcount - 1].pr_statuscode);
	else
	    return job->j_procs[job->j_pcount - 1].pr_statuscode;
    case JS_STOPPED:
	for (int i = job->j_pcount; --i >= 0; ) {
	    if (job->j_procs[i].pr_status == JS_STOPPED)
		return calc_status(job->j_procs[i].pr_statuscode);
	}
	/* falls thru! */
    default:
	assert(false);
    }
}

/* Returns the name of the specified job.
 * If the job has only one process, `job->j_procs[0].pr_name' is returned.
 * Otherwise, the names of all the process are concatenated and returned, which
 * must be freed by the caller. */
wchar_t *get_job_name(const job_T *job)
{
    if (job->j_pcount == 1)
	return job->j_procs[0].pr_name;

    xwcsbuf_T buf;
    wb_init(&buf);
    if (job->j_loop)
	wb_cat(&buf, L"| ");
    for (size_t i = 0; i < job->j_pcount; i++) {
	if (i > 0)
	    wb_cat(&buf, L" | ");
	wb_cat(&buf, job->j_procs[i].pr_name);
    }
    return wb_towcs(&buf);
}

/* Returns a string that describes the status of the specified process
 * such as "Running" and "Stopped(SIGTSTP)".
 * The returned string must be freed by the caller iff `*needfree' is assigned
 * true. */
char *get_process_status_string(const process_T *p, bool *needfree)
{
    int status;

    switch (p->pr_status) {
    case JS_RUNNING:
	*needfree = false;
	return gt("Running");
    case JS_STOPPED:
	*needfree = true;
	return malloc_printf(gt("Stopped(SIG%s)"),
		get_signal_name(WSTOPSIG(p->pr_statuscode)));
    case JS_DONE:
	status = p->pr_statuscode;
	if (p->pr_pid == 0)
	    goto exitstatus;
	if (WIFEXITED(status)) {
	    status = WEXITSTATUS(status);
exitstatus:
	    if (status == EXIT_SUCCESS) {
		*needfree = false;
		return gt("Done");
	    } else {
		*needfree = true;
		return malloc_printf(gt("Done(%d)"), status);
	    }
	} else {
	    assert(WIFSIGNALED(status));
	    *needfree = true;
	    status = WTERMSIG(status);
#ifdef WCOREDUMP
	    if (WCOREDUMP(status)) {
		return malloc_printf(gt("Killed (SIG%s: core dumped)"),
			get_signal_name(status));
	    }
#endif
	    return malloc_printf(gt("Killed (SIG%s)"),
		    get_signal_name(status));
	}
    }
    assert(false);
}

/* Returns a string that describes the status of the specified job
 * such as "Running" and "Stopped(SIGTSTP)".
 * The returned string must be freed by the caller iff `*needfree' is assigned
 * true. */
char *get_job_status_string(const job_T *job, bool *needfree)
{
    switch (job->j_status) {
    case JS_RUNNING:
	*needfree = false;
	return gt("Running");
    case JS_STOPPED:
	/* find a stopped process */
	for (size_t i = job->j_pcount; ; )
	    if (job->j_procs[--i].pr_status == JS_STOPPED)
		return get_process_status_string(&job->j_procs[i], needfree);
	assert(false);
    case JS_DONE:
	return get_process_status_string(
		&job->j_procs[job->j_pcount - 1], needfree);
    }
    assert(false);
}

/* Prints the status of job(s).
 * Finished jobs are removed from the job list after the status is printed.
 * If `jobnumber' is PJS_ALL, all the jobs are printed. If the specified job
 * doesn't exist, nothing is printed (it isn't an error).
 * If `changedonly' is true, only jobs whose `j_statuschanged' is true is
 * printed. If `verbose' is true, the status is printed in the process-wise
 * format rather than the usual job-wise format. */
void print_job_status(size_t jobnumber, bool changedonly, bool verbose, FILE *f)
{
    if (jobnumber == PJS_ALL) {
	for (size_t i = 1; i < joblist.length; i++)
	    print_job_status(i, changedonly, verbose, f);
	return;
    }

    job_T *job = get_job(jobnumber);
    if (!job || (changedonly && !job->j_statuschanged))
	return;

    char current;
    if      (jobnumber == current_jobnumber)  current = '+';
    else if (jobnumber == previous_jobnumber) current = '-';
    else                                      current = ' ';

    if (!verbose) {
	bool needfree;
	char *status = get_job_status_string(job, &needfree);
	wchar_t *jobname = get_job_name(job);

	/* TRANSLATORS: the translated format string can be different 
	 * from the original only in the number of spaces. This is required
	 * for POSIX compliance. */
	fprintf(f, gt("[%zu] %c %-20s %ls\n"),
		jobnumber, current, status, jobname);

	if (needfree)
	    free(status);
	if (jobname != job->j_procs[0].pr_name)
	    free(jobname);
    } else {
	bool needfree;
	pid_t pid = job->j_procs[0].pr_pid;
	char *status = get_process_status_string(&job->j_procs[0], &needfree);
	char looppipe = job->j_loop ? '|' : ' ';
	wchar_t *jobname = job->j_procs[0].pr_name;

	/* TRANSLATORS: the translated format string can be different 
	 * from the original only in the number of spaces. This is required
	 * for POSIX compliance. */
	fprintf(f, gt("[%zu] %c %5jd %-20s %c %ls\n"),
		jobnumber, current, (intmax_t) pid, status, looppipe, jobname);
	if (needfree)
	    free(status);

	for (size_t i = 1; i < job->j_pcount; i++) {
	    pid = job->j_procs[i].pr_pid;
	    status = get_process_status_string(&job->j_procs[i], &needfree);
	    jobname = job->j_procs[i].pr_name;

	    /* TRANSLATORS: the translated format string can be different 
	     * from the original only in the number of spaces. This is required
	     * for POSIX compliance. */
	    fprintf(f, gt("      %5jd %-20s | %ls\n"),
		    (intmax_t) pid, (posixly_correct ? "" : status), jobname);
	    if (needfree)
		free(status);
	}
    }
    job->j_statuschanged = false;
    if (job->j_status == JS_DONE)
	remove_job(jobnumber);
}


/* vim: set ts=8 sts=4 sw=4 noet: */
