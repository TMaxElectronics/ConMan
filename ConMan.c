/*
 * File:   ConfigManager.c
 * Author: Thorb
 *
 * Created on 11 January 2023, 22:30
 */


#include <xc.h>
#include <string.h>
#include <limits.h>

#include "DLL.h"
#include "FreeRTOS.h"
#include "NVM.h"
#include "TTerm_AC.h"
#include "ConMan.h"
#include "ConManConfig.h"
#include "stream_buffer.h"


static uint32_t crc(uint8_t* data, uint32_t size, uint32_t seed);
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
    if(ConMan_memoryDescriptor.dataCRC != crc(ConMan_memoryDescriptor.dataPtr, ConMan_memoryDescriptor.memorySize, 0)){
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
    descriptor->dataCRC = crc(descriptor->dataPtr, descriptor->memorySize, 0);

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
        currCrc = crc(currChunk, bytesRemainingInChunk, currCrc);
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

static uint32_t crc(uint8_t* data, uint32_t size, uint32_t seed){
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
        //configASSERT(0);
    }
#endif
    
    //and finally add the write timeout handler. From now on writes won't happen at predictable times. Needs to be at the highest priority of any tasks accessing the config
    xTaskCreate(ConMan_task, "ConMan Task", configMINIMAL_STACK_SIZE+128, NULL, tskIDLE_PRIORITY + 6, NULL);
}

//get a pointer to the struct containing serial numbers. This is a unbuffered read as this value is not written through the buffered NVM operation
ConMan_SerialNumber_t * ConMan_getSerialisationData(){
    return &ConMan_serialNumber;
}

ConMan_Result_t ConMan_addParameter(char* strParameterKey, uint32_t dataSize, ConMan_CallbackHandler_t callback, void * callbackData, uint32_t version){
    uint32_t time = _CP0_GET_COUNT();
    uint32_t time2 = _CP0_GET_COUNT();
    //hash the string to find
    uint32_t hashToFind = crc(strParameterKey, strlen(strParameterKey), 0);
    
    //UART_print("\t\t\t (%09d ticks) hash calc \r\n", _CP0_GET_COUNT() - time);
    time2 = _CP0_GET_COUNT();
    
    ConMan_ParameterDescriptor_t * descriptor;
    
    //try to locate the config in flash
    ConMan_Result_t res = findParameterInFlash(hashToFind, &descriptor);
    //UART_print("\t\t\t (%09d ticks) flash search \r\n", _CP0_GET_COUNT() - time);
    time2 = _CP0_GET_COUNT();
    if(res == CONFIG_LAST_ENTRY_FOUND){
        //not found...
        
        //the pointer to the CONMAN_LAST_ENTRY_HASH descriptor is stored in the descriptor variable by findParameterInFlash()
        
        //calculate space remaining
        uint32_t spaceRemaining = (uint32_t) ConMan_memoryDescriptor.memorySize - ((uint32_t) descriptor - (uint32_t) ConMan_memoryDescriptor.dataPtr);

        if(spaceRemaining - sizeof(ConMan_ParameterDescriptor_t) < dataSize){
            //not enough space available for the data... do something
            //TODO handle this error
            configASSERT(0);
        }

        //now we have the place to write the data to, and know there is enough space there.
        
        //create the new entry
        ConMan_ParameterDescriptor_t * tempData = pvPortMalloc(sizeof(ConMan_ParameterDescriptor_t));
        memset(tempData, 0, sizeof(ConMan_ParameterDescriptor_t));
        tempData->dataSizeBytes = dataSize;
        tempData->keyHash = hashToFind;
        tempData->dataVersion = version;

        //write descriptor to memory
        NVM_memcpyBuffered((uint8_t*) descriptor, (uint8_t*) tempData, sizeof(ConMan_ParameterDescriptor_t));
        
        //UART_print("\t\t\t\t (%09d ticks) descriptor write \r\n", _CP0_GET_COUNT() - time);
        time2 = _CP0_GET_COUNT();
        
        //have the callback handler populate the data with default values. This should call ConMan_writeData() if default data other than 0xff should be populated
        ConMan_CallbackData_t cbd = {.userData = callbackData, .callbackData = descriptor}; //no not the drug...
        DATA_CREATED_HANDLER(callback, &cbd);
        //UART_print("\t\t\t\t (%09d ticks) callback handler \r\n", _CP0_GET_COUNT() - time);
        time2 = _CP0_GET_COUNT();
        
        //update descriptor portion of tempData to resemble a CONMAN_LAST_ENTRY_HASH descriptor
        memset(tempData, 0, sizeof(ConMan_ParameterDescriptor_t));
        tempData->keyHash = CONMAN_LAST_ENTRY_HASH;
        
        //write that descriptor after the current one
        NVM_memcpyBuffered((uint8_t*) ((uint32_t) descriptor + sizeof(ConMan_ParameterDescriptor_t) + dataSize), (uint8_t*) tempData, sizeof(ConMan_ParameterDescriptor_t));
        
        //UART_print("\t\t\t\t (%09d ticks) 2nd descriptor write \r\n", _CP0_GET_COUNT() - time);
        time2 = _CP0_GET_COUNT();

        //can we avoid this? this way we wast quite some write cycles...
        //updateCRC();
        //UART_print("\t\t\t\t (%09d ticks) crc update\r\n", _CP0_GET_COUNT() - time);
        //time2 = _CP0_GET_COUNT();
        //NVM_flush();
        
        //free the temporary data
        vPortFree(tempData);
    }else if(res == CONFIG_ENTRY_NOT_FOUND){
        //no last entry was found... there is some problem with the storage
        configASSERT(0);
        //TODO handle this...
    }
    
    
    
    //check if the existing data entry is the same size as the one we are looking for
    if(descriptor->dataSizeBytes != dataSize){
        //no! we need to re-size the existing data
        //TODO
        //is the old data small enough to move?
    }

    //does the data version in flash match the one we are trying to read?
    if(descriptor->dataVersion != version){
        //no! call the event handler and let it handle this
        //TODO call handler
    }
    
    //add the pointer containing dynamic data such as the callback handler pointer to the handler list
    ConMan_HandlerDescriptor_t * desc = pvPortMalloc(sizeof(ConMan_HandlerDescriptor_t));

    desc->targetConfig = descriptor;
    desc->callbackData = callbackData;
    desc->callbackHandler = callback;
    desc->keyHash = hashToFind;

    DLL_add(desc, handlerDescriptorList);

    ConMan_CallbackData_t cbd = {.userData = callbackData, .callbackData = descriptor}; //no not the drug...
    DATA_LOADED_HANDLER(desc->callbackHandler, &cbd);
    //UART_print("\t\t\t parameter addition time = %09d ticks \r\n", _CP0_GET_COUNT() - time);
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
    
    NVM_memcpyBuffered((uint8_t *) ((uint32_t) CONMAN_DESCRIPTOR_ADDRESS_TO_DATA_ADDRESS(descriptor) + dataOffset), newDataPtr, newDataSize);
}

ConMan_Result_t ConMan_eraseData(ConMan_ParameterDescriptor_t * descriptor, uint32_t dataOffset, uint32_t eraseLength){
    //write that descriptor after the current one
    /*if(descriptor->dataSizeBytes != newDataSize){
        //user messed up... data size being written is not wat was set when adding the parameter
        configASSERT(0);
        //TODO handle this properly
    }*/
    
    //renew the write timeout
    CONMAN_RENEW_WRITE_TIMEOUT();
    
    uint32_t dataSizeInDescriptor = 0;
    NVM_memcpyBuffered((uint8_t*) &dataSizeInDescriptor, (uint8_t*) &descriptor->dataSizeBytes, 4);
    
    if(dataSizeInDescriptor < dataOffset + eraseLength) return CONFIG_ENTRY_SIZE_MISMATCH;
    
    NVM_memsetBuffered((uint8_t *) ((uint32_t) CONMAN_DESCRIPTOR_ADDRESS_TO_DATA_ADDRESS(descriptor) + dataOffset), 0xff, eraseLength);
}

ConMan_Result_t ConMan_updateParameter(char* strParameterKey, uint32_t dataOffset, uint8_t* newDataPtr, uint32_t newDataSize, uint32_t version){
    //hash the string to find
    uint32_t hashToFind = crc(strParameterKey, strlen(strParameterKey), 0);
    
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
    
    //check if the descriptor needs to be updated (at the moment the only thing that could change here is the version. The sizechange is handled in changeParameterSize())
    if(descriptor->dataVersion != version){
        // jep something in there has changed, go ahead and update this. If in the future flags are introduced that could change this needs to be added here too
        
        //load current descriptor
        ConMan_ParameterDescriptor_t * tempDesc = pvPortMalloc(sizeof(ConMan_ParameterDescriptor_t));
        memcpy(tempDesc, descriptor, sizeof(ConMan_ParameterDescriptor_t));
        
        //modify values
        tempDesc->dataVersion = version;
        
        //and write to flash (or at least the buffer)
        NVM_memcpyBuffered((uint8_t*) descriptor, (uint8_t*) tempDesc, sizeof(ConMan_ParameterDescriptor_t));
        
        vPortFree(tempDesc);
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
    uint32_t hashToFind = crc(strParameterKey, strlen(strParameterKey), 0);
    
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
    uint32_t hashToFind = crc(strParameterKey, strlen(strParameterKey), 0);
    
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
    uint32_t hashToFind = crc(strParameterKey, strlen(strParameterKey), 0);
    
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
    uint32_t hashToFind = crc(strParameterKey, strlen(strParameterKey), 0);
    
    ConMan_ParameterDescriptor_t * descriptor;
    
    //try to locate the config in flash
    if(findParameterInFlash(hashToFind, &descriptor) == CONFIG_OK){
        return ConMan_readData(descriptor, offset, (uint8_t*) dataTarget, dataTargetSize);
    }else{
        return CONFIG_ENTRY_NOT_FOUND;
    }
}