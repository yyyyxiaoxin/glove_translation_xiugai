#include "wifi_portal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"
#include "glove_identity.h"
#include "glove_recording.h"
#include "wifi_transport.h"
#include "cloud_upload.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static esp_err_t json(httpd_req_t *r, cJSON *object)
{
    char *body=cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r,"application/json");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    esp_err_t result=httpd_resp_sendstr(r,body);
    free(body);
    return result;
}
static esp_err_t error(httpd_req_t *r,const char *status,const char *message)
{
    httpd_resp_set_status(r,status);
    cJSON *d=cJSON_CreateObject();cJSON_AddStringToObject(d,"error",message);
    return json(r,d);
}
static esp_err_t page(httpd_req_t *r)
{
    FILE *f=fopen("/wifi_web/index.html","rb");
    if(!f) return error(r,"503 Service Unavailable","配网页面分区未烧录");
    httpd_resp_set_type(r,"text/html; charset=utf-8");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    char buffer[1024];size_t n;esp_err_t result=ESP_OK;
    while((n=fread(buffer,1,sizeof(buffer),f))>0){
        if((result=httpd_resp_send_chunk(r,buffer,n))!=ESP_OK)break;
    }
    fclose(f);
    if(result==ESP_OK)result=httpd_resp_send_chunk(r,NULL,0);
    return result;
}
static esp_err_t status(httpd_req_t *r)
{
    wifi_config_t config={0};esp_wifi_get_config(WIFI_IF_STA,&config);
    cJSON *d=cJSON_CreateObject();
    cJSON_AddStringToObject(d,"hand",GLOVE_HAND_NAME);
    char ssid[33]={0};memcpy(ssid,config.sta.ssid,32);
    cJSON_AddStringToObject(d,"ssid",ssid);
    cJSON_AddBoolToObject(d,"connected",wifi_transport_is_connected());
    cJSON_AddBoolToObject(d,"recording",glove_recording_active());
    cJSON_AddBoolToObject(d,"uploading",cloud_upload_has_pending());
    return json(r,d);
}
static esp_err_t scan(httpd_req_t *r)
{
    if(glove_recording_active() || cloud_upload_has_pending()) return error(r,"409 Conflict","录制或补传期间不能扫描 Wi-Fi");
    uint16_t count=32;
    wifi_ap_record_t *aps=calloc(count,sizeof(*aps));
    if(!aps){esp_wifi_clear_ap_list();return ESP_ERR_NO_MEM;}
    esp_err_t ret=wifi_transport_scan(&count,aps);
    if(ret!=ESP_OK){free(aps);return error(r,"503 Service Unavailable","扫描失败，请稍后重试");}
    cJSON *d=cJSON_CreateObject(),*list=cJSON_AddArrayToObject(d,"networks");
    if(ret==ESP_OK) for(unsigned i=0;i<count;i++){
        if(!aps[i].ssid[0])continue;
        cJSON *item=cJSON_CreateObject();
        cJSON_AddStringToObject(item,"ssid",(char *)aps[i].ssid);
        cJSON_AddNumberToObject(item,"rssi",aps[i].rssi);
        cJSON_AddBoolToObject(item,"secure",aps[i].authmode!=WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(list,item);
    }
    free(aps);return json(r,d);
}
static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));esp_restart();
}
static esp_err_t configure(httpd_req_t *r)
{
    if(glove_recording_active() || cloud_upload_has_pending())return error(r,"409 Conflict","请先停止录制并等待数据上传完成");
    if(r->content_len<=0||r->content_len>1024)return error(r,"400 Bad Request","配置长度无效");
    char buffer[1025];int total=0;
    while(total<r->content_len){int n=httpd_req_recv(r,buffer+total,r->content_len-total);if(n<=0)return ESP_FAIL;total+=n;}
    buffer[total]=0;cJSON *d=cJSON_Parse(buffer);
    cJSON *ssid=cJSON_GetObjectItemCaseSensitive(d,"ssid"),*password=cJSON_GetObjectItemCaseSensitive(d,"password");
    if(!cJSON_IsString(ssid)||!cJSON_IsString(password)||strlen(ssid->valuestring)<1||strlen(ssid->valuestring)>32||strlen(password->valuestring)>63||(strlen(password->valuestring)>0&&strlen(password->valuestring)<8)){
        cJSON_Delete(d);return error(r,"400 Bad Request","名称需为 1–32 字节，密码需为 8–63 字节或留空");
    }
    nvs_handle_t handle;esp_err_t result=nvs_open("wifi_portal",NVS_READWRITE,&handle);
    if(result==ESP_OK){
        result=nvs_set_str(handle,"ssid",ssid->valuestring);
        if(result==ESP_OK)result=nvs_set_str(handle,"password",password->valuestring);
        if(result==ESP_OK)result=nvs_commit(handle);
        nvs_close(handle);
    }
    cJSON_Delete(d);
    if(result!=ESP_OK)return error(r,"500 Internal Server Error","配置保存失败");
    cJSON *response=cJSON_CreateObject();cJSON_AddBoolToObject(response,"ok",true);
    result=json(r,response);
    xTaskCreate(restart_task,"wifi_restart",2048,NULL,2,NULL);
    return result;
}
esp_err_t wifi_portal_start(void)
{
    esp_vfs_spiffs_conf_t fs={.base_path="/wifi_web",.partition_label="wifi_web",.max_files=2,.format_if_mount_failed=false};
    esp_err_t ret=esp_vfs_spiffs_register(&fs);
    if(ret!=ESP_OK)ESP_LOGE("wifi_portal","Web partition mount failed: %s",esp_err_to_name(ret));
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();config.stack_size=8192;config.max_uri_handlers=5;
    httpd_handle_t server=NULL;
    ret=httpd_start(&server,&config);if(ret!=ESP_OK)return ret;
    httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=page},
        {.uri="/api/status",.method=HTTP_GET,.handler=status},
        {.uri="/api/scan",.method=HTTP_GET,.handler=scan},
        {.uri="/api/config",.method=HTTP_POST,.handler=configure}
    };
    for(unsigned i=0;i<sizeof(routes)/sizeof(routes[0]);i++){ret=httpd_register_uri_handler(server,&routes[i]);if(ret!=ESP_OK)return ret;}
    ESP_LOGI("wifi_portal","Open http://192.168.4.1 after connecting to glove AP");
    return ESP_OK;
}
