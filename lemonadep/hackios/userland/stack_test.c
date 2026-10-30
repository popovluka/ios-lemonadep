/*
 * stack_test NAME...: where every thread of the programs with these names is, as a debugger would say
 * it -- each thread's pc and lr and the return addresses down its frame-pointer chain. Addresses in the
 * shared cache are also given unslid (as the cache file has them, base 0x180000000), for
 * `ipsw dyld a2s` on the host. For a program that has hung and says nothing about why (lemonadep:
 * Safari, whose main thread stops once a page is asked for).
 */
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* (libproc.h is not in the iOS SDK; libSystem has the calls) */
#define PROC_ALL_PIDS 1
extern int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);
extern int syscall(int number, ...);
#define shared_region_check_np(start) syscall(294 /* SYS_shared_region_check_np, not in the SDK's stubs */, (start))
extern kern_return_t task_for_pid(mach_port_t target, int pid, mach_port_t *task);

#define CACHE_UNSLID 0x180000000ull

static uint64_t cache_base;

static void
say_address(const char *what, uint64_t a)
{
	a &= 0x0000000fffffffffull;     /* (pointer authentication bits off) */
	if (cache_base != 0 && a >= cache_base && a < cache_base + 0x200000000ull) {
		printf("  %s 0x%llx  cache 0x%llx\n", what, a, a - cache_base + CACHE_UNSLID);
	} else {
		printf("  %s 0x%llx\n", what, a);
	}
}

static void
threads_of(int pid, const char *name)
{
	mach_port_t task;
	thread_act_array_t threads;
	mach_msg_type_number_t count;
	kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);

	if (kr != KERN_SUCCESS) {
		printf("stack_test: %s[%d]: task_for_pid: %#x\n", name, pid, kr);
		return;
	}
	if (task_threads(task, &threads, &count) != KERN_SUCCESS) {
		printf("stack_test: %s[%d]: no threads\n", name, pid);
		return;
	}
	printf("stack_test: %s[%d]: %u threads\n", name, pid, count);
	for (mach_msg_type_number_t t = 0; t < count; t++) {
		arm_thread_state64_t state;
		mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
		uint64_t fp;

		if (thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&state, &n) != KERN_SUCCESS) {
			printf(" thread %u: no state\n", t);
			continue;
		}
		printf(" thread %u:\n", t);
		say_address("pc", (uint64_t)__darwin_arm_thread_state64_get_pc(state));
		say_address("lr", (uint64_t)__darwin_arm_thread_state64_get_lr(state));
		fp = (uint64_t)__darwin_arm_thread_state64_get_fp(state);
		for (int depth = 0; depth < 40 && fp != 0; depth++) {
			uint64_t frame[2];
			vm_size_t got = 0;

			if (vm_read_overwrite(task, (vm_address_t)fp, sizeof(frame), (vm_address_t)frame, &got) != KERN_SUCCESS ||
			    got != sizeof(frame) || frame[1] == 0) {
				break;
			}
			say_address("ret", frame[1]);
			if ((frame[0] & 0x0000000fffffffffull) <= fp) {
				break;
			}
			fp = frame[0] & 0x0000000fffffffffull;
		}
	}
}

/*
 * The kernel's own stackshot of one process (root needs no entitlement for it here): kcdata, with each
 * thread's user and kernel frames, what it waits on and the libraries loaded, printed as hex lines
 * "SSHEX ..." for xnu's tools/lldbmacros/kcdata.py on the host.
 */
struct stackshot_config {
	int sc_pid;
	uint64_t sc_flags, sc_delta_timestamp, sc_buffer;
	uint32_t sc_size;
	uint64_t sc_out_buffer_addr, sc_out_size_addr, sc_pagetable_mask;
};

static void
kcdata_of(int pid, const char *name)
{
	uint64_t buffer = 0;
	uint32_t size = 0;
	struct stackshot_config config = {
		.sc_pid = pid,
		/* KCDATA_FORMAT, SAVE_LOADINFO, THREAD_WAITINFO, GET_DQ, NO_IO_STATS */
		.sc_flags = 0x10000 | 0x2 | 0x1000000 | 0x1 | 0x800000,
		.sc_out_buffer_addr = (uint64_t)(uintptr_t)&buffer, .sc_out_size_addr = (uint64_t)(uintptr_t)&size,
	};
	int r = syscall(491 /* stack_snapshot_with_config */, 1 /* STACKSHOT_CONFIG_TYPE */, &config, sizeof(config));

	printf("stack_test: kcdata of %s[%d]: %d, %u bytes\n", name, pid, r, size);
	for (uint32_t i = 0; r == 0 && i < size; i += 64) {
		printf("SSHEX %d ", pid);
		for (uint32_t k = i; k < i + 64 && k < size; k++) {
			printf("%02x", ((const uint8_t *)(uintptr_t)buffer)[k]);
		}
		printf("\n");
	}
}

int
main(int argc, char **argv)
{
	int pids[4096];
	int n = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)) / (int)sizeof(int);
	int kcdata = argc > 1 && strcmp(argv[1], "-k") == 0;

	if (kcdata) {
		argv++, argc--;
	}
	shared_region_check_np(&cache_base);
	printf("stack_test: the shared cache is at 0x%llx (slide 0x%llx)\n", cache_base, cache_base - CACHE_UNSLID);
	for (int a = 1; a < argc; a++) {
		for (int i = 0; i < n; i++) {
			char name[64] = "";

			if (pids[i] > 0 && proc_name(pids[i], name, sizeof(name)) > 0 && strcmp(name, argv[a]) == 0) {
				if (kcdata) {
					kcdata_of(pids[i], name);
				} else {
					threads_of(pids[i], name);
				}
			}
		}
	}
	return 0;
}
