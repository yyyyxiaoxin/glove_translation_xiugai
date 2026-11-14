#include "serial_recording.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "driver/uart.h"
#include "glove_identity.h"
#include "glove_recording.h"
#include "max98357a_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_GLOVE_HAND_LEFT
static void command_task(void *unused)
{
    char line[96];size_t used=0;bool overflow=false;
    for(;;){
        uint8_t c;
        if(uart_read_bytes(UART_NUM_0,&c,1,pdMS_TO_TICKS(100))<=0)continue;
        if(c=='\r')continue;
        if(c!='\n'){
            if(used<sizeof(line)-1)line[used++]=(char)c;else overflow=true;
            continue;
        }
        line[used]=0;used=0;
        if(overflow){overflow=false;continue;}
        unsigned long id;char action[12],extra;
        if(sscanf(line,"@REC %lu %11s %c",&id,action,&extra)!=2)continue;
        bool changed=false;
        if(strcmp(action,"START")==0){
            if(!glove_recording_active())changed=glove_recording_start();
        }else if(strcmp(action,"STOP")==0){
            if(glove_recording_active())changed=glove_recording_stop();
        }else if(strcmp(action,"STATUS")!=0)continue;
        if(changed)max98357a_announce_recording(glove_recording_active());
        uint64_t session;bool active=glove_recording_get(&session);
        printf("[REC_ACK] %lu %s %016" PRIx64 "\n",id,active?"RECORDING":"STOPPED",session);
        fflush(stdout);
    }
}
#endif
esp_err_t serial_recording_init(void)
{
#if CONFIG_GLOVE_HAND_LEFT
    esp_err_t result=uart_driver_install(UART_NUM_0,1024,0,0,NULL,0);
    if(result!=ESP_OK)return result;
    if(xTaskCreate(command_task,"serial_record",3072,NULL,3,NULL)!=pdPASS)return ESP_ERR_NO_MEM;
#endif
    return ESP_OK;
}
