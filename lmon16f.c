#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <ncurses.h>
#include <signal.h>
#include <pwd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>

/* Windows moved here so they can be cleared when the screen mode changes */
WINDOW *padwelcome = NULL;
WINDOW *padtop = NULL;
WINDOW *padmem = NULL;
WINDOW *padlarge = NULL;
WINDOW *padpage = NULL;
WINDOW *padker = NULL;
WINDOW *padnet = NULL;
WINDOW *padneterr = NULL;
WINDOW *padnfs = NULL;
WINDOW *padres = NULL;
WINDOW *padsmp = NULL;
WINDOW *padutil = NULL;
WINDOW *padwide = NULL;
WINDOW *padgpu = NULL;
WINDOW *padmhz = NULL;
WINDOW *padlong = NULL;
WINDOW *paddisk = NULL;
WINDOW *paddg = NULL;
WINDOW *padmap = NULL;
WINDOW *padjfs = NULL;
WINDOW *padverb = NULL;
WINDOW *padhelp = NULL;

#define FLIP(variable) if(variable) variable=0; else variable=1;
#define MALLOC(argument)        malloc(argument)
#define FREE(argument)          free(argument)
#define REALLOC(argument1,argument2)    realloc(argument1,argument2)

#define P_STAT		1
#define P_VERSION	2
#define P_VMSTAT	8
#define P_NUMBER	9

char *check_call_string(char *callback, const char *name)
{
    char *tmp_ptr = callback;
    if (strlen(callback) > 256) {
	fprintf(stderr, "ERROR nmon: ignoring %s - too long\n", name);
	return (char *) NULL;
    }

    for (; *tmp_ptr != '\0' && *tmp_ptr != ' ' && *tmp_ptr != '&';
	 ++tmp_ptr);

    *tmp_ptr = '\0';

    if (tmp_ptr == callback)
	return (char *) NULL;
    else
	return callback;
}

/* Remove error output to this buffer and display it if NMONDEBUG=1 */
char errorstr[70];
int error_on = 0;
void error(char *err)
{
    strncpy(errorstr, err, 69);
}

#define PROC_MAXLINES (16*1024)	/*MAGIC COOKIE WARNING */

int reread = 0;
struct {
    FILE *fp;
    char *filename;
    int size;
    int lines;
    char *line[PROC_MAXLINES];
    char *buf;
    int read_this_interval;	/* track updates for each update to stop  double data collection */
} proc[P_NUMBER];

void proc_init()
{
    proc[P_VMSTAT].filename = "/proc/vmstat";
}

void proc_read(int num)
{
    int i;
    int size;
    int found;
    char buf[1024];

    if (proc[num].read_this_interval == 1)
	return;

    if (proc[num].fp == 0) {
	if ((proc[num].fp = fopen(proc[num].filename, "r")) == NULL) {
	    sprintf(buf, "failed to open file %s", proc[num].filename);
	    error(buf);
	    proc[num].fp = 0;
	    return;
	}
    }
    rewind(proc[num].fp);

    /* We re-read P_STAT, now flag proc_cpu() that it has to re-process that data */
    if (num == P_STAT)

    if (proc[num].size == 0) {
	/* first time so allocate  initial now */
	proc[num].buf = MALLOC(512);
	proc[num].size = 512;
    }

    for (i = 0; i < 4096; i++) {	/* MAGIC COOKIE WARNING  POWER8 default install can have 2655 processes */
	size = fread(proc[num].buf, 1, proc[num].size - 1, proc[num].fp);
	if (size < proc[num].size - 1)
	    break;
	proc[num].size += 512;
	proc[num].buf = REALLOC(proc[num].buf, proc[num].size);
	rewind(proc[num].fp);
    }

    proc[num].buf[size] = 0;
    proc[num].lines = 0;
    proc[num].line[0] = &proc[num].buf[0];
    if (num == P_VERSION) {
	found = 0;
	for (i = 0; i < size; i++) {	/* remove some weird stuff found the hard way in various Linux versions and device drivers */
	    /* place ") (" on two lines */
	    if (found == 0 &&
		proc[num].buf[i] == ')' &&
		proc[num].buf[i + 1] == ' ' &&
		proc[num].buf[i + 2] == '(') {
		proc[num].buf[i + 1] = '\n';
		found = 1;
	    } else {
		/* place ") #" on two lines */
		if (proc[num].buf[i] == ')' &&
		    proc[num].buf[i + 1] == ' ' &&
		    proc[num].buf[i + 2] == '#') {
		    proc[num].buf[i + 1] = '\n';
		}
		/* place "#1" on two lines */
		if (proc[num].buf[i] == '#' && proc[num].buf[i + 2] == '1') {
		    proc[num].buf[i] = '\n';
		}
	    }
	}
    }
    for (i = 0; i < size; i++) {
	/* replace Tab characters with space */
	if (proc[num].buf[i] == '\t') {
	    proc[num].buf[i] = ' ';
	} else if (proc[num].buf[i] == '\n') {
	    /* replace newline characters with null */
	    proc[num].lines++;
	    proc[num].buf[i] = '\0';
	    proc[num].line[proc[num].lines] = &proc[num].buf[i + 1];
	}
	if (proc[num].lines == PROC_MAXLINES - 1)
	    break;
    }
    if (reread) {
	fclose(proc[num].fp);
	proc[num].fp = 0;
    }
    /* Set flag so we do not re-read the data even if called multiple times in same interval */
    proc[num].read_this_interval = 1;
}

#include <dirent.h>
#include <mntent.h>
#include <fstab.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <net/if.h>

int debug = 0;
time_t timer;			/* used to work out the hour/min/second */


int seconds = -1;		/* pause interval */
int maxloops = -1;		/* stop after this number of updates */
char hostname[256];
char run_name[256];
int run_name_set = 0;
char fullhostname[256];
int loop;

#define DPL 150			/* Disks per line for file output to ensure it 
				   does not overflow the spreadsheet input line max */

int disks_per_line = DPL;

int show_vm = 0;

#define RRD if(show_rrd)

double ignore_procdisk_threshold = 0.1;
double ignore_io_threshold = 0.1;
/* Curses support */
#define CURSE if(cursed)	/* Only use this for single line curses calls */
#define COLOUR if(colour)	/* Only use this for single line colour curses calls */
int cursed = 1;			/* 1 = using curses and 
				   0 = loging output for a spreadsheet */
int colour = 1;			/* 1 = using colour curses and 
				   0 = using black and white curses  (see -b flag) */
#define MVPRINTW(row,col,string) {move((row),(col)); \
					attron(A_STANDOUT); \
					printw(string); \
					attroff(A_STANDOUT); }
FILE *fp;			/* filepointer for spreadsheet output */


char *timestamp(int loop, time_t eon)
{
    static char string[64];
    return string;
}

#define LOOP timestamp(loop,timer)
#define WARNING "needs root permission or file not present"

/* Global name of programme for printing it */
char *progname;

struct vm_stat {
    long long nr_dirty;
    long long nr_writeback;
    long long nr_unstable;
    long long nr_page_table_pages;
    long long nr_mapped;
    long long nr_slab;
    long long pgpgin;
    long long pgpgout;
    long long pswpin;
    long long pswpout;
    long long pgalloc_high;
    long long pgalloc_normal;
    long long pgalloc_dma;
    long long pgfree;
    long long pgactivate;
    long long pgdeactivate;
    long long pgfault;
    long long pgmajfault;
    long long pgrefill_high;
    long long pgrefill_normal;
    long long pgrefill_dma;
    long long pgsteal_high;
    long long pgsteal_normal;
    long long pgsteal_dma;
    long long pgscan_kswapd_high;
    long long pgscan_kswapd_normal;
    long long pgscan_kswapd_dma;
    long long pgscan_direct_high;
    long long pgscan_direct_normal;
    long long pgscan_direct_dma;
    long long pginodesteal;
    long long slabs_scanned;
    long long kswapd_steal;
    long long kswapd_inodesteal;
    long long pageoutrun;
    long long allocstall;
    long long pgrotated;
};

struct data {
    struct vm_stat vm;
} database[2], *p, *q;


long long get_vm_value(char *s)
{
    int currline;
    int currchar;
    long long result = -1;
    char *check;
    int len;
    int found;

    for (currline = 0; currline < proc[P_VMSTAT].lines; currline++) {
	len = strlen(s);
	for (currchar = 0, found = 1; currchar < len; currchar++) {
	    if (proc[P_VMSTAT].line[currline][currchar] == 0 ||
		s[currchar] != proc[P_VMSTAT].line[currline][currchar]) {
		found = 0;
		break;
	    }
	}
	if (found && proc[P_VMSTAT].line[currline][currchar] == ' ') {
	    result =
		strtoll(&proc[P_VMSTAT].line[currline][currchar + 1],
			&check, 10);
	    if (*check == proc[P_VMSTAT].line[currline][currchar + 1]) {
		fprintf(stderr, "%s has an unexpected format: >%s<\n",
			proc[P_VMSTAT].filename,
			proc[P_VMSTAT].line[currline]);
		return -1;
	    }
	    return result;
	}
    }
    return -1;
}

#define GETVM(variable) p->vm.variable = get_vm_value(__STRING(variable) );

int read_vmstat()
{
    proc_read(P_VMSTAT);
    if (proc[P_VMSTAT].read_this_interval == 0
	|| proc[P_VMSTAT].lines == 0)
	return (-1);

    /* Note: if the variable requested is not found in /proc/vmstat then it is set to -1 */
    GETVM(nr_dirty);
    GETVM(nr_writeback);
    GETVM(nr_unstable);
    GETVM(nr_page_table_pages);
    GETVM(nr_mapped);
    GETVM(nr_slab);
    GETVM(pgpgin);
    GETVM(pgpgout);
    GETVM(pswpin);
    GETVM(pswpout);
    GETVM(pgalloc_high);
    GETVM(pgalloc_normal);
    GETVM(pgalloc_dma);
    GETVM(pgfree);
    GETVM(pgactivate);
    GETVM(pgdeactivate);
    GETVM(pgfault);
    GETVM(pgmajfault);
    GETVM(pgrefill_high);
    GETVM(pgrefill_normal);
    GETVM(pgrefill_dma);
    GETVM(pgsteal_high);
    GETVM(pgsteal_normal);
    GETVM(pgsteal_dma);
    GETVM(pgscan_kswapd_high);
    GETVM(pgscan_kswapd_normal);
    GETVM(pgscan_kswapd_dma);
    GETVM(pgscan_direct_high);
    GETVM(pgscan_direct_normal);
    GETVM(pgscan_direct_dma);
    GETVM(pginodesteal);
    GETVM(slabs_scanned);
    GETVM(kswapd_steal);
    GETVM(kswapd_inodesteal);
    GETVM(pageoutrun);
    GETVM(allocstall);
    GETVM(pgrotated);
    return 1;
}

/* Added variable to remember started children
 * 0 - start
 * 1 - snap
 * 2 - end
*/
#define CHLD_START 0
#define CHLD_SNAP 1
#define CHLD_END 2
int nmon_children[3] = { -1, -1, -1 };

void init_pairs()
{
    COLOUR init_pair((short) 0, (short) 7, (short) 0);	/* White */
    COLOUR init_pair((short) 1, (short) 1, (short) 0);	/* Red */
    COLOUR init_pair((short) 2, (short) 2, (short) 0);	/* Green */
    COLOUR init_pair((short) 3, (short) 3, (short) 0);	/* Yellow */
    COLOUR init_pair((short) 4, (short) 4, (short) 0);	/* Blue */
    COLOUR init_pair((short) 5, (short) 5, (short) 0);	/* Magenta */
    COLOUR init_pair((short) 6, (short) 6, (short) 0);	/* Cyan */
    COLOUR init_pair((short) 7, (short) 7, (short) 0);	/* White */
    COLOUR init_pair((short) 8, (short) 0, (short) 1);	/* Red background, red text */
    COLOUR init_pair((short) 9, (short) 0, (short) 2);	/* Green background, green text */
    COLOUR init_pair((short) 10, (short) 0, (short) 4);	/* Blue background, blue text */
    COLOUR init_pair((short) 11, (short) 0, (short) 3);	/* Yellow background, yellow text */
    COLOUR init_pair((short) 12, (short) 0, (short) 6);	/* Cyan background, cyan text */
}

/* Signal handler 
 * SIGUSR1 or 2 is used to stop nmon cleanly
 * SIGWINCH is used when the window size is changed
 */
void interrupt(int signum)
{
    int child_pid;
    int waitstatus;
    if (signum == SIGCHLD) {
		while ((child_pid = waitpid(0, &waitstatus, 0)) == -1) {
		    if (errno == EINTR)	/* retry */
			continue;
		    return;		/* ECHLD, EFAULT */
		}
    }
    if (signum == SIGUSR1 || signum == SIGUSR2) {
		maxloops = loop;
		return;
    }
    if (signum == SIGWINCH) {
		CURSE endwin();
		CURSE initscr();
		CURSE cbreak();
		signal(SIGWINCH, interrupt);
		COLOUR colour = has_colors();
		COLOUR start_color();
		COLOUR init_pairs();
		CURSE clear();
		return;
    }
    CURSE endwin();
    exit(0);
}


/* only place the q=previous and p=currect pointers are modified */
void switcher(void)
{
    static int which = 1;
    int i;
    if (which) {
		p = &database[0];
		q = &database[1];
		which = 0;
    } else {
		p = &database[1];
		q = &database[0];
		which = 1;
    }

    /* Reset flags so /proc/... is re-read in next interval */
    for (i = 0; i < P_NUMBER; i++) {
	proc[i].read_this_interval = 0;
    }
}


/* checkinput is the subroutine to handle user input */
int checkinput(void)
{
    static int use_env = 1;
    char buf[1024];
    int bytes;
    int chars;
    int i;
    char *p;

    ioctl(fileno(stdin), FIONREAD, &bytes);

    if (bytes > 0 || use_env) {
		if (use_env) {
		    use_env = 0;
		    p = getenv("NMON");
		    if (p != 0) {
			strcpy(buf, p);
			chars = strlen(buf);
		    } else
			chars = 0;
		} else chars = read(fileno(stdin), buf, bytes);
		if (chars > 0) {
		    for (i = 0; i < chars; i++) {
				switch (buf[i]) {
				case 'V':
				    FLIP(show_vm);
				    break;
				case 'q':
				    nocbreak();
				    endwin();
				    exit(0);
				}
		    }
		    return 1;
		}
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i = 0;
    int x = 0;
       
#define MAXROWS 256
#define MAXCOLS 150		/* changed to allow maximum column widths */
#define BANNER(pad,string) {mvwhline(pad, 0, 0, ACS_HLINE,COLS-2); \
                                        wmove(pad,0,0); \
                                        wattron(pad,A_STANDOUT); \
                                        wprintw(pad," "); \
                                        wprintw(pad,string); \
                                        wprintw(pad," "); \
                                        wattroff(pad,A_STANDOUT); }

#define DISPLAY(pad,rows) { \
                        if(x+2+(rows)>LINES)\
                                pnoutrefresh(pad, 0,0,x,1,LINES-2, COLS-2); \
                        else \
                                pnoutrefresh(pad, 0,0,x,1,x+rows+1,COLS-2); \
                        x=x+(rows);     \
                        if(x+4>LINES) { \
                                mvwprintw(stdscr,LINES-1,10,"Warning: Some Statistics may not shown"); \
                        }               \
                       }

    /* check the user supplied options */
    progname = argv[0];
    for (i = (int) strlen(progname) - 1; i > 0; i--)
	if (progname[i] == '/') {
	    progname = &progname[i + 1];
	}
  
    proc_init();
    
    /* Set parameters if not set by above */
    if (maxloops == -1) maxloops = 9999999;
    if (seconds == -1) seconds = 2;

    switcher();

    /* Start Curses */
    if (cursed) {
	initscr();
	cbreak();
	move(0, 0);
	refresh();
	COLOUR colour = has_colors();
	COLOUR start_color();
	COLOUR init_pairs();
	clear();
	padwelcome = newpad(24, MAXCOLS);
	padmap = newpad(24, MAXCOLS);
	padhelp = newpad(24, MAXCOLS);
	padmem = newpad(20, MAXCOLS);
	padlarge = newpad(20, MAXCOLS);
	padpage = newpad(20, MAXCOLS);
	padres = newpad(20, MAXCOLS);
	padsmp = newpad(MAXROWS, MAXCOLS);
	padutil = newpad(MAXROWS, MAXCOLS);
	padlong = newpad(MAXROWS, MAXCOLS);
	padwide = newpad(MAXROWS, MAXCOLS);
	padmhz = newpad(24, MAXCOLS);
	padgpu = newpad(10, MAXCOLS);
	padnet = newpad(MAXROWS, MAXCOLS);
	padneterr = newpad(MAXROWS, MAXCOLS);
	paddisk = newpad(MAXROWS, MAXCOLS);
	paddg = newpad(MAXROWS, MAXCOLS);
	padjfs = newpad(MAXROWS, MAXCOLS);
	padker = newpad(12, MAXCOLS);
	padverb = newpad(8, MAXCOLS);
	padnfs = newpad(25, MAXCOLS);
	padtop = newpad(MAXROWS, MAXCOLS * 2);
    }

    clear();
    fflush(NULL);

    /* Main loop of the code */
    for (loop = 1;; loop++) {
	timer = time(0);

	/* Reset the cursor position to top left */
	x = 0;

	if (cursed)
	{
		FLIP(show_vm);
	}

#define VMDELTA(variable) (p->vm.variable - q->vm.variable)
#define VMCOUNT(variable) (p->vm.variable                 )

	if (show_vm) {
		read_vmstat();
		BANNER(padpage, "Virtual Memory");
		COLOUR wattrset(padpage, COLOR_PAIR(6));
		mvwprintw(padpage, 1, 0,
			"nr_dirty    =%9lld pgpgin      =%8lld",
			VMCOUNT(nr_dirty), VMDELTA(pgpgin));
		mvwprintw(padpage, 2, 0,
			"nr_writeback=%9lld pgpgout     =%8lld",
			VMCOUNT(nr_writeback), VMDELTA(pgpgout));
		mvwprintw(padpage, 3, 0,
		    "nr_unstable =%9lld pgpswpin    =%8lld",
			VMCOUNT(nr_unstable), VMDELTA(pswpin));
		mvwprintw(padpage, 4, 0,
			"nr_table_pgs=%9lld pgpswpout   =%8lld",
			VMCOUNT(nr_page_table_pages),
			VMDELTA(pswpout));
		mvwprintw(padpage, 5, 0,
			"nr_mapped   =%9lld pgfree      =%8lld",
			VMCOUNT(nr_mapped), VMDELTA(pgfree));
		mvwprintw(padpage, 6, 0,
			"nr_slab     =%9lld pgactivate  =%8lld",
			VMCOUNT(nr_slab), VMDELTA(pgactivate));
		mvwprintw(padpage, 7, 0,
			"                       pgdeactivate=%8lld",
			VMDELTA(pgdeactivate));
		mvwprintw(padpage, 8, 0,
			"allocstall  =%9lld pgfault     =%8lld  kswapd_steal     =%7lld",
			VMDELTA(allocstall), VMDELTA(pgfault),
			VMDELTA(kswapd_steal));
		mvwprintw(padpage, 9, 0,
			"pageoutrun  =%9lld pgmajfault  =%8lld  kswapd_inodesteal=%7lld",
	     	VMDELTA(pageoutrun), VMDELTA(pgmajfault),
			VMDELTA(kswapd_inodesteal));
		mvwprintw(padpage, 10, 0,
			"slabs_scanned=%8lld pgrotated   =%8lld  pginodesteal     =%7lld",
			VMDELTA(slabs_scanned),
			VMDELTA(pgrotated),
			VMDELTA(pginodesteal));
		mvwprintw(padpage, 1, 46,
			"              High Normal    DMA");
		mvwprintw(padpage, 2, 46,
			"alloc      %7lld%7lld%7lld",
			VMDELTA(pgalloc_high),
			VMDELTA(pgalloc_normal),
			VMDELTA(pgalloc_dma));
		mvwprintw(padpage, 3, 46,
			"refill     %7lld%7lld%7lld",
			VMDELTA(pgrefill_high),
			VMDELTA(pgrefill_normal),
			VMDELTA(pgrefill_dma));
		mvwprintw(padpage, 4, 46,
			"steal      %7lld%7lld%7lld",
			VMDELTA(pgsteal_high),
			VMDELTA(pgsteal_normal),
			VMDELTA(pgsteal_dma));
		mvwprintw(padpage, 5, 46,
			"scan_kswapd%7lld%7lld%7lld",
			VMDELTA(pgscan_kswapd_high),
			VMDELTA(pgscan_kswapd_normal),
			VMDELTA(pgscan_kswapd_dma));
		mvwprintw(padpage, 6, 46,
			"scan_direct%7lld%7lld%7lld",
			VMDELTA(pgscan_direct_high),
			VMDELTA(pgscan_direct_normal),
			VMDELTA(pgscan_direct_dma));
		COLOUR wattrset(padpage, COLOR_PAIR(0));
		DISPLAY(padpage, 11);
	}

	if (cursed) {
	    if (x < LINES - 2)
		mvwhline(stdscr, x, 1, ACS_HLINE, COLS - 2);

	    wmove(stdscr, 0, 0);
	    wrefresh(stdscr);
	    doupdate();
	    for (i = 0; i < seconds; i++) {
			sleep(1);
			if (checkinput()) break;
	    }
	}
	switcher();
    }
}