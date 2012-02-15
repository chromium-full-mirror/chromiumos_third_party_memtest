/* main.c - MemTest-86  Version 3.5
 *
 * Released under version 2 of the Gnu Public License.
 * By Chris Brady
 */
#include "stdint.h"
#include "stddef.h"
#include "test.h"
#include "defs.h"
#include "smp.h"
#undef TEST_TIMES
#define DEFTESTS 12

/* The main stack is allocated during boot time. The stack size should
 * preferably be a multiple of page size(4Kbytes)
*/
#define STACKSIZE (6*1024)
#define MAX_MEM   0x1000000	/* 64 GB */
#define TWO_GB    0x80000	/* 2 GB */

extern void bzero();
extern ulong rand(int cpu);
extern void get_mem_speed(int cpu, int ncpus);
extern void rand_seed(unsigned int seed1, unsigned int seed2, int cpu);

const struct tseq tseq[] = {
	{-1,  0,   6, 0, "[Address test, walking ones, no cache] "},
	{-1,  1,   6, 0, "[Address test, own address Seqential]  "},
	{16,  2,   6, 0, "[Address test, own address Parallel]   "},
	{-1,  3,   6, 0, "[Moving inversions, 1s & 0s] Seqential]"},
	{16,  4,   6, 0, "[Moving inversions, 1s & 0s] Parallel] "},
	{16,  5,   3, 0, "[Moving inversions, 8 bit pattern]     "},
	{16,  6,  30, 0, "[Moving inversions, random pattern]    "},
	{16,  7,  81, 0, "[Block move]                           "},
	{16,  8,   3, 0, "[Moving inversions, 32 bit pattern]    "},
	{16,  9,  24, 0, "[Random number sequence]               "},
        {16, 10,   6, 0, "[Modulo 20, Random pattern]            "},
	{1,  11, 300, 0, "[Bit fade test, 2 patterns]            "},
	{0,   0,   0, 0, NULL}
};

extern struct barrier_s *barr;
extern unsigned num_cpus;
volatile int    mstr_cpu;
volatile int	run_cpus;
volatile int	test;
volatile short  cpu_sel = 0;
bool            smp_mode = TRUE;
bool	        restart_pending = FALSE;
uint8_t         volatile stacks[MAX_CPUS][STACKSIZE];
int bitf_seq = 0;

char cmdline_parsed = 0;
struct vars variables = {};
struct vars * const v = &variables;

volatile int bail = 0;
int test_ticks;
volatile int segs;
int nticks;
ulong high_test_adr;

volatile short start_seq = 0;
volatile short cpu_mode = CPM_ALL;
static int c_iter;
volatile static int window;
volatile static unsigned long win_next;
volatile static ulong win0_start;	/* Start test address for window 0 */
volatile static ulong win1_end;		/* End address for relocation */
volatile static struct pmap winx;  	/* Window struct for mapping windows */

#if (LOW_TEST_ADR > (400*1024))
#error LOW_TEST_ADR must be below 400K
#endif

static int find_ticks_for_test(int test);
void find_ticks_for_pass(void);
int find_chunks(int test);
static void test_setup(void);
static int compute_segments(struct pmap map);
int do_test(int cpu);

/* Relocate the test to a new address. Be careful to not overlap! */
static void run_at(unsigned long addr, int cpu)
{
	ulong *ja = (ulong *)(addr + startup_32 - _start);

	/* CPU 0, Copy memtest86 code */
	if (cpu == 0) {
		memmove((void *)addr, &_start, _end - _start);
	}

	/* Wait for the copy */
	barrier();

	/* We use a lock to insure that only one CPU at a time jumps to
	 * the new code. Some of the startup stuff is not thread safe! */
        spin_lock(&barr->mutex);   

	/* Jump to the start address */
	goto *ja;
}

/* Switch from the boot stack to the main stack. First the main stack
 * is allocated, then the contents of the boot stack are copied, then
 * ESP is adjusted to point to the new stack.  
 */
static void
switch_to_main_stack(unsigned cpu_num)
{
	extern uintptr_t boot_stack;
	extern uintptr_t boot_stack_top; 
	uintptr_t *src, *dst;
	int offs;
	uint8_t * stackAddr, *stackTop;
   
	stackAddr = (uint8_t *) &stacks[cpu_num][0];

	stackTop  = stackAddr + STACKSIZE;
   
	src = (uintptr_t*)&boot_stack_top;
	dst = (uintptr_t*)stackTop;
	do {
		src--; dst--;
		*dst = *src;
	} while ((uintptr_t *)src > (uintptr_t *)&boot_stack);

	offs = (uint8_t *)&boot_stack_top - stackTop;
	__asm__ __volatile__ (
	"subl %%eax, %%esp" 
		: /*no output*/
		: "a" (offs) : "memory" 
	);
}

void restart_internal(int cpu)
{
	/* clear variables */
	smp_mode        = TRUE;
        restart_pending = FALSE;

	run_at(LOW_TEST_ADR, cpu);
}

void restart(void)
{
	bail++;
        restart_pending = TRUE;
}

void initialise_cpus(void)
{
        int cpu_num;

        smp_init_bsp();

	/* Initialize the barrier before starting AP's */
	barrier_init(num_cpus);

        /* let the BSP initialise the APs. */
        for(cpu_num = 1; cpu_num < num_cpus; cpu_num++) {
             smp_boot_ap(cpu_num);
        }
}

/* command line passing using the 'old' boot protocol */
#define MK_PTR(seg,off) ((void*)(((unsigned long)(seg) << 4) + (off)))
#define OLD_CL_MAGIC_ADDR ((unsigned short*) MK_PTR(INITSEG,0x20))
#define OLD_CL_MAGIC 0xA33F 
#define OLD_CL_OFFSET_ADDR ((unsigned short*) MK_PTR(INITSEG,0x22))

static void parse_command_line(void)
{
	char *cmdline;

	if (cmdline_parsed)
		return;

	if (*OLD_CL_MAGIC_ADDR != OLD_CL_MAGIC)
		return;

	unsigned short offset = *OLD_CL_OFFSET_ADDR;
	cmdline = MK_PTR(INITSEG, offset);

	/* skip leading spaces */
	while (*cmdline == ' ')
		cmdline++;

	while (*cmdline) {
		if (!strncmp(cmdline, "console=", 8)) {
			cmdline += 8;
			serial_console_setup(cmdline);
		}

		/* go to the next parameter */
		while (*cmdline && *cmdline != ' ')
			cmdline++;
		while (*cmdline == ' ')
			cmdline++;
	}

	cmdline_parsed = 1;
}

/* This is the test entry point. We get here on statup and also whenever
 * we relocate. */
void test_start(void)
{
	int my_cpu_num, run;


	/* First thing, switch to main stack */
	my_cpu_num = smp_my_cpu_num();
	switch_to_main_stack(my_cpu_num);

	/* First time initialization */
	if (start_seq == 0) {

	    /* These steps are only done by the boot cpu */
	    if (my_cpu_num == 0) {
		parse_command_line();
		mem_size();	/* must be called before initialise_cpus(); */
		initialise_cpus();
	   	init();

		test = 0;
	        win_next = 0;
	        window = 0;
	        bail = 0;
	
		/* Setup base address for testing */
		win0_start = (LOW_TEST_ADR+(_end - _start)+8191) >> 12;

		/* Set relocation address to 32Mb if there is enough
		 * memory. Otherwise set it to 3Mb */
		/* Large reloc addr allows for more testing overlap */
	        if ((ulong)v->pmap[v->msegs-1].end > 0x2f00) {
			high_test_adr = 0x2000000;
	        } else {
			high_test_adr = 0x300000;
		} 
		win1_end = (high_test_adr >> 12);

		/* Adjust the map to not test the page at 939k,
		 *  reserved for locks */
		v->pmap[0].end--;

		find_ticks_for_pass();
       	    } else {
		/* AP only, Register the APs */
		smp_ap_booted(my_cpu_num);
	    }
	} else {
	    /* Unlock after a relocation restart */
	    spin_unlock(&barr->mutex);
	}

	/* A barrier to insure that all of the CPUs are done with startup */
	barrier();

	/* Measure memory speed, we do it here because we need all of the
	 * available CPUs */
	if (start_seq == 0) {
		get_mem_speed(my_cpu_num, num_cpus);
	}

	/* Set the initialized flag only after all of the CPU's have
	 * Reached the barrier. This insures that relocation has
	 * been completed for each CPU. */
	start_seq = 1;

	/* Loop through all tests */
	while (1) {
	    /* Skip tests 2 and 4 if we are using only one CPU */
	    if (tseq[test].pat == 2 || tseq[test].pat == 4) {
		if (num_cpus == 1 || cpu_mode != CPM_ALL) {
			test++;
			continue;
		}
	    }

	    test_setup();

	    /* Loop through all possible windows */
	    while (win_next <= ((ulong)v->pmap[v->msegs-1].end + TWO_GB)) {

		/* Main scheduling barrier */
		cprint(8, 2*my_cpu_num+7, "W");
		barrier();

		/* Don't go over the 64 GB PAE limit */
		if (win_next > MAX_MEM) {
			break;
		}

		/* For the bit fade test, #11, we cannot relocate so bump the
		 * window to 1 */
		if (tseq[test].pat == 11 && window == 0) {
			window = 1;
		}

		/* Relocate if required */
		if (window != 0 && (ulong)&_start != LOW_TEST_ADR) {
			run_at(LOW_TEST_ADR, my_cpu_num);
	        }
		if (window == 0 && (ulong)&_start == LOW_TEST_ADR) {
			run_at(high_test_adr, my_cpu_num);
		}

		/* Decide which CPU(s) to use */
		run = 1;
		switch(cpu_mode) {
		case CPM_RROBIN:
		case CPM_SEQ:
			/* Select a single CPU */
			if (my_cpu_num == cpu_sel) {
				mstr_cpu = cpu_sel;
				run_cpus = 1;
	    		} else {
				run = 0;
			}
			break;
		case CPM_ALL:
		    /* Use all CPUs */
		    if (tseq[test].cpu_sel == -1) {
			/* Round robin through all of the CPUs */
			if (my_cpu_num == cpu_sel) {
				mstr_cpu = cpu_sel;
				run_cpus = 1;
	    		} else {
				run = 0;
			}
		    } else {
			/* Use the number of CPUs specified by the test,
			 * Starting with zero */
			if (my_cpu_num >= tseq[test].cpu_sel) {
				run = 0;
			}
			/* Set the master CPU to the highest CPU number 
			 * that has been selected */
			if (num_cpus < tseq[test].cpu_sel) {
				mstr_cpu = num_cpus-1;
				run_cpus = num_cpus;
			} else {
				mstr_cpu = tseq[test].cpu_sel-1;
				run_cpus = tseq[test].cpu_sel;
			}
		    }
		}
		barrier();
		dprint(8, 54, run_cpus, 2, 0);

		/* Setup a sub barrier for only the selected CPUs */
		if (my_cpu_num == mstr_cpu) {
			s_barrier_init(run_cpus);
		}

		/* Make sure the the sub barrier is ready before proceeding */
		barrier();

		/* Not selected CPUs go back to the scheduling barrier */
		if (run == 0 ) {
			continue;
		}
		cprint(8, 2*my_cpu_num+7, "-");

		/* Do we need to exit */
		if(restart_pending) {
		    restart_internal(my_cpu_num);
	 	}

		if (my_cpu_num == mstr_cpu) {
		    switch (window) {
		    /* Special case for relocation */
		    case 0:
			winx.start = 0;
			winx.end = win1_end;
			window++;
			break;
		    /* Special case for first 2 GB */
		    case 1:
			winx.start = win0_start;
			winx.end = TWO_GB;
			win_next += TWO_GB;
			window++;
			break;
		    /* For all other windows */
		    default:
			winx.start = win_next;
			win_next += TWO_GB;
			winx.end = win_next;
		    }

	            /* Find the memory areas to test */
	            segs = compute_segments(winx);
		}
		s_barrier();

	        if (segs == 0) {
		/* No memory in this window so skip it */
		    continue;
	        }

		/* map in the window... */
		if (map_page(v->map[0].pbase_addr) < 0) {
		    continue;
		}

		do_test(my_cpu_num);
		if (bail) {
		    break;
		}

            	paging_off();

	    } /* End of window loop */

	    s_barrier();

	    /* Setup for the next set of windows */
	    win_next = 0;
	    window = 0;
	    bail = 0;

	    /* Only the master CPU does the end of test housekeeping */
	    if (my_cpu_num != mstr_cpu) {
		continue;
	    }

	    /* Special handling for the bit fade test #11 */
	    if (tseq[test].pat == 11 && bitf_seq != 6) {
		/* Keep going until the sequence is complete. */
		bitf_seq++;
		continue;
	    } else {
		bitf_seq = 0;
	    }

	    /* Select advancement of CPUs and next test */
	    switch(cpu_mode) {
	    case CPM_RROBIN:
		if (++cpu_sel >= num_cpus) {
		    cpu_sel = 0;
		}
		test++;
		break;
	    case CPM_SEQ:
		if (++cpu_sel >= num_cpus) {
		    cpu_sel = 0;
		    test++;
		}
		break;
	    case CPM_ALL:
	        if (tseq[test].cpu_sel == -1) {
		    /* Do the same test for each CPU */
		    if (++cpu_sel >= num_cpus) {
		        cpu_sel = 0;
		        test++;
		    } else {
		        continue;
		    }
	        } else {
		    test++;
		}
	    }

	    /* If this was the last test then we finished a pass */
	    if (test >= DEFTESTS ||
			(v->testsel >= 0 && cpu_sel == (num_cpus-1))) {
		v->pass++;
		dprint(LINE_INFO, 55, v->pass, 5, 0);
		v->total_ticks = 0;
		find_ticks_for_pass();
		cprint(1, COL_MID+8,
			"                                         ");
		if (v->ecount == 0 && v->testsel < 0) {
		    cprint(LINE_MSG, COL_MSG,
			"Pass complete, no errors, press Esc to exit");
		}
	    }
	    if (test >= DEFTESTS) {
		test = 0;
	    }

	    bail=0;
	} /* End test loop */
}

void test_setup()
{
	static int ltest = -1;

	/* Only do the setup if this is a new test */
	if (test == ltest) {
		return;
	}
	ltest = test;

	/* Now setup the test parameters based on the current test number */
	if (v->pass == 0) {
		/* Reduce iterations for first pass */
		c_iter = tseq[test].iter/3;
	} else {
		c_iter = tseq[test].iter;
	}

	/* Set the number of iterations. We only do half of the iterations */
        /* on the first pass */
	dprint(LINE_INFO, 28, c_iter, 3, 0);
	test_ticks = find_ticks_for_test(test);
	nticks = 0;
	v->tptr = 0;

	cprint(LINE_PAT, COL_PAT, "            ");
	cprint(LINE_PAT, COL_PAT-3, "   ");
	dprint(LINE_TST, COL_MID+6, test, 2, 1);
	cprint(LINE_TST, COL_MID+9, tseq[test].msg);
	cprint(2, COL_MID+8, "                                         ");
}

/* A couple static variables for when all cpus share the same pattern */
static ulong sp1, sp2;

int do_test(int my_cpu)
{
	int i=0, j=0;
	static int bitf_sleep;
	unsigned long p0=0, p1=0, p2=0;

	if (my_cpu == mstr_cpu) {
	    if ((ulong)&_start > LOW_TEST_ADR) {
		/* Relocated so we need to test all selected lower memory */
		v->map[0].start = mapping(v->plim_lower);
		cprint(LINE_PAT, COL_MID+28, " Relocated");
	    } else {
		cprint(LINE_PAT, COL_MID+28, "          ");
	    }

	    /* Update display of memory segments being tested */
	    p0 = page_of(v->map[0].start);
	    p1 = page_of(v->map[segs-1].end);
	    aprint(LINE_RANGE, COL_MID+9, p0);
	    cprint(LINE_RANGE, COL_MID+14, " - ");
	    aprint(LINE_RANGE, COL_MID+17, p1);
	    aprint(LINE_RANGE, COL_MID+25, p1-p0);
	    cprint(LINE_RANGE, COL_MID+30, " of ");
	    aprint(LINE_RANGE, COL_MID+34, v->selected_pages);
	}
	
	switch(tseq[test].pat) {

	/* Do the testing according to the selected pattern */

	case 0: /* Address test, walking ones (test #0) */
		/* Run with cache turned off */
		set_cache(0);
		addr_tst1(my_cpu);
		set_cache(1);
		BAILOUT;
		break;

	case 1:
	case 2: /* Address test, own address (test #1, 2) */
		addr_tst2(my_cpu);
		BAILOUT;
		break;

	case 3:
	case 4:	/* Moving inversions, all ones and zeros (tests #3, 4) */
		p1 = 0;
		p2 = ~p1;
		s_barrier();
		movinv1(c_iter,p1,p2,my_cpu);
		BAILOUT;
	
		/* Switch patterns */
		s_barrier();
		movinv1(c_iter,p2,p1,my_cpu);
		BAILOUT;
		break;
		
	case 5: /* Moving inversions, 8 bit walking ones and zeros (test #5) */
		p0 = 0x80;
		for (i=0; i<8; i++, p0=p0>>1) {
			p1 = p0 | (p0<<8) | (p0<<16) | (p0<<24);
			p2 = ~p1;
			s_barrier();
			movinv1(c_iter,p1,p2, my_cpu);
			BAILOUT;
	
			/* Switch patterns */
			s_barrier();
			movinv1(c_iter,p2,p1, my_cpu);
			BAILOUT
		}
		break;

	case 6: /* Random Data (test #6) */
		/* Seed the random number generator */
		if (my_cpu == mstr_cpu) {
		    if (v->rdtsc) {
                	asm __volatile__ ("rdtsc":"=a" (sp1),"=d" (sp2));
        	    } else {
                	sp1 = 521288629 + v->pass;
                	sp2 = 362436069 - v->pass;
        	    }
		    rand_seed(sp1, sp2, 0);
		}

		s_barrier();
		for (i=0; i < c_iter; i++) {
			if (my_cpu == mstr_cpu) {
				sp1 = rand(0);
				sp2 = ~p1;
			}
			s_barrier();
			movinv1(2,sp1,sp2, my_cpu);
			BAILOUT;
		}
		break;

	case 7: /* Block move (test #7) */
		block_move(c_iter, my_cpu);
		BAILOUT;
		break;

	case 8: /* Moving inversions, 32 bit shifting pattern (test #8) */
		for (i=0, p1=1; p1; p1=p1<<1, i++) {
			s_barrier();
			movinv32(c_iter,p1, 1, 0x80000000, 0, i, my_cpu);
			BAILOUT
			s_barrier();
			movinv32(c_iter,~p1, 0xfffffffe,
				0x7fffffff, 1, i, my_cpu);
			BAILOUT
		}
		break;

	case 9: /* Random Data Sequence (test #9) */
		for (i=0; i < c_iter; i++) {
			s_barrier();
			movinvr(my_cpu);
			BAILOUT;
		}
		break;

	case 10: /* Modulo 20 check, Random pattern (test #10) */
		for (j=0; j<c_iter; j++) {
			p1 = rand(0);
			for (i=0; i<MOD_SZ; i++) {
				p2 = ~p1;
				s_barrier();
				modtst(i, 2, p1, p2, my_cpu);
				BAILOUT

				/* Switch patterns */
				s_barrier();
				modtst(i, 2, p2, p1, my_cpu);
				BAILOUT
			}
		}
		break;

	case 11: /* Bit fade test, fill (test #11) */
		/* Use a sequence to process all windows for each stage */
		switch(bitf_seq) {
		case 0:	/* Fill all of memory 0's */
			bit_fade_fill(0, my_cpu);
			bitf_sleep = 1;
			break;
		case 1: /* Sleep for the specified time */
			/* Only sleep once */
			if (bitf_sleep) {
				sleep(c_iter, 1, my_cpu);
				bitf_sleep = 0;
			}
			break;
		case 2: /* Now check all of memory for changes */
			bit_fade_chk(0, my_cpu);
			break;
		case 3:	/* Fill all of memory 1's */
			bit_fade_fill(-1, my_cpu);
			bitf_sleep = 1;
			break;
		case 4: /* Sleep for the specified time */
			/* Only sleep once */
			if (bitf_sleep) {
				sleep(c_iter, 1, my_cpu);
				bitf_sleep = 0;
			}
			break;
		case 5: /* Now check all of memory for changes */
			bit_fade_chk(-1, my_cpu);
			break;
		}
		BAILOUT;
		break;

	case 90: /* Modulo 20 check, all ones and zeros (unused) */
		p1=0;
		for (i=0; i<MOD_SZ; i++) {
			p2 = ~p1;
			modtst(i, c_iter, p1, p2, my_cpu);
			BAILOUT

			/* Switch patterns */
			p2 = p1;
			p1 = ~p2;
			modtst(i, c_iter, p1,p2, my_cpu);
			BAILOUT
		}
		break;

	case 91: /* Modulo 20 check, 8 bit pattern (unused) */
		p0 = 0x80;
		for (j=0; j<8; j++, p0=p0>>1) {
			p1 = p0 | (p0<<8) | (p0<<16) | (p0<<24);
			for (i=0; i<MOD_SZ; i++) {
				p2 = ~p1;
				modtst(i, c_iter, p1, p2, my_cpu);
				BAILOUT

				/* Switch patterns */
				p2 = p1;
				p1 = ~p2;
				modtst(i, c_iter, p1, p2, my_cpu);
				BAILOUT
			}
		}
		break;
	}
	return(0);
}

/* Compute number of SPINSZ chunks being tested */
int find_chunks(int tst) 
{
	int i, j, sg, wmax, ch;
	struct pmap twin={0,0};
	unsigned long wnxt = TWO_GB;
	unsigned long len;

	wmax = MAX_MEM/TWO_GB+2;  /* The number of 2 GB segments +2 */
	/* Compute the number of SPINSZ memory segments */
	ch = 0;
	for(j = 0; j < wmax; j++) {
		/* special case for relocation */
		if (j == 0) {
			twin.start = 0;
			twin.end = win1_end;
		}

		/* special case for first 2 GB */
		if (j == 1) {
			twin.start = win0_start;
			twin.end = TWO_GB;
		}

		/* For all other windows */
		if (j > 1) {
			twin.start = wnxt;
			wnxt += TWO_GB;
			twin.end = wnxt;
		}

	        /* Find the memory areas I am going to test */
		sg = compute_segments(twin);
		for(i = 0; i < sg; i++) {
			len = v->map[i].end - v->map[i].start;

			if (cpu_mode == CPM_ALL && num_cpus > 1) {
				switch(tseq[tst].pat) {
				case 2:
				case 4:
				case 5:
				case 6:
				case 9:
				case 10:
				    len /= num_cpus;
				    break;
				case 7:
				case 8:
				    len /= (num_cpus & 0xe);
				    break;
				}
			}
			ch += (len + SPINSZ -1)/SPINSZ;
		}
	}
	return(ch);
}

/* Compute the total number of ticks per pass */
void find_ticks_for_pass(void)
{
	int i;

	v->pptr = 0;
	v->pass_ticks=0;
	if (v->testsel >= 0) {
		v->pass_ticks = find_ticks_for_test(v->testsel);
	} else {
		for (i=0; i<DEFTESTS; i++) {
			/* Skip tests 2 and 4 if we are using 1 cpu */
			if (num_cpus == 1 && (i == 2 || i == 4)) { 
			    continue;
			}
			v->pass_ticks += find_ticks_for_test(i);
		}
	}
}

static int find_ticks_for_test(int tst)
{
	int ticks=0, c, ch;

	/* Determine the number of chunks for this test */
	ch = find_chunks(tst);

	/* Set the number of iterations. We only do 1/3 of the iterations */
        /* on the first pass */
	if (v->pass == 0) {
		c = tseq[tst].iter/3;
	} else {
		c = tseq[tst].iter;
	}

	switch(tseq[tst].pat) {
	case 0: /* Address test, walking ones */
		ticks = 2;
		break;
	case 1: /* Address test, own address */
	case 2:
		ticks = 2;
		break;
	case 3: /* Moving inversions, all ones and zeros */
	case 4:
		ticks = 2 + 4 * c;
		break;
	case 5: /* Moving inversions, 8 bit walking ones and zeros */
		ticks = 24 + 24 * c;
		break;
	case 6: /* Random Data */
		ticks = c + 4 * c;
		break;
	case 7: /* Block move */
		ticks = (ch + ch/num_cpus + c*ch);
		break;
	case 8: /* Moving inversions, 32 bit shifting pattern */
		ticks = (1 + c * 2) * 64;
		break;
	case 9: /* Random Data Sequence */
		ticks = 3 * c;
		break;
	case 10: /* Modulo 20 check, Random pattern */
		ticks = 4 * 40 * c;
		break;
	case 11: /* Bit fade test */
		ticks = c * 2 + 4 * ch;
		break;
	case 90: /* Modulo 20 check, all ones and zeros (unused) */
		ticks = (2 + c) * 40;
		break;
	case 91: /* Modulo 20 check, 8 bit pattern (unused) */
		ticks = (2 + c) * 40 * 8;
		break;
	}
	if (cpu_mode == CPM_SEQ || tseq[tst].cpu_sel == -1) {
		ticks *= num_cpus;
	}
	if (tseq[tst].pat == 7 || tseq[tst].pat == 11) {
		return ticks;
	}
	return ticks*ch;
}

static int compute_segments(struct pmap win)
{
	unsigned long wstart, wend;
	int i, sg;

	/* Compute the window I am testing memory in */
	wstart = win.start;
	wend = win.end;
	sg = 0;

	/* Now reduce my window to the area of memory I want to test */
	if (wstart < v->plim_lower) {
		wstart = v->plim_lower;
	}
	if (wend > v->plim_upper) {
		wend = v->plim_upper;
	}
	if (wstart >= wend) {
		return(0);
	}
	/* List the segments being tested */
	for (i=0; i< v->msegs; i++) {
		unsigned long start, end;
		start = v->pmap[i].start;
		end = v->pmap[i].end;
		if (start <= wstart) {
			start = wstart;
		}
		if (end >= wend) {
			end = wend;
		}
#if 0
		cprint(LINE_SCROLL+(2*i), 0, " (");
		hprint(LINE_SCROLL+(2*i), 2, start);
		cprint(LINE_SCROLL+(2*i), 10, ", ");
		hprint(LINE_SCROLL+(2*i), 12, end);
		cprint(LINE_SCROLL+(2*i), 20, ") ");

		cprint(LINE_SCROLL+(2*i), 22, "r(");
		hprint(LINE_SCROLL+(2*i), 24, wstart);
		cprint(LINE_SCROLL+(2*i), 32, ", ");
		hprint(LINE_SCROLL+(2*i), 34, wend);
		cprint(LINE_SCROLL+(2*i), 42, ") ");

		cprint(LINE_SCROLL+(2*i), 44, "p(");
		hprint(LINE_SCROLL+(2*i), 46, v->plim_lower);
		cprint(LINE_SCROLL+(2*i), 54, ", ");
		hprint(LINE_SCROLL+(2*i), 56, v->plim_upper);
		cprint(LINE_SCROLL+(2*i), 64, ") ");

		cprint(LINE_SCROLL+(2*i+1),  0, "w(");
		hprint(LINE_SCROLL+(2*i+1),  2, win.start);
		cprint(LINE_SCROLL+(2*i+1), 10, ", ");
		hprint(LINE_SCROLL+(2*i+1), 12, win.end);
		cprint(LINE_SCROLL+(2*i+1), 20, ") ");

		cprint(LINE_SCROLL+(2*i+1), 22, "m(");
		hprint(LINE_SCROLL+(2*i+1), 24, v->pmap[i].start);
		cprint(LINE_SCROLL+(2*i+1), 32, ", ");
		hprint(LINE_SCROLL+(2*i+1), 34, v->pmap[i].end);
		cprint(LINE_SCROLL+(2*i+1), 42, ") ");

		cprint(LINE_SCROLL+(2*i+1), 44, "i=");
		hprint(LINE_SCROLL+(2*i+1), 46, i);
		
		cprint(LINE_SCROLL+(2*i+2), 0, 
			"                                        "
			"                                        ");
		cprint(LINE_SCROLL+(2*i+3), 0, 
			"                                        "
			"                                        ");
#endif
		if ((start < end) && (start < wend) && (end > wstart)) {
			v->map[sg].pbase_addr = start;
			v->map[sg].start = mapping(start);
			v->map[sg].end = emapping(end);
#if 0
		hprint(LINE_SCROLL+(sg+1), 0, sg);
		hprint(LINE_SCROLL+(sg+1), 12, v->map[sg].pbase_addr);
		hprint(LINE_SCROLL+(sg+1), 22, start);
		hprint(LINE_SCROLL+(sg+1), 32, end);
		hprint(LINE_SCROLL+(sg+1), 42, mapping(start));
		hprint(LINE_SCROLL+(sg+1), 52, emapping(end));
		cprint(LINE_SCROLL+(sg+2), 0, 
			"                                        "
			"                                        ");
#endif
#if 0
		cprint(LINE_SCROLL+(2*i+1), 54, ", sg=");
		hprint(LINE_SCROLL+(2*i+1), 59, sg);
#endif
			sg++;
		}
	}
	return (sg);
}

