#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <THREADSLib.h>
#include <Messaging.h>
#include <Scheduler.h>
#include <TList.h>
#include <libuser.h>
#include <SystemCalls.h>
#include <Devices.h>

/* Set the disk arm scheduling algorithm.
 * See Devices.h for available constants (DISK_ARM_ALG_FCFS, DISK_ARM_ALG_SSTF, etc.).
 * You must implement FCFS and SSTF. Change this value to test each algorithm.
 * Submissions will be assessed with DISK_ARM_ALG_FCFS and DISK_ARM_ALG_SSTF. */
#define DISK_ARM_ALG   DISK_ARM_ALG_FCFS

static int ClockDriver(char*);
static int DiskDriver(char*);

typedef struct devices_proc
{
    struct devices_proc* pNext;
    struct devices_proc* pPrev;
    int pid;


    /* Additions for the Clock Driver */
    int wakeTime;       /* Target wake time in milliseconds */
    int isSleeping;     /* Boolean flag: 1 if sleeping, 0 if awake */
    int syncMbox;       /* A private mailbox used to block this specific process */


} DevicesProcess;

typedef struct
{
    int tracks;
    int platters;
    char deviceName[THREADS_MAX_DEVICE_NAME];
} DiskInformation;

static DevicesProcess devicesProcs[MAXPROC];
static DiskInformation diskInfo[THREADS_MAX_DISKS];

static inline void checkKernelMode(const char* functionName);
extern int DevicesEntryPoint(char*);

/* You'll need to map this function to your systemCallVectors array in SystemCallsEntryPoint */
void SleepSecondsHandler(sysargs* args)
{
    checkKernelMode(__func__);

    // Cast to intptr_t first to clear the C4311 compiler warning
    int seconds = (int)(intptr_t)args->arg1;

    // Use k_getpid() instead of getpid()
    int pid = k_getpid();
    int currentTime;

    // Based on the output of DevicesTest20, invalid sleep times return -1
    if (seconds <= 0)
    {
        args->arg4 = (void*)-1;
        return;
    }

    // ADD THIS (Using the kernel clock function from THREADSLib.h):
    currentTime = system_clock();

    // The system clock is in MICROSECONDS. 1 second = 1,000,000 ticks.
    devicesProcs[pid].wakeTime = currentTime + (seconds * 1000000);
    devicesProcs[pid].isSleeping = 1;

    // ACTUAL BLOCKING MECHANISM:
    int dummy = 0;

    // Use mailbox_receive with the TRUE flag to block
    mailbox_receive(devicesProcs[pid].syncMbox, &dummy, sizeof(int), TRUE);

    // When MboxReceive unblocks, the clock driver has woken us up!
    args->arg4 = (void*)0; // Return success status
}

int SystemCallsEntryPoint(char* arg)
{
    char    buf[25];
    char    name[128];
    int     i;
    int     clockPID = 0;
    int     diskPids[THREADS_MAX_DISKS];
    int     status;

    checkKernelMode(__func__);

    /* Assign system call handlers */
    systemCallVector[SYS_SLEEP] = (void*)SleepSecondsHandler;

    /* Initialize the process table */
    for (int i = 0; i < MAXPROC; ++i)
    {
        devicesProcs[i].isSleeping = 0;
        // FIX: Use the correct function from Messaging.h
        devicesProcs[i].syncMbox = mailbox_create(0, sizeof(int));
    }

    /* Create and start the clock driver */
    clockPID = k_spawn("Clock driver", ClockDriver, NULL, THREADS_MIN_STACK_SIZE, HIGHEST_PRIORITY);
    if (clockPID < 0)
    {
        console_output(TRUE, "start3(): Can't create clock driver\n");
        stop(1);
    }

    /* Create the disk drivers */
    for (i = 0; i < THREADS_MAX_DISKS; i++)
    {
        sprintf(buf, "%d", i);
        sprintf(name, "DiskDriver%d", i);
        diskPids[i] = k_spawn(name, DiskDriver, buf, THREADS_MIN_STACK_SIZE * 4, HIGHEST_PRIORITY);
        if (diskPids[i] < 0)
        {
            console_output(TRUE, "start3(): Can't create disk driver %d\n", i);
            stop(1);
        }
    }

    /* Create first user-level process and wait for it to finish */
    sys_spawn("DevicesEntryPoint", DevicesEntryPoint, NULL, 8 * THREADS_MIN_STACK_SIZE, 3);
    sys_wait(&status);

    // =================================================================
    // Cleanup:Kill AND REAP the background drivers so the OS can cleanly shut down
    // =================================================================
    k_kill(clockPID, SIG_TERM);
    k_join(clockPID, &status);   // <-- ADD THIS: Reap the clock driver

    for (i = 0; i < THREADS_MAX_DISKS; i++)
    {
        k_kill(diskPids[i], SIG_TERM);

        // ==========================================================
        // Poke the hardware to fire an interrupt and wake the driver
        // ==========================================================
        char devName[16];
        sprintf(devName, "disk%d", i);

        device_control_block_t dcb;
        dcb.command = DISK_INFO;
        dcb.control1 = 0;
        dcb.control2 = 0;
        dcb.input_data = NULL;
        dcb.output_data = NULL;
        dcb.data_length = 0;

        device_control(devName, dcb);
        // ==========================================================

        k_join(diskPids[i], &status); // Now it will successfully reap!
    }
    // =================================================================

    return 0;
}


static int ClockDriver(char* arg)
{
    int result;
    int status;
    int currentTime;
    int i;

    set_psr(get_psr() | PSR_INTERRUPTS);

    while (!signaled())
    {
        /* 1. Wait for the hardware clock tick */
        result = wait_device("clock", &status);
        if (result != 0)
        {
            return 0;
        }

        /* 2. Determine current time. */
        //Don't trust 'status'. Query the clock directly!
        currentTime = system_clock();

        /* 3. Check the process table for sleeping processes whose time has come */
        for (i = 0; i < MAXPROC; i++)
        {
            if (devicesProcs[i].isSleeping && currentTime >= devicesProcs[i].wakeTime)
            {
                // Time's up! Mark as awake.
                devicesProcs[i].isSleeping = 0;

                // ACTUAL WAKE MECHANISM:
                int dummy = 0;
                // Use mailbox_send with the TRUE flag
                mailbox_send(devicesProcs[i].syncMbox, &dummy, sizeof(int), TRUE);
            }
        }
    }
    /* Compute the current time and wake up any processes whose time has come */
    return 0;
        
   
}


static int DiskDriver(char* arg)
{
    int unit = atoi(arg);
    int currentTrack = 0;
    device_control_block_t devRequest;

    char devName[16];
    sprintf(devName, "disk%d", unit);
    int status;
    int result; // <-- ADD THIS

    set_psr(get_psr() | PSR_INTERRUPTS);

    /* Read the disk info */

    /* Operating loop */
    while (!signaled())
    {
        // Capture the return value of wait_device
        result = wait_device(devName, &status);

        // If the system is shutting down, wait_device returns a non-zero error.
        // We must break the loop so the thread can cleanly exit.
        if (result != 0)
        {
            break;
        }
    }
    return 0;
}


struct psr_bits {
    unsigned int cur_int_enable : 1;
    unsigned int cur_mode : 1;
    unsigned int prev_int_enable : 1;
    unsigned int prev_mode : 1;
    unsigned int unused : 28;
};

union psr_values {
    struct psr_bits bits;
    unsigned int integer_part;
};

/*****************************************************************************
   Name - checkKernelMode
   Purpose - Checks the PSR for kernel mode and stops if in user mode
   Parameters -
   Returns -
   Side Effects - Will stop if not in kernel mode
****************************************************************************/
static inline void checkKernelMode(const char* functionName)
{
    union psr_values psrValue;

    psrValue.integer_part = get_psr();
    if (psrValue.bits.cur_mode == 0)
    {
        console_output(FALSE, "Kernel mode expected, but function called in user mode.\n");
        stop(1);
    }
}

