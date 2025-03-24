#ifndef CONMAN_INC
#define CONMAN_INC

#include <sys/kmem.h>
#include "ConManConfig.h"

#define CONMAN_DESCRIPTOR_ADDRESS_TO_DATA_ADDRESS(X) (uint8_t*) ((uint32_t) X + sizeof(ConMan_ParameterDescriptor_t))

#define DATA_LOADED_HANDLER(CBH, DATAPTR) (*(CBH))(CONFIG_ENTRY_LOADED, DATAPTR)
#define DATA_CREATED_HANDLER(CBH, DATAPTR) (*(CBH))(CONFIG_ENTRY_CREATED, DATAPTR)
#define DATA_UPDATED_HANDLER(CBH, DATAPTR) (*(CBH))(CONFIG_ENTRY_VERSION_MISMATCH, DATAPTR)
//hash of the last (invalid) entry in the list. Used to find the definite end of the data
#define CONMAN_LAST_ENTRY_HASH 0xffffffff
#define CONMAN_DELETED_ENTRY_HASH 0xfffffffe

#define CONMAN_VERSION 1
#define CONMAN_VERSION_UNINITIALIZED 0xffffffff

#define CONMAN_SERIALNR_UNINITIALIZED 0xffffffff

#define CONMAN_FLAG_WRITE_PROTECTED 0x00000001
#define CONMAN_FLAG_UPDATE_PENDING 0x00000002

typedef enum{CONFIG_OK, CONFIG_ERROR, CONFIG_LAST_ENTRY_FOUND, CONFIG_ENTRY_NOT_FOUND, CONFIG_ENTRY_CREATED, CONFIG_ENTRY_UPDATED, CONFIG_ENTRY_SIZE_MISMATCH, CONFIG_ENTRY_VERSION_MISMATCH, CONFIG_ENTRY_LOADED, CONFIG_VERIFY_VALUE} ConMan_Result_t;

typedef struct __CallbackData__ ConMan_CallbackData_t;
typedef ConMan_Result_t (* ConMan_CallbackHandler_t)(ConMan_Result_t evt, ConMan_CallbackData_t * data);

//struct that lies in front of every piece of data in the configuration memory
typedef struct{
    //hashed string key for finding the value
    uint32_t keyHash;
    
    //data version to allow for proper update compatibility
    uint32_t dataVersion;
    
    //placeholder values incase we need more data in the future
    uint32_t flags;
    uint32_t reserved;
    
    //data size and finally start of data
    uint32_t dataSizeBytes;
    
    //in memory the data will start after this entry
}ConMan_ParameterDescriptor_t;

//dynamic descriptor for every parameter. Will be stored in ram dynamically to allow for changing callbackHandler locations. This also caches the location of the corresponding ConMan_ParameterDescriptor_t
typedef struct{
    //hashed string key for finding the value
    uint32_t keyHash;
    
    //pointer to corresponding ConMan_ParameterDescriptor_t in flash, not 100% necessary but reduces need for re-searching the list on another access
    ConMan_ParameterDescriptor_t * targetConfig;
    
    //callback handler data
    ConMan_CallbackHandler_t callbackHandler;
    void * callbackData;
} ConMan_HandlerDescriptor_t;

//internal descriptor for state of configuration memory
typedef struct{
    //version so ConMan knows if it can read the list properly
    uint32_t descriptorVersion;
    
    //total size of the storage array, incase this changes in the future
    uint32_t memorySize;
    
    //total size of the storage array, incase this changes in the future
    uint32_t dataCRC;
    
    //location of the array (will probably be const, but we need to make sure that we can find it if it isn't)
    uint8_t* dataPtr;
} ConMan_MemoryDescriptor_t;

//Serialisation data type
typedef struct{
    uint32_t serialNumber;
    uint32_t batchNumber;
    
    uint32_t manufacturingDate;
    
    //make sure we have space for further expansion
    uint32_t reserved;
    uint32_t reserved2;
    
    //this should be set by the bootloader
    uint32_t bootLoaderVersion;
} __attribute__((packed)) ConMan_SerialNumber_t;

struct __CallbackData__{
    void * userData;
    ConMan_ParameterDescriptor_t * callbackData;
};


//time in ms after which written data is automatically flushed to NVM even if ConMan_writeFinishedHandler() is not called
#define CONMAN_WRITETIMEOUT 25

extern const volatile uint8_t ConMan_data[];

ConMan_Result_t ConMan_writeData(ConMan_ParameterDescriptor_t * descriptor, uint32_t dataOffset, uint8_t* newDataPtr, uint32_t newDataSize);
ConMan_Result_t ConMan_eraseData(ConMan_ParameterDescriptor_t * descriptor, uint32_t dataOffset, uint32_t eraseLength);
ConMan_Result_t ConMan_eraseParameterData(char* strParameterKey, uint32_t dataOffset, uint32_t eraseLength);
ConMan_Result_t ConMan_updateParameter(char* strParameterKey, uint32_t dataOffset, uint8_t* newDataPtr, uint32_t newDataSize, uint32_t version);
ConMan_Result_t ConMan_addParameter(char* strParameterKey, uint32_t dataSize, ConMan_CallbackHandler_t callback, void * callbackData, uint32_t version);
void ConMan_flushBuffer();
void ConMan_handleException(uint32_t code, uint32_t arg1, uint32_t arg2, uint32_t arg3);
uint32_t * ConMan_getExceptionCause();
void ConMan_init();
ConMan_SerialNumber_t * ConMan_getSerialisationData();
void ConMan_allParameterAddedCallback();
ConMan_Result_t ConMan_readData(ConMan_ParameterDescriptor_t * descriptor, uint32_t dataOffset, uint8_t* dataTarget, uint32_t dataTargetSize);
ConMan_Result_t ConMan_getParameterData(char* strParameterKey, uint32_t offset, uint8_t* dataTarget, uint32_t dataTargetSize);
uint32_t ConMan_getDataSize(char* strParameterKey);
void * ConMan_getDataPtr(ConMan_ParameterDescriptor_t* descriptor);
uint32_t ConMan_getDataSizeFromDescriptor(ConMan_ParameterDescriptor_t* descriptor);
ConMan_ParameterDescriptor_t * ConMan_getParameterDescriptor(char* strParameterKey);
uint32_t ConMan_crc(uint8_t* data, uint32_t size, uint32_t seed);

#endif