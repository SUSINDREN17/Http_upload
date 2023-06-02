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
#include "ql_gpio.h"
#include "ql_api_nw.h"
#include "ql_uart.h"
#include "ql_log.h"
#include "ql_api_datacall.h"
#include "ql_mqttclient.h"
#include "mqtt_demo.h"
#include "ql_gpio.h"
#include "ql_api_sim.h"
#include "ql_api_dev.h"
#include "ql_power.h"
#include "ql_embed_nor_flash.h"
#include "ql_fs.h"
#include "ql_gnss.h"
#include "ql_http_client.h"

#define QL_UART1_TX_FUNC             0x03
#define QL_UART1_RX_FUNC             0x03
#define QUEC_PIN_UART1_RXD           68
#define QUEC_PIN_UART1_TXD           67

// #define MQTT_CLIENT_IDENTITY        "quectel_01"
// #define MQTT_CLIENT_USER            "user"
// #define MQTT_CLIENT_PASS            "user@123"
// #define MQTT_HOST                   "mqtt://120.138.14.51:1883"

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

static ql_task_t mqtt_task = NULL;
static ql_sem_t  mqtt_semp;
mqtt_client_t mqtt_cli;

char outbuff[1024],uartbuf[1000],tempbuf[500],MQTT_CLIENT_IDENTITY[50],MQTT_CLIENT_USER[25],MQTT_CLIENT_PASS[25],MQTT_HOST[50], pubtopic[100], subtopic[100];
char *decodearr[25];
int mqtt_ret=0,timeout=0,profile_id=2,mqtt_flag = 0,data_reg,ret;
char apn[20],nwoperator[20],nwtype[10];
unsigned char csq;

bool connectflag=0,registered = 0;
uint8_t nSim=0;
uint16_t sim_cid;

ql_data_call_info_s info;

struct mqtt_connect_client_info_t  clint_info = {0};
static int mqtt_connected=0;
ql_nw_reg_status_info_s nwk_info = {0};
ql_nw_operator_info_s opr_info = {0};


void ql_uart_notify_cb(unsigned int ind_type, ql_uart_port_number_e port, unsigned int size)
{
    unsigned char *recv_buff = calloc(1, QL_UART_RX_BUFF_SIZE+1);
    unsigned int real_size = 0;
    int read_len = 0;
    
    //APP_DEBUG("UART port %d receive ind type:0x%x, receive data size:%d\n", port, ind_type, size);
    switch(ind_type)
    {
        case QUEC_UART_RX_OVERFLOW_IND:  //rx buffer overflow
        case QUEC_UART_RX_RECV_DATA_IND:
        {
            while(size > 0)
            {
                memset(recv_buff, 0, QL_UART_RX_BUFF_SIZE+1);
                real_size= MIN(size, QL_UART_RX_BUFF_SIZE);
                read_len = ql_uart_read(port, recv_buff, real_size);
                //APP_DEBUG("read_len=%d, recv_data=%s\n", read_len, recv_buff);
                if((read_len > 0) && (size >= read_len))
                {
                    size -= read_len;
                }
                else
                {
                    break;
                }
				sprintf(tempbuf,"%s",recv_buff);
				APP_DEBUG("\n%s %s",recv_buff,tempbuf);
				strcat(uartbuf,tempbuf);
            }
            break;
        }       
    }
    free(recv_buff);
    recv_buff = NULL;
}



static void mqtt_state_exception_cb(mqtt_client_t *client)
{
	mqtt_connected = 0;
}
static void mqtt_connect_result_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_e status)
{
	//char d[50];
	//sprintf(d,"\nstatus : %d",status);
	//APP_DEBUG("\n %s",d);
	if(status == 0)
	{
		mqtt_connected = 1;	
		//APP_DEBUG("\nmqtt connected");
		APP_DEBUG("\n&Connected$");
	}
	ql_rtos_semaphore_release(mqtt_semp);
}
static void mqtt_requst_result_cb(mqtt_client_t *client, void *arg,int err)
{
	//APP_DEBUG("\nrequest result callback : %d",err);
	if(valid)
	{
		APP_DEBUG("\n&&*~Data_received,%d,%s,%s,%d,%c%f,%c%f,%02d/%02d/%02d %02d:%02d:%02d~$\n",registered,nwtype,nwoperator,csq,latit_cardinal,latit,longit_cardinal,longit,gps_yr,gps_mn,gps_dt,gps_hh,gps_mm,gps_ss);
	}
	else
	{
		APP_DEBUG("\n&&*~Data_received,%d,%s,%s,%d,ND,ND,ND,ND,ND,ND,ND,ND,ND,ND~$\n", registered,nwtype,nwoperator,csq);
	}
	ql_rtos_semaphore_release(mqtt_semp);
}
static void mqtt_inpub_data_cb(mqtt_client_t *client, void *arg, int pkt_id, const char *topic, const unsigned char *payload, unsigned short payload_len)
{
	//APP_DEBUG("topic: %s\n", topic);
	APP_DEBUG("%s", payload);
}

void network()
{
	timeout = 0;

	while((mqtt_ret = ql_network_register_wait(nSim, 30)) != 0 && timeout < 1)
	{		
		APP_DEBUG("\nql_network_register_wait");
		timeout++;
		//ql_power_reset(RESET_QUICK);
		if(ql_dev_set_modem_fun(QL_DEV_CFUN_MIN, 0, 0) == QL_DEV_SUCCESS)
		{
			//APP_DEBUG("\ngsm minimum func");
		}
		ql_rtos_task_sleep_ms(5000);
		if(ql_dev_set_modem_fun(QL_DEV_CFUN_FULL, 0, 0) == QL_DEV_SUCCESS)
		{
			//APP_DEBUG("\ngsm full func");
		}
		ql_rtos_task_sleep_s(1);
	}
	if(mqtt_ret == 0)
	{
		APP_DEBUG("\nNETWORK REGISTERED -%d\n",profile_id);
	}
	ql_set_data_call_asyn_mode(nSim, profile_id, 0);

	ql_nw_get_operator_name(nSim,&opr_info);
	sprintf(nwoperator,"%s",opr_info.short_oper_name);
	
	if(strstr(nwoperator,"JIO"))
	sprintf(apn,"Jionet");
	else if(strstr(nwoperator,"BSNL MOBILE"))
	sprintf(apn,"bsnlnet");
	else//(strstr(nwoperator,"airtel"))
	sprintf(apn,"airtelgprs.com");

	if(nwk_info.data_reg.act == 0)//network type 2g or 4g
		sprintf(nwtype,"2G");
    else if(nwk_info.data_reg.act == 7)
		sprintf(nwtype,"4G");
	else
		sprintf(nwtype,"UN");
    
	if(!(strcmp(apn,"Jionet")))
	{
		mqtt_ret = ql_start_data_call(nSim, profile_id, QL_PDP_TYPE_IPV6, apn, NULL, NULL, 0);
	}
	else
	{
		mqtt_ret = ql_start_data_call(nSim, profile_id, QL_PDP_TYPE_IPV4V6, apn, NULL, NULL, 0);
	}

	APP_DEBUG("mqtt_ret = %d",mqtt_ret);
	if(mqtt_ret == 0)
	{
		APP_DEBUG("\nDATA CALL SUCCESS");
	}
	memset(&info, 0x00, sizeof(ql_data_call_info_s));
	mqtt_ret = ql_get_data_call_info(nSim, profile_id, &info);
	if(mqtt_ret != 0){
		ql_stop_data_call(nSim, profile_id);
	}
	else{
		APP_DEBUG("\nDATA CALL INFO");
	}
}

void 
network_check()
{
	ret = ql_nw_get_reg_status(nSim, &nwk_info);//*****
	data_reg = nwk_info.data_reg.state;// network state registered or not

	if((data_reg == 1)||(data_reg == 5))
	{
		registered = 1;
	}
	else
	{
		registered = 0;
	}
	ql_nw_get_csq(nSim, &csq);//rf range

	//APP_DEBUG("\nnetwrk_reg = %d\n",registered);
	//APP_DEBUG("\nlong op : %s  mnc : %s  mcc : %s  short op : %s",opr_info.long_oper_name,opr_info.mnc,opr_info.mcc,opr_info.short_oper_name);

}
void mqtt_connect()
{
	mqtt_connected = 0;
    ql_rtos_semaphore_create(&mqtt_semp, 0);
	
	clint_info.keep_alive = 60;
	clint_info.clean_session = 1;
	clint_info.will_qos = 0;
	clint_info.will_retain = 0;
	clint_info.will_topic = NULL;
	clint_info.will_msg = NULL;
	clint_info.client_id = MQTT_CLIENT_IDENTITY;
	clint_info.client_user = MQTT_CLIENT_USER;
	clint_info.client_pass = MQTT_CLIENT_PASS;
	mqtt_ret = ql_bind_sim_and_profile(nSim, profile_id, &sim_cid);
	if(mqtt_ret == 0)
	{
		APP_DEBUG("\nGET SIM CID");
	}
	mqtt_ret = ql_mqtt_client_init(&mqtt_cli, sim_cid);
	if(mqtt_ret == 0)
	{
		APP_DEBUG("\nCLIENT INIT");
	}
	ql_mqtt_connect(&mqtt_cli, MQTT_HOST, mqtt_connect_result_cb, NULL, (const struct mqtt_connect_client_info_t *)&clint_info, mqtt_state_exception_cb);
	timeout = 0;
	while ((mqtt_connected == 0) && (timeout <= 10))
	{
		ql_rtos_task_sleep_s(1);
		timeout++;
	}
	//APP_DEBUG("\nTopic = %s\n",topic1);

	ql_mqtt_sub_unsub(&mqtt_cli, subtopic, 1, mqtt_requst_result_cb,NULL, 1);
	ql_rtos_task_sleep_s(1);
	ql_mqtt_set_inpub_callback(&mqtt_cli, mqtt_inpub_data_cb, NULL);
}
void strdecode(char *decodebuf)
{
	uint8_t maxdata;
	memset(&decodearr,0,sizeof(decodearr));
	maxdata = 0;
	decodearr[maxdata] = strtok(decodebuf,",");
	while((decodearr[maxdata] != NULL) && (maxdata <= 10))
	{
		maxdata++;
		decodearr[maxdata] = strtok(NULL,",");
		//APP_DEBUG("decodearr[%d]-%s\n",maxdata,decodearr[maxdata]);
	}
}
void modemcmddecode(void)
{
	//APP_DEBUG("outbuff-%s",outbuff);
	if(strstr(outbuff,"@@"))
	{
		strdecode(uartbuf);
		if(strstr(decodearr[1],"GPRS"))
		{
			memset(&MQTT_HOST,0,sizeof(MQTT_HOST));
			memset(&MQTT_CLIENT_USER,0,sizeof(MQTT_CLIENT_USER));
			memset(&MQTT_CLIENT_PASS,0,sizeof(MQTT_CLIENT_PASS));			
			memset(&MQTT_CLIENT_IDENTITY,0,sizeof(MQTT_CLIENT_IDENTITY));

			memcpy(MQTT_HOST,decodearr[2],sizeof(MQTT_HOST));
			memcpy(MQTT_CLIENT_USER,decodearr[3],sizeof(MQTT_CLIENT_USER));
			memcpy(MQTT_CLIENT_PASS,decodearr[4],sizeof(MQTT_CLIENT_PASS));
			memcpy(MQTT_CLIENT_IDENTITY,decodearr[5],sizeof(MQTT_CLIENT_IDENTITY));
			connectflag = 1;
			sprintf(pubtopic,"DAG/%s",MQTT_CLIENT_IDENTITY);
			sprintf(subtopic,"DAG/%s/CMD",MQTT_CLIENT_IDENTITY);
			//APP_DEBUG("GPRS- %s - %s - %s - %s",MQTT_HOST,MQTT_CLIENT_USER,MQTT_CLIENT_PASS,MQTT_CLIENT_IDENTITY);
		}
	}
	memset(uartbuf,0,sizeof(uartbuf));
}

static void mqtt_disconnect_result_cb(mqtt_client_t *client, void *arg,int err)
{
	if(err == 0)
	{
		mqtt_flag = 1;
		APP_DEBUG("\nMQTT Disconnected\n");
	}
	else{
		mqtt_flag = 0;
		APP_DEBUG("\nMQTT Disconnect failed\n");
	}
}

void mqtt_publsh(void)
{
	if(mqtt_connected == 1)
	{
		//APP_DEBUG("\nbuffer : %s",outbuff);
		if(strstr(outbuff,"##"))
		{
			if(ql_mqtt_publish(&mqtt_cli, pubtopic,outbuff, strlen(outbuff), 0, 0, mqtt_requst_result_cb,NULL) == 0)
			{
				APP_DEBUG("\n*Publish$");
			}
		}
		else if(strstr(outbuff,"@@"))
		{
			ql_mqtt_disconnect(&mqtt_cli, mqtt_disconnect_result_cb, NULL);
			ql_rtos_task_sleep_s(2);
			ql_mqtt_client_deinit(&mqtt_cli);
			ql_rtos_task_sleep_s(1); 

			if(mqtt_flag == 1)
			{
				modemcmddecode();
				mqtt_connect();
				mqtt_flag = 0;
			}
			else{
				APP_DEBUG("\nMqtt Reconnect failed...\n");
			}
		}
	}
	else
	APP_DEBUG("MQTT Not connected\n");
}

static void mqtt_app_thread(void * arg)
{	
	APP_DEBUG("mqtt_app_thread start\n");
	network();
	network_check();

	while(connectflag == 0)
	{
		network_check();
		if(registered == 1)
		{
			timeout++;
			if(timeout >= 500)
			{
				APP_DEBUG("&&*~GPRS?~$");
				timeout = 0;
			}
			if(strlen(uartbuf) > 10)		
			{
				//APP_DEBUG("Data recived\n");
				ql_rtos_task_sleep_ms(200);
				sprintf(outbuff,"%s",uartbuf);
				modemcmddecode();
			}
			ql_rtos_task_sleep_ms(10);
		}
		else
		{
			APP_DEBUG("\nNetwork Not Registered..\n");
			if(strlen(uartbuf) > 1)
			{
				APP_DEBUG("\nData received");
			}
			network();
		}
	}
	mqtt_connect();
	while(1)
	{
		network_check();
		//APP_DEBUG("mqtt_app_thread running\n");
		ql_rtos_task_sleep_s(1);
		if(registered == 1)
		{
			if(strlen(uartbuf) > 10)
			{
				ql_rtos_task_sleep_ms(200);
			//APP_DEBUG("Uart data - %s\n",uartbuf);
				sprintf(outbuff,"%s",uartbuf);
				mqtt_publsh();
				memset(uartbuf,0,sizeof(uartbuf));
			}
		}
		else{
			APP_DEBUG("\nNetwork Not Registered..\n");
			if(strlen(uartbuf) > 1)
			{
				APP_DEBUG("\nData received");
			}
			network();
			ql_rtos_task_sleep_s(2);
		}
	}
}

 
int ql_mqtt_app_init(void)
{
	ql_gpio_init(GPIO_2,GPIO_OUTPUT,PULL_DOWN,LVL_LOW);
	ql_gpio_set_level(GPIO_2,LVL_LOW);
	QlOSStatus err = QL_OSI_SUCCESS;
	ql_uart_config_s uart_cfg = {0};

	uart_cfg.baudrate = QL_UART_BAUD_115200;
    uart_cfg.flow_ctrl = QL_FC_NONE;
    uart_cfg.data_bit = QL_UART_DATABIT_8;
    uart_cfg.stop_bit = QL_UART_STOP_1;
    uart_cfg.parity_bit = QL_UART_PARITY_NONE;

    mqtt_ret = ql_uart_set_dcbconfig(QL_UART_PORT_1, &uart_cfg);
	ql_pin_set_func(QUEC_PIN_UART1_TXD, QL_UART1_TX_FUNC);
	ql_pin_set_func(QUEC_PIN_UART1_RXD, QL_UART1_RX_FUNC);
	ql_uart_open(QL_UART_PORT_1);

	mqtt_ret = ql_uart_register_cb(QL_UART_PORT_1, ql_uart_notify_cb);
	APP_DEBUG("mqtt_app init start\n");
    err = ql_rtos_task_create(&mqtt_task, 4096,  APP_PRIORITY_REALTIME, "mqtt_app", mqtt_app_thread, NULL, 5);
	if(err != QL_OSI_SUCCESS)
    {
		APP_DEBUG("mqtt_app init failed");
	}

	return err;
}

