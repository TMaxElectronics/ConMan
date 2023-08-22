#ifndef CONMAN_CONF
#define CONMAN_CONF

#include <sys/kmem.h>

/*
 * ConMan configuration
 * 
 * This file contains the defines necessary to use the conman.
 * 
 * Location of the configuration can be selected, but it is critical that this does not change between program versions or the parameters will be lost.
 * 
 */

//Serialnumber address is recommended to be in boot flash. This will also contain the first stage bootloader and should never be re-written.
//put the serialnumber at the very end of boot flash
#define __KSEG0_BOOT_MEM_BASE KVA1_TO_KVA0(__KSEG1_BOOT_MEM_BASE)       //why don't the mem-defs contain this??
#define __KSEG0_BOOT_MEM_LENGTH 0xbf0                                   //kinda hackey, TODO can we make this dynamic? 
#define CONMAN_SERIALNR_ADDRESS ( ( (uint32_t) ((__KSEG0_BOOT_MEM_BASE + __KSEG0_BOOT_MEM_LENGTH) - sizeof(ConMan_SerialNumber_t)) / 16 ) * 16 ) //byte align to 16 byte-boundaries 

//This address is critical and MUST NOT CHANGE unless the flash is completely re-written. It contains the descriptor of the data array, the location of which is kinda dynamic as it is read from the descriptor at startup
#define CONMAN_MEMDESCRIPTOR_ADDRESS 0x9d030000
#define CONMAN_DATA_ADDRESS 0x9d030010
#define CONMAN_DATA_SIZE 4096

#endif