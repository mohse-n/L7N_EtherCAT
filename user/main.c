
#include "ecrt.h"

#include <string.h>
#include <stdio.h>
/* For setting the process's priority (setpriority) */
#include <sys/resource.h>
/* For pid_t and getpid() */
#include <unistd.h>
#include <sys/types.h>
/* For locking the program in RAM (mlockall) to prevent swapping */
#include <sys/mman.h>
/* clock_gettime, struct timespec, etc. */
#include <time.h>
/* Header for handling signals (definition of SIGINT) */
#include <signal.h>
/* For using real-time scheduling policy (FIFO) */
#include <sched.h>
/* For using uint32_t format specifier, PRIu32 */
#include <inttypes.h>

/*****************************************************************************/

/* One motor revolution increments the encoder by 2^19 -1. */
#define ENCODER_RES 524287
/* The maximum stack size which is guranteed safe to access without faulting. */       
#define MAX_SAFE_STACK (8 * 1024) 

/* Comment to disable distributed clocks */
#define DC
/* Measure the difference in reference slave's clock timstamp each cycle, and print the result. */
/* Note: Only works with DC enabled. */
#define MEASURE_TIMING

#define NSEC_PER_SEC (1000000000L)
#define FREQUENCY 1000
/* Period of motion loop, in nanoseconds */
#define PERIOD_NS (NSEC_PER_SEC / FREQUENCY)

#ifdef DC

/* SYNC0 event happens halfway through the cycle */
#define SHIFT0 (PERIOD_NS/2)
#define TIMESPEC2NS(T) ((uint64_t) (T).tv_sec * NSEC_PER_SEC + (T).tv_nsec)

#endif

/*****************************************************************************/

void ODwrite(ec_master_t* master, uint16_t slavePos, uint16_t index, uint8_t subIndex, uint8_t objectValue)
{
	/* Blocks until a reponse is received */
	uint8_t retVal = ecrt_master_sdo_download(master, slavePos, index, subIndex, &objectValue, sizeof(objectValue), NULL);
	/* retVal != 0: Failure */
	if (retVal)
		printf("OD write unsuccessful\n");
}

void initDrive(ec_master_t* master, uint16_t slavePos)
{
	/* Reset alarm */
	ODwrite(master, slavePos, 0x6040, 0x00, 128);
	/* Servo on and operational */
	ODwrite(master, slavePos, 0x6040, 0x00, 0xF);
	/* Mode of operation, CSP */
	ODwrite(master, slavePos, 0x6060, 0x00, 0x8);
}

/*****************************************************************************/

/* Copy-pasted from dc_user/main.c */
struct timespec timespec_add(struct timespec time1, struct timespec time2)
{
	struct timespec result;

	if ((time1.tv_nsec + time2.tv_nsec) >= NSEC_PER_SEC) 
	{
		result.tv_sec = time1.tv_sec + time2.tv_sec + 1;
		result.tv_nsec = time1.tv_nsec + time2.tv_nsec - NSEC_PER_SEC;
	} 
	else 
	{
		result.tv_sec = time1.tv_sec + time2.tv_sec;
		result.tv_nsec = time1.tv_nsec + time2.tv_nsec;
	}

	return result;
}

/*****************************************************************************/

/* We have to pass "master" to ecrt_release_master in signal_handler, but it is not possible
   to define one with more than one argument. Therefore, master should be a global variable. 
*/
ec_master_t* master;

void signal_handler(int sig)
{
	printf("\nReleasing master...\n");
	ecrt_release_master(master);
	pid_t pid = getpid();
	kill(pid, SIGKILL);
}

/*****************************************************************************/

void stack_prefault(void)
{
    unsigned char dummy[MAX_SAFE_STACK];

    memset(dummy, 0, MAX_SAFE_STACK);
}

/*****************************************************************************/

int main(int argc, char **argv)
{
	
	struct sched_param param = {};
	param.sched_priority = sched_get_priority_max(SCHED_FIFO);
	printf("Using priority %i.\n", param.sched_priority);
	if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) 
	{
		perror("sched_setscheduler failed\n");
	}
	
	/* Lock the program into RAM to prevent page faults and swapping */
	/* MCL_CURRENT: Lock in all current pages.
	   MCL_FUTURE:  Lock in pages for heap and stack and shared memory.
	*/
	if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
	{
		printf("mlockall failed\n");
		return -1;
	}
	
	stack_prefault();
	
	/* Register the signal handler function. */
	signal(SIGINT, signal_handler);
	
	/* Reserve the first master (0) (/etc/init.d/ethercat start) for this program */
	master = ecrt_request_master(0);
	if (!master)
		printf("Requesting master failed\n");
	
	initDrive(master, 0);
	initDrive(master, 1);
	
	uint16_t alias = 0;
	uint16_t position0 = 0;
	uint16_t position1 = 1;
	uint32_t vendor_id = 0x00007595;
	uint32_t product_code = 0x00000000;
	
	/* Creates and returns a slave configuration object, ec_slave_config_t*, for the given alias and position. */
	/* Returns NULL (0) in case of error and pointer to the configuration struct otherwise */
	ec_slave_config_t* drive0 = ecrt_master_slave_config(master, alias, position0, vendor_id, product_code);
	ec_slave_config_t* drive1 = ecrt_master_slave_config(master, alias, position1, vendor_id, product_code);
	
	
	/* If the drive0 = NULL or drive1 = NULL */
	if (!drive0 || !drive1)
	{
		printf("Failed to get slave configuration\n");
		return -1;
	}
	
	/* Structures obtained from $ethercat cstruct -p 0 */
	/***************************************************/
	/* Slave 0's structures */
	ec_pdo_entry_info_t slave_0_pdo_entries[] = 
	{
	{0x6040, 0x00, 16}, /* Controlword */
	{0x607a, 0x00, 32}, /* Target Position */
	{0x6041, 0x00, 16}, /* Statusword */
	{0x6064, 0x00, 32}, /* Position Actual Value */
	};
	
	ec_pdo_info_t slave_0_pdos[] =
	{
	{0x1601, 2, slave_0_pdo_entries + 0}, /* 2nd Receive PDO Mapping */
	{0x1a01, 2, slave_0_pdo_entries + 2}, /* 2nd Transmit PDO Mapping */
	};
	
	ec_sync_info_t slave_0_syncs[] =
	{
	{0, EC_DIR_OUTPUT, 0, NULL            , EC_WD_DISABLE},
	{1, EC_DIR_INPUT , 0, NULL            , EC_WD_DISABLE},
	{2, EC_DIR_OUTPUT, 1, slave_0_pdos + 0, EC_WD_DISABLE},
	{3, EC_DIR_INPUT , 1, slave_0_pdos + 1, EC_WD_DISABLE},
	{0xFF}
	};
	
	/* Slave 1's structures */
	ec_pdo_entry_info_t slave_1_pdo_entries[] = 
	{
	{0x6040, 0x00, 16}, /* Controlword */
	{0x607a, 0x00, 32}, /* Target Position */
	{0x6041, 0x00, 16}, /* Statusword */
	{0x6064, 0x00, 32}, /* Position Actual Value */
	};
	
	ec_pdo_info_t slave_1_pdos[] =
	{
	{0x1601, 2, slave_1_pdo_entries + 0}, /* 2nd Receive PDO Mapping */
	{0x1a01, 2, slave_1_pdo_entries + 2}, /* 2nd Transmit PDO Mapping */
	};
	
	ec_sync_info_t slave_1_syncs[] =
	{
	{0, EC_DIR_OUTPUT, 0, NULL            , EC_WD_DISABLE},
	{1, EC_DIR_INPUT , 0, NULL            , EC_WD_DISABLE},
	{2, EC_DIR_OUTPUT, 1, slave_1_pdos + 0, EC_WD_DISABLE},
	{3, EC_DIR_INPUT , 1, slave_1_pdos + 1, EC_WD_DISABLE},
	{0xFF}
	};
	
	/***************************************************/
	
	if (ecrt_slave_config_pdos(drive0, EC_END, slave_0_syncs))
	{
		printf("Failed to configure slave 0 PDOs\n");
		return -1;
	}
	
	if (ecrt_slave_config_pdos(drive1, EC_END, slave_1_syncs))
	{
		printf("Failed to configure slave 1 PDOs\n");
		return -1;
	}

	unsigned int offset_controlWord0, offset_targetPos0, offset_statusWord0, offset_actPos0;
	unsigned int offset_controlWord1, offset_targetPos1, offset_statusWord1, offset_actPos1;
	
	ec_pdo_entry_reg_t domain1_regs[] =
	{
	{0, 0, 0x00007595, 0x00000000, 0x6040, 0x00, &offset_controlWord0},
	{0, 0, 0x00007595, 0x00000000, 0x607a, 0x00, &offset_targetPos0  },
	{0, 0, 0x00007595, 0x00000000, 0x6041, 0x00, &offset_statusWord0 },
	{0, 0, 0x00007595, 0x00000000, 0x6064, 0x00, &offset_actPos0     },
	
	{0, 1, 0x00007595, 0x00000000, 0x6040, 0x00, &offset_controlWord1},
	{0, 1, 0x00007595, 0x00000000, 0x607a, 0x00, &offset_targetPos1  },
	{0, 1, 0x00007595, 0x00000000, 0x6041, 0x00, &offset_statusWord1 },
	{0, 1, 0x00007595, 0x00000000, 0x6064, 0x00, &offset_actPos1     },
	{}
	};
	
	/* Creates a new process data domain. */
	/* For process data exchange, at least one process data domain is needed. */
	ec_domain_t* domain1 = ecrt_master_create_domain(master);
	
	/* Registers PDOs for a domain. */
	/* Returns 0 on success. */
	if (ecrt_domain_reg_pdo_entry_list(domain1, domain1_regs))
	{
		printf("PDO entry registration failed\n");
		return -1;
	}
	
	#ifdef DC
	/* Do not enable Sync1 */
	ecrt_slave_config_dc(drive0, 0x0300, PERIOD_NS, SHIFT0, 0, 0);
	ecrt_slave_config_dc(drive1, 0x0300, PERIOD_NS, SHIFT0, 0, 0);
	#endif
	
	/* Up to this point, we have only requested the master. See log messages */
	printf("Activating master...\n");
	/* Important points from ecrt.h 
	   - This function tells the master that the configuration phase is finished and
	     the real-time operation will begin. 
	   - It tells the master state machine that the bus configuration is now to be applied.
	   - By calling the ecrt master activate() method, all slaves are configured according to
             the prior method calls and are brought into OP state.
	   - After this function has been called, the real-time application is in charge of cylically
	     calling ecrt_master_send() and ecrt_master_receive(). Before calling this function, the 
	     master thread is responsible for that. 
	   - This method allocated memory and should not be called in real-time context.
	*/
	     
	if (ecrt_master_activate(master))
		return -1;

	
	uint8_t* domain1_pd;
	/* Returns a pointer to (I think) the first byte of PDO data of the domain */
	if (!(domain1_pd = ecrt_domain_data(domain1)))
		return -1;
	
	ec_slave_config_state_t slaveState0;
	ec_slave_config_state_t slaveState1;
	struct timespec wakeupTime;
	
	#ifdef DC
	struct timespec	time;
	#endif
	
	struct timespec cycleTime = {0, PERIOD_NS};
	clock_gettime(CLOCK_MONOTONIC, &wakeupTime);
	
	/* The slaves (drives) enter OP mode after exchanging a few frames. */
	/* We exchange frames with no RPDOs (targetPos) untill all slaves have 
	   reached OP state, and then we break out of the loop.
	*/
	while (1)
	{
		
		wakeupTime = timespec_add(wakeupTime, cycleTime);
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeupTime, NULL);
		
		ecrt_master_receive(master);
		ecrt_domain_process(domain1);
		
		ecrt_slave_config_state(drive0, &slaveState0);
		ecrt_slave_config_state(drive1, &slaveState1);
		
		if (slaveState0.operational && slaveState1.operational)
		{
			printf("All slaves have reached OP state\n");
			break;
		}
	
		ecrt_domain_queue(domain1);
		
		#ifdef DC
		/* Distributed clocks */
		clock_gettime(CLOCK_MONOTONIC, &time);
		ecrt_master_application_time(master, TIMESPEC2NS(time));
		ecrt_master_sync_reference_clock(master);
		ecrt_master_sync_slave_clocks(master);
		#endif
		
		ecrt_master_send(master);
	
	}
	
	int32_t actPos0, targetPos0;
	int32_t actPos1, targetPos1;
	#ifde MEASURE_TIMING
	/* The slave time received in the current and the previous cycle */
	uint32_t t_cur, t_prev;
	#endif
	
	/* Update wakeupTime = current time */
	clock_gettime(CLOCK_MONOTONIC, &wakeupTime);
	
	while (1)
	{
		/* Wake up at wakeupTime + cycleTime. */
		/* Q: Why don't we call clock_gettime instead of assuming the previous cycle has exactly taken cycleTime? 
		   A: Perhaps better performance (no systemcall in the loop). 
		*/
		wakeupTime = timespec_add(wakeupTime, cycleTime);
		/* Sleep to adjust the update frequency */
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeupTime, NULL);
		/* Fetches received frames from the newtork device and processes the datagrams. */
		ecrt_master_receive(master);
		/* Evaluates the working counters of the received datagrams and outputs statistics,
		   if necessary.
		   This function is NOT essential to the receive/process/send procedure and can be 
		   commented out 
		*/
		ecrt_domain_process(domain1);
		
		#ifdef MEASURE_TIMING
		ecrt_master_reference_clock_time(master, &t_cur);
		printf("%" PRIu32 "\n", t_cur - t_prev);
		t_prev = t_curv;
		#endif
		/********************************************************************************/
		
		/* Read PDOs from the datagram */
		actPos0 = EC_READ_S32(domain1_pd + offset_actPos0);
		actPos1 = EC_READ_S32(domain1_pd + offset_actPos1);
		
		/* Process the received data */
		targetPos0 = actPos0 + 5000;
		targetPos1 = actPos1 - 5000;
		
		/* Write PDOs to the datagram */
		EC_WRITE_U8  (domain1_pd + offset_controlWord0, 0xF );
		EC_WRITE_S32 (domain1_pd + offset_targetPos0  , targetPos0);
		
		EC_WRITE_U8  (domain1_pd + offset_controlWord1, 0xF );
		EC_WRITE_S32 (domain1_pd + offset_targetPos1  , targetPos1);
		
		/********************************************************************************/
		/* Queues all domain datagrams in the master's datagram queue. 
		   Call this function to mark the domain's datagrams for exchanging at the
		   next call of ecrt_master_send() 
		*/
		ecrt_domain_queue(domain1);
		
		#ifdef DC
		/* Distributed clocks */
		clock_gettime(CLOCK_MONOTONIC, &time);
		ecrt_master_application_time(master, TIMESPEC2NS(time));
		ecrt_master_sync_reference_clock(master);
		ecrt_master_sync_slave_clocks(master);
		#endif
		
		/* Sends all datagrams in the queue.
		   This method takes all datagrams that have been queued for transmission,
		   puts them into frames, and passes them to the Ethernet device for sending. 
		*/
		ecrt_master_send(master);
	
	}
	
	return 0;
}
