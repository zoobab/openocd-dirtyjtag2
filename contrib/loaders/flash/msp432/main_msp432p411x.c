/* SPDX-License-Identifier: BSD-3-Clause */

/******************************************************************************
*
* Copyright (C) 2013-2018 Texas Instruments Incorporated - http://www.ti.com/
*
******************************************************************************/

#include <stdint.h>
#include <stdbool.h>
#include "driverlib.h"

#include "MSP432P4_FlashLibIf.h"

/* Number of erase repeats until timeout */
#define FLASH_MAX_REPEATS 5

/* Local prototypes */
void msp432_flash_init(void);
void msp432_flash_mass_erase(void);
void msp432_flash_sector_erase(void);
void msp432_flash_write(void);
void msp432_flash_continous_write(void);
void msp432_flash_exit(void);
void unlock_flash_sectors(void);
void unlock_all_flash_sectors(void);
void lock_all_flash_sectors(void);
void __cs_set_dco_frequency_range(uint32_t dco_freq);
static bool program_device(void *src, void *dest, uint32_t length);

struct backup_params {
	uint32_t BANK0_WAIT_RESTORE;
	uint32_t BANK1_WAIT_RESTORE;
	uint32_t CS_DC0_FREQ_RESTORE;
	uint8_t  VCORE_LEVEL_RESTORE;
	uint8_t  PCM_VCORE_LEVEL_RESTORE;
};

#define BACKUP_PARAMS      ((struct backup_params *) 0x20000180)
#define INFO_FLASH_START  __INFO_FLASH_A_TECH_START__
#define INFO_FLASH_MIDDLE __INFO_FLASH_A_TECH_MIDDLE__
#define BSL_FLASH_START   BSL_API_TABLE_ADDR

/* Main with trampoline */
int main(void)
{
	/* Halt watchdog */
	MAP_WDT_A_HOLD_TIMER();

	/* Disable interrupts */
	cpu_cpsid();

	while (1) {
		switch (FLASH_LOADER->FLASH_FUNCTION) {
			case FLASH_INIT:
				FLASH_LOADER->RETURN_CODE = FLASH_BUSY;
				msp432_flash_init();
				FLASH_LOADER->FLASH_FUNCTION = 0;
				break;
			case FLASH_MASS_ERASE:
				FLASH_LOADER->RETURN_CODE = FLASH_BUSY;
				msp432_flash_mass_erase();
				FLASH_LOADER->FLASH_FUNCTION = 0;
				break;
			case FLASH_SECTOR_ERASE:
				FLASH_LOADER->RETURN_CODE = FLASH_BUSY;
				msp432_flash_sector_erase();
				FLASH_LOADER->FLASH_FUNCTION = 0;
				break;
			case FLASH_PROGRAM:
				FLASH_LOADER->RETURN_CODE = FLASH_BUSY;
				msp432_flash_write();
				FLASH_LOADER->FLASH_FUNCTION = 0;
				break;
			case FLASH_CONTINUOUS_PROGRAM:
				FLASH_LOADER->RETURN_CODE = FLASH_BUSY;
				msp432_flash_continous_write();
				FLASH_LOADER->FLASH_FUNCTION = 0;
				break;
			case FLASH_EXIT:
				FLASH_LOADER->RETURN_CODE = FLASH_BUSY;
				msp432_flash_exit();
				FLASH_LOADER->FLASH_FUNCTION = 0;
				break;
			case FLASH_NO_COMMAND:
				break;
			default:
				FLASH_LOADER->RETURN_CODE = FLASH_WRONG_COMMAND;
				break;
		}
	}
}

/* Initialize flash */
void msp432_flash_init(void)
{
	bool success = false;

	/* Point to vector table in RAM */
	SCB->VTOR = (uint32_t)0x01000000;

	/* backup system parameters */
	BACKUP_PARAMS->BANK0_WAIT_RESTORE =
		MAP_FLASH_CTL_A_GET_WAIT_STATE(FLASH_A_BANK0);
	BACKUP_PARAMS->BANK1_WAIT_RESTORE =
		MAP_FLASH_CTL_A_GET_WAIT_STATE(FLASH_A_BANK1);
	BACKUP_PARAMS->VCORE_LEVEL_RESTORE = MAP_PCM_GET_CORE_VOLTAGE_LEVEL();
	BACKUP_PARAMS->PCM_VCORE_LEVEL_RESTORE = MAP_PCM_GET_POWER_STATE();
	BACKUP_PARAMS->CS_DC0_FREQ_RESTORE = CS->CTL0 & CS_CTL0_DCORSEL_MASK;

	/* set parameters for flashing */
	success = MAP_PCM_SET_POWER_STATE(PCM_AM_LDO_VCORE0);

	/* Set Flash wait states to 2 */
	MAP_FLASH_CTL_A_SET_WAIT_STATE(FLASH_A_BANK0, 2);
	MAP_FLASH_CTL_A_SET_WAIT_STATE(FLASH_A_BANK1, 2);

	/* Set CPU speed to 24MHz */
	__cs_set_dco_frequency_range(CS_DCO_FREQUENCY_24);

	if (!success) {
		/* Indicate failed power switch */
		FLASH_LOADER->RETURN_CODE = FLASH_POWER_ERROR;
	} else
		FLASH_LOADER->RETURN_CODE = FLASH_SUCCESS;
}

/* Erase entire flash */
void msp432_flash_mass_erase(void)
{
	bool success = false;

	/* Allow flash writes */
	unlock_flash_sectors();

	/* Allow some mass erase repeats before timeout with error */
	int erase_repeats = FLASH_MAX_REPEATS;
	while (!success && (erase_repeats > 0)) {
		/* Mass erase with post-verify */
		success = ROM_FLASH_CTL_A_PERFORM_MASS_ERASE();
		erase_repeats--;
	}

	if (erase_repeats == 0)
		FLASH_LOADER->RETURN_CODE = FLASH_VERIFY_ERROR;
	else
		FLASH_LOADER->RETURN_CODE = FLASH_SUCCESS;

	/* Block flash writes */
	lock_all_flash_sectors();
}

/* Erase one flash sector */
void msp432_flash_sector_erase(void)
{
	bool success = false;

	/* Allow flash writes */
	unlock_all_flash_sectors();

	/* Allow some sector erase repeats before timeout with error */
	int erase_repeats = FLASH_MAX_REPEATS;
	while (!success && (erase_repeats > 0)) {
		/* Sector erase with post-verify */
		success = MAP_FLASH_CTL_A_ERASE_SECTOR(FLASH_LOADER->DST_ADDRESS);
		erase_repeats--;
	}

	if (erase_repeats == 0)
		FLASH_LOADER->RETURN_CODE = FLASH_ERROR;
	else
		FLASH_LOADER->RETURN_CODE = FLASH_SUCCESS;

	/* Block flash writes */
	lock_all_flash_sectors();
}

/* Write data to flash with the help of DriverLib */
void msp432_flash_write(void)
{
	bool success = false;

	/* Allow flash writes */
	unlock_all_flash_sectors();

	while (!(FLASH_LOADER->BUFFER1_STATUS_REGISTER & BUFFER_DATA_READY))
		;

	FLASH_LOADER->BUFFER1_STATUS_REGISTER |= BUFFER_ACTIVE;

	/* Program memory */
	success = program_device((uint32_t *)RAM_LOADER_BUFFER1,
		(void *)FLASH_LOADER->DST_ADDRESS, FLASH_LOADER->SRC_LENGTH);

	FLASH_LOADER->BUFFER1_STATUS_REGISTER &=
		~(BUFFER_ACTIVE | BUFFER_DATA_READY);

	/* Block flash writes */
	lock_all_flash_sectors();

	if (!success)
		FLASH_LOADER->RETURN_CODE = FLASH_ERROR;
	else
		FLASH_LOADER->RETURN_CODE = FLASH_SUCCESS;
}

/* Write data to flash with the help of DriverLib with auto-increment */
void msp432_flash_continous_write(void)
{
	bool buffer1_in_use = false;
	bool buffer2_in_use = false;
	uint32_t *src_address = NULL;
	bool success = false;

	uint32_t bytes_to_write = FLASH_LOADER->SRC_LENGTH;
	uint32_t write_package = 0;
	uint32_t start_addr = FLASH_LOADER->DST_ADDRESS;

	while (bytes_to_write > 0) {
		if (bytes_to_write > SRC_LENGTH_MAX) {
			write_package = SRC_LENGTH_MAX;
			bytes_to_write -= write_package;
		} else {
			write_package = bytes_to_write;
			bytes_to_write -= write_package;
		}
		unlock_all_flash_sectors();
		while (!(FLASH_LOADER->BUFFER1_STATUS_REGISTER & BUFFER_DATA_READY) &&
			!(FLASH_LOADER->BUFFER2_STATUS_REGISTER & BUFFER_DATA_READY))
			;

		if (FLASH_LOADER->BUFFER1_STATUS_REGISTER & BUFFER_DATA_READY) {
			FLASH_LOADER->BUFFER1_STATUS_REGISTER |= BUFFER_ACTIVE;
			src_address = (uint32_t *) RAM_LOADER_BUFFER1;
			buffer1_in_use = true;
		} else if (FLASH_LOADER->BUFFER2_STATUS_REGISTER & BUFFER_DATA_READY) {
			FLASH_LOADER->BUFFER2_STATUS_REGISTER |= BUFFER_ACTIVE;
			src_address = (uint32_t *) RAM_LOADER_BUFFER2;
			buffer2_in_use = true;
		}
		if (buffer1_in_use || buffer2_in_use) {
			success = program_device(src_address, (void *) start_addr, write_package);
			start_addr += write_package;
		}
		if (buffer1_in_use) {
			FLASH_LOADER->BUFFER1_STATUS_REGISTER &= ~(BUFFER_ACTIVE | BUFFER_DATA_READY);
			buffer1_in_use = false;
		} else if (buffer2_in_use) {
			FLASH_LOADER->BUFFER2_STATUS_REGISTER &= ~(BUFFER_ACTIVE | BUFFER_DATA_READY);
			buffer2_in_use = false;
		}
		/* Block flash writes */
		lock_all_flash_sectors();

		if (!success) {
			FLASH_LOADER->RETURN_CODE = FLASH_ERROR;
			break;
		}
	}
	if (success)
		FLASH_LOADER->RETURN_CODE = FLASH_SUCCESS;
}

/* Unlock Main/Info Flash sectors */
void unlock_flash_sectors(void)
{
	if (FLASH_LOADER->ERASE_PARAM & ERASE_MAIN)
		MAP_FLASH_CTL_A_UNPROTECT_MEMORY(FLASH_BASE, FLASH_BASE +
			MAP_SYS_CTL_A_GET_FLASH_SIZE() - 1);

	if (FLASH_LOADER->ERASE_PARAM & ERASE_INFO) {
		MAP_FLASH_CTL_A_UNPROTECT_MEMORY(INFO_FLASH_START, TLV_BASE - 1);
		if (FLASH_LOADER->UNLOCK_BSL == UNLOCK_BSL_KEY)
			MAP_FLASH_CTL_A_UNPROTECT_MEMORY(BSL_FLASH_START,
				INFO_FLASH_MIDDLE - 1);
		MAP_FLASH_CTL_A_UNPROTECT_MEMORY(INFO_FLASH_MIDDLE, INFO_FLASH_MIDDLE +
			MAP_SYS_CTL_A_GET_INFO_FLASH_SIZE() - 1);
	}
}

/* Unlock All Flash sectors */
void unlock_all_flash_sectors(void)
{
	MAP_FLASH_CTL_A_UNPROTECT_MEMORY(FLASH_BASE, FLASH_BASE +
		MAP_SYS_CTL_A_GET_FLASH_SIZE() - 1);
	MAP_FLASH_CTL_A_UNPROTECT_MEMORY(INFO_FLASH_START, TLV_BASE - 1);
	if (FLASH_LOADER->UNLOCK_BSL == UNLOCK_BSL_KEY)
		MAP_FLASH_CTL_A_UNPROTECT_MEMORY(BSL_FLASH_START,
			INFO_FLASH_MIDDLE - 1);
	MAP_FLASH_CTL_A_UNPROTECT_MEMORY(INFO_FLASH_MIDDLE,  INFO_FLASH_MIDDLE +
		MAP_SYS_CTL_A_GET_INFO_FLASH_SIZE() - 1);
}

/* Lock all Flash sectors */
void lock_all_flash_sectors(void)
{
	MAP_FLASH_CTL_A_PROTECT_MEMORY(FLASH_BASE, FLASH_BASE +
		MAP_SYS_CTL_A_GET_FLASH_SIZE() - 1);
	MAP_FLASH_CTL_A_PROTECT_MEMORY(INFO_FLASH_START, INFO_FLASH_START +
		MAP_SYS_CTL_A_GET_INFO_FLASH_SIZE() - 1);
}

/* Force DCO frequency range */
void __cs_set_dco_frequency_range(uint32_t dco_freq)
{
	/* Unlocking the CS Module */
	CS->KEY = CS_KEY_VAL;

	/* Resetting Tuning Parameters and Setting the frequency */
	CS->CTL0 = (CS->CTL0 & ~CS_CTL0_DCORSEL_MASK) | dco_freq;

	/* Locking the CS Module */
	CS->KEY = 0;
}

/* Exit flash programming */
void msp432_flash_exit(void)
{
	bool success = false;

	/* Restore modified registers, in reverse order */
	__cs_set_dco_frequency_range(CS_DCO_FREQUENCY_3);

	MAP_FLASH_CTL_A_SET_WAIT_STATE(FLASH_A_BANK0,
		BACKUP_PARAMS->BANK0_WAIT_RESTORE);
	MAP_FLASH_CTL_A_SET_WAIT_STATE(FLASH_A_BANK1,
		BACKUP_PARAMS->BANK1_WAIT_RESTORE);

	success = MAP_PCM_SET_POWER_STATE(BACKUP_PARAMS->PCM_VCORE_LEVEL_RESTORE);

	success &= MAP_PCM_SET_CORE_VOLTAGE_LEVEL(
		BACKUP_PARAMS->VCORE_LEVEL_RESTORE);

	__cs_set_dco_frequency_range(BACKUP_PARAMS->CS_DC0_FREQ_RESTORE);

	/* Point to vector table in Flash */
	SCB->VTOR = (uint32_t)0x00000000;

	if (!success)
		FLASH_LOADER->RETURN_CODE = FLASH_ERROR;
	else
		FLASH_LOADER->RETURN_CODE = FLASH_SUCCESS;
}

static bool program_device(void *src, void *dest, uint32_t length)
{
	uint32_t dst_address = (uint32_t)dest;

	/* Flash main memory first, then information memory */
	if ((dst_address < INFO_FLASH_START) && ((dst_address + length) >
		INFO_FLASH_START)) {
		uint32_t block_length = INFO_FLASH_START - dst_address;
		uint32_t src_address = (uint32_t)src;
		/* Main memory block */
		bool success = MAP_FLASH_CTL_A_PROGRAM_MEMORY(src, dest, block_length);

		src_address = src_address + block_length;
		block_length = length - block_length;
		/* Information memory block */
		success &= MAP_FLASH_CTL_A_PROGRAM_MEMORY((void *)src_address,
			(void *)INFO_FLASH_START, block_length);
		return success;
	} else
		return MAP_FLASH_CTL_A_PROGRAM_MEMORY(src, dest, length);
}
