/************
 *  cdns.c  *
 ************
 *
 * A threaded DNS lookup implementation.
 *
 * This was written exclusively to get around the fact that
 * gethostbyaddr()/gethostbyname() will BLOCK until the resolver either
 * gets an answer, or times out. So we create a thread, and allow
 * the calls to block as long as they want, as the main app's thread
 * will keep on chugging along regardless.
 *
 * BTW, even tho the file is .cc, it really is only C code, I just like
 * C++'s strict type checking.
 *
 * Written by Scott H Kilau
 *
 * Copyright(c) 1999
 *
 * See the COPYRIGHT file, or do a HELP IRCII COPYRIGHT
 * $Id: cdns.c,v 1.1.1.1 2000/11/23 18:11:14 edwards Exp $
 */

#include "cdns.h"
#include "teknap.h"	/* To pick up our next #define checks */
#include "commands.h"

#ifdef TEKNAP
#include "struct.h"
#include "newio.h"
#include "output.h"	/* For say() */
#endif

#if defined(WANT_THREAD) && defined(WANT_NSLOOKUP)

#define DNS_TYPE_STOP 0
#define DNS_TYPE_KILL 1 
#define DNS_RETURN_QUIT 0
#define DNS_RETURN_CANCEL 1
#define DNS_RETURN_KILL 2
#define DNS_RETURN_FAIL 3
#define DNS_RETURN_NOTRUNNING 4

/* Our static functions */
static void init_dns_mutexes(void);
static void *start_dns_thread(void *);
static void do_dns_lookup(DNS_QUEUE *);
static void cleanup_dns(void);
static void destroy_dns_queue(DNS_QUEUE **, DNS_QUEUE **);
static void free_dns_entry(DNS_QUEUE *);
static void kill_dns_thread(int);
static int do_dns_stopkill(int);
static DNS_QUEUE *build_dns_entry(char *, void (*)(DNS_QUEUE *), char *, void *);
static DNS_QUEUE *dns_dequeue(DNS_QUEUE **,  DNS_QUEUE **);
static DNS_QUEUE *dns_enqueue(DNS_QUEUE **, DNS_QUEUE **, DNS_QUEUE *);
static DNS_QUEUE *dns_enqueue_urgent(DNS_QUEUE **, DNS_QUEUE **, DNS_QUEUE *);

/* Our static globals */
static pthread_t dns_thread = {0};
static pthread_mutex_t pending_queue_mutex = {0};
static pthread_mutex_t finished_queue_mutex = {0};
static pthread_mutex_t quit_mutex = {0};
static pthread_cond_t pending_queue_cond;
static DNS_QUEUE *PendingQueueHead = NULL, *PendingQueueTail = NULL;
static DNS_QUEUE *FinishedQueueHead = NULL, *FinishedQueueTail = NULL;
static int cdns_write = -1;
static int cdns_read = -1;
static volatile int dns_thread_dead = 0;
static char *dns_return[] =
	{	"DNS Thread Quit Successfully!",
		"DNS Thread Cancelled Successfully!",
		"DNS Thread Killed Successfully!",
		"DNS Thread Kill Failed!",
		"DNS Thread Kill Not Running!"
	};

/*
 * start_dns : This should be called by the main app thread,
 * whenever it wants to start up the dns stuff
 */
void start_dns(void)
{
	int fd_array[2];
	/* Init our queues. */
	Q_OPEN(&PendingQueueHead, &PendingQueueTail);
	Q_OPEN(&FinishedQueueHead, &FinishedQueueTail);
	/* Create our mutexes */
	init_dns_mutexes();
	/* init our pending queue condition, and set to default attributes (NULL) */
	pthread_cond_init(&pending_queue_cond, NULL);
	/* lock up the quit mutex */
	pthread_mutex_lock(&quit_mutex);
	/* create our pipe [0] = main to read from, [1] = cdns to write to */
	pipe(fd_array);
	cdns_read = fd_array[0];
	cdns_write = fd_array[1];
#ifdef TEKNAP
        new_open(cdns_read);
        new_open(cdns_write);
#endif     
	/* create our dns thread */
	pthread_create(&dns_thread, NULL, start_dns_thread, NULL);
}

/* 
 * stop_dns : This should be called by the main app thread,
 * whenever it wants to stop the dns stuff.
 */
void stop_dns(void)
{
	int retval = do_dns_stopkill(DNS_TYPE_STOP);
        if (retval != DNS_RETURN_NOTRUNNING) {
		/* If stop failed, go and ahead and kill it */
		if (retval == DNS_RETURN_FAIL)
			retval = do_dns_stopkill(DNS_TYPE_KILL);
		say("%s", dns_return[retval]);
	}
}

/* 
 * kill_dns : This should be called by the main app thread,
 * when it wants the dns thread to go away forever. This function
 * kills the thread uncleanly, and probably should only be used
 * when the main app is about to exit()
 */
void kill_dns(void)
{
	int retval = do_dns_stopkill(DNS_TYPE_KILL);
        if (retval != DNS_RETURN_NOTRUNNING)
		say("%s", dns_return[retval]);
}

/*
 * add_to_dns_queue : This should be called by the main app thread,
 * whenever it wants us to resolve something... host <---> ip.
 */
void 
add_to_dns_queue(char *userhost, void (*callback)(DNS_QUEUE *), char *cmd, void *data, int urgency)
{
	char *split = (char *) 0;
	DNS_QUEUE *tmp;

	if (userhost && *userhost) {
		/* Look for user@host */
		split = index(userhost, '@');
		if (split)
			split++;
		else
			split = userhost;
		if (split && *split) {
			/* Build dns entry */
			tmp = build_dns_entry(split, callback, cmd, data);
			/* Wait until we can get mutex lock for queue */
			pthread_mutex_lock(&pending_queue_mutex);
			/* Enqueue the entry, checking urgency */
			if (urgency == DNS_URGENT)
				dns_enqueue_urgent(&PendingQueueHead,
					&PendingQueueTail, tmp);
			else
				dns_enqueue(&PendingQueueHead,
					&PendingQueueTail, tmp);
			/* signal dns thread, its got work to do */
			pthread_cond_signal(&pending_queue_cond);
			/* Give lock back, so dns thread can react to signal */
			pthread_mutex_unlock(&pending_queue_mutex);
		}
		else
			fprintf(stderr, "%s:%d error: No host!", __FILE__, __LINE__);
	}
	else
		fprintf(stderr, "%s:%d error: NULL!", __FILE__, __LINE__);
}

/* set_dns_output_fd : This makes check_dns_queue much better.
 * This will add our piped fd into the main's select() loop, so
 * we will know right away when the output queue has data waiting.
 */
void set_dns_output_fd(fd_set *rd)
{
	if (cdns_read >= 0)
		FD_SET(cdns_read, rd);
}

/* dns_check : See above. */
void dns_check(fd_set *rd)
{
	char blah[2];
	if (cdns_read >= 0 && FD_ISSET(cdns_read, rd)) {
		read(cdns_read, &blah, 1);
		check_dns_queue();
	}
}

/*
 * check_dns_queue : This should be called by the main app thread, at
 * periodic intervals, ie, whenever its convenient.
 */
void check_dns_queue()
{
	DNS_QUEUE *dns = (DNS_QUEUE *) 0;
	while (pthread_mutex_trylock(&finished_queue_mutex) == 0) {
		dns = dns_dequeue(&FinishedQueueHead, &FinishedQueueTail);
		pthread_mutex_unlock(&finished_queue_mutex);
		if (dns) {
#ifdef MY_DEBUG
			fprintf(stderr, "DNS IN: %s: %s <-> %s",
				dns->in ? dns->in : "<NULL>",
				dns->in ? dns->in : "<NULL>",
				dns->out ? dns->out : "<NULL>");
#endif
			if (dns->alias)
			{
				char buffer[BIG_BUFFER_SIZE+1];
				snprintf(buffer, BIG_BUFFER_SIZE, "%s %s ", dns->in ? dns->in : "<NULL>", dns->out ? dns->out : "<NULL>");
				parse_line("NSLOOKUP", dns->alias, buffer, 0, 0, 1);
			}
			else if (dns->callback)
				dns->callback(dns);
			else
				fprintf(stderr, "%s:%d error: No callback!",
				   __FILE__, __LINE__);
			free_dns_entry(dns);
		}
		else
			return;
	}
}

static int do_dns_stopkill(int type)
{
	sigset_t set, oldset;
	void *ptr;

	if (dns_thread <= 0)
		return(DNS_RETURN_NOTRUNNING);

	/* Here is the deal... There are 3 ways for us to kill this thread. In order of friendliness:
	 * 1) Unlock the quit mutex, send a signal to the thread to wakeup. It should quit itself.
	 * 2) Try to cancel the thread. This works better on some OS's than others.
	 * 3) Finally, brute force. Issue the inevitable pthread_kill(). This is ugly.
	 *
	 * So we will try it one by one, till one of them works.
	 */
	
	/* Unlock the quit mutex */
	pthread_mutex_unlock(&quit_mutex);
	/* signal thread to wake up */
	pthread_cond_signal(&pending_queue_cond);
	/* Give back the cpu just long enuf to pretty much guarentee the
	 * other thread will run and sense the quit mutex is unlocked.
	*/
#ifndef HAVE_PTHREAD_CANCEL
	sleep(1);
#else
	usleep(50000);
#endif
	if (dns_thread_dead) {
		pthread_join(dns_thread, &ptr);
		cleanup_dns();
		return(DNS_RETURN_QUIT);
	}
#ifndef HAVE_PTHREAD_CANCEL
	else {
		if (type == DNS_TYPE_STOP) {
			/* since our OS doesn't seem to support cancel points,
			 * we will simply have to block until the thread sees that
			 * the quit mutex is unlocked.
			 */
			pthread_join(dns_thread, &ptr);
			cleanup_dns();
			return(DNS_RETURN_QUIT);
		}
	}
#else
	/* The thread didn't see us unlock our quit mutex quick
	 * enough, so lets try cancelling the thread instead.
	 */
	pthread_cancel(dns_thread);
	usleep(50000);
	if (dns_thread_dead) {
		pthread_join(dns_thread, &ptr);
		cleanup_dns();
		return(DNS_RETURN_CANCEL);
	}
	else {
		if (type == DNS_TYPE_STOP) {
			/* since we have run out of options to kill the thread when we are
			 * "stopping" the thread, we will simply have to block until the
			 * thread sees that the quit mutex is unlocked or that we cancelled it.
			 */
			pthread_join(dns_thread, &ptr);
			cleanup_dns();
			return(DNS_RETURN_CANCEL);
		}
	}
#endif
	/* Well, thats the ballgame. None of the "nice" ways worked.
	 * Now we have to get dirty. Lets just kill the thing.
	 */

	/* Fill set with all known signals */
	sigfillset(&set);
	/* Remove the few signals that POSIX says we should */
	sigdelset(&set, SIGFPE);
	sigdelset(&set, SIGILL);
	/* Tell our thread (main) to block against the above signals */
	sigprocmask(SIG_BLOCK, &set, &oldset);
	/* Before we issue the kill, lets check to see if the thread is dead */
	if (dns_thread_dead) {
		sigprocmask(SIG_BLOCK, &oldset, &set);
		pthread_join(dns_thread, &ptr);
		cleanup_dns();
		return(DNS_RETURN_CANCEL);
	}
	/* Kill the dns thread, using the SIGQUIT signal */
	pthread_kill(dns_thread, SIGQUIT);
	/* Put back the previous blocking signals */
	sigprocmask(SIG_BLOCK, &oldset, &set);
	/* Wait until thread dies */
	pthread_join(dns_thread, NULL);
	/* cleanup anything dealing with the dns stuff */
	cleanup_dns();
	return(DNS_RETURN_KILL);
}


/* Give back memory that we allocated for this entry */
static void free_dns_entry(DNS_QUEUE *tmp)
{
	if (tmp->in)
		free(tmp->in);
	if (tmp->out)
		free(tmp->out);
	if (tmp->alias)
		free(tmp->alias);
	free(tmp);
}

/* init our dns mutexes */
static void init_dns_mutexes(void)
{
	pthread_mutex_init(&pending_queue_mutex, NULL);
	pthread_mutex_init(&finished_queue_mutex, NULL);
	pthread_mutex_init(&quit_mutex, NULL);
}

static void kill_dns_thread(int notused)
{
        dns_thread_dead = 1;
}

static void kill_dns_thread2(int notused)
{
	int ecode = 0;
        dns_thread_dead = 1;
	pthread_exit(&ecode);
}

static void dns_thread_signal_setup(void)
{
	sigset_t set;

	/* Create SIGQUIT signal handler for this thread */
#if 0
	signal(SIGQUIT, kill_dns_thread);
#endif
	/* Use ircii's portable implementation of signal() instead */
	my_signal(SIGQUIT, (sigfunc *) kill_dns_thread, 0);
	/* Fill set with every signal */
	sigfillset(&set);
	/* Remove the SIGQUIT signal from the set */
	sigdelset(&set, SIGQUIT);
	/* Apply the mask on this thread */
	pthread_sigmask(SIG_BLOCK, &set, NULL);
}	

/* Our main function of the dns thread */
static void *start_dns_thread(void *args)
{
	DNS_QUEUE *dns = NULL;

	/* Set up the thread's signal handlers and mask */
	dns_thread_signal_setup();
	/* loop */
	while(1) {
		/* Try the quit mutex, if we get it, that means
		 * main() wants us to die.
		 */
		if (pthread_mutex_trylock(&quit_mutex) == 0) {
			kill_dns_thread2(0);
		}
		/* Lock the queue */
		pthread_mutex_lock(&pending_queue_mutex);
		dns = dns_dequeue(&PendingQueueHead, &PendingQueueTail);
		if (!dns) {
			/*
			 * Give back the cpu, and go to sleep until cond gets signalled.
			 * Note: the mutex MUST be locked, before going into the wait!
			 */
			pthread_cond_wait(&pending_queue_cond, &pending_queue_mutex);
			/* We have been woken up. Thus, 2 conditions are present.
			 * 1) We have been signalled that data is waiting.
			 * 2) The mutex is locked.
			 */
			pthread_mutex_unlock(&pending_queue_mutex);
		}
		else {
			char c = ' ';
			pthread_mutex_unlock(&pending_queue_mutex);
#ifdef HAVE_PTHREAD_TESTCANCEL
			/* This is here because not all OS's have cancel points in get*host*() calls */
			pthread_testcancel();
#endif
			do_dns_lookup(dns);
#ifdef HAVE_PTHREAD_TESTCANCEL
			/* Same reason as above */
			pthread_testcancel();
#endif
			/* block until we get the lock */
			pthread_mutex_lock(&finished_queue_mutex);
			dns_enqueue(&FinishedQueueHead, &FinishedQueueTail, dns);
			pthread_mutex_unlock(&finished_queue_mutex);
			/* Write to our pipe, this will wake up mains select loop */
			write(cdns_write, &c, 1);
		}
	}
}

/* Does the actual DNS lookup.... host <---> ip  */
static void do_dns_lookup(DNS_QUEUE *dns)
{
	struct hostent *temp;
	struct in_addr temp1;
	int ip = 0;
 
	/* If nothing, give back nothing */
	if (!dns->in)
		return;
	if (isdigit(*(dns->in + strlen(dns->in) - 1))) {
		ip = 1;
		temp1.s_addr = inet_addr(dns->in);
		temp = gethostbyaddr((char*) &temp1,
			sizeof (struct in_addr), AF_INET);
	}
	else {
		temp = gethostbyname(dns->in);
		if (temp)
#if defined(_Windows)
			memcpy(&temp1, temp->h_addr, temp->h_length);
#else
			memcpy((caddr_t)&temp1, temp->h_addr, temp->h_length);
#endif
		else
			return;
	}
	if (!temp)
		return;
	if (ip) {
		dns->ip = 1;
		if (temp->h_name && *temp->h_name) {
			dns->out = (char *) malloc(strlen(temp->h_name) + 1);
			strcpy(dns->out, temp->h_name);
		}
	}
	else {
		dns->ip = 0;
		dns->out = (char *) malloc(strlen(inet_ntoa(temp1)) + 1);
		strcpy(dns->out, inet_ntoa(temp1));
	}
}

/* dequeue an entry from the passed in queue */
DNS_QUEUE *
dns_dequeue(DNS_QUEUE **headp, DNS_QUEUE **tailp)
{
	DNS_QUEUE *tmp = NULL;

	if (*headp == NULL)
		return NULL;
	tmp = *headp;
	*headp = Q_NEXT(tmp);
	if (*headp == NULL)
		*tailp = NULL;
	Q_NEXT(tmp) = NULL;
	return tmp;
}

/* enqueue a request onto the passed in queue */
DNS_QUEUE *
dns_enqueue(DNS_QUEUE **headp, DNS_QUEUE **tailp, DNS_QUEUE *tmp)
{
	Q_NEXT(tmp) = NULL;
	if (*headp == NULL)
		*headp = *tailp = tmp;
	else {
		Q_NEXT(*tailp) = tmp;
		*tailp = tmp;
	}
	return NULL;
}

/* enqueue a request onto the passed in queue, putting it at the front
 * of the queue. This means it will be the next requested dequeue.
 */
DNS_QUEUE *
dns_enqueue_urgent(DNS_QUEUE **headp, DNS_QUEUE **tailp, DNS_QUEUE *tmp)
{
	Q_NEXT(tmp) = *headp;
	*headp = tmp;
	if (*tailp == NULL)
		*tailp = tmp;
	return NULL;
}

/* build a dns entry struct */
static DNS_QUEUE *
build_dns_entry(char *text, void (*callback) (DNS_QUEUE *), char *cmd, void *data)
{
	DNS_QUEUE *tmp = (DNS_QUEUE *) malloc(sizeof(DNS_QUEUE));
	bzero(tmp, sizeof(DNS_QUEUE));
	tmp->in = (char *) malloc(strlen(text) + 1);
	strcpy(tmp->in, text);
	tmp->callback = callback;
	tmp->callinfo = data;
        if (cmd)
		tmp->alias = strdup(cmd);
	return tmp;
}

/* cleanup_dns : cleanup anything regarding the dns thread */
static void cleanup_dns(void)
{
	pthread_mutex_destroy(&pending_queue_mutex);
	pthread_mutex_destroy(&finished_queue_mutex);
	pthread_mutex_destroy(&quit_mutex);
	pthread_cond_destroy(&pending_queue_cond);
	destroy_dns_queue(&PendingQueueHead, &PendingQueueTail);
	destroy_dns_queue(&FinishedQueueHead, &FinishedQueueTail);
#ifdef TEKNAP
        new_close(cdns_read);
        new_close(cdns_write);
#else
        close(cdns_read);
        close(cdns_write);
#endif  
	cdns_read = cdns_write = -1;
	dns_thread = 0;
}

/* destroy_dns_queue : Walks the queue, blowing away each node */
static void destroy_dns_queue(DNS_QUEUE **QueueHead, DNS_QUEUE **QueueTail)
{
	DNS_QUEUE *dns;
	
	while((dns = dns_dequeue(QueueHead, QueueTail)) != NULL)
		free_dns_entry(dns);
}
#endif /* THREAD  && NSLOOKUP */
