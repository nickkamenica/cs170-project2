#include <pthread.h>
#include <stdlib.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <stdbool.h>
#include <string.h>

#define MAX_THREADS 128
#define STACK_SIZE 32767

// 50 milliseconds
#define THREAD_TIMER 50000  

// register indices
#define JB_RBX 0
#define JB_RBP 1
#define JB_R12 2
#define JB_R13 3
#define JB_R14 4
#define JB_R15 5
#define JB_RSP 6
#define JB_PC  7

// Thread states
typedef enum {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_EXITED
} thread_state_t;

// Thread Control Block
typedef struct {
    pthread_t thread_id;
    thread_state_t state;
    void *stack;
    jmp_buf context;
    void *(*start_routine)(void*);
    void *arg;
} TCB;

// Global state !
static TCB thread_table[MAX_THREADS];
static int current_thread_index = 0;
static bool initialized = false;

static void schedule(void);
static void sigalrm_handler(int sig);
static void thread_wrapper(void);
static void init_thread_system(void);
static long int i64_ptr_mangle(long int p);

//from canvas
static long int i64_ptr_mangle(long int p) {
    long int ret;
    asm(" mov %1, %%rax;\n"
        " xor %%fs:0x30, %%rax;"
        " rol $0x11, %%rax;"
        " mov %%rax, %0;"
    : "=r"(ret)
    : "r"(p)
    : "%rax"
    );
    return ret;
}

// Initialize
static void init_thread_system(void) {
    if (initialized) {
        return;
    }
    
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_table[i].thread_id = 0;
        thread_table[i].state = THREAD_EXITED;
        thread_table[i].stack = NULL;
        thread_table[i].start_routine = NULL;
        thread_table[i].arg = NULL;
    }
    
    // Set up thread 0
    thread_table[0].thread_id = 0;
    thread_table[0].state = THREAD_RUNNING;
    thread_table[0].stack = NULL;
    thread_table[0].start_routine = NULL;
    thread_table[0].arg = NULL;
    
    current_thread_index = 0;
    
    // signal handler for SIGALRM
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_handler;

    sa.sa_flags = SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    
    // Set up timer for round robin
    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = THREAD_TIMER;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = THREAD_TIMER;
    setitimer(ITIMER_REAL, &timer, NULL);
    
    initialized = true;
}

// runs when thread starts
static void thread_wrapper(void) {
    // Get TCB of current thread
    TCB *tcb = &thread_table[current_thread_index];
    
    void *result = tcb->start_routine(tcb->arg);
    
    // when start routine returns, call exit
    pthread_exit(result);
}

// Create new thread
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, 
                   void *(*start_routine)(void*), void *arg) {
    if (!initialized) {
        init_thread_system();
    }
    
    // Find a slot in thread table
    int new_thread_index = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state == THREAD_EXITED) {
            new_thread_index = i;
            break;
        }
    }
    
    if (new_thread_index == -1) {
        return -1;  // No spots left :(
    }
    
    // new thread = new space in stack
    void *stack = malloc(STACK_SIZE);
    if (stack == NULL) {
        return -1;  // failed
    }
    
    // Initialize TCB
    TCB *tcb = &thread_table[new_thread_index];
    tcb->thread_id = new_thread_index;
    tcb->state = THREAD_READY;
    tcb->stack = stack;
    tcb->start_routine = start_routine;
    tcb->arg = arg;
    
    setjmp(tcb->context);
    
    // Stack grows downward
    void *stack_top = (void *)((unsigned long)stack + STACK_SIZE);
    
    long int *jb = (long int *)(tcb->context);
    
    // Set program counter to thread_wrapper
    jb[JB_PC] = i64_ptr_mangle((long int)thread_wrapper);
    
    unsigned long stack_address = (unsigned long)stack_top;
    stack_address &= ~0xFUL;  // Align to 16 bytes
    jb[JB_RSP] = i64_ptr_mangle(stack_address);
    
    // Return thread ID
    *thread = tcb->thread_id;
    
    return 0;
}

void pthread_exit(void *value_ptr) {
    // Mark current thread as exited
    TCB *tcb = &thread_table[current_thread_index];
    tcb->state = THREAD_EXITED;
    
    // Check if all threads exited
    bool all_exited = true;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state != THREAD_EXITED) {
            all_exited = false;
            break;
        }
    }
    
    if (all_exited) {
        exit(0);
    }
    
    schedule();
    
    __builtin_unreachable();
}

// Get current thread ID
pthread_t pthread_self(void) {
    if (!initialized) {
        init_thread_system();
    }
    return thread_table[current_thread_index].thread_id;
}

// Round-robin
static void schedule(void) {
    int prev_thread_idx = current_thread_index;
    
    if (thread_table[prev_thread_idx].state != THREAD_EXITED) {
        // Save current thread context
        int ret = setjmp(thread_table[prev_thread_idx].context);
        if (ret != 0) {
            return;
        }
        
        // Mark as ready 
        thread_table[prev_thread_idx].state = THREAD_READY;
    }
    
    // round robin
    int next_thread_idx = -1;
    int search_start = (current_thread_index + 1) % MAX_THREADS;
    
    for (int i = 0; i < MAX_THREADS; i++) {
        int idx = (search_start + i) % MAX_THREADS;
        if (thread_table[idx].state == THREAD_READY) {
            next_thread_idx = idx;
            break;
        }
    }
    
    if (next_thread_idx == -1) {
        if (thread_table[current_thread_index].state == THREAD_READY) {
            next_thread_idx = current_thread_index;
        } else {
            // shouldnt happen
            exit(1);
        }
    }
    
    // Switch to next thread
    current_thread_index = next_thread_idx;
    thread_table[current_thread_index].state = THREAD_RUNNING;
    
    // get next thread's context
    longjmp(thread_table[current_thread_index].context, 1);
}

// Signal handler for SIGALRM 
static void sigalrm_handler(int sig) {
    schedule();
}