/*
 * File:   ConfigManager.c
 * Author: Thorb
 *
 * Created on 11 January 2023, 22:30
 */


#include <xc.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

#include "DLL.h"
#include "FreeRTOS.h"
#include "NVM.h"
#include "ConMan.h"
#include "ConManConfig.h"
#include "stream_buffer.h"
#include "System.h"
#include "TTerm_AC.h"

static void createMemoryDecriptor();

#define CONMAN_RENEW_WRITE_TIMEOUT() uint8_t temp = 0; xStreamBufferSend(watchDogBuffer, &temp, 1, 0);

/*
 * intended process for nvm storage of settings
 * 
 * 1. add setting with ConMan_addParameter
 *      -> conman will try to find a matching ConMan_ParameterDescriptor_t in the flash
 *          - if found, a ConMan_TempParameterDescriptor_t will be added to the list of subscribed callbackHandlers
 *          - if not found, an entry will be created and the callback handler called with CONFIG_ENTRY_CREATED, to allow the function to populate the new datafield
 *              - for this conman will first ascertain if enough space is still available
 *                  - if not an unrecoverable error is thrown
 *              - if space is found a new entry is written at that position and the amount of available space reduced by sizeof(data) + sizeof(ConMan_ParameterDescriptor_t) - 1
 *          - if an entry with a different datasize is found configAssert(0) will be called and the data resized
 *              - first check if enough space is available
 *              - then remove the data from the list (will be lost)
 *              - move all data after the removed entry backwards to fill the now empty space
 *              - re-add data with new size at the end of the list
 *  2. if a value is changed with ConMan_updateParameter() the ConMan_CallbackHandler is called with CONFIG_ENTRY_UPDATED to allow the client to reload any new data from flash
 */

//Static variable definitions. Address of these must be fixed as they will be persistent across updates. Persistent also prevents defaults from being written, this is our job. See createMemoryDecriptor()
const volatile uint8_t ConMan_data[CONMAN_DATA_SIZE] __attribute__((address(CONMAN_DATA_ADDRESS), persistent)) = {[0 ... CONMAN_DATA_SIZE-1] = 0xff}; //persistent attribute prevents initialisation data from being stored in flash by the compiler for whatever reason
const volatile ConMan_MemoryDescriptor_t ConMan_memoryDescriptor __attribute__((address(CONMAN_MEMDESCRIPTOR_ADDRESS), persistent)) = {.dataPtr = ConMan_data, .descriptorVersion = CONMAN_VERSION, .memorySize = CONMAN_DATA_SIZE};
const volatile ConMan_SerialNumber_t ConMan_serialNumber __attribute__((address(CONMAN_SERIALNR_ADDRESS), persistent)) = {.serialNumber = CONMAN_SERIALNR_UNINITIALIZED};
const volatile uint32_t ConMan_exceptionRegister[4] __attribute__((address(CONMAN_EXC_ADRESS), persistent)) = {SYS_RESET_FLASHED,0xffffffff,0xffffffff,0xffffffff};
static uint32_t ConMan_exceptionRegisterTemp[4];

StreamBufferHandle_t watchDogBuffer;
DLLObject * handlerDescriptorList;

static void ConMan_task(void*params){
    uint32_t trash;
    while(1){
        //try to receive Data from the timeout streamBuffer
        if(xStreamBufferReceive(watchDogBuffer, &trash, 1, pdMS_TO_TICKS(CONMAN_WRITETIMEOUT)) == 0){
            //no we didn't receive anything 
            ConMan_flushBuffer();
			
            /*//now we could enable the generator again
			SigGen_setOutputEnabled(1);*/
            
            //now that data was flushed we don't need to re-flush after the next timeout, so we wait until something is written again for as long as we can
            while(!xStreamBufferReceive(watchDogBuffer, &trash, 1, portMAX_DELAY));
            //once we are here a write has just begun after a longer while of no writes
			
            /*//disable the signal generator until data has been flushed. All values read will still be valid but pro. don't want the coil doing anything until its all stored in flash
			SigGen_setOutputEnabled(0);*/
        }
    }
}

void ConMan_init(){
    //before any significant code happens we must check if an exception cause was set on last reset
    if(ConMan_exceptionRegister[0] != 0xffffffff || ConMan_exceptionRegister[1] != 0xffffffff || ConMan_exceptionRegister[2] != 0xffffffff || ConMan_exceptionRegister[3] != 0xffffffff){
        //at least one of the exception registers is set
        
        //copy data into the RAM for future access
        memcpy(ConMan_exceptionRegisterTemp, ConMan_exceptionRegister, sizeof(uint32_t) * 4);
        
        //then create a 0xffffffff-vector to copy into the registers in order to clear them
        //we write 0xffffffff (all bits = 1) so the exception handler doesn't need to perform an erase operation
        uint32_t vector[4] = {0xffffffff,0xffffffff,0xffffffff,0xffffffff};
        NVM_memcpyBuffered(ConMan_exceptionRegister, vector, sizeof(uint32_t) * 4);
        NVM_flush();
    }
    
    //create the list containing value subscribers
    handlerDescriptorList = DLL_create();
    
    //create the timeout buffer. Size = 1 so we never have more than one write event in there to renew the timeout
    watchDogBuffer = xStreamBufferCreate(1, 0);
    
    //check out the data that we have in the flash
    
    //make sure we can interpret it correctly
    if(ConMan_memoryDescriptor.descriptorVersion == CONMAN_VERSION_UNINITIALIZED){
        //no configuration data is present in the flash yet. Likely the device was just programmed, initialize stuff
        createMemoryDecriptor();
    }else if(ConMan_memoryDescriptor.descriptorVersion < CONMAN_VERSION){
        //data was written by an old version of ConMan, we need to update it
        //TODO implement updating of memoryDescriptor
    }else if(ConMan_memoryDescriptor.descriptorVersion > CONMAN_VERSION){
        //configuration is newer than what we can understand
        //TODO decide what we need to do... do we erase and start from scratch or do we sit here and do nothing instead?
    }else ; //data is from a ConMan of the same version as ours
    
    //check crc. No need to read buffered as no data should have been written yet
    //TODO add interlock?
    if(ConMan_memoryDescriptor.dataCRC != ConMan_crc(ConMan_memoryDescriptor.dataPtr, ConMan_memoryDescriptor.memorySize, 0)){
        //data is corrupt, throw an error and {do something?}
        //TODO decide what we need to do... do we erase and start from scratch or do we sit here and do nothing instead?
        //for testing: delete everything
        uint8_t * dst = &ConMan_memoryDescriptor;
        uint32_t ff = 0xffffffff;
        for(uint32_t cb = 0; cb < CONMAN_DATA_SIZE+sizeof(ConMan_MemoryDescriptor_t); cb+=4){
            NVM_memcpyBuffered(dst, &ff, 4);
            dst+=4;
        }
        NVM_flush();
        
        createMemoryDecriptor();
        configASSERT(0);
    }
}

static void createMemoryDecriptor(){
    //populate a new memory descriptor with the default (at least for this build) values
    ConMan_MemoryDescriptor_t * descriptor = pvPortMalloc(sizeof(ConMan_MemoryDescriptor_t));
    memset(descriptor, 0, sizeof(ConMan_MemoryDescriptor_t));
    descriptor->memorySize = CONMAN_DATA_SIZE;
    descriptor->descriptorVersion = CONMAN_VERSION;
    descriptor->dataPtr = ConMan_data;
    descriptor->dataCRC = ConMan_crc(descriptor->dataPtr, descriptor->memorySize, 0);

    //write descriptor to memory
    NVM_memcpyBuffered((uint8_t*) &ConMan_memoryDescriptor, (uint8_t*) descriptor, sizeof(ConMan_MemoryDescriptor_t));
    NVM_flush();
    
    vPortFree(descriptor);
}

static void updateCRC(){
    uint8_t * currChunk = pvPortMalloc(64);
    uint32_t currCrc = 0;
    
    //step through the config memory and calculate the crc
    for(int32_t cb = 0; cb < CONMAN_DATA_SIZE; cb += 64){
        //see how many bytes we still need to process
        uint32_t bytesRemaining = CONMAN_DATA_SIZE - cb;
        uint32_t bytesRemainingInChunk = (bytesRemaining >= 64) ? 64 : bytesRemaining;
        
        //load data through the NVM library to include still buffered data in the new crc
        NVM_memcpyBuffered(currChunk, (uint8_t*) ((uint32_t) ConMan_memoryDescriptor.dataPtr + cb), bytesRemainingInChunk);
        
        //step crc
        currCrc = ConMan_crc(currChunk, bytesRemainingInChunk, currCrc);
    }

    vPortFree(currChunk);
    
    //write crc to memory
    NVM_memcpyBuffered((uint8_t*) &(ConMan_memoryDescriptor.dataCRC), (uint8_t*) &currCrc, sizeof(uint32_t));
}

void ConMan_flushBuffer(){
    //shouldn't really be interrupted during the update
    vTaskEnterCritical();
    
    //calculate the current crc
    updateCRC();
    
    //and make sure all the data from the buffer has been flushed
    NVM_flush();
    
    vTaskExitCritical();
}

uint32_t * ConMan_getExceptionCause(){
    return ConMan_exceptionRegisterTemp;
}

void ConMan_handleException(uint32_t code, uint32_t arg1, uint32_t arg2, uint32_t arg3){
    //an exception has occurred and we must remember what exactly happened => write data to flash
    uint32_t vector[4] = {code,arg1,arg2,arg3};
    NVM_memcpy4(ConMan_exceptionRegister, vector, sizeof(uint32_t) * 4);
}

uint32_t ConMan_crc(uint8_t* data, uint32_t size, uint32_t seed){
    if(data == 0) return 0;
         
    uint32_t ret = seed;   
    
    for(uint32_t cb = 0; cb < size; cb++){
        ret ^= data[cb];
        for (uint32_t i = 0; i < 8; ++i) {
            if (ret & 1){
                ret = (ret >> 1) ^ 0xA001;
            }else{
                ret = (ret >> 1);
            }
        }
    }
    
    return ret;
}

static ConMan_Result_t findParameterInFlash(uint32_t strHash, ConMan_ParameterDescriptor_t ** targetPtr){
    //start at the beginning of the data field. Load the descriptor via NVM lib, incase we read across a buffered data boundary
    ConMan_ParameterDescriptor_t currDescriptor;
    ConMan_ParameterDescriptor_t * currDescriptorPtr = (ConMan_ParameterDescriptor_t *) ConMan_memoryDescriptor.dataPtr;
    NVM_memcpyBuffered((uint8_t*) &currDescriptor, (uint8_t*) currDescriptorPtr, sizeof(ConMan_ParameterDescriptor_t));
    
    
    //go through all descriptors making sure not to read more data than the list has
    uint32_t bytesScanned = 0;
    ConMan_ParameterDescriptor_t * lastDescriptor = 0;
    while(bytesScanned < ConMan_memoryDescriptor.memorySize){ //entry with CONMAN_LAST_ENTRY_HASH as hash indicates the list has ended and no more data can be read
        if(currDescriptor.keyHash == CONMAN_LAST_ENTRY_HASH){
            *targetPtr = currDescriptorPtr;
            return CONFIG_LAST_ENTRY_FOUND;
        }
            
        //is the hash the one we are looking for?
        if(currDescriptor.keyHash == strHash){
            *targetPtr = currDescriptorPtr;
            return CONFIG_OK;
        }
        
        //update amount of bytes scanned as well as the pointer to the current descriptor
        bytesScanned += sizeof(ConMan_ParameterDescriptor_t) + currDescriptor.dataSizeBytes;
        lastDescriptor = currDescriptorPtr;
        currDescriptorPtr = (ConMan_ParameterDescriptor_t *) ((uint32_t) ConMan_memoryDescriptor.dataPtr + bytesScanned);
        NVM_memcpyBuffered((uint8_t*) &currDescriptor, (uint8_t*) currDescriptorPtr, sizeof(ConMan_ParameterDescriptor_t));
    }
    
    return CONFIG_ENTRY_NOT_FOUND;
}

void ConMan_allParameterAddedCallback(){
    //this gets called by the startup function once all parameters have been added. We want to make sure that this gets flushed to Flash
    updateCRC();
    NVM_flush();
    
#ifdef __DEBUG
    //for debug: calculate space left in configuration memory
    ConMan_ParameterDescriptor_t * descriptor;
    ConMan_Result_t res = findParameterInFlash(CONMAN_LAST_ENTRY_HASH, &descriptor);
    if(res == CONFIG_LAST_ENTRY_FOUND){
        volatile uint32_t spaceRemaining = (uint32_t) ConMan_memoryDescriptor.memorySize - ((uint32_t) descriptor - (uint32_t) ConMan_memoryDescriptor.dataPtr);
        configASSERT(0);
    }
#endif
    
    //and finally add the write timeout handler. From now on writes won't happen at predictable times. Needs to be at the highest priority of any tasks accessing the config
    xTaskCreate(ConMan_task, "ConMan Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 6, NULL);
}

//get a pointer to the struct containing serial numbers. This is a unbuffered read as this value is not written through the buffered NVM operation
ConMan_SerialNumber_t * ConMan_getSerialisationData(){
    return &ConMan_serialNumber;
}

static ConMan_Result_t ConMan_findFreeSpace(uint32_t size, ConMan_ParameterDescriptor_t ** targetDescriptor){
    //start at the beginning of the data field. Load the descriptor via NVM lib, incase we read across a buffered data boundary
    ConMan_ParameterDescriptor_t currDescriptor;
    ConMan_ParameterDescriptor_t * currDescriptorPtr = (ConMan_ParameterDescriptor_t *) ConMan_memoryDescriptor.dataPtr;
    NVM_memcpyBuffered((uint8_t*) &currDescriptor, (uint8_t*) currDescriptorPtr, sizeof(ConMan_ParameterDescriptor_t));
    
    
    //go through all descriptors making sure not to read more data than the list has
    uint32_t bytesScanned = 0;
    ConMan_ParameterDescriptor_t * lastDescriptor = 0;
    while(bytesScanned < ConMan_memoryDescriptor.memorySize){ 
        //check if the current descriptor is the last one or if its a deleted item
        
        if(currDescriptor.keyHash == CONMAN_LAST_ENTRY_HASH){
            //yes we're at the end of the list. Check if enough space is available here
            uint32_t spaceRemaining = (uint32_t) ConMan_memoryDescriptor.memorySize - ((uint32_t) currDescriptorPtr - (uint32_t) ConMan_memoryDescriptor.dataPtr) - sizeof(ConMan_ParameterDescriptor_t);
            
            //is the space large enough?
            if(spaceRemaining >= size){
                //yes! Set the pointer and return
                *targetDescriptor = currDescriptorPtr;
                return CONFIG_OK;
            }
            
            //even the space remaining after the last entry is too small, return an error
            return CONFIG_LAST_ENTRY_FOUND;
        }
            
        //is the item a deleted one? (AKA free space)
        if(currDescriptor.keyHash == CONMAN_DELETED_ENTRY_HASH){
            //yes, check if its size would fit the new data and an additional descriptor required to keep the linked list valid
            if(currDescriptor.dataSizeBytes - sizeof(ConMan_ParameterDescriptor_t) >= size){
                //yes that would fit => update the pointer and return
                *targetDescriptor = currDescriptorPtr;
                return CONFIG_OK;
                
            //no, but would the new data fit perfectly by any chance?
            }else if(currDescriptor.dataSizeBytes == size){
                //yes! update and return
                *targetDescriptor = currDescriptorPtr;
                return CONFIG_OK;
            }
        }
        
        //normal entry, go on to the next one
        
        //update amount of bytes scanned as well as the pointer to the current descriptor
        bytesScanned += sizeof(ConMan_ParameterDescriptor_t) + currDescriptor.dataSizeBytes;
        lastDescriptor = currDescriptorPtr;
        currDescriptorPtr = (ConMan_ParameterDescriptor_t *) ((uint32_t) ConMan_memoryDescriptor.dataPtr + bytesScanned);
        NVM_memcpyBuffered((uint8_t*) &currDescriptor, (uint8_t*) currDescriptorPtr, sizeof(ConMan_ParameterDescriptor_t));
    }
    
    return CONFIG_ENTRY_NOT_FOUND;
}

static ConMan_Result_t ConMan_createParameter(ConMan_ParameterDescriptor_t ** newDescriptor, uint32_t strHash, uint32_t dataSize, ConMan_CallbackHandler_t callback, void * callbackData, uint32_t version){
    //add a new parameter
    
    ConMan_ParameterDescriptor_t * target;
    
    //try to find some space in the flash
    ConMan_Result_t res = ConMan_findFreeSpace(dataSize, &target);
    
    //did we find some?
    if(res != CONFIG_OK){
        //no, return
        *newDescriptor = NULL;
        return CONFIG_ERROR;
    }

    //now we have the place to write the data to, and know there is enough space there.
    
    //get the current descriptor
    ConMan_ParameterDescriptor_t currDescriptor;
    NVM_memcpyBuffered((uint8_t*) &currDescriptor, (uint8_t*) target, sizeof(ConMan_ParameterDescriptor_t));
    
    //check if we are splitting the space of a deleted item or moving the last descriptor of the memory
    if(currDescriptor.keyHash == CONMAN_DELETED_ENTRY_HASH || (currDescriptor.keyHash == CONMAN_DELETED_ENTRY_HASH && currDescriptor.dataSizeBytes != dataSize)){
        //yes, we need to add a descriptor after the current one and reduce its size by what we are about to write
        
        //update the dataSize
        currDescriptor.dataSizeBytes = currDescriptor.dataSizeBytes - sizeof(ConMan_ParameterDescriptor_t) - dataSize;
        
        //and write the descriptor to the new location
        NVM_memcpyBuffered((uint8_t*) ((uint32_t) target + sizeof(ConMan_ParameterDescriptor_t) + dataSize), (uint8_t*) &currDescriptor, sizeof(ConMan_ParameterDescriptor_t));
    }

    //now create the new entry
    ConMan_ParameterDescriptor_t * tempData = pvPortMalloc(sizeof(ConMan_ParameterDescriptor_t));
    memset(tempData, 0, sizeof(ConMan_ParameterDescriptor_t));
    tempData->dataSizeBytes = dataSize;
    tempData->keyHash = strHash;
    tempData->dataVersion = version;

    //write descriptor to memory
    NVM_memcpyBuffered((uint8_t*) target, (uint8_t*) tempData, sizeof(ConMan_ParameterDescriptor_t));

    //is there a callback? If not make sure the data is erased
    if(callback != NULL){    
        //have the callback handler populate the data with default values. This should call ConMan_writeData() if default data other than 0xff should be populated
        ConMan_CallbackData_t cbd = {.userData = callbackData, .callbackData = target}; //no not the drug...
        DATA_CREATED_HANDLER(callback, &cbd);
    }else{
        ConMan_eraseData(target, 0, dataSize);
    }

    *newDescriptor = target;
    
    //free the temporary data
    vPortFree(tempData);
}

static ConMan_Result_t ConMan_deleteParameter(ConMan_ParameterDescriptor_t * itemToDelete){
    //we're going to delete an item. This is done by setting the hash to the magic number that indicates a deleted item
    
    //first load the current entry
    ConMan_ParameterDescriptor_t currDescriptor;
    NVM_memcpyBuffered((uint8_t*) &currDescriptor, (uint8_t*) itemToDelete, sizeof(ConMan_ParameterDescriptor_t));
    
    //make sure we aren't accidentally deleting the last entry 
    if(currDescriptor.keyHash == CONMAN_LAST_ENTRY_HASH) return CONFIG_ERROR;
        
    //update the hash
    currDescriptor.keyHash = CONMAN_DELETED_ENTRY_HASH;
    
    //and write the data
    //write descriptor to memory
    NVM_memcpyBuffered((uint8_t*) itemToDelete, (uint8_t*) &currDescriptor, sizeof(ConMan_ParameterDescriptor_t));
    
    return CONFIG_OK;
}

ConMan_Result_t ConMan_updateDescriptor(ConMan_ParameterDescriptor_t ** descriptor, uint32_t newDataSize, uint32_t newVersion){
    //user just got the request to update a parameter and wants to update the descriptor to reflect that change
    
    //load the old descriptor
    ConMan_ParameterDescriptor_t currDescriptor;
    NVM_memcpyBuffered((uint8_t*) &currDescriptor, (uint8_t*) (*descriptor), sizeof(ConMan_ParameterDescriptor_t));
    
    //will the stuff stay in the same place?
    if(newDataSize != currDescriptor.dataSizeBytes){
        //no, descriptor will move. Delete the current one and create it again in the new place
        ConMan_Result_t res = ConMan_deleteParameter(*descriptor);
        
        //did that work?
        if(res == CONFIG_ERROR){
            //no :( best we can do is return
            return CONFIG_ERROR;
        }
        
        //create the new descriptor, but without calling any callbacks
        res = ConMan_createParameter(descriptor, currDescriptor.keyHash, newDataSize, NULL, NULL, newVersion);
        
        //did that work?
        if(res == CONFIG_ERROR){
            //no :( This likely means that the new descriptor would be too big for the memory. 
            //Return an error and invalidate the descriptor pointer as the old one has been deleted now
            return CONFIG_ERROR;
        }
        
        //yep all good, descriptor has also been updated by the createParameter function, but let the user know the data was resized
        return CONFIG_ENTRY_SIZE_MISMATCH;
    }
    
    //yes location won't change, just update the version and update pending flag in the descriptor and write that to flash
    currDescriptor.dataVersion = newVersion;
    currDescriptor.flags &= ~CONMAN_FLAG_UPDATE_PENDING;
    NVM_memcpyBuffered((uint8_t*) (*descriptor), (uint8_t*) &currDescriptor, sizeof(ConMan_ParameterDescriptor_t));
    
    return CONFIG_OK;
}

ConMan_Result_t ConMan_addParameter(char* strParameterKey, uint32_t dataSize, ConMan_CallbackHandler_t callback, void * callbackData, uint32_t version){
    //hash the string to find
    uint32_t hashToFind = ConMan_crc(strParameterKey, strlen(strParameterKey), 0);
    
    //TODO add a check if the parameter has already been added => if so throw an error
    
    ConMan_ParameterDescriptor_t * descriptorPointer;
    ConMan_ParameterDescriptor_t descriptor;
    
    //try to locate the config in flash
    ConMan_Result_t res = findParameterInFlash(hashToFind, &descriptorPointer);
    if(res == CONFIG_LAST_ENTRY_FOUND){
        //not found... create a new entry. The ConMan_createParameter fucntion also calls the entry created callback
        if(ConMan_createParameter(&descriptorPointer, hashToFind, dataSize, callback, callbackData, version) != CONFIG_OK){
            //something went wrong, return
            return CONFIG_ERROR;
        }
        
    }else if(res == CONFIG_ENTRY_NOT_FOUND){
        //no last entry was found... there is some problem with the storage
        configASSERT(0);
        //TODO handle this...
    }
    
    //entry with matching key was found. Now see if the other data matches
    
    NVM_memcpyBuffered((uint8_t*) &descriptor, (uint8_t*) (descriptorPointer), sizeof(ConMan_ParameterDescriptor_t));

    //does the data version in flash match the one we are trying to read?
    if(descriptor.dataVersion != version){
        //no! first make sure that the current descriptor is marked as pending an update
        descriptor.flags |= CONMAN_FLAG_UPDATE_PENDING;
        NVM_memcpyBuffered((uint8_t*) descriptorPointer, (uint8_t*) &descriptor, sizeof(ConMan_ParameterDescriptor_t));

        //then call the event handler and let it handle this
        ConMan_CallbackData_t cbd = {.userData = callbackData, .callbackData = descriptorPointer}; //no not the drug...
        ConMan_Result_t updateRes = DATA_UPDATED_HANDLER(callback, &cbd);
        
        //make sure that our descriptor is updated if the DATA_UPDATED_HANDLER ended up moving it
        if(descriptorPointer != cbd.callbackData){
            //yep it moved, update the descriptor pointer and reload it
            descriptorPointer = cbd.callbackData;
        }
        
        //reload the new descriptor
        NVM_memcpyBuffered((uint8_t*) &descriptor, (uint8_t*) (descriptorPointer), sizeof(ConMan_ParameterDescriptor_t));
        
        //did the user actually perform an update?
        if(descriptor.flags & CONMAN_FLAG_UPDATE_PENDING){
            //lol no? Seems he didn't implement this function. Pretend like we just created the parameter
            
            //try to delete the old parameter
            if(ConMan_deleteParameter(descriptorPointer) == CONFIG_ERROR){
                //failed :( best we can do is return
                return CONFIG_ERROR;
            }
        
            //create the new descriptor and have the user populate it as if it was just created like normal
            if(ConMan_createParameter(&descriptorPointer, hashToFind, dataSize, callback, callbackData, version) != CONFIG_OK){
                //something went wrong, return
                return CONFIG_ERROR;
            }
            
            //reload descriptor again
            NVM_memcpyBuffered((uint8_t*) &descriptor, (uint8_t*) (descriptorPointer), sizeof(ConMan_ParameterDescriptor_t));
        }
    }
    
    //check if the existing data entry is the same size as the one we are looking for
    //WARNING:  this really should only be the case if the data version changed (which is handled in the state above).
    //          This will also result in calling updateDescriptor which updates the data size.
    if(descriptor.dataSizeBytes != dataSize){
        //no! we need to re-size the existing data. Since the user didn't bother to update it best we can do is erase everything and pretend we're starting over
        //try to delete the old parameter
        if(ConMan_deleteParameter(descriptorPointer) == CONFIG_ERROR){
            //failed :( best we can do is return
            return CONFIG_ERROR;
        }

        //create the new descriptor and have the user populate it as if it was just created like normal
        if(ConMan_createParameter(&descriptorPointer, hashToFind, dataSize, callback, callbackData, version) != CONFIG_OK){
            //something went wrong, return
            return CONFIG_ERROR;
        }
        
        //TODO if any access to descriptor is added after this we need to reload it!
    }
    
    //add the pointer containing dynamic data such as the callback handler pointer to the handler list
    ConMan_HandlerDescriptor_t * desc = pvPortMalloc(sizeof(ConMan_HandlerDescriptor_t));

    desc->targetConfig = descriptorPointer;
    desc->callbackData = callbackData;
    desc->callbackHandler = callback;
    desc->keyHash = hashToFind;

    DLL_add(desc, handlerDescriptorList);

    ConMan_CallbackData_t cbd = {.userData = callbackData, .callbackData = descriptorPointer}; //no not the drug...
    DATA_LOADED_HANDLER(callback, &cbd);
    
    return CONFIG_ENTRY_CREATED;
}

ConMan_Result_t ConMan_writeData(ConMan_ParameterDescriptor_t * descriptor, uint32_t dataOffset, uint8_t* newDataPtr, uint32_t newDataSize){
    //write that descriptor after the current one
    /*if(descriptor->dataSizeBytes != newDataSize){
        //user messed up... data size being written is not wat was set when adding the parameter
        configASSERT(0);
        //TODO handle this properly
    }*/
    
    //renew the write timeout
    CONMAN_RENEW_WRITE_TIMEOUT();
    
    //TODO: cache this maybe? like for example in the list with the callback handler...
    uint32_t dataSizeInDescriptor = 0;
    NVM_memcpyBuffered((uint8_t*) &dataSizeInDescriptor, (uint8_t*) &descriptor->dataSizeBytes, 4);
    
    if(dataSizeInDescriptor < dataOffset + newDataSize) return CONFIG_ENTRY_SIZE_MISMATCH;
    
    if(NVM_memcpyBuffered((uint8_t *) ((uint32_t) CONMAN_DESCRIPTOR_ADDRESS_TO_DATA_ADDRESS(descriptor) + dataOffset), newDataPtr, newDataSize) == NVM_ERROR) return CONFIG_ERROR;
    
    //TODO fix this 
    return CONFIG_OK;
}

ConMan_Result_t ConMan_eraseData(ConMan_ParameterDescriptor_t * descriptor, uint32_t dataOffset, uint32_t eraseLength){
    //renew the write timeout
    CONMAN_RENEW_WRITE_TIMEOUT();
    
    uint32_t dataSizeInDescriptor = 0;
    NVM_memcpyBuffered((uint8_t*) &dataSizeInDescriptor, (uint8_t*) &descriptor->dataSizeBytes, 4);
    
    if(dataSizeInDescriptor < dataOffset + eraseLength) return CONFIG_ENTRY_SIZE_MISMATCH;
    
    NVM_memsetBuffered((uint8_t *) ((uint32_t) CONMAN_DESCRIPTOR_ADDRESS_TO_DATA_ADDRESS(descriptor) + dataOffset), CONMAN_EMPTY_FLASH_VALUE_WORD, eraseLength);
    
    //TODO fix this 
    return CONFIG_ERROR;
}

ConMan_Result_t ConMan_updateParameter(char* strParameterKey, uint32_t dataOffset, uint8_t* newDataPtr, uint32_t newDataSize, uint32_t version){
    //hash the string to find
    uint32_t hashToFind = ConMan_crc(strParameterKey, strlen(strParameterKey), 0);
    
    //try to find the parameter in the list of subscribed values
    ConMan_HandlerDescriptor_t * handlerDescriptor = NULL;
    uint32_t found = 0;
    DLL_FOREACH(currDLLDescriptor, handlerDescriptorList){
        handlerDescriptor = currDLLDescriptor->data;
        if(handlerDescriptor->keyHash == hashToFind){ 
            found = 1;
            break;
        }
    }
    
    ConMan_ParameterDescriptor_t * descriptor;
    
    //did we find it?
    if(found){
        //yes! now check if the data length is correct
        if(handlerDescriptor->targetConfig->dataSizeBytes != newDataSize){
            //no the data size is different... that means either that we have the wrong target, or that the size has changed.
            //TODO move data in preparation of writing
        }
        //we don't need to write yet, that will be done with the same code that writes to a flash only entry
        
        //now assign the descriptor
        descriptor = handlerDescriptor->targetConfig;
        
    //no, search in flash next
    }else if(findParameterInFlash(hashToFind, &descriptor) == CONFIG_OK){
        //yes! now check if the data length is correct
        if(handlerDescriptor->targetConfig->dataSizeBytes != newDataSize){
            //no the data size is different... that means either that we have the wrong target, or that the size has changed.
            //TODO move data in preparation of writing
        }
    }else{
        //hmm its not even in flash... should we create it or do nothing?
        configASSERT(0);
        //TODO handle this. should we maybe create the entry?
        return CONFIG_ERROR;
    }
    
    //now we have the pointer to the data and can update it in flash
    ConMan_ParameterDescriptor_t currDescriptor;
    NVM_memcpyBuffered((uint8_t*) &currDescriptor, (uint8_t*) descriptor, sizeof(ConMan_ParameterDescriptor_t));
    
    //is the version outdated?
    if(currDescriptor.dataVersion != version){
        //yes version changed! This is no longer allowed in a normal write. User should call updateDescriptor first if the version upgrade is intentional
        return CONFIG_ENTRY_VERSION_MISMATCH;
    }
    
    ConMan_writeData(descriptor, dataOffset, newDataPtr, newDataSize);
    
    //and finally call the dataupdated handler if we found one
    if(found){
        /*ConMan_ParameterDescriptor_t d;
        NVM_memcpyBuffered((uint8_t*) &d, (uint8_t*) handlerDescriptor->targetConfig, sizeof(ConMan_ParameterDescriptor_t));*/
        
        ConMan_CallbackData_t cbd = {.userData = handlerDescriptor->callbackData, .callbackData = descriptor}; //no not the drug...
        DATA_UPDATED_HANDLER(handlerDescriptor->callbackHandler, &cbd);
    }
    //TODO we still need to decide when to flush the data so it's not lost in case of a sudden power loss (e.g. unplugging the device)
    //NVM_flush();
}

ConMan_Result_t ConMan_eraseParameterData(char* strParameterKey, uint32_t dataOffset, uint32_t eraseLength){
    //hash the string to find
    uint32_t hashToFind = ConMan_crc(strParameterKey, strlen(strParameterKey), 0);
    
    //try to find the parameter in the list of subscribed values
    ConMan_HandlerDescriptor_t * handlerDescriptor = NULL;
    uint32_t found = 0;
    DLL_FOREACH(currDescriptor, handlerDescriptorList){
        handlerDescriptor = currDescriptor->data;
        if(handlerDescriptor->keyHash == hashToFind){ 
            found = 1;
            break;
        }
    }
    
    ConMan_ParameterDescriptor_t * descriptor;
    
    //did we find it?
    if(found){
        //yes! now check if the data length is correct
        if(handlerDescriptor->targetConfig->dataSizeBytes != eraseLength){
            //no the data size is different... that means either that we have the wrong target, or that the size has changed.
            //TODO move data in preparation of writing
        }
        //we don't need to write yet, that will be done with the same code that writes to a flash only entry
    //no, search in flash next
    }else if(findParameterInFlash(hashToFind, &descriptor) == CONFIG_OK){
        //yes! now check if the data length is correct
        if(handlerDescriptor->targetConfig->dataSizeBytes != eraseLength){
            //no the data size is different... that means either that we have the wrong target, or that the size has changed.
            //TODO move data in preparation of writing
        }
    }else{
        //hmm its not even in flash... should we create it or do nothing?
        configASSERT(0);
        //TODO handle this. should we maybe create the entry?
        return CONFIG_ERROR;
    }
    
    ConMan_eraseData(descriptor, dataOffset, eraseLength);
    
    //and finally call the data updated handler if we found one
    if(found){
        ConMan_ParameterDescriptor_t d;
        NVM_memcpyBuffered((uint8_t*) &d, (uint8_t*) handlerDescriptor->targetConfig, sizeof(ConMan_ParameterDescriptor_t));
        
        ConMan_CallbackData_t cbd = {.userData = handlerDescriptor->callbackData, .callbackData = &d}; //no not the drug...
        DATA_UPDATED_HANDLER(handlerDescriptor->callbackHandler, &cbd);
    }
    //TODO we still need to decide when to flush the data so it's not lost in case of a sudden power loss (e.g. unplugging the device)
    //NVM_flush();
}

uint32_t ConMan_getDataSize(char* strParameterKey){
    //hash the string to find
    uint32_t hashToFind = ConMan_crc(strParameterKey, strlen(strParameterKey), 0);
    
    ConMan_ParameterDescriptor_t * descriptor;
    
    //try to locate the config in flash
    if(findParameterInFlash(hashToFind, &descriptor) == CONFIG_OK){
        NVM_memcpyBuffered((uint8_t*) descriptor, (uint8_t*) descriptor, sizeof(ConMan_ParameterDescriptor_t));
        return descriptor->dataSizeBytes;
    }else{
        return 0xffffffff;
    }
}

uint32_t ConMan_getDataSizeFromDescriptor(ConMan_ParameterDescriptor_t* descriptor){
    ConMan_ParameterDescriptor_t d;
    
    NVM_memcpyBuffered((uint8_t*) &d, (uint8_t*) descriptor, sizeof(ConMan_ParameterDescriptor_t));
        
    return d.dataSizeBytes;
}

//WARNING: be careful when using this function. The memory might not be coherent with what was last written due to the nvm libraries ram buffer
void * ConMan_getDataPtr(ConMan_ParameterDescriptor_t* descriptor){
    return CONMAN_DESCRIPTOR_ADDRESS_TO_DATA_ADDRESS(descriptor);
}

ConMan_ParameterDescriptor_t * ConMan_getParameterDescriptor(char* strParameterKey){
    //hash the string to find
    uint32_t hashToFind = ConMan_crc(strParameterKey, strlen(strParameterKey), 0);
    
    ConMan_ParameterDescriptor_t * descriptor;
    
    //try to locate the config in flash
    if(findParameterInFlash(hashToFind, &descriptor) == CONFIG_OK){
        return descriptor;
    }else{
        return NULL;
    }
}

ConMan_Result_t ConMan_readData(ConMan_ParameterDescriptor_t * descriptor, uint32_t offset, uint8_t* dataTarget, uint32_t dataTargetSize){
    //check if the size matches the one we are looking for
    uint32_t dataSizeInDescriptor = 0;
    NVM_memcpyBuffered((uint8_t*) &dataSizeInDescriptor, (uint8_t*) &descriptor->dataSizeBytes, 4);
    
    if(dataSizeInDescriptor < offset + dataTargetSize) return CONFIG_ENTRY_SIZE_MISMATCH;

    //copy the data
    NVM_memcpyBuffered(dataTarget, (uint8_t*) ((uint32_t) CONMAN_DESCRIPTOR_ADDRESS_TO_DATA_ADDRESS(descriptor) + offset), dataTargetSize);

    return CONFIG_OK;
}

ConMan_Result_t ConMan_getParameterData(char* strParameterKey, uint32_t offset, uint8_t* dataTarget, uint32_t dataTargetSize){
    //hash the string to find
    uint32_t hashToFind = ConMan_crc(strParameterKey, strlen(strParameterKey), 0);
    
    ConMan_ParameterDescriptor_t * descriptor;
    
    //try to locate the config in flash
    if(findParameterInFlash(hashToFind, &descriptor) == CONFIG_OK){
        return ConMan_readData(descriptor, offset, (uint8_t*) dataTarget, dataTargetSize);
    }else{
        return CONFIG_ENTRY_NOT_FOUND;
    }
}