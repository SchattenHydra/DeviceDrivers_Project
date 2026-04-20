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
#include <stddef.h> 


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

typedef struct
{
    TListNode link;         /* Required by TList to link nodes together */
    int pid;                /* Who requested this? */
    int type;               /* DISK_READ or DISK_WRITE */
    int track;              /* Which track? */
    int firstSector;        /* Starting sector */
    int numSectors;         /* How many sectors? */
    void* buffer;           /* Data buffer */
    int status;             /* Result of the operation */
    int syncMbox;           /* Mailbox to wake the process when done */
} DiskRequest;

/* Create an array of lists, one for each disk */
static TList diskQueues[THREADS_MAX_DISKS];
static int diskSemaphores[THREADS_MAX_DISKS]; // <-- ADD THIS

static DevicesProcess devicesProcs[MAXPROC];
static DiskInformation diskInfo[THREADS_MAX_DISKS];

static inline void checkKernelMode(const char* functionName);
extern int DevicesEntryPoint(char*);

/* You'll need to map this function to your systemCallVectors array in SystemCallsEntryPoint */
void SleepSecondsHandler(system_call_arguments_t* args)
{
    checkKernelMode(__func__);

    // Arguments are stored in a 0-indexed array
    int seconds = (int)args->arguments[0];
    int pid = k_getpid();
    int currentTime;

    if (seconds <= 0)
    {
        args->arguments[0] = -1; // Return error in index 0
        return;
    }

    currentTime = system_clock();

    devicesProcs[pid].wakeTime = currentTime + (seconds * 1000000);
    devicesProcs[pid].isSleeping = 1;

    int dummy = 0;
    mailbox_receive(devicesProcs[pid].syncMbox, &dummy, sizeof(int), TRUE);

    args->arguments[0] = 0; // Return success in index 0
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

    /*Initialize the process table */
        for (int i = 0; i < MAXPROC; ++i)
        {
            devicesProcs[i].isSleeping = 0;
            devicesProcs[i].syncMbox = mailbox_create(0, sizeof(int));
        }

    // ADD THIS TO INITIALIZE QUEUES AND SEMAPHORES:
    for (i = 0; i < THREADS_MAX_DISKS; i++)
    {
        TListInitialize(&diskQueues[i], offsetof(DiskRequest, link), NULL);
        diskSemaphores[i] = k_semcreate(0);
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
        // Poke the SEMAPHORE to wake the driver so it can die cleanly
        // ==========================================================
        k_semv(diskSemaphores[i]);
        // ==========================================================

        k_join(diskPids[i], &status); // Now it will successfully reap!
    }

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
        // 1. Sleep here until a handler calls k_semv() indicating a new request!
        k_semp(diskSemaphores[unit]);

        if (signaled()) break; // Catch shutdown signals

        // 2. Pop the oldest request
        DiskRequest* nextReq = NULL;
        if (DISK_ARM_ALG == DISK_ARM_ALG_FCFS)
        {
            nextReq = (DiskRequest*)TListPopNode(&diskQueues[unit]);
        }

        if (nextReq != NULL)
        {
            // 3. Talk to the physical hardware
            device_control_block_t dcb;
            dcb.command = nextReq->type;
            dcb.input_data = nextReq->buffer;
            dcb.data_length = nextReq->numSectors * THREADS_DISK_SECTOR_SIZE;
            // NOTE: We will pack control1 and control2 in the next step!

            device_control(devName, dcb);

            // 4. Sleep until the physical disk finishes spinning
            result = wait_device(devName, &status);
            if (result != 0) break;

            // 5. Mark success and wake the user process
            nextReq->status = status;
            currentTrack = nextReq->track;

            int dummy = 0;
            mailbox_send(nextReq->syncMbox, &dummy, sizeof(int), TRUE);
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

