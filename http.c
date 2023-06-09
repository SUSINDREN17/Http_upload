/*================================================================
  Copyright (c) 2020 Quectel Wireless Solution, Co., Ltd.  All Rights Reserved.
  Quectel Wireless Solution Proprietary and Confidential.
=================================================================*/
/*=================================================================

                        EDIT HISTORY FOR MODULE

This section contains comments describing changes made to the module.
Notice that changes are listed in reverse chronological order.

WHEN              WHO         WHAT, WHERE, WHY
------------     -------     -------------------------------------------------------------------------------

=================================================================*/


#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ql_api_osi.h"

#include "ql_log.h"
#include "ql_api_datacall.h"
#include "ql_http_client.h"
#include "ql_uart.h"
#include "ql_gpio.h"
#include "ql_fs.h"
#include "ql_sdmmc.h"
#include "ql_mqttclient.h"

#define QL_HTTP_LOG_LEVEL           	QL_LOG_LEVEL_INFO
#define QL_HTTP_LOG(msg, ...)			QL_LOG(QL_HTTP_LOG_LEVEL, "ql_HTTP", msg, ##__VA_ARGS__)
#define QL_HTTP_LOG_PUSH(msg, ...)	    QL_LOG_PUSH("ql_HTTP", msg, ##__VA_ARGS__)

#define QL_UART1_TX_FUNC             0x03
#define QL_UART1_RX_FUNC             0x03
#define QUEC_PIN_UART1_RXD           68
#define QUEC_PIN_UART1_TXD           67

#define DEBUG_ENABLE 1
#if DEBUG_ENABLE > 0
#define DEBUG_PORT  QL_UART_PORT_1
#define DBG_BUF_LEN   1024
static char DBG_BUFFER[DBG_BUF_LEN];

#define APP_DEBUG(FORMAT,...) {\
    memset(DBG_BUFFER, 0, DBG_BUF_LEN);\
    sprintf(DBG_BUFFER,FORMAT,##__VA_ARGS__); \
    if (QL_UART_PORT_1 == (DEBUG_PORT)) \
    {\
        ql_uart_write((ql_uart_port_number_e)(DEBUG_PORT), (unsigned char *)(DBG_BUFFER), strlen((char *)(DBG_BUFFER)));\
    }\
}
#else
#define APP_DEBUG(FORMAT,...) 
#endif

#define QL_UART_RX_BUFF_SIZE       2048
#define MIN(a,b) ((a) < (b) ? (a) : (b))

//ql_uart_config_s Uart_cfg = {0};				


typedef enum{
	QHTTPC_EVENT_RESPONSE     	= 1001,
	QHTTPC_EVENT_END,
}qhttpc_event_code_e;

typedef struct
{
	http_client_t  			  	http_client;
	ql_queue_t 					queue;

	ql_mutex_t                	simple_lock;
	bool						dl_block;
	int							dl_high_line;
	int							dl_total_len;

	QFILE						upload_fd;
	QFILE						dload_fd;
}qhttpc_ctx_t;


#define 		HTTP_MAX_MSG_CNT		8
#define 		HTTP_DLOAD_HIGH_LINE	40960
ql_task_t 		http_task = NULL;
qhttpc_ctx_t 	http_demo_client = {0};

int ret,res_code = 0;

static void http_event_cb(http_client_t *client, int evt, int evt_code, void *arg)
{
	qhttpc_ctx_t *client_ptr = (qhttpc_ctx_t *)arg;
	ql_event_t qhttpc_event_send = {0};

	//APP_DEBUG("\nenter");
	
	if(client_ptr == NULL)
		return;
	
	//APP_DEBUG("\n*client:%d, http_cli:%d", *client, client_ptr->http_client);

	if(*client != client_ptr->http_client)
		return;
	//APP_DEBUG("\nevt:%d, evt_code:%d", evt, evt_code);
	switch(evt){
	case HTTP_EVENT_SESSION_ESTABLISH:{
			if(evt_code != HTTP_SUCCESS){
				APP_DEBUG("\nHTTP session create failed!!!!!");
				qhttpc_event_send.id = QHTTPC_EVENT_END;
				qhttpc_event_send.param1 = (uint32)client_ptr;
				ql_rtos_queue_release(client_ptr->queue, sizeof(ql_event_t), (uint8 *)&qhttpc_event_send, QL_WAIT_FOREVER);
			}
		}
		break;
	case HTTP_EVENT_RESPONE_STATE_LINE:{
			if(evt_code == HTTP_SUCCESS){
				int content_length = 0;
				int chunk_encode = 0;
				char *location = NULL;
				ql_httpc_getinfo(client, HTTP_INFO_RESPONSE_CODE, &res_code);
				APP_DEBUG("\nresponse code:%d", res_code);
				ql_httpc_getinfo(client, HTTP_INFO_CHUNK_ENCODE, &chunk_encode);
				if(chunk_encode == 0){
					ql_httpc_getinfo(client, HTTP_INFO_CONTENT_LEN, &content_length);
					APP_DEBUG("\ncontent_length:%d",content_length);
				}else{
					APP_DEBUG("\nhttp chunk encode!!!");
				}

				if(res_code >= 300 && res_code < 400){
					ql_httpc_getinfo(client, HTTP_INFO_LOCATION, &location);
					APP_DEBUG("\nredirect location:%s", location);
					free(location);
				}
			}
		}
		break;
	case HTTP_EVENT_SESSION_DISCONNECT:{
			if(evt_code == HTTP_SUCCESS){
				APP_DEBUG("\n===http transfer end!!!!");
			}else{
				APP_DEBUG("\n===http transfer occur exception!!!!!");
			}			
			qhttpc_event_send.id = QHTTPC_EVENT_END;
			qhttpc_event_send.param1 = (uint32)client_ptr;
			ql_rtos_queue_release(client_ptr->queue, sizeof(ql_event_t), (uint8 *)&qhttpc_event_send, QL_WAIT_FOREVER);
		}
		break;
	}
}

static int http_write_response_data(http_client_t *client, void *arg, char *data, int size, unsigned char end)
{
	int ret = size;
	uint32 msg_cnt = 0;
	char *read_buff = NULL;
	qhttpc_ctx_t *client_ptr = (qhttpc_ctx_t *)arg;
	ql_event_t qhttpc_event_send = {0};

	//APP_DEBUG("\nenter");	
	
	if(client_ptr == NULL)
		return 0;
	
	//APP_DEBUG("\n*client:%d, http_cli:%d", *client, client_ptr->http_client);

	if(*client != client_ptr->http_client)
		return 0;

	read_buff = (char *)malloc(size+1);
	if(read_buff == NULL)
	{
		APP_DEBUG("\nmem faild");
		return 0;
	}

	memcpy(read_buff, data, size);

	if(QL_OSI_SUCCESS != ql_rtos_queue_get_cnt(client_ptr->queue, &msg_cnt))
	{
		free(read_buff);
		APP_DEBUG("\nql_rtos_queue_get_cnt faild");
		return 0;
	}
	
	ql_rtos_mutex_lock(client_ptr->simple_lock, 100);
	if(msg_cnt >= (HTTP_MAX_MSG_CNT-1) || (client_ptr->dl_total_len + size) >= client_ptr->dl_high_line)
	{
		client_ptr->dl_block = true;
		ret = HTTP_ERR_WOUNDBLOCK;
	}
	ql_rtos_mutex_unlock(client_ptr->simple_lock);

	//APP_DEBUG("\nmsg_cnt %d, total_len+size %d", msg_cnt, (client_ptr->dl_total_len + size));

	qhttpc_event_send.id = QHTTPC_EVENT_RESPONSE;
	qhttpc_event_send.param1 = (uint32)client_ptr;
	qhttpc_event_send.param2 = (uint32)read_buff;
	qhttpc_event_send.param3 = (uint32)size;
	if(QL_OSI_SUCCESS != ql_rtos_queue_release(client_ptr->queue, sizeof(ql_event_t), (uint8 *)&qhttpc_event_send, 0))
	{
		free(read_buff);
		APP_DEBUG("\nql_rtos_queue_release faild");
		return 0;
	}
	
	ql_rtos_mutex_lock(client_ptr->simple_lock, 100);
	client_ptr->dl_total_len += size;
	ql_rtos_mutex_unlock(client_ptr->simple_lock);
	
	APP_DEBUG("\nhttp response :%d bytes data", size);
	
	return ret;
}

static int http_read_request_data(http_client_t *client, void *arg, char *data, int size)
{
	int ret = 0;
	QFILE fd = 0;
	qhttpc_ctx_t *client_ptr = (qhttpc_ctx_t *)arg;
	
	//APP_DEBUG("\nenter");	
	
	if(client_ptr == NULL)
		return 0;
	
	//APP_DEBUG("\n*client:%d, http_cli:%d", *client, client_ptr->http_client);

	if(*client != client_ptr->http_client)
		return 0;

	ql_rtos_mutex_lock(client_ptr->simple_lock, 100);
	fd = client_ptr->upload_fd;
	ql_rtos_mutex_unlock(client_ptr->simple_lock);

	//APP_DEBUG("\nfd:%d", fd);	
	
	if(fd < 0)
		return 0;
	//APP_DEBUG("\nread size:%d", size);
	ret = ql_fread(data, size, 1, fd);
	APP_DEBUG("\nhttp read :%d bytes data", ret);
	if(ret > 0)
		return ret;

	return 0;
}

static void http_app_thread(void * arg)
{
	// ql_uart_set_dcbconfig(QL_UART_PORT_1, &uart_cfg);
	// ql_pin_set_func(QUEC_PIN_UART1_TXD, QL_UART1_TX_FUNC);
	// ql_pin_set_func(QUEC_PIN_UART1_RXD, QL_UART1_RX_FUNC);
	// ql_uart_open(QL_UART_PORT_1);
 
    int ret = 0;// i = 0;
    //int profile_idx = 1;
   // ql_data_call_info_s info;
	//char ip4_addr_str[16] = {0};
	int run_num = 0;
	struct stat dload_stat;
	//uint8_t nSim = 0;
	int flags_break = 0;
	ql_event_t qhttpc_event_msg = {0};
	
	ql_rtos_task_sleep_s(5);
	// APP_DEBUG("\n========== http demo start ==========");
	// APP_DEBUG("\nwait for network register done");
	
	// while((ret = ql_network_register_wait(nSim, 120)) != 0 && i < 10){
    // 	i++;
	// 	ql_rtos_task_sleep_ms(1000);
	// }
	// if(ret == 0){
	// 	i = 0;
	// 	APP_DEBUG("\n====network registered!!!!====");
	// }else{
	// 	APP_DEBUG("\n====network register failure!!!!!====");
	// 	goto exit;
	// }
	// ql_set_data_call_asyn_mode(nSim, profile_idx, 0);
	// ret=ql_start_data_call(nSim, profile_idx, QL_PDP_TYPE_IP, "uninet", NULL, NULL, 0); 
	// if(ret != 0){
	// 	APP_DEBUG("\n====data call failure!!!!=====");
	// }
	// memset(&info, 0x00, sizeof(ql_data_call_info_s));
	
	// ret = ql_get_data_call_info(nSim, profile_idx, &info);
	// if(ret != 0){
	// 	APP_DEBUG("\nql_get_data_call_info ret: %d", ret);
	// 	ql_stop_data_call(nSim, profile_idx);
	// 	goto exit;
	// }
    // APP_DEBUG("info->profile_idx: %d", info.profile_idx);
	// APP_DEBUG("info->ip_version: %d", info.ip_version);
            
	// APP_DEBUG("info->v4.state: %d", info.v4.state); 
	// inet_ntop(AF_INET, &info.v4.addr.ip, ip4_addr_str, sizeof(ip4_addr_str));
	// APP_DEBUG("info.v4.addr.ip: %s\r\n", ip4_addr_str);

	// inet_ntop(AF_INET, &info.v4.addr.pri_dns, ip4_addr_str, sizeof(ip4_addr_str));
	// APP_DEBUG("info.v4.addr.pri_dns: %s\r\n", ip4_addr_str);

	// inet_ntop(AF_INET, &info.v4.addr.sec_dns, ip4_addr_str, sizeof(ip4_addr_str));
	// APP_DEBUG("info.v4.addr.sec_dns: %s\r\n", ip4_addr_str);

	while(1){
		
		ql_rtos_task_sleep_ms(500);
		if(upload_flag == 1 && sd_flag == 1 && re_upload <= 1)
		{
			upload_flag = 0;
		int http_method = HTTP_METHOD_NONE;
		//char dload_file[] = "UFS:http_dload.txt";
		//char upload_file[] = "SD:http_upload.txt";
		
		APP_DEBUG("\n==============http_client_test[%d]================\n",run_num+1);

		memset(&http_demo_client, 0x00, sizeof(qhttpc_ctx_t));

		http_demo_client.dl_block = false;
		http_demo_client.dl_high_line = HTTP_DLOAD_HIGH_LINE;

		ret = ql_rtos_mutex_create(&http_demo_client.simple_lock);
		if (ret) 
		{
			APP_DEBUG("\nql_rtos_mutex_create failed!!!!");
			break;
		}

		ret = ql_rtos_queue_create(&http_demo_client.queue, sizeof(ql_event_t), HTTP_MAX_MSG_CNT);
		if (ret) 
		{
			APP_DEBUG("\nql_rtos_queue_create failed!!!!");
			break;
		}
		
		if(ql_httpc_new(&http_demo_client.http_client, http_event_cb, (void *)&http_demo_client) != HTTP_SUCCESS){
			APP_DEBUG("\nhttp client create failed!!!!");
			break;
		}
		profile_id = 1;
		ql_httpc_setopt(&http_demo_client.http_client, HTTP_CLIENT_OPT_SIM_ID, nSim);
		ql_httpc_setopt(&http_demo_client.http_client, HTTP_CLIENT_OPT_PDPCID, profile_id);
		ql_httpc_setopt(&http_demo_client.http_client, HTTP_CLIENT_OPT_WRITE_FUNC, http_write_response_data);
		ql_httpc_setopt(&http_demo_client.http_client, HTTP_CLIENT_OPT_WRITE_DATA, (void *)&http_demo_client);
							
		char url[] = "http://dev.mwsresearchcentre.com:8000/device/c91f185c-ffd5-4d29-bf68-96dbb7cf5539/upload";
		struct stat stat_buf;
		http_method = HTTP_METHOD_POST;
			
		http_demo_client.upload_fd = ql_fopen(upload_file, "r");
        APP_DEBUG("\nfile FD :%d", http_demo_client.upload_fd );

		if(http_demo_client.upload_fd < 0)
		{
			ql_fclose(http_demo_client.dload_fd);
			ql_httpc_release(&http_demo_client.http_client);
			goto exit;
		}
		memset(&stat_buf, 0x00, sizeof(struct stat));

		ql_fstat(http_demo_client.upload_fd, &stat_buf);
		//APP_DEBUG("file size:%d", stat_buf.st_size);
		if(stat_buf.st_size == 0)
		{
			ql_fclose(http_demo_client.upload_fd);
			ql_fclose(http_demo_client.dload_fd);
			ql_httpc_release(&http_demo_client.http_client);
		}
		ql_httpc_setopt(&http_demo_client.http_client, HTTP_CLIENT_OPT_METHOD, http_method);
		ql_httpc_setopt(&http_demo_client.http_client, HTTP_CLIENT_OPT_URL, (char *)url);
		ql_httpc_setopt(&http_demo_client.http_client, HTTP_CLIENT_OPT_REQUEST_HEADER, "Content-type: multipart/form-data");
		ql_httpc_formadd(&http_demo_client.http_client, HTTP_FORM_NAME, "file");
		ql_httpc_formadd(&http_demo_client.http_client, HTTP_FORM_FILENAME, upload_file);
		ql_httpc_formadd(&http_demo_client.http_client, HTTP_FORM_CONTENT_TYPE, "text/plain");
				
		ql_httpc_setopt(&http_demo_client.http_client, HTTP_CLIENT_OPT_READ_FUNC, http_read_request_data);
		ql_httpc_setopt(&http_demo_client.http_client, HTTP_CLIENT_OPT_READ_DATA, (void *)&http_demo_client);
		ql_httpc_setopt(&http_demo_client.http_client, HTTP_CLIENT_OPT_UPLOAD_LEN, stat_buf.st_size);

		if(ql_httpc_perform(&http_demo_client.http_client) == HTTP_SUCCESS)
		{
			APP_DEBUG("\nwait http perform end!!!!!!");

			flags_break = 0;
			for (;;)
			{
				memset(&qhttpc_event_msg, 0x00, sizeof(ql_event_t));
				
				ql_rtos_queue_wait(http_demo_client.queue, (uint8 *)&qhttpc_event_msg, sizeof(ql_event_t), QL_WAIT_FOREVER);

				switch(qhttpc_event_msg.id)
				{
					case QHTTPC_EVENT_END:
					{
						flags_break = 1;
					}
						break;
					default:
						break;
				}

				if(flags_break)
					break;
			}
		}else{
			APP_DEBUG("\nhttp perform failed!!!!!!!!!!");
		}
		memset(&dload_stat, 0x00, sizeof(struct stat));
		ql_fstat(http_demo_client.dload_fd, &dload_stat);
		//APP_DEBUG("=========dload_file_size:%d", dload_stat.st_size);
		if(http_demo_client.dload_fd >= 0){
			ql_fclose(http_demo_client.dload_fd);
			http_demo_client.dload_fd = -1;
		}
		ql_rtos_mutex_lock(http_demo_client.simple_lock, 100);
		if(http_demo_client.upload_fd >= 0){
			ql_fclose(http_demo_client.upload_fd);
			http_demo_client.upload_fd = -1;
		}
		ql_rtos_mutex_unlock(http_demo_client.simple_lock);
		
		ql_httpc_release(&http_demo_client.http_client);
		http_demo_client.http_client = 0;
		APP_DEBUG("\n==============http_client_test_end[%d]================\n",run_num+1);
		run_num++;

		if(http_demo_client.queue != NULL)
		{
			while(1)
			{
				memset(&qhttpc_event_msg, 0x00, sizeof(ql_event_t));
				
				if(QL_OSI_SUCCESS != ql_rtos_queue_wait(http_demo_client.queue, (uint8 *)&qhttpc_event_msg, sizeof(ql_event_t), 0))
					break;

				switch(qhttpc_event_msg.id)
				{
					case QHTTPC_EVENT_RESPONSE:
					{
						free((void *)(qhttpc_event_msg.param2));
					}
						break;
					default:
						break;
				}
			}
			ql_rtos_queue_delete(http_demo_client.queue);
			http_demo_client.queue = NULL;
		}

		ql_rtos_mutex_delete(http_demo_client.simple_lock);
		http_demo_client.simple_lock = NULL;
		
		ql_rtos_task_sleep_s(3);
		if(res_code == 200)
		{
			int http_fd = 0;
			http_fd = ql_remove(upload_file);
        	if(http_fd < 0)
        	{
            	APP_DEBUG("\nfile remove failed\n");
        	}
        	else
        	{
				APP_DEBUG("\nfile removed %d\n",http_fd);
        	}
		}
		else
		{
			upload_flag = 1;
			re_upload++;
		}
	}
	}
exit:
	if(http_demo_client.queue != NULL)
	{
		while(1)
		{
			memset(&qhttpc_event_msg, 0x00, sizeof(ql_event_t));
			
			if(QL_OSI_SUCCESS != ql_rtos_queue_wait(http_demo_client.queue, (uint8 *)&qhttpc_event_msg, sizeof(ql_event_t), 0))
				break;

			switch(qhttpc_event_msg.id)
			{
				case QHTTPC_EVENT_RESPONSE:
				{
					free((void *)(qhttpc_event_msg.param2));
				}
					break;
				default:
					break;
			}
		}
		ql_rtos_queue_delete(http_demo_client.queue);
	}

	if(http_demo_client.simple_lock != NULL)
		ql_rtos_mutex_delete(http_demo_client.simple_lock);
	
  	ql_rtos_task_delete(http_task);	
    return;		
}

void ql_http_app_init(void)
{
    QlOSStatus err = QL_OSI_SUCCESS;
    
    err = ql_rtos_task_create(&http_task, 4096, APP_PRIORITY_NORMAL, "QhttpApp", http_app_thread, NULL, 5);
	if(err != QL_OSI_SUCCESS)
	{
		APP_DEBUG("\ncreated task failed");
	}
}

