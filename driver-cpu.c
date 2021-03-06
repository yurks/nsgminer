/*
 * Copyright 2011-2012 Con Kolivas
 * Copyright 2011-2013 Luke Dashjr
 * Copyright 2010 Jeff Garzik
 * Copyright 2015 John Doering
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>

#include <sys/stat.h>
#include <sys/types.h>

#ifndef WIN32
#include <sys/wait.h>
#include <sys/resource.h>
#endif
#include <libgen.h>

#include "compat.h"
#include "miner.h"
#include "bench_block.h"
#include "driver-cpu.h"

#include "neoscrypt.h"

#if defined(unix)
	#include <errno.h>
	#include <fcntl.h>
#endif

#if defined(__linux) && defined(CPU_ZERO)  /* Linux specific policy and affinity management */
#include <sched.h>
static inline void drop_policy(void)
{
	struct sched_param param;

#ifdef SCHED_BATCH
#ifdef SCHED_IDLE
	if (unlikely(sched_setscheduler(0, SCHED_IDLE, &param) == -1))
#endif
		sched_setscheduler(0, SCHED_BATCH, &param);
#endif
}

static inline void affine_to_cpu(int id, int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(&set), &set);
	applog(LOG_INFO, "Binding cpu mining thread %d to cpu %d", id, cpu);
}
#else
static inline void drop_policy(void)
{
}

static inline void affine_to_cpu(int __maybe_unused id, int __maybe_unused cpu)
{
}
#endif



/* TODO: resolve externals */
extern void submit_work_async(const struct work *work_in, struct timeval *tv);
extern char *set_int_range(const char *arg, int *i, int min, int max);
extern int dev_from_id(int thr_id);


#ifdef USE_SHA256D
/* chipset-optimized hash functions */
extern bool ScanHash_4WaySSE2(struct thr_info*, const unsigned char *pmidstate,
	unsigned char *pdata, unsigned char *phash1, unsigned char *phash,
	const unsigned char *ptarget,
	uint32_t max_nonce, uint32_t *last_nonce, uint32_t nonce);

extern bool ScanHash_altivec_4way(struct thr_info*, const unsigned char *pmidstate,
	unsigned char *pdata,
	unsigned char *phash1, unsigned char *phash,
	const unsigned char *ptarget,
	uint32_t max_nonce, uint32_t *last_nonce, uint32_t nonce);

extern bool scanhash_via(struct thr_info*, const unsigned char *pmidstate,
	unsigned char *pdata,
	unsigned char *phash1, unsigned char *phash,
	const unsigned char *target,
	uint32_t max_nonce, uint32_t *last_nonce, uint32_t n);

extern bool scanhash_c(struct thr_info*, const unsigned char *midstate, unsigned char *data,
	      unsigned char *hash1, unsigned char *hash,
	      const unsigned char *target,
	      uint32_t max_nonce, uint32_t *last_nonce, uint32_t n);

extern bool scanhash_cryptopp(struct thr_info*, const unsigned char *midstate,unsigned char *data,
	      unsigned char *hash1, unsigned char *hash,
	      const unsigned char *target,
	      uint32_t max_nonce, uint32_t *last_nonce, uint32_t n);

extern bool scanhash_asm32(struct thr_info*, const unsigned char *midstate,unsigned char *data,
	      unsigned char *hash1, unsigned char *hash,
	      const unsigned char *target,
	      uint32_t max_nonce, uint32_t *last_nonce, uint32_t nonce);

extern bool scanhash_sse2_64(struct thr_info*, const unsigned char *pmidstate, unsigned char *pdata,
	unsigned char *phash1, unsigned char *phash,
	const unsigned char *ptarget,
	uint32_t max_nonce, uint32_t *last_nonce,
	uint32_t nonce);

extern bool scanhash_sse4_64(struct thr_info*, const unsigned char *pmidstate, unsigned char *pdata,
	unsigned char *phash1, unsigned char *phash,
	const unsigned char *ptarget,
	uint32_t max_nonce, uint32_t *last_nonce,
	uint32_t nonce);

extern bool scanhash_sse2_32(struct thr_info*, const unsigned char *pmidstate, unsigned char *pdata,
	unsigned char *phash1, unsigned char *phash,
	const unsigned char *ptarget,
	uint32_t max_nonce, uint32_t *last_nonce,
	uint32_t nonce);
#endif /* USE_SHA256D */


#ifdef WANT_CPUMINE
static size_t max_name_len = 0;
static char *name_spaces_pad = NULL;
const char *algo_names[] = {
#ifdef USE_SHA256D
	[ALGO_C]		= "c",
#ifdef WANT_SSE2_4WAY
	[ALGO_4WAY]		= "4way",
#endif
#ifdef WANT_VIA_PADLOCK
	[ALGO_VIA]		= "via",
#endif
	[ALGO_CRYPTOPP]		= "cryptopp",
#ifdef WANT_CRYPTOPP_ASM32
	[ALGO_CRYPTOPP_ASM32]	= "cryptopp_asm32",
#endif
#ifdef WANT_X8632_SSE2
	[ALGO_SSE2_32]		= "sse2_32",
#endif
#ifdef WANT_X8664_SSE2
	[ALGO_SSE2_64]		= "sse2_64",
#endif
#ifdef WANT_X8664_SSE4
	[ALGO_SSE4_64]		= "sse4_64",
#endif
#ifdef WANT_ALTIVEC_4WAY
    [ALGO_ALTIVEC_4WAY] = "altivec_4way",
#endif
#endif /* USE_SHA256D */
#ifdef USE_NEOSCRYPT
    [ALGO_NEOSCRYPT] = "neoscrypt",
#endif
#ifdef USE_SCRYPT
    [ALGO_SCRYPT] = "scrypt",
#endif
    [ALGO_VOID] = "void",
};

#ifdef USE_SHA256D
static const sha256_func sha256_funcs[] = {
	[ALGO_C]		= (sha256_func)scanhash_c,
#ifdef WANT_SSE2_4WAY
	[ALGO_4WAY]		= (sha256_func)ScanHash_4WaySSE2,
#endif
#ifdef WANT_ALTIVEC_4WAY
    [ALGO_ALTIVEC_4WAY] = (sha256_func) ScanHash_altivec_4way,
#endif
#ifdef WANT_VIA_PADLOCK
	[ALGO_VIA]		= (sha256_func)scanhash_via,
#endif
	[ALGO_CRYPTOPP]		=  (sha256_func)scanhash_cryptopp,
#ifdef WANT_CRYPTOPP_ASM32
	[ALGO_CRYPTOPP_ASM32]	= (sha256_func)scanhash_asm32,
#endif
#ifdef WANT_X8632_SSE2
	[ALGO_SSE2_32]		= (sha256_func)scanhash_sse2_32,
#endif
#ifdef WANT_X8664_SSE2
	[ALGO_SSE2_64]		= (sha256_func)scanhash_sse2_64,
#endif
#ifdef WANT_X8664_SSE4
	[ALGO_SSE4_64]		= (sha256_func)scanhash_sse4_64,
#endif
};
#endif /* USE_SHA256D */
#endif



#ifdef WANT_CPUMINE
#ifdef USE_SHA256D
#if defined(WANT_X8664_SSE2) && defined(__SSE2__)
enum algo_types opt_algo = ALGO_SSE2_64;
#elif defined(WANT_X8632_SSE2) && defined(__SSE2__)
enum algo_types opt_algo = ALGO_SSE2_32;
#else
enum algo_types opt_algo = ALGO_C;
#endif
#else
enum algo_types opt_algo = ALGO_VOID;
#endif /* USE_SHA256D */
bool opt_usecpu = false;
static bool forced_n_threads;
#endif

static const uint32_t hash1_init[] = {
	0,0,0,0,0,0,0,0,
	0x80000000,
	  0,0,0,0,0,0,
	          0x100,
};




#ifdef WANT_CPUMINE
#ifdef USE_SHA256D
// Algo benchmark, crash-prone, system independent stage
double bench_algo_stage3(enum algo_types algo) {
	// Use a random work block pulled from a pool
	static uint8_t bench_block[] = { CGMINER_BENCHMARK_BLOCK };
	struct work work __attribute__((aligned(128)));
	unsigned char hash1[64];

	size_t bench_size = sizeof(work);
	size_t work_size = sizeof(bench_block);
	size_t min_size = (work_size < bench_size ? work_size : bench_size);
	memset(&work, 0, sizeof(work));
	memcpy(&work, &bench_block, min_size);

	static struct thr_info dummy;

	struct timeval end;
	struct timeval start;
	uint32_t max_nonce = (1<<22);
	uint32_t last_nonce = 0;

	memcpy(&hash1[0], &hash1_init[0], sizeof(hash1));

	gettimeofday(&start, 0);
			{
				sha256_func func = sha256_funcs[algo];
				(*func)(
					&dummy,
					work.midstate,
					work.data,
					hash1,
					work.hash,
					work.target,
					max_nonce,
					&last_nonce,
					work.blk.nonce
				);
			}
	gettimeofday(&end, 0);

	uint64_t usec_end = ((uint64_t)end.tv_sec)*1000*1000 + end.tv_usec;
	uint64_t usec_start = ((uint64_t)start.tv_sec)*1000*1000 + start.tv_usec;
	uint64_t usec_elapsed = usec_end - usec_start;

	double rate = -1.0;
	if (0<usec_elapsed) {
		rate = (1.0*(last_nonce+1))/usec_elapsed;
	}
	return rate;
}

#if defined(unix)

	// Change non-blocking status on a file descriptor
	static void set_non_blocking(
		int fd,
		int yes
	)
	{
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags<0) {
			perror("fcntl(GET) failed");
			exit(1);
		}
		flags = yes ? (flags|O_NONBLOCK) : (flags&~O_NONBLOCK);

		int r = fcntl(fd, F_SETFL, flags);
		if (r<0) {
			perror("fcntl(SET) failed");
			exit(1);
		}
	}

#endif // defined(unix)

// Algo benchmark, crash-safe, system-dependent stage
static double bench_algo_stage2(enum algo_types algo) {
	// Here, the gig is to safely run a piece of code that potentially
	// crashes. Unfortunately, the Right Way (tm) to do this is rather
	// heavily platform dependent :(

	double rate = -1.23457;

	#if defined(unix)

		// Make a pipe: [readFD, writeFD]
		int pfd[2];
		int r = pipe(pfd);
		if (r<0) {
			perror("pipe - failed to create pipe for --algo auto");
			exit(1);
		}

		// Make pipe non blocking
		set_non_blocking(pfd[0], 1);
		set_non_blocking(pfd[1], 1);

		// Don't allow a crashing child to kill the main process
		sighandler_t sr0 = signal(SIGPIPE, SIG_IGN);
		sighandler_t sr1 = signal(SIGPIPE, SIG_IGN);
		if (SIG_ERR==sr0 || SIG_ERR==sr1) {
			perror("signal - failed to edit signal mask for --algo auto");
			exit(1);
		}

		// Fork a child to do the actual benchmarking
		pid_t child_pid = fork();
		if (child_pid<0) {
			perror("fork - failed to create a child process for --algo auto");
			exit(1);
		}

		// Do the dangerous work in the child, knowing we might crash
		if (0==child_pid) {

			// TODO: some umask trickery to prevent coredumps

			// Benchmark this algorithm
			double r = bench_algo_stage3(algo);

			// We survived, send result to parent and bail
			int loop_count = 0;
			while (1) {
				ssize_t bytes_written = write(pfd[1], &r, sizeof(r));
				int try_again = (0==bytes_written || (bytes_written<0 && EAGAIN==errno));
				int success = (sizeof(r)==(size_t)bytes_written);

				if (success)
					break;

				if (!try_again) {
					perror("write - child failed to write benchmark result to pipe");
					exit(1);
				}

				if (5<loop_count) {
					applog(LOG_ERR, "child tried %d times to communicate with parent, giving up", loop_count);
					exit(1);
				}
				++loop_count;
				sleep(1);
			}
			exit(0);
		}

		// Parent waits for a result from child
		int loop_count = 0;
		while (1) {

			// Wait for child to die
			int status;
			int r = waitpid(child_pid, &status, WNOHANG);
			if ((child_pid==r) || (r<0 && ECHILD==errno)) {

				// Child died somehow. Grab result and bail
				double tmp;
				ssize_t bytes_read = read(pfd[0], &tmp, sizeof(tmp));
				if (sizeof(tmp)==(size_t)bytes_read)
					rate = tmp;
				break;

			} else if (r<0) {
				perror("bench_algo: waitpid failed. giving up.");
				exit(1);
			}

			// Give up on child after a ~60s
			if (60<loop_count) {
				kill(child_pid, SIGKILL);
				waitpid(child_pid, &status, 0);
				break;
			}

			// Wait a bit longer
			++loop_count;
			sleep(1);
		}

		// Close pipe
		r = close(pfd[0]);
		if (r<0) {
			perror("close - failed to close read end of pipe for --algo auto");
			exit(1);
		}
		r = close(pfd[1]);
		if (r<0) {
			perror("close - failed to close read end of pipe for --algo auto");
			exit(1);
		}

	#elif defined(WIN32)

		// Get handle to current exe
		HINSTANCE module = GetModuleHandle(0);
		if (!module) {
			applog(LOG_ERR, "failed to retrieve module handle");
			exit(1);
		}

		// Create a unique name
		char unique_name[33];
		snprintf(
			unique_name,
			sizeof(unique_name)-1,
			"nsgminer-%p",
			(void*)module
		);

		// Create and init a chunked of shared memory
		HANDLE map_handle = CreateFileMapping(
			INVALID_HANDLE_VALUE,   // use paging file
			NULL,                   // default security attributes
			PAGE_READWRITE,         // read/write access
			0,                      // size: high 32-bits
			4096,			// size: low 32-bits
			unique_name		// name of map object
		);
		if (NULL==map_handle) {
			applog(LOG_ERR, "could not create shared memory");
			exit(1);
		}

		void *shared_mem = MapViewOfFile(
			map_handle,	// object to map view of
			FILE_MAP_WRITE, // read/write access
			0,              // high offset:  map from
			0,              // low offset:   beginning
			0		// default: map entire file
		);
		if (NULL==shared_mem) {
			applog(LOG_ERR, "could not map shared memory");
			exit(1);
		}
        SetEnvironmentVariable("NSGMINER_SHARED_MEM", unique_name);
		CopyMemory(shared_mem, &rate, sizeof(rate));

		// Get path to current exe
		char cmd_line[256 + MAX_PATH];
		const size_t n = sizeof(cmd_line)-200;
		DWORD size = GetModuleFileName(module, cmd_line, n);
		if (0==size) {
			applog(LOG_ERR, "failed to retrieve module path");
			exit(1);
		}

		// Construct new command line based on that
		char *p = strlen(cmd_line) + cmd_line;
		sprintf(p, " --bench-algo %d", algo);
        SetEnvironmentVariable("NSGMINER_BENCH_ALGO", "1");

		// Launch a debug copy of BFGMiner
		STARTUPINFO startup_info;
		PROCESS_INFORMATION process_info;
		ZeroMemory(&startup_info, sizeof(startup_info));
		ZeroMemory(&process_info, sizeof(process_info));
		startup_info.cb = sizeof(startup_info);

		BOOL ok = CreateProcess(
			NULL,			// No module name (use command line)
			cmd_line,		// Command line
			NULL,			// Process handle not inheritable
			NULL,			// Thread handle not inheritable
			FALSE,			// Set handle inheritance to FALSE
			DEBUG_ONLY_THIS_PROCESS,// We're going to debug the child
			NULL,			// Use parent's environment block
			NULL,			// Use parent's starting directory
			&startup_info,		// Pointer to STARTUPINFO structure
			&process_info		// Pointer to PROCESS_INFORMATION structure
		);
		if (!ok) {
			applog(LOG_ERR, "CreateProcess failed with error %ld\n", (long)GetLastError() );
			exit(1);
		}

		// Debug the child (only clean way to catch exceptions)
		while (1) {

			// Wait for child to do something
			DEBUG_EVENT debug_event;
			ZeroMemory(&debug_event, sizeof(debug_event));

			BOOL ok = WaitForDebugEvent(&debug_event, 60 * 1000);
			if (!ok)
				break;

			// Decide if event is "normal"
			int go_on =
				CREATE_PROCESS_DEBUG_EVENT== debug_event.dwDebugEventCode	||
				CREATE_THREAD_DEBUG_EVENT == debug_event.dwDebugEventCode	||
				EXIT_THREAD_DEBUG_EVENT   == debug_event.dwDebugEventCode	||
				EXCEPTION_DEBUG_EVENT     == debug_event.dwDebugEventCode	||
				LOAD_DLL_DEBUG_EVENT      == debug_event.dwDebugEventCode	||
				OUTPUT_DEBUG_STRING_EVENT == debug_event.dwDebugEventCode	||
				UNLOAD_DLL_DEBUG_EVENT    == debug_event.dwDebugEventCode;
			if (!go_on)
				break;

			// Some exceptions are also "normal", apparently.
			if (EXCEPTION_DEBUG_EVENT== debug_event.dwDebugEventCode) {

				int go_on =
					EXCEPTION_BREAKPOINT== debug_event.u.Exception.ExceptionRecord.ExceptionCode;
				if (!go_on)
					break;
			}

			// If nothing unexpected happened, let child proceed
			ContinueDebugEvent(
				debug_event.dwProcessId,
				debug_event.dwThreadId,
				DBG_CONTINUE
			);
		}

		// Clean up child process
		TerminateProcess(process_info.hProcess, 1);
		CloseHandle(process_info.hProcess);
		CloseHandle(process_info.hThread);

		// Reap return value and cleanup
		CopyMemory(&rate, shared_mem, sizeof(rate));
		(void)UnmapViewOfFile(shared_mem);
		(void)CloseHandle(map_handle);

	#else

		// Not linux, not unix, not WIN32 ... do our best
		rate = bench_algo_stage3(algo);

	#endif // defined(unix)

	// Done
	return rate;
}

static void bench_algo(double *best_rate, enum algo_types *best_algo,
  enum algo_types algo) {
	size_t n = max_name_len - strlen(algo_names[algo]);
	memset(name_spaces_pad, ' ', n);
	name_spaces_pad[n] = 0;

	applog(
		LOG_ERR,
		"\"%s\"%s : benchmarking algorithm ...",
		algo_names[algo],
		name_spaces_pad
	);

	double rate = bench_algo_stage2(algo);
	if (rate<0.0) {
		applog(
			LOG_ERR,
			"\"%s\"%s : algorithm fails on this platform",
			algo_names[algo],
			name_spaces_pad
		);
	} else {
		applog(
			LOG_ERR,
			"\"%s\"%s : algorithm runs at %.5f MH/s",
			algo_names[algo],
			name_spaces_pad,
			rate
		);
		if (*best_rate<rate) {
			*best_rate = rate;
			*best_algo = algo;
		}
	}
}

// Pick the fastest CPU hasher
static enum algo_types pick_fastest_algo() {
	double best_rate = -1.0;
    enum algo_types best_algo = 0;
	applog(LOG_ERR, "benchmarking all sha256 algorithms ...");

	bench_algo(&best_rate, &best_algo, ALGO_C);

	#if defined(WANT_SSE2_4WAY)
		bench_algo(&best_rate, &best_algo, ALGO_4WAY);
	#endif

	#if defined(WANT_VIA_PADLOCK)
		bench_algo(&best_rate, &best_algo, ALGO_VIA);
	#endif

	bench_algo(&best_rate, &best_algo, ALGO_CRYPTOPP);

	#if defined(WANT_CRYPTOPP_ASM32)
		bench_algo(&best_rate, &best_algo, ALGO_CRYPTOPP_ASM32);
	#endif

	#if defined(WANT_X8632_SSE2)
		bench_algo(&best_rate, &best_algo, ALGO_SSE2_32);
	#endif

	#if defined(WANT_X8664_SSE2)
		bench_algo(&best_rate, &best_algo, ALGO_SSE2_64);
	#endif

	#if defined(WANT_X8664_SSE4)
		bench_algo(&best_rate, &best_algo, ALGO_SSE4_64);
	#endif

        #if defined(WANT_ALTIVEC_4WAY)
                bench_algo(&best_rate, &best_algo, ALGO_ALTIVEC_4WAY);
        #endif

	size_t n = max_name_len - strlen(algo_names[best_algo]);
	memset(name_spaces_pad, ' ', n);
	name_spaces_pad[n] = 0;
	applog(
		LOG_ERR,
		"\"%s\"%s : is fastest algorithm at %.5f MH/s",
		algo_names[best_algo],
		name_spaces_pad,
		best_rate
	);
	return best_algo;
}
#endif /* USE_SHA256D */

// Figure out the longest algorithm name
void init_max_name_len()
{
	size_t i;
	size_t nb_names = sizeof(algo_names)/sizeof(algo_names[0]);
	for (i=0; i<nb_names; ++i) {
		const char *p = algo_names[i];
		size_t name_len = p ? strlen(p) : 0;
		if (max_name_len<name_len)
			max_name_len = name_len;
	}

	name_spaces_pad = (char*) malloc(max_name_len+16);
	if (0==name_spaces_pad) {
		perror("malloc failed");
		exit(1);
	}
}

/* FIXME: Use asprintf for better errors. */
char *set_algo(const char *arg, enum algo_types *algo) {
    enum algo_types i;

    if(!opt_sha256d)
      return("default");

#ifdef USE_SHA256D
	if (!strcmp(arg, "auto")) {
		*algo = pick_fastest_algo();
		return NULL;
	}

	for (i = 0; i < ARRAY_SIZE(algo_names); i++) {
		if (algo_names[i] && !strcmp(arg, algo_names[i])) {
			*algo = i;
			return NULL;
		}
	}
#endif /* USE_SHA256D */

    return("void");
}

void *set_algo_quick(enum algo_types *algo) {

    if(opt_neoscrypt) {
        *algo = ALGO_NEOSCRYPT;
    } else if(opt_scrypt) {
        *algo = ALGO_SCRYPT;
    } else {
        *algo = ALGO_VOID;
    }

}

void show_algo(char buf[OPT_SHOW_LEN], const enum algo_types *algo) {
    strncpy(buf, algo_names[*algo], OPT_SHOW_LEN);
}
#endif

#ifdef WANT_CPUMINE
char *force_nthreads_int(const char *arg, int *i)
{
	forced_n_threads = true;
	return set_int_range(arg, i, 0, 9999);
}
#endif

#ifdef WANT_CPUMINE
static void cpu_detect()
{
	int i;

	// Reckon number of cores in the box
	#if defined(WIN32)
	{
		DWORD_PTR system_am;
		DWORD_PTR process_am;
		BOOL ok = GetProcessAffinityMask(
			GetCurrentProcess(),
			&system_am,
			&process_am
		);
		if (!ok) {
			applog(LOG_ERR, "couldn't figure out number of processors :(");
			num_processors = 1;
		} else {
			size_t n = 32;
			num_processors = 0;
			while (n--)
				if (process_am & (1<<n))
					++num_processors;
		}
	}
	#elif defined(_SC_NPROCESSORS_ONLN)
		num_processors = sysconf(_SC_NPROCESSORS_ONLN);
	#elif defined(HW_NCPU)
		int req[] = { CTL_HW, HW_NCPU };
		size_t len = sizeof(num_processors);
		v = sysctl(req, 2, &num_processors, &len, NULL, 0);
	#else
		num_processors = 1;
	#endif /* !WIN32 */

	if (opt_n_threads < 0 || !forced_n_threads) {
		if (total_devices && !opt_usecpu)
			opt_n_threads = 0;
		else
			opt_n_threads = num_processors;
	}
	if (num_processors < 1)
		return;

	cpus = calloc(opt_n_threads, sizeof(struct cgpu_info));
	if (unlikely(!cpus))
		quit(1, "Failed to calloc cpus");
	for (i = 0; i < opt_n_threads; ++i) {
		struct cgpu_info *cgpu;

		cgpu = &cpus[i];
		cgpu->api = &cpu_api;
		cgpu->devtype = "CPU";
		cgpu->deven = DEV_ENABLED;
		cgpu->threads = 1;
		cgpu->kname = algo_names[opt_algo];
		add_cgpu(cgpu);
	}
}

static bool cpu_thread_prepare(struct thr_info *thr)
{
	thread_reportin(thr);

	return true;
}

static uint64_t cpu_can_limit_work(struct thr_info __maybe_unused *thr)
{
	return 0xffff;
}

static bool cpu_thread_init(struct thr_info *thr)
{
	const int thr_id = thr->id;

	/* Set worker threads to nice 19 and then preferentially to SCHED_IDLE
	 * and if that fails, then SCHED_BATCH. No need for this to be an
	 * error if it fails */
	setpriority(PRIO_PROCESS, 0, 19);
	drop_policy();
	/* Cpu affinity only makes sense if the number of threads is a multiple
	 * of the number of CPUs */
	if (!(opt_n_threads % num_processors))
		affine_to_cpu(dev_from_id(thr_id), dev_from_id(thr_id) % num_processors);
	return true;
}

#ifdef USE_NEOSCRYPT
/* NeoScrypt(128, 2, 1) with Salsa20/20 and ChaCha20/20 */
static int scanhash_neoscrypt(struct thr_info *thr, uint *pdata, const uint *ptarget,
  uint *phash, uint start_nonce, uint max_nonce, uint *final_nonce) {
    uint hash[8];
    uint i, inc_nonce = 1;
    const uint t32 = ptarget[7];

    pdata[19] = start_nonce;

    while((pdata[19] < max_nonce) && !thr->work_restart) {

        neoscrypt((uchar *) pdata, (uchar *) hash, 0x80000620);

        /* Quick hash check */
        if(hash[7] <= t32) {
            /* Complete hash check */
            if(fulltest_le(hash, ptarget)) {
                *final_nonce = pdata[19];
                /* LE straight ordered */
                for(i = 0; i < 8; i++)
                  phash[i] = htole32(hash[i]);
                return(1);
            }
        }

        pdata[19] += inc_nonce;

    }

    *final_nonce = pdata[19];
    return(0);
}
#endif

#ifdef USE_SCRYPT
/* Scrypt(1024, 1, 1) with Salsa20/8 through NeoScrypt */
static int scanhash_altscrypt(struct thr_info *thr, uint *pdata, const uint *ptarget,
  uint *phash, uint start_nonce, uint max_nonce, uint *final_nonce) {
    uint hash[8], data[20];
    uint inc_nonce = 1;
    const uint t32 = ptarget[7];
    uint i;

    /* Convert BE to LE */
    for(i = 0; i < 19; i++)
      data[i] = be32toh(pdata[i]);

    data[19] = start_nonce;

    while((data[19] < max_nonce) && !thr->work_restart) {

        neoscrypt((uchar *) data, (uchar *) hash, 0x80000903);

        /* Quick hash check */
        if(hash[7] <= t32) {
            /* Complete hash check */
            if(fulltest_le(hash, ptarget)) {
                /* Convert LE to BE and write back */
                pdata[19] = htobe32(data[19]);
                *final_nonce = data[19];
                /* LE straight ordered */
                for(i = 0; i < 8; i++)
                  phash[i] = htole32(hash[i]);
                return(1);
            }
        }

        data[19] += inc_nonce;

    }

    *final_nonce = data[19];
    return(0);
}
#endif

static int64_t cpu_scanhash(struct thr_info *thr, struct work *work, int64_t max_nonce) {
    const int thr_id = thr->id;
    const uint first_nonce = work->blk.nonce;
    uint final_nonce, rc = 0;

    while(!thr->work_restart) {

        final_nonce = work->blk.nonce;

#ifdef USE_NEOSCRYPT
        if(opt_neoscrypt) {
            rc = scanhash_neoscrypt(thr, (uint *) work->data, (uint *) work->target,
              (uint *) work->hash, work->blk.nonce, max_nonce, &final_nonce);
        } else
#endif
#ifdef USE_SCRYPT
        if(opt_scrypt) {
            rc = scanhash_altscrypt(thr, (uint *) work->data, (uint *) work->target,
              (uint *) work->hash, work->blk.nonce, max_nonce, &final_nonce);
        } else
#endif
#ifdef USE_SHA256D
        if(opt_sha256d) {
            uchar hash1[64];
	    memcpy(&hash1[0], &hash1_init[0], sizeof(hash1));
            sha256_func func = sha256_funcs[opt_algo];
            rc = (*func) (thr, work->midstate, work->data, hash1, work->hash,
              work->target, max_nonce, &final_nonce, work->blk.nonce);
        } else
#endif
        {
            usleep(1000);
        }

        if(rc) {
            applog(LOG_DEBUG, "CPU thread %d found nonce 0x%X",
              dev_from_id(thr_id), final_nonce);
            submit_work_async(work, NULL);
            /* Set a new start nonce and keep hashing */
            work->blk.nonce = final_nonce + 1;
        } else break;

    }

    if(final_nonce != first_nonce) {
        work->blk.nonce = final_nonce + 1;
        return(final_nonce - first_nonce + 1);
    }

    return(0);
}

struct device_api cpu_api = {
	.dname = "cpu",
	.name = "CPU",
	.api_detect = cpu_detect,
	.thread_prepare = cpu_thread_prepare,
	.can_limit_work = cpu_can_limit_work,
	.thread_init = cpu_thread_init,
	.scanhash = cpu_scanhash,
};
#endif



