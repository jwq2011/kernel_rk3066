/* drivers/input/touchscreen/gt9xx.c
 * 
 * 2010 - 2013 Goodix Technology.
 *  input_set_capability(ts->input_dev, EV_KEY, KEY_POWER);

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be a reference 
 * to you, when you are integrating the GOODiX's CTP IC into your system, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU 
 * General Public License for more details.
 * 
 * Version: 1.8
 * Authors: andrew@goodix.com, meta@goodix.com
 * Release Date: 2013/04/25
 * Revision record:
 *      V1.0:    KEY_POWER
 *          first Release. By Andrew, 2012/08/31 
 *      V1.2:
 *          modify gtp_reset_guitar,slot report,tracking_id & 0x0F. By Andrew, 2012/10/15
 *      V1.4:
 *          modify gt9xx_update.c. By Andrew, 2012/12/12
 *      V1.6: 
 *          1. new heartbeat/esd_protect mechanism(add external watchdog)
 *          2. doze mode, sliding wakeup 
 *          3. 3 more cfg_group(GT9 Sensor_ID: 0~5) 
 *          3. config length verification
 *          4. names & comments
 *                  By Meta, 2013/03/11
 *      V1.8:
 *          1. pen/stylus identification 
 *          2. read double check & fixed config support
 *          2. new esd & slide wakeup optimization
 *                  By Meta, 2013/06/08
 */
#include <plat/board.h> ///wj add for struct goodix_platform_data
#include <linux/irq.h>
#include "gt9xx.h"


#if GTP_ICS_SLOT_REPORT
    #include <linux/input/mt.h>
#endif
struct input_dev *input_dev_g;
static const char *goodix_ts_name = "Goodix Capacitive TouchScreen";
static struct workqueue_struct *goodix_wq;
struct i2c_client * i2c_connect_client = NULL; 
u8 config[GTP_CONFIG_MAX_LENGTH + GTP_ADDR_LENGTH]
                = {GTP_REG_CONFIG_DATA >> 8, GTP_REG_CONFIG_DATA & 0xff};

#if GTP_HAVE_TOUCH_KEY
    static const u16 touch_key_array[] = GTP_KEY_TAB;
    #define GTP_MAX_KEY_NUM  (sizeof(touch_key_array)/sizeof(touch_key_array[0]))
    
#if GTP_DEBUG_ON
    static const int  key_codes[] = {KEY_HOME, KEY_BACK, KEY_SELF_MENU, KEY_SEARCH};
    static const char *key_names[] = {"Key_Home", "Key_Back", "Key_Menu", "Key_Search"};
#endif
    
#endif

static s8 gtp_i2c_test(struct i2c_client *client);
void gtp_reset_guitar(struct i2c_client *client, s32 ms);
void gtp_int_sync(s32 ms);

#ifdef CONFIG_HAS_EARLYSUSPEND
static void goodix_ts_early_suspend(struct early_suspend *h);
static void goodix_ts_late_resume(struct early_suspend *h);
#endif
 
#if GTP_CREATE_WR_NODE
extern s32 init_wr_node(struct i2c_client*);
extern void uninit_wr_node(void);
#endif

#if GTP_AUTO_UPDATE
extern u8 gup_init_update_proc(struct goodix_ts_data *);
#endif

#if GTP_ESD_PROTECT
static struct delayed_work gtp_esd_check_work;
static struct workqueue_struct * gtp_esd_check_workqueue = NULL;
static void gtp_esd_check_func(struct work_struct *);
static s32 gtp_init_ext_watchdog(struct i2c_client *client);
void gtp_esd_switch(struct i2c_client *, s32);
#endif
#define POWEROFFKEYFUN  251
#define MUTE_SOUND_FUN  250
#define nwd_dbg_tp()  printk(" ===%d %s ===\n",__LINE__,__FUNCTION__)

#if GTP_SLIDE_WAKEUP
typedef enum
{
    DOZE_DISABLED = 0,
    DOZE_ENABLED = 1,
    DOZE_WAKEUP = 2,
}DOZE_T;
static DOZE_T doze_status = DOZE_DISABLED;
static s8 gtp_enter_doze(struct goodix_ts_data *ts);
#endif

static u8 chip_gt9xxs = 0;  // true if ic is gt9xxs, like gt915s
u8 grp_cfg_version = 0;

extern unsigned char RunSystemTouchFlag;
extern unsigned char RunSystemTouchBuf[7];

static struct timer_list Timer_keyboard_6718;
unsigned char mKeyboardValue=0;
/*******************************************************
Function:
    Read data from the i2c slave device.
Input:
    client:     i2c device.
    buf[0~1]:   read start address.
    buf[2~len-1]:   read data buffer.
    len:    GTP_ADDR_LENGTH + read bytes count
Output:
    numbers of i2c_msgs to transfer: 
      2: succeed, otherwise: failed
*********************************************************/
s32 gtp_i2c_read(struct i2c_client *client, u8 *buf, s32 len)
{
    struct i2c_msg msgs[2];
    s32 ret=-1;
    s32 retries = 0;

    GTP_DEBUG_FUNC();

    msgs[0].flags = !I2C_M_RD|I2C_M_IGNORE_NAK;
    msgs[0].addr  = client->addr;
    msgs[0].len   = GTP_ADDR_LENGTH;
    msgs[0].buf   = &buf[0];            ///buf[0] buf[1] is reg addr
    msgs[0].scl_rate = 400 * 1000;    // for Rockchip
    msgs[0].udelay = 5;

    msgs[1].flags = I2C_M_RD|I2C_M_IGNORE_NAK;
    msgs[1].addr  = client->addr;
    msgs[1].len   = len - GTP_ADDR_LENGTH;
    msgs[1].buf   = &buf[GTP_ADDR_LENGTH];
    msgs[1].scl_rate = 400 * 1000;
    msgs[1].udelay = 5;

    while(retries < 5)
    {
        ret = i2c_transfer(client->adapter, msgs, 2);
        if(ret == 2)break;
        retries++;
    }
    if((retries >= 5))
    {
    #if GTP_SLIDE_WAKEUP
        // reset chip would quit doze mode
        if (DOZE_ENABLED == doze_status)
        {
            return ret;
        }
    #endif
        printk("I2C communication timeout, resetting chip...");
        gtp_reset_guitar(client, 10);
    }
    return ret;
}

/*******************************************************
Function:
    Write data to the i2c slave device.
Input:
    client:     i2c device.
    buf[0~1]:   write start address.
    buf[2~len-1]:   data buffer
    len:    GTP_ADDR_LENGTH + write bytes count
Output:
    numbers of i2c_msgs to transfer: 
        1: succeed, otherwise: failed
*********************************************************/
s32 gtp_i2c_write(struct i2c_client *client,u8 *buf,s32 len)
{
    struct i2c_msg msg;
    s32 ret = -1;
    s32 retries = 0;

    GTP_DEBUG_FUNC();

    msg.flags = !I2C_M_RD|I2C_M_IGNORE_NAK;
    msg.addr  = client->addr;
    msg.len   = len;
    msg.buf   = buf;
    msg.scl_rate = 400 * 1000;   // for Rockchip
    msg.udelay = 5;
    while(retries < 5)
    {
        ret = i2c_transfer(client->adapter, &msg, 1);
        if (ret == 1)break;
        retries++;
    }
    if((retries >= 5))
    {
    #if GTP_SLIDE_WAKEUP
        if (DOZE_ENABLED == doze_status)
        {
            return ret;
        }
    #endif
        GTP_DEBUG("I2C communication timeout, resetting chip...");
        gtp_reset_guitar(client, 10);
    }
    return ret;
}
/*******************************************************
Function:
    i2c read twice, compare the results
Input:
    client:  i2c device
    addr:    operate address
    rxbuf:   read data to store, if compare successful
    len:     bytes to read
Output:
    FAIL:    read failed
    SUCCESS: read successful
*********************************************************/
s32 gtp_i2c_read_dbl_check(struct i2c_client *client, u16 addr, u8 *rxbuf, int len)
{
    u8 buf[16] = {0};
    u8 confirm_buf[16] = {0};
    u8 retry = 0;
    
    while (retry++ < 3)
    {
        memset(buf, 0xAA, 16);
        buf[0] = (u8)(addr >> 8);
        buf[1] = (u8)(addr & 0xFF);
        gtp_i2c_read(client, buf, len + 2);
        
        memset(confirm_buf, 0xAB, 16);
        confirm_buf[0] = (u8)(addr >> 8);
        confirm_buf[1] = (u8)(addr & 0xFF);
        gtp_i2c_read(client, confirm_buf, len + 2);
        
        if (!memcmp(buf, confirm_buf, len+2))
        {
            break;
        }
    }    
    if (retry < 3)
    {
        memcpy(rxbuf, confirm_buf+2, len);
        return SUCCESS;
    }
    else
    {
        GTP_ERROR("i2c read 0x%04X, %d bytes, double check failed!", addr, len);
        return FAIL;
    }
}

/*******************************************************
Function:
    Send config.
Input:
    client: i2c device.
Output:
    result of i2c write operation. 
        1: succeed, otherwise: failed
*********************************************************/
s32 gtp_send_cfg(struct i2c_client *client)
{
    s32 ret = 2;
    
#if GTP_DRIVER_SEND_CFG
    s32 retry = 0;
    struct goodix_ts_data *ts = i2c_get_clientdata(client);
    
    if (ts->fixed_cfg)
    {
        GTP_INFO("Ic fixed config, no config sent!");
        return 2;
    }

    for (retry = 0; retry < 5; retry++)
    {
        ret = gtp_i2c_write(client, config , GTP_CONFIG_MAX_LENGTH + GTP_ADDR_LENGTH);
        if (ret > 0)
        {
            break;
        }
    }
#endif

    return ret;
}

/*******************************************************
Function:
    Disable irq function
Input:
    ts: goodix i2c_client private data
Output:
    None.
*********************************************************/
void gtp_irq_disable(struct goodix_ts_data *ts)
{
    unsigned long irqflags;

    GTP_DEBUG_FUNC();

    spin_lock_irqsave(&ts->irq_lock, irqflags);
    if (!ts->irq_is_disable)
    {
        ts->irq_is_disable = 1; 
        disable_irq_nosync(ts->client->irq);
    }
    spin_unlock_irqrestore(&ts->irq_lock, irqflags);
}

/*******************************************************
Function:
    Enable irq function
Input:
    ts: goodix i2c_client private data
Output:
    None.
*********************************************************/
void gtp_irq_enable(struct goodix_ts_data *ts)
{
    unsigned long irqflags = 0;

    GTP_DEBUG_FUNC();
    
    spin_lock_irqsave(&ts->irq_lock, irqflags);
    if (ts->irq_is_disable) 
    {
        enable_irq(ts->client->irq);
        ts->irq_is_disable = 0; 
    }
    spin_unlock_irqrestore(&ts->irq_lock, irqflags);
}

unsigned long  time_prev=0;
unsigned long  time_prev5=0;
int key_pressed = 9;
int key_released= 0;
int press_dou = 0;
int press_once = 0;
int tp_int_trigered;
int long_pressed = 1;
int short_pressed = 1;
int long_press_time;
static struct timer_list tpsl_timer;


unsigned long  time_prevhome=0;
unsigned long  time_prev5home=0;
///int key_pressed = 9;
int key_releasedhome= 0;
int press_douhome = 0;
int press_oncehome = 0;
int tp_int_trigeredhome;
int long_pressedhome = 1;
int short_pressedhome = 1;
int long_press_timehome;
static struct timer_list tpsl_timerhome;
static struct timer_list tpsl_timer_backmenu_afterup;
/*******************************************************
Function:
    Report touch point event 
Input:
    ts: goodix i2c_client private data
    id: trackId
    x:  input x coordinate
    y:  input y coordinate
    w:  input pressure
Output:
    None.
*********************************************************/
/***
void rk28_send_wakeup_key(void)
{
	if (!input_dev_g)
		return;

	input_report_key(input_dev_g, KEY_WAKEUP, 1);
	input_sync(input_dev_g);
	input_report_key(input_dev_g, KEY_WAKEUP, 0);
	input_sync(input_dev_g);
}*/
home_key_to1();
home_key_to0();
menu_key_to0();
menu_key_to1();
short_key_to1();
long_key_to1();

struct input_dev *input_dev_g;
static void gtp_touch_down(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
    printk("gtp_touch_down x=%u y=%u...\n",x,y);
    if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
	goto key_event;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id); 
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y-74);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:  
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(y>1 && y<52) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
											static timecnthome=1;
	
	/// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//	if( 900< x && x <940 ) ///back
	if( 890< x && x <950 ) ///back
	{   	if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
		 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
		///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
		key_pressed= 1;
	}
//	if( 776< x && x <820 ) ///menu
	if( 766< x && x <830 ) ///menu
	{	if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
		///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
		//menu_key_to1();
//		printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);
		key_pressed= 0;
	}
//	if( 330< x && x <365 )
	if( 320< x && x <375 )
	{	 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
	}
//	if( 667< x && x <705 )
	if( 657< x && x <715 )
	{	 
		
		/***
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;		
		printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
		///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
		home_key_to1();
		///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/
		
		
		
		
		
		
		
		short_pressedhome = 0; long_pressedhome = 0;
		if( press_douhome == 0) 
			press_oncehome++; 
		if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  
		{
			press_douhome = 1; 
			return ;
		}
		printk("press_once:%d.............\n",press_oncehome);
 		press_douhome = 0; // time_prev5 = jiffies;
//		if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
		if(press_oncehome>2 && key_releasedhome!= 1)
		{  
			printk("press_once home:%d.............\n",press_oncehome);
			mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
			timecnthome++; 
			key_pressed=5;
			time_prev5home = jiffies;
			key_releasedhome = 0; 
			return ;

		}
/*
		if(press_oncehome==40) 
		{	
			input_set_capability(input_dev_g, EV_KEY, 252);
			///home_key_to1();
			///input_report_key(input_dev_g, 252,1);input_sync(input_dev_g);
			input_report_key(ts->input_dev, 252,1); 
			///long_key_to1();
			printk("===========%d %s====\n",__LINE__,__FUNCTION__);
			input_sync(ts->input_dev);key_pressed  = 2;
			key_releasedhome = 0;
//			press_oncehome = 0;   //del by lusterzhang
			//input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
			//input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
		///	input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
		///home_key_to1();
			input_report_key(ts->input_dev, 252,1);  input_sync(input_dev_g);
			long_pressedhome = 1; long_press_timehome =  jiffies;


 ///	input_sync(ts->input_dev);
			timecnthome++;
			time_prev5home = jiffies;
			key_releasedhome = 0; 
			return ;


		}///long press

		///input_report_key(ts->input_dev, KEY_DISPLAYTOGGLE_NWD, 1); nwd_dbg_tp();
		///input_report_key(ts->input_dev, KEY_DISPLAYTOGGLE_NWD,0); 
		///input_report_key(ts->input_dev, KEY_POWER, 1);   ///short :250 long:251
		printk("press_once long :%d.............\n",press_oncehome);
		nwd_dbg_tp();
		
		///input_report_key(ts->input_dev, KEY_DISPLAYTOGGLE_NWD,1); 
			key_pressed = 2;

	/// input_sync(ts->input_dev);
		timecnthome++;
		time_prevhome = jiffies;
		time_prev5home = jiffies;
		key_releasedhome = 0;
  		tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
		mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
          	return ;
*/		
		
		
		
	}
//	if( 215< x && x <245 )
	if( 205< x && x <255 )
	{	if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
//	if( 85< x && x <135 )
	if( 75< x && x <145 )
	{	short_pressed = 0; long_pressed = 0;
		if( press_dou == 0) press_once++; 
		///	if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
		printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
 		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
			{  
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;
		
 ///input_sync(ts->input_dev);
	timecnt++; key_pressed=5;
	time_prev5 = jiffies;
	key_released = 0; return ;

}///short press
		///if(press_once==50&&key_released!= 1) 
		if(press_once==40) 
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1); 
			//input_report_key(input_dev_g, 250,1);
			input_sync(input_dev_g);
			///long_key_to1();
					printk("===========%d %s====\n",__LINE__,__FUNCTION__);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			//input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
			//input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
			long_pressed = 1; long_press_time =  jiffies;


 ///	input_sync(ts->input_dev);
			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;


		}///long press

		///input_report_key(ts->input_dev, KEY_DISPLAYTOGGLE_NWD, 1); nwd_dbg_tp();
		///input_report_key(ts->input_dev, KEY_DISPLAYTOGGLE_NWD,0); 
		///input_report_key(ts->input_dev, KEY_POWER, 1);   ///short :250 long:251
		
		nwd_dbg_tp();
		
		///input_report_key(ts->input_dev, KEY_DISPLAYTOGGLE_NWD,1); 
			key_pressed= 5;

	/// input_sync(ts->input_dev);
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
  		tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
          	return ;


	}
	/// input_sync(ts->input_dev);
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
	///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
	///key_pressed = 1;
    } 
     ///printk("ID:%d, X:%d, Y:%d, W:%d\n", id, x, y, w);
   /// printk("==================ID:%d, X:%d, Y:%d, W:%d\n", id, x, y-74, w);
	///printk("===nwd ID:%d, X:%d, Y:%d, W:%d\n", id, 800- x*800/864, y, w);
}
static void gtp_touch_down_528(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
 	
//    printk("gtp_touch_down x=%u y=%u...\n",x,y);
 	if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y-74);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
    if(y>1 && y<52) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;

        if( 890< x && x <950 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
                key_pressed= 1;
        }
        if( 766< x && x <830 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) 
			return ;
		key_pressed= 0;
        }
        if( 320< x && x <375 )
        {       
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
        }
        if( 657< x && x <715 )
        {
                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                press_douhome = 0; // time_prev5 = jiffies;
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
        if( 205< x && x <255 )
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
        if( 75< x && x <145 )
        {       short_pressed = 0; 
		long_pressed = 0;
                if( press_dou == 0) 
			press_once++;
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  
		{
			press_dou = 1; 
			return ;
		}
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                {
                	mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));

        		timecnt++; 
			key_pressed=5;
        		time_prev5 = jiffies;
        		key_released = 0; 
			return ;

		}
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        input_sync(input_dev_g);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        long_pressed = 1; long_press_time =  jiffies;


                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                nwd_dbg_tp();

                key_pressed= 5;

                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  	tp_int_trigeredhome = 1;
    }
}


static void gtp_touch_down_520(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
    if(x>1024) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,1024 - x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
	
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>1024) ///GTP_HAVE_TOUCH_KEY
    {  
        int key_value = 1; 
	static timecnt=1;
        static timecnthome=1;

        if( 300< y && y <360 ) ///menu
        {       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) 
			return ;
		key_pressed= 0;
        }
        if( 560< y && y <620 )
        {
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
        }

        if( 180< y && y <240 ) ///
        {

                short_pressedhome = 0; 
		long_pressedhome = 0;
                if( press_douhome == 0) 
			press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  
		{
			press_douhome = 1; 
			return ;
		}
                press_douhome = 0; 
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
        if( 420< y && y <480 )
        {
	        if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); 
		key_pressed= 4;
        }
        if( 50< y && y <110 )
        {       short_pressed = 0; 
		long_pressed = 0;
                if( press_dou == 0) 
		press_once++;
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  
		{
			press_dou = 1; 
			return ;
		}
                press_dou = 0; 
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                {
                	mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        		timecnt++; key_pressed=5;
        		time_prev5 = jiffies;
        		key_released = 0; 
			return ;

		}
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        input_sync(input_dev_g);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
                        long_pressed = 1; long_press_time =  jiffies;
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; 
			return ;

                }///long press

                        key_pressed= 5;

                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  	tp_int_trigeredhome = 1;
    }
}
static void gtp_touch_down_520W(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
    if(x>1024) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,1024 - x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>1024) ///GTP_HAVE_TOUCH_KEY
    {  
        int key_value = 1; 
	static timecnt=1;
        static timecnthome=1;

        if( 300< y && y <360 ) ///menu
        {       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) 
			return ;
		key_pressed= 0;
        }
        if( 560< y && y <620 )
        {
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
        }

        if( 180< y && y <240 ) ///
        {

                short_pressedhome = 0; 
		long_pressedhome = 0;
                if( press_douhome == 0) 
			press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  
		{
			press_douhome = 1; 
			return ;
		}
                press_douhome = 0; 
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
        if( 420< y && y <480 )
        {
	        if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); 
		key_pressed= 4;
        }
        if( 50< y && y <110 )
        {       short_pressed = 0; 
		long_pressed = 0;
                if( press_dou == 0) 
		press_once++;
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  
		{
			press_dou = 1; 
			return ;
		}
                press_dou = 0; 
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                {
                	mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        		timecnt++; key_pressed=5;
        		time_prev5 = jiffies;
        		key_released = 0; 
			return ;

		}
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        input_sync(input_dev_g);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
                        long_pressed = 1; long_press_time =  jiffies;
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; 
			return ;

                }///long press

                        key_pressed= 5;

                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  	tp_int_trigeredhome = 1;
    }
}


static void gtp_touch_down_568(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
//    printk("568 gtp_touch_down x=%u y=%u...\n",x,y);
    if(x>1 && x<60) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>60 && x<75)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-75);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>1 && x<55) ///GTP_HAVE_TOUCH_KEY
    {  
        int key_value = 1; 
	static timecnt=1;
        static timecnthome=1;

        if( 279< y && y <339 ) ///back
        {
	       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
               key_pressed= 1;
        }

        if( 346< y && y < 406) //display 
        {
               if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
               key_pressed= 11;
        }
        if( 136< y && y <196 )
        {       
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
        }
      if( 208< y && y <268 )
//       if( 208< y && y <258 )
        {

                short_pressedhome = 0; 
		long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                press_douhome = 0; 
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }

        }
        if( 68< y && y <128 )
        {
	       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); 
		key_pressed= 4;
        }
        if( 1< y && y <52 )
        { 
	      	short_pressed = 0; 
		long_pressed = 0;
                if( press_dou == 0) press_once++;
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  
		{
			press_dou = 1; 
			return ;
		}
                press_dou = 0; 
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                {
                	mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));

        		timecnt++; 
			key_pressed=5;
        		time_prev5 = jiffies;
        		key_released = 0; 
			return ;

		}///short press
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        input_sync(input_dev_g);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        long_pressed = 1; 
			long_press_time =  jiffies;

                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; 
			return ;


                }///long press
                key_pressed= 5;
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  	tp_int_trigeredhome = 1;
    }
}
static void gtp_touch_down_568Y(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
//    printk("568 gtp_touch_down x=%u y=%u...\n",x,y);
    if(x>1 && x<60) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>60 && x<70)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-70);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>1 && x<55) ///GTP_HAVE_TOUCH_KEY
    {  
        int key_value = 1; 
	static timecnt=1;
        static timecnthome=1;

        if( 279< y && y <339 ) ///back
        {
	       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
               key_pressed= 1;
        }

        if( 346< y && y < 406) //display 
        {
               if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
               key_pressed= 11;
        }
        if( 136< y && y <196 )
        {       
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
        }
      if( 208< y && y <268 )
//       if( 208< y && y <258 )
        {

                short_pressedhome = 0; 
		long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                press_douhome = 0; 
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }

        }
        if( 68< y && y <128 )
        {
	       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); 
		key_pressed= 4;
        }
        if( 1< y && y <52 )
        { 
	      	short_pressed = 0; 
		long_pressed = 0;
                if( press_dou == 0) press_once++;
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  
		{
			press_dou = 1; 
			return ;
		}
                press_dou = 0; 
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                {
                	mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));

        		timecnt++; 
			key_pressed=5;
        		time_prev5 = jiffies;
        		key_released = 0; 
			return ;

		}///short press
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        input_sync(input_dev_g);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        long_pressed = 1; 
			long_press_time =  jiffies;

                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; 
			return ;


                }///long press
                key_pressed= 5;
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  	tp_int_trigeredhome = 1;
    }
}
static void gtp_touch_down_584(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
//    printk("gtp_touch_down x=%u y=%u...\n",x,y);
    if(x>1 && x<60) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>60 && x<76)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-76);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif
key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>1 && x<60) ///GTP_HAVE_TOUCH_KEY
    {  
        int key_value = 1; 
	static timecnt=1;
        static timecnthome=1;

        if( 198< y && y <278 ) ///back
        {       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
                key_pressed= 1;
        }
        if( 766< x && x <830 ) ///menu
        {       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) 
			return ;
		key_pressed= 0;
        }
        if( 414< y && y <494 )
        { 
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
        }
        if( 90< y && y <170 )
        {

                short_pressedhome = 0; 
		long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                press_douhome = 0; // time_prev5 = jiffies;
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
        if( 305< y && y <380 )
        {       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); 
		key_pressed= 4;
        }
        if( 1< y && y <75 )
        {       
		short_pressed = 0; 
		long_pressed = 0;
                if( press_dou == 0) 
			press_once++;
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  
		{
			press_dou = 1; 
			return ;
		}
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                {
                	mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        		timecnt++; 
			key_pressed=5;
        		time_prev5 = jiffies;
        		key_released = 0; 
			return ;

		}///short press
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        input_sync(input_dev_g);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        long_pressed = 1; long_press_time =  jiffies;
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; 
			return ;


                }///long press


                nwd_dbg_tp();

                        key_pressed= 5;

                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  	tp_int_trigeredhome = 1;
    }
}
static void gtp_touch_down_585(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
/*
    s32 temp;
    temp = x;
    x = y;
    y = temp;
*/
//    printk("gtp_touch_down x=%u y=%u...\n",x,y);
    if(x>1 && x<55) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>55 && x<60)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-60);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>1 && x<55) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;

        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
        if( 180< y && y <270 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
//      if( 776< x && x <820 ) ///menu
        if( 766< x && x <830 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }
//      if( 330< x && x <365 )
        if( 400< y && y <470 )
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
//      if( 667< x && x <705 )
        if( 85< y && y <160 )
        {
                short_pressedhome = 0; long_pressedhome = 0;
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_oncehome);
                if( press_douhome == 0)
                    press_oncehome++;
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_oncehome);
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_douhome = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
                if(press_oncehome>2&&press_oncehome < 39 &&key_releasedhome!= 1)
                        {
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0; return ;

                }

        }
//      if( 215< x && x <245 )
        if( 305< y && y <380 )
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 4< y && y <75 )
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                ///input_report_key(ts->input_dev, KEY_DISPLAYTOGGLE_NWD,1); 
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_586(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
/*
    s32 temp;
    temp = x;
    x = y;
    y = temp;
*/
//    printk("gtp_touch_down x=%u y=%u...\n",x,y);
    if(x>1 && x<50) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>50 && x<60)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-60);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>1 && x<50) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;

        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
        if( 92< y && y <128 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
//      if( 776< x && x <820 ) ///menu
        if( 766< x && x <830 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }
//      if( 330< x && x <365 )
        if( 180< y && y <216 )
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
//      if( 667< x && x <705 )
        if( 48< y && y <84 )
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/


                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 136< y && y <172 )
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 4< y && y <40 )
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}
static void gtp_touch_down_589(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
	    GTP_SWAP(x, y);
#endif
	x = 1100-x;
//	printk("589   gtp_touch_down x=%u y=%u...\n",x,y);
	if(x>1 && x<70) ///GTP_HAVE_TOUCH_KEY
		goto key_event;
	else if(x>60 && x<76)
		return;

#if GTP_ICS_SLOT_REPORT
	input_mt_slot(ts->input_dev, id);
	input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-76);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
	input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
	input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>1 && x<76) ///GTP_HAVE_TOUCH_KEY
	{
		///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
		int key_value = 1; static timecnt=1; static timecnthome=1;
		if( 377< y && y <427 ) ///back
		{
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;
			printk("===========%d %s====\n",__LINE__,__FUNCTION__);
			key_pressed= 1;
		}

		if( 206< y && y <276 )
		{
			printk("===========%d %s====\n",__LINE__,__FUNCTION__);
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
			input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
		}

		if( 287< y && y <357)
		{
			short_pressedhome = 0; long_pressedhome = 0;
			if( press_douhome == 0)
				press_oncehome++;
			if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
			{
				press_douhome = 1;
				return ;
			}
			printk("press_once:%d.............\n",press_oncehome);
			press_douhome = 0; // time_prev5 = jiffies;
			if(press_oncehome>2 && key_releasedhome!= 1)
			{
				printk("press_once home:%d.............\n",press_oncehome);
				mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
				timecnthome++;
				key_pressed=5;
				time_prev5home = jiffies;
				key_releasedhome = 0;
				return ;
			}
		}

		if( 126< y && y <196)
		{
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
			input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
		}

		if( 51< y && y <120 )
		{
			short_pressed = 0; long_pressed = 0;
			if( press_dou == 0) press_once++;
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
			printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
			press_dou = 0; // time_prev5 = jiffies;
			if(press_once>2&&press_once < 39 &&key_released!= 1)
			{
				mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
				timecnt++; key_pressed=5;
				time_prev5 = jiffies;
				key_released = 0; return ;
			}///short press
			if(press_once==40)
			{
				input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
				input_sync(input_dev_g);
				printk("===========%d %s====\n",__LINE__,__FUNCTION__);
				input_sync(ts->input_dev);key_pressed = 5;
				key_released = 0;
				press_once = 0;
				input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
				printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
				long_pressed = 1; long_press_time =  jiffies;
				timecnt++;
				time_prev5 = jiffies;
				key_released = 0; return ;
			}///long press
			key_pressed= 5;
			timecnt++;
			time_prev = jiffies;
			time_prev5 = jiffies;
			key_released = 0;
			tp_int_trigeredhome = 1;
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			return ;
		}

		timecnt++;time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
	}
}

static void gtp_touch_down_589W(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
/*
    s32 temp;
    temp = x;
    x = y;
    y = temp;
*/
//    printk("589   gtp_touch_down x=%u y=%u...\n",x,y);
    if(x>1 && x<60) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>60 && x<70)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-70);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>1 && x<55) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;

        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
        if( 377< y && y <427 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
        if( 206< y && y <276 )
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
//      if( 667< x && x <705 )
        if( 287< y && y <357)
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/




                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 126< y && y <196)
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 51< y && y <120 )
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_591(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
/*
    s32 temp;
    temp = x;
    x = y;
    y = temp;
*/
//    printk("591 gtp_touch_down x=%u y=%u...\n",x,y);
    if(x>1 && x<60) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if((x>60 && x<70) || (y<25))
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-70);
     y=y-25;
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>1 && x<60) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;

        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
        if( 285< y && y <355 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
//      if( 776< x && x <820 ) ///menu
        if( 766< x && x <830 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }
//      if( 330< x && x <365 )
        if( 131< y && y <201 )
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
//      if( 667< x && x <705 )
        if( 211< y && y <281 )
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/




                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }

        }
//      if( 215< x && x <245 )
        if( 64< y && y <134 )
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 1< y && y <52 )
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_592(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
/*
    s32 temp;
    temp = x;
    x = y;
    y = temp;
*/
//    printk("gtp_touch_down x=%u y=%u...\n",x,y);
    if(x>1 && x<90) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>90 && x<100)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-100);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
//    if(x>1 && x<90) ///GTP_HAVE_TOUCH_KEY	//modify by Jiawq
    if(x>1 && x<70) ///GTP_HAVE_TOUCH_KEY  modify by Jiawq 
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
/*      
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
        if( 285< y && y <355 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
*/
//      if( 776< x && x <820 ) ///menu
        if( 432< y && y <512 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

//      if( 330< x && x <365 )
        if( 225< y && y <305 )
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
//      if( 667< x && x <705 )
        if( 336< y && y <416 )
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/




                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 125< y && y <205 )
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 1< y && y <90 )
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
    }
}
static void gtp_touch_down_594(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
/*
    s32 temp;
    temp = x;
    x = y;
    y = temp;
*/
//    printk("594 gtp_touch_down x=%u y=%u...\n",x,y);
    if((x>1 && x<110)||(x>1200 && x<1275)) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if((x>110 && x<128) || (x>1152))
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-128);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if((x>1 && x<110)||(x>1200 && x<1275))
    {
 ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
/*      
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
        if( 285< y && y <355 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
*/
        if(( 118< y && y <172 ) && (x>1200 && x<1275)) //sound
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 10;
        }
//      if( 776< x && x <820 ) ///menu
        if(( 58< y && y <110 ) && (x>1200 && x<1275))
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }
//      if( 330< x && x <365 )
        if(( 118< y && y <171 ) && (x>1 && x<110))
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
//      if( 667< x && x <705 )
        if(( 1< y && y <50 ) && (x>1200 && x<1275))
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/




                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }

        }
//      if( 215< x && x <245 )
        if(( 64< y && y <110 )&& (x>1 && x<110))
        {
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 )
                        return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if(( 1< y && y <52 ) && (x>1 && x<110))
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        	timecnt++; key_pressed=5;
        	time_prev5 = jiffies;
        	key_released = 0; 
		return ;

	}///short press
               if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press

                ///input_report_key(ts->input_dev, KEY_DISPLAYTOGGLE_NWD, 1); nwd_dbg_tp();
                ///input_report_key(ts->input_dev, KEY_DISPLAYTOGGLE_NWD,0); 
                ///input_report_key(ts->input_dev, KEY_POWER, 1);   ///short :250 long:251

                nwd_dbg_tp();

                ///input_report_key(ts->input_dev, KEY_DISPLAYTOGGLE_NWD,1); 
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
        tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_596(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
/*
    s32 temp;
    temp = x;
    x = y;
    y = temp;
*/
//    printk("596 gtp_touch_down x=%u y=%u...\n",x,y);	//20150410

    if(x>1 && x<60) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>60 && x<70)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-70);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
//    if(x>1 && x<55) ///GTP_HAVE_TOUCH_KEY
    if(x>1 && x<33) ///GTP_HAVE_TOUCH_KEY     Modify 20150327 by Jiwq
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
/*      
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
        if( 400< y && y <471 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
*/
//      if( 776< x && x <820 ) ///menu
        if( 496< y && y <576 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

//      if( 330< x && x <365 )
        if( 265< y && y <345 )
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
//      if( 667< x && x <705 )
        if( 383< y && y <463)
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/




                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 153< y && y <233 )
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
//        if( 28< y && y <118 )
        if( 60< y && y <118 )	//modify 20150327 by Jiawq
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_596Z(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
/*
    s32 temp;
    temp = x;
    x = y;
    y = temp;
*/
//    printk("596Z gtp_touch_down x=%u y=%u...\n",x,y);

    if(x>1 && x<36) ///GTP_HAVE_TOUCH_KEY
        goto key_event;

    else if(x<39)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-40);	//modify by Jiawq
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>1 && x<36) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
/*      
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
        if( 400< y && y <471 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
*/
//      if( 776< x && x <820 ) ///menu
        if( 496< y && y <576 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

//      if( 330< x && x <365 )
        if( 265< y && y <345 )
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
//      if( 667< x && x <705 )
        if( 383< y && y <463)
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/




                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 153< y && y <233 )
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 28< y && y <118 )
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}


static void gtp_touch_down_8198D(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
/*
    s32 temp;
    temp = x;
    x = y;
    y = temp;
*/
//    printk("gtp_touch_down8198D x=%u y=%u...\n",x,y);

    if(x>1 && x<50) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<54)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-54);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>1 && x<50) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
/*      
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
        if( 400< y && y <471 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
*/
//      if( 776< x && x <820 ) ///menu
        if( 275< y && y <345 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

//      if( 330< x && x <365 )
        if( 515< y && y <585 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
//      if( 667< x && x <705 )
        if( 165< y && y <235) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/




                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 395< y && y <465 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 50< y && y <120 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}


static void gtp_touch_down_709(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
/*
    s32 temp;
    temp = x;
    x = y;
    iy = temp;
*/
	x = 1081 - x;
//    printk("gtp_touch_down709 x=%u y=%u...\n",x,y);

    if(x>1 && x<50) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<57)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-57);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<50) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
        if( 150< y && y <180 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
/*
//      if( 776< x && x <820 ) ///menu
        if( 90< y && y <110 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }
*/
//      if( 330< x && x <365 )
        if( 275< y && y <295 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
//      if( 667< x && x <705 )
        if( 90< y && y <120) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/




                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 220< y && y <240 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 35< y && y <55 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_721(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif

//    printk("gtp_touch_down721 x=%u y=%u...\n",x,y);	

    if((x>1 && x<70)||(x>1120 && x<1184)) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if((x<70) || (x>1105))
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-77);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if((x>1 && x<55)||(x>1120 && x<1184)) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);

        if(( 145< y && y <185) && (x>1120 && x<1184)) //menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

        if(( 215< y && y <250 ) && (x>1 && x<55))//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }

        if(( 65< y && y <110) && (x>1120 && x<1184)) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }

        if(( 145< y && y <180 ) && (x>1 && x<55))//+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }

        if(( 215< y && y <255 ) && (x>1120 && x<1184)) //sound
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 10;
        }

        if(( 65< y && y <115 ) && (x>1 && x<55)) //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 40 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_de:v, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;


        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_720(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
/*
    s32 temp;
    temp = x;
    x = y;
    y = temp;
	x = 4096 - x;
	x = x/4;
	y = y/6;
*/
//    printk("gtp_touch_down720 x=%u y=%u...\n",x,y);

    if(x>1 && x<55) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<62)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-62);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<55) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
//       if( 150< y && y <180 ) ///back
//        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
//                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
//                key_pressed= 1;
//        }

//      if( 776< x && x <820 ) ///menu
        if( 190< y && y <250 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

//      if( 330< x && x <365 )
        if( 342< y && y <392 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
//      if( 667< x && x <705 )
        if( 112< y && y <172) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/




                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 265< y && y <315 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 51< y && y <111 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_708(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif 
//	y = 600 - y;

//    printk("gtp_touch_down708 x=%u y=%u...\n",x,y);
/*
    if(x>1 && x<50) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<56)
        return;
*/

#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x<0) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
      if( 240< y && y <300 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
/*
        if( 240< y && y <300 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }
*/
        if( 435< y && y <505 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 135< y && y <195) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 340< y && y <400 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 30< y && y <90 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_701(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
	x = 1080 - x;

//    printk("gtp_touch_down701 x=%u y=%u...\n",x,y);

    if(x>1 && x<50) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<56)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-56);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<50) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
      if( 240< y && y <300 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
/*
        if( 240< y && y <300 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }
*/
        if( 435< y && y <505 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 135< y && y <195) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 340< y && y <400 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 30< y && y <90 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_702(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
	x = 1080 - x;

//    printk("gtp_touch_down702 x=%u y=%u...\n",x,y);

    if(x>1 && x<50) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<56)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-56);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<50) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
      if( 230< y && y <280 ) ///back
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
                key_pressed= 1;
        }
/*
        if( 240< y && y <300 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }
*/
        if( 435< y && y <495 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 120< y && y <180) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 330< y && y <390 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 20< y && y <80 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_722(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
	x  = 1086 -x;
//    printk("gtp_touch_down722 x=%u y=%u...\n",x,y);

    if(x>1 && x<50) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<62)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-62);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<50) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
//      if( 230< y && y <280 ) ///back
 //       {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
  //               printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
 //               key_pressed= 1;
 //       }
        if( 175< y && y <225 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

        if( 325< y && y <375 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 90< y && y <140) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 260< y && y <310 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 0< y && y <45 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_521(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x  = 1094 -x;
//    printk("gtp_touch_down521 x=%u y=%u...\n",x,y);

    if(x>1 && x<50) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<57)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-57);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<50) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
//      if( 230< y && y <280 ) ///back
 //       {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
  //               printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
 //               key_pressed= 1;
 //       }
        if( 240< y && y <280 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

        if( 420< y && y <460 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 142< y && y <182) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 334< y && y <374 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 49< y && y <89 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_700(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x  = 1086 -x;
//	printk("gtp_touch_down700 x=%u y=%u...\n",x,y);

    if(y>680 && y<700) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(y>600)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	  if(y>680 && y<700) ///GTP_HAVE_TOUCH_KEY
//    if(x>0 && x<50) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
//      if( 230< y && y <280 ) ///back
 //       {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
  //               printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
 //               key_pressed= 1;
 //       }
        if( 395< x && x <435 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

        if( 220< x && x <260 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 307< x && x <347) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 140< x && x <180 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 51< x && x <91 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_8198Z(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x  = 1086 -x;
//    printk("gtp_touch_down8198Z x=%u y=%u...\n",x,y);

    if(0<=0 && x<60) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<64)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-64);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(0<=x && x<40) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
//      if( 230< y && y <280 ) ///back
 //       {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
  //               printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
 //               key_pressed= 1;
 //       }
        if( 295< y && y <355 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

        if( 515< y && y <575 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 170< y && y <230) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 405< y && y <465 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 50< y && y <125 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}


static void gtp_touch_down_723(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
	x = 1100-x;
//    printk("gtp_touch_down723 x=%u y=%u...\n",x,y);

    if(x>0 && x<70) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<76)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-76);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<76) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
//      if( 230< y && y <280 ) ///back
 //       {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
  //               printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
 //               key_pressed= 1;
 //       }
        if( 410< y && y <450 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

        if( 245< y && y <285 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 330< y && y <370) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 165< y && y <205 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 80< y && y <125 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_723W(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//    printk("gtp_touch_down723 x=%u y=%u...\n",x,y);

    if(x>0 && x<60) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<64)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-64);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<45) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
//      if( 230< y && y <280 ) ///back
 //       {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
  //               printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
 //               key_pressed= 1;
 //       }
        if( 410< y && y <450 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

        if( 245< y && y <285 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 330< y && y <370) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 165< y && y <205 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 80< y && y <125 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_703(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
	x = 1073 -x;
//	printk("gtp_touch_down703 x=%u y=%u...\n",x,y);

    if(x>0 && x<40) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<49)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-49);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<40) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
      if( 210< y && y <250 ) ///back
      {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
               key_pressed= 1;
       }
//        if( 410< y && y <450 ) ///menu
//        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
//                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
//        }

        if( 365< y && y <405 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 135< y && y <175) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 285< y && y <325 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 50< y && y <70 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_704(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
	x = 1068 -x;
//    printk("gtp_touch_down704 x=%u y=%u...\n",x,y);

    if(x>0 && x<40) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<44)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-44);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<40) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
      if( 295< y && y <335 ) ///back
      {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
               key_pressed= 1;
       }
//        if( 410< y && y <450 ) ///menu
//        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
//                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
//        }

        if( 515< y && y <555 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 174< y && y <214) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 400< y && y <445 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 58< y && y <98 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_529(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down529 x=%u y=%u...\n",x,y);

    if(x>0 && x<50) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<62)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-70);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<50) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
//      if( 210< y && y <250 ) ///back
//      {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
//                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
//               key_pressed= 1;
//       }
        if( 164< y && y <204 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

        if( 302< y && y <342 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 103< y && y <143) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 232< y && y <272 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 30< y && y <70 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_735(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
	    GTP_SWAP(x, y);
#endif
	x = 1100-x;
	//printk("gtp_touch_down735 x=%u y=%u...\n",x,y);
	if(x>0 && x<70) ///GTP_HAVE_TOUCH_KEY
		goto key_event;
	else if(x<76)
		return;

#if GTP_ICS_SLOT_REPORT
	input_mt_slot(ts->input_dev, id);
	input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-76);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
	    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
		    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
			    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
				    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
					    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>0 && x<70) ///GTP_HAVE_TOUCH_KEY
	{
		///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
		int key_value = 1; static timecnt=1; static timecnthome=1;
	
		if( 255< y && y <305 ) ///back
		{
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;
			printk("===========%d %s====\n",__LINE__,__FUNCTION__);
			 key_pressed= 1;
		}

		if( 460< y && y <510 )//-
		{
			printk("===========%d %s====\n",__LINE__,__FUNCTION__);
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
			input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
		}

		if( 160< y && y <210) //home
		{
			short_pressedhome = 0; long_pressedhome = 0;
			if( press_douhome == 0)
				press_oncehome++;
			if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
			{
				press_douhome = 1;
				return ;
			}
			printk("press_once:%d.............\n",press_oncehome);
			press_douhome = 0; // time_prev5 = jiffies;
			if(press_oncehome>1 && key_releasedhome!= 1)
			{
				printk("press_once home:%d.............\n",press_oncehome);
				mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
				timecnthome++;
				key_pressed=5;
				time_prev5home = jiffies;
				key_releasedhome = 0;
				return ;
			}
		}

		if( 360< y && y <410 ) //+
		{
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
			input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
		}

		if( 60< y && y <110 )  //power
		{
			short_pressed = 0; long_pressed = 0;
			if( press_dou == 0) press_once++;
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
			printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
			press_dou = 0; // time_prev5 = jiffies;
			if(press_once>2&&press_once < 39 &&key_released!= 1)
			{
				mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
				timecnt++; key_pressed=5;
				time_prev5 = jiffies;
				key_released = 0; return ;
			}
			if(press_once==40)
			{
				input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
				input_sync(input_dev_g);
				printk("===========%d %s====\n",__LINE__,__FUNCTION__);
				input_sync(ts->input_dev);key_pressed = 5;
				key_released = 0;
				press_once = 0;
				input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
				printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
				long_pressed = 1; long_press_time =  jiffies;
				timecnt++;
				time_prev5 = jiffies;
				key_released = 0; return ;
			}
			key_pressed= 5;
			timecnt++;
			time_prev = jiffies;
			time_prev5 = jiffies;
			key_released = 0;
			tp_int_trigeredhome = 1;
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			return ;
		}

		timecnt++;time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
	}
}

static void gtp_touch_down_735W(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down735W x=%u y=%u...\n",x,y);

    if(x>0 && x<48) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<64)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-68);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<48) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
      if( 260< y && y <300 ) ///back
      {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
               key_pressed= 1;
       }
//        if( 410< y && y <450 ) ///menu
//        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
//                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
//        }

        if( 458< y && y <498 )//-
        {
	//        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 170< y && y <210) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 362< y && y <402 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 70< y && y <110 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_724(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down724 x=%u y=%u...\n",x,y);

    if(x>0 && x<70) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<74)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-74);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x>0 && x<70) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
      /*if( 260< y && y <300 ) ///back
      {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
               key_pressed= 1;
       }*/
//        if( 410< y && y <450 ) ///menu
//        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
//                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
//        }

        if( 320< y && y <380 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 70< y && y <130) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }

		if( 158< y && y <218 ) //menu
		{
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
			input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
			menu_key_to1();
			printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
		}

//      if( 215< x && x <245 )
        if( 236< y && y <296 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 0< y && y <40 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_738(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
    //printk("gtp_touch_down738 x=%u y=%u...\n",x,y);

    if(x>0 && x<100) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    //else if(x<106)
    //    return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-100);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
    if(x>0 && x<100) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
      /*if( 260< y && y <300 ) ///back
      {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
               key_pressed= 1;
       }*/
        if( 490< y && y <590 ) ///menu
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                menu_key_to1();
                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
        }

        if( 250< y && y <350 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 370< y && y <470) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
        if( 130< y && y <230 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
        if( 10< y && y <110 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_765(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down765 x=%u y=%u...\n",x,y);

    if(x<0) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<0)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x<0) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
      if( 260< y && y <300 ) ///back
      {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
               key_pressed= 1;
       }
//        if( 410< y && y <450 ) ///menu
//        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
//                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
//        }

        if( 458< y && y <498 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 170< y && y <210) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 362< y && y <402 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 70< y && y <110 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_764(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down764 x=%u y=%u...\n",x,y);

    if(x<0) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x<0)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//  if(y>1 && y<45) ///GTP_HAVE_TOUCH_KEY
    if(x<0) ///GTP_HAVE_TOUCH_KEY
    {  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
        int key_value = 1; static timecnt=1;
                                                                                        static timecnthome=1;
  
        /// printk("===========%d %s====\n",__LINE__,__FUNCTION__);
//      if( 900< x && x <940 ) ///back
      if( 260< y && y <300 ) ///back
      {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;     
                 printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                ///input_report_key(ts->input_dev, KEY_BACK, 1); nwd_dbg_tp();
               key_pressed= 1;
       }
//        if( 410< y && y <450 ) ///menu
//        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) return ;
                ///input_report_key(ts->input_dev,KEY_SELF_MENU ,1);  ///KEY_F1
                //menu_key_to1();
//                printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 0;
//        }

        if( 458< y && y <498 )//-
        {        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); nwd_dbg_tp();key_pressed= 3;
        }
        if( 170< y && y <210) //home
        {

                /***
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) return ;            
                printk("===========%d %s====\n",__LINE__,__FUNCTION__);key_pressed= 2;
                ///input_report_key(ts->input_dev, KEY_HOME, 1); ///nwd_dbg_tp();
                home_key_to1();
                ///input_report_key(ts->input_dev, KEY_HOME, 1); nwd_dbg_tp();*/

                short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>1 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;

                }
        }
//      if( 215< x && x <245 )
        if( 362< y && y <402 ) //+
        {       if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
                input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
        }
//      if( 85< x && x <135 )
        if( 70< y && y <110 )  //power
        {       short_pressed = 0; long_pressed = 0;
                if( press_dou == 0) press_once++;
                ///     if (long_pressed == 1 && timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 300 )  { return ;}
                if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  {press_dou = 1; return ;}
                printk("===========%d %s  bb press_once:%d====\n",__LINE__,__FUNCTION__,press_once);
                press_dou = 0; // time_prev5 = jiffies;
                if(press_once>2&&press_once < 39 &&key_released!= 1)
                        {
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
////input_report_key(ts->input_dev, 250,1); key_released = 0;press_once = 0;

 ///input_sync(ts->input_dev);
        timecnt++; key_pressed=5;
        time_prev5 = jiffies;
        key_released = 0; return ;

}///short press
                ///if(press_once==50&&key_released!= 1) 
                if(press_once==40)
                {
                        input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
                        //input_report_key(input_dev_g, 250,1);
                        input_sync(input_dev_g);
                        ///long_key_to1();
                                        printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                        input_sync(ts->input_dev);key_pressed = 5;
                        key_released = 0;
                        press_once = 0;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g); long_pressed = 1;
                        //input_report_key(input_dev_g, 250,0);input_sync(input_dev_g);
                        input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
                        printk("====1222331111111lllllllllllllllllllllllllllllllllllllll=======%d %s====\n",__LINE__,__FUNCTION__);
                        long_pressed = 1; long_press_time =  jiffies;


 ///    input_sync(ts->input_dev);
                        timecnt++;
                        time_prev5 = jiffies;
                        key_released = 0; return ;


                }///long press
                        key_pressed= 5;

        /// input_sync(ts->input_dev);
                timecnt++;
                time_prev = jiffies;
                time_prev5 = jiffies;
                key_released = 0;
                tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
                mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
                return ;
        }
        /// input_sync(ts->input_dev);
        timecnt++;time_prev = jiffies;
        time_prev5 = jiffies;
        key_released = 0;
  tp_int_trigeredhome = 1;
///setup_timer( &led2_timer, tpsl_timer_callback, 0 );
        ///mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
        ///key_pressed = 1;
    }
}

static void gtp_touch_down_737(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
	GTP_SWAP(x, y);
#endif
//	printk("gtp_touch_down737 x=%u y=%u...\n",x,y);
	if(x>0 && x<70) ///GTP_HAVE_TOUCH_KEY
		goto key_event;
	if(x<80)   return;
	if(x>1104) x=1104;
	
#if GTP_ICS_SLOT_REPORT
	input_mt_slot(ts->input_dev, id);
	input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-80);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
	input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
	input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>0 && x<40) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

	if( 210< y && y <290 ) ///back
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
	/*if( 766< x && x <830 ) ///menu
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) 
			return ;
		key_pressed= 0;
	}*/
	if( 410< y && y <490 )
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
	if( 110< y && y <190 )
	{
		short_pressedhome = 0; long_pressedhome = 0;
		if( press_douhome == 0)
			press_oncehome++;
		if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_douhome = 1;
			return ;
		}
		press_douhome = 0; // time_prev5 = jiffies;
		if(press_oncehome>2 && key_releasedhome!= 1)
		{
			mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
			timecnthome++;
			key_pressed=5;
			time_prev5home = jiffies;
			key_releasedhome = 0;
			return ;
		}
	}
	if( 310< y && y <390 )
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
	if( 10< y && y <90 )
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
			{
				press_dou = 1; 
				return ;
			}
			press_dou = 0; // time_prev5 = jiffies;
			if(press_once>2&&press_once < 39 &&key_released!= 1)
			{
				mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
				timecnt++; 
				key_pressed=5;
				time_prev5 = jiffies;
				key_released = 0; 
				return ;
			}
			if(press_once==40)
			{
				input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
				input_sync(input_dev_g);
				input_sync(ts->input_dev);key_pressed = 5;
				key_released = 0;
				press_once = 0;
				input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
				long_pressed = 1; long_press_time =  jiffies;

				timecnt++;
				time_prev5 = jiffies;
				key_released = 0; return ;
			}
			nwd_dbg_tp();
			key_pressed= 5;
			timecnt++;
			time_prev = jiffies;
			time_prev5 = jiffies;
			key_released = 0;
			tp_int_trigeredhome = 1;
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			return ;
		}
		timecnt++;time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
	}
}

static void gtp_touch_down_737L(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
	GTP_SWAP(x, y);
#endif
	printk("gtp_touch_down737L x=%u y=%u...\n",x,y);
	if(x>0 && x<70) ///GTP_HAVE_TOUCH_KEY
		goto key_event;
	if(x<80)   return;
	if(x>1104) x=1104;
	
#if GTP_ICS_SLOT_REPORT
	input_mt_slot(ts->input_dev, id);
	input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-80);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
	input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
	input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>0 && x<40) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

	if( 210< y && y <290 ) ///back
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
	/*if( 766< x && x <830 ) ///menu
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) 
			return ;
		key_pressed= 0;
	}*/
	if( 410< y && y <490 )
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
	if( 110< y && y <190 )
	{
		short_pressedhome = 0; long_pressedhome = 0;
		if( press_douhome == 0)
			press_oncehome++;
		if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_douhome = 1;
			return ;
		}
		press_douhome = 0; // time_prev5 = jiffies;
		if(press_oncehome>2 && key_releasedhome!= 1)
		{
			mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
			timecnthome++;
			key_pressed=5;
			time_prev5home = jiffies;
			key_releasedhome = 0;
			return ;
		}
	}
	if( 310< y && y <390 )
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
	if( 10< y && y <90 )
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
			{
				press_dou = 1; 
				return ;
			}
			press_dou = 0; // time_prev5 = jiffies;
			if(press_once>2&&press_once < 39 &&key_released!= 1)
			{
				mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
				timecnt++; 
				key_pressed=5;
				time_prev5 = jiffies;
				key_released = 0; 
				return ;
			}
			if(press_once==40)
			{
				input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
				input_sync(input_dev_g);
				input_sync(ts->input_dev);key_pressed = 5;
				key_released = 0;
				press_once = 0;
				input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
				long_pressed = 1; long_press_time =  jiffies;

				timecnt++;
				time_prev5 = jiffies;
				key_released = 0; return ;
			}
			nwd_dbg_tp();
			key_pressed= 5;
			timecnt++;
			time_prev = jiffies;
			time_prev5 = jiffies;
			key_released = 0;
			tp_int_trigeredhome = 1;
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			return ;
		}
		timecnt++;time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
	}
}

static void gtp_touch_down_534(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down534 x=%u y=%u...\n",x,y);

    if(x>1 && x<50) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>50 && x< 80)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-85);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>1 && x<40) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

	if( 298< y && y <338 )//volume- 318
	{ 
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 )
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
	if( 237< y && y <277 )//volume+ 257
	{
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
	if( 176< y && y <216 ) ///back 196
	{
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 )
			return ;
		key_pressed= 1;
	}
	if( 115< y && y <155 ) ///home 135
	{
		short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;
                }
	}
	if( 47< y && y <87 )	//power 67
	{
		short_pressed = 0;
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_535(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
	GTP_SWAP(x, y);
#endif
	//printk("gtp_touch_down535 x=%u y=%u...\n",x,y);
	if(x>0 && x<140)
		goto key_event;
#if GTP_ICS_SLOT_REPORT
	input_mt_slot(ts->input_dev, id);
	input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-140);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
	input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
	input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>0 && x<140) ///GTP_HAVE_TOUCH_KEY
	{
		int key_value = 1; static timecnt=1;
		static timecnthome=1;
		/*if( 890< y && y <950 ) ///back
		{
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
				return ;
			key_pressed= 1;
		}*/
		if( 250< y && y <350 ) ///menu
		{
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) 
				return ;
			key_pressed= 0;
		}
		if( 490< y && y <590 )
		{
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
				return ;
			input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1);
			key_pressed= 3;
		}
		if( 130< y && y <230 )
		{
			short_pressedhome = 0; long_pressedhome = 0;
			if( press_douhome == 0)
				press_oncehome++;
			if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
			{
				press_douhome = 1;
				return ;
			}
			press_douhome = 0; // time_prev5 = jiffies;
			if(press_oncehome>2 && key_releasedhome!= 1)
			{
				mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
				timecnthome++;
				key_pressed=5;
				time_prev5home = jiffies;
				key_releasedhome = 0;
				return ;
			}
		}
		if( 370< y && y <470 )
		{
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
			input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
		}
		if( 10< y && y <110 )
		{
			short_pressed = 0; 
			long_pressed = 0;
			if( press_dou == 0) 
				press_once++;
			if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )  
			{
				press_dou = 1;
				return ;
			}
			press_dou = 0; // time_prev5 = jiffies;
			if(press_once>2&&press_once < 39 &&key_released!= 1)
			{
				mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
				timecnt++; 
				key_pressed=5;
				time_prev5 = jiffies;
				key_released = 0; 
				return ;
			}

			if(press_once==40)
			{
				input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
				input_sync(input_dev_g);
				input_sync(ts->input_dev);key_pressed = 5;
				key_released = 0;
				press_once = 0;
				input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
				long_pressed = 1; long_press_time =  jiffies;
				timecnt++;
				time_prev5 = jiffies;
				key_released = 0; return ;
			}
			nwd_dbg_tp();
			key_pressed= 5;
			timecnt++;
			time_prev = jiffies;
			time_prev5 = jiffies;
			key_released = 0;
			tp_int_trigeredhome = 1;
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			return ;
		}
		timecnt++;time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
	}
}

static void gtp_touch_down_900(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down900 x=%u y=%u...\n",x,y);

    if(x>1034 && x<1094) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>1024 && x<1034)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
//	if(x>1034 && x<1094) ///GTP_HAVE_TOUCH_KEY     Add 20150410 by Jiwq
	if(x>1060 && x<1094) ///GTP_HAVE_TOUCH_KEY     Add 20150410 by Jiwq
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

	/*if( 210< y && y <290 ) ///back
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
	if( 766< x && x <830 ) ///menu
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 300 ) 
			return ;
		key_pressed= 0;
	}*/
//	if( 310< y && y <390 )//volume+
	if( 415< y && y <485 )//volume+
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
//	if( 410< y && y <490 )//volume-
	if( 505< y && y <575 )//volume-
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
//	if( 110< y && y <190 )
	if( 115< y && y <185 )//mute
	{
/*		short_pressedhome = 0; long_pressedhome = 0;
		if( press_douhome == 0)
			press_oncehome++;
		if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_douhome = 1;
			return ;
		}
		press_douhome = 0; // time_prev5 = jiffies;
		if(press_oncehome>2 && key_releasedhome!= 1)
		{
			mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
			timecnthome++;
			key_pressed=5;
			time_prev5home = jiffies;
			key_releasedhome = 0;
			return ;
		}
*/		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
	}
//	if( 10< y && y <90 )
	if( 25< y && y <95 )//power short
	{       
/*		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
*/		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_6718(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down6718 x=%u y=%u...\n",x,y);

    if(x>1034 && x<1094) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>1024 && x<1034)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>1060 && x<1094) ///GTP_HAVE_TOUCH_KEY     Add 20150410 by Jiwq
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	if( 25< y && y <95 ) ///NAVI
	{     
		mKeyboardValue |= 0x01;
		mod_timer(&Timer_keyboard_6718, jiffies + msecs_to_jiffies(1));
	}
	else if( 115< y && y <185 ) ///MODE
	{       
		mKeyboardValue |= 0x02;
		mod_timer(&Timer_keyboard_6718, jiffies + msecs_to_jiffies(1));
	}

	else if( 415< y && y <485 )//volume+
	{       
		mKeyboardValue |= 0x04;
		mod_timer(&Timer_keyboard_6718, jiffies + msecs_to_jiffies(1));
	}
	else if( 505< y && y <575 )//volume-
	{       
		mKeyboardValue |= 0x08;
		mod_timer(&Timer_keyboard_6718, jiffies + msecs_to_jiffies(1));
	}
    }
}

static void gtp_touch_down_781(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down781 x=%u y=%u...\n",x,y);

    if(x>1 && x<20) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>20 && x< 25)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-25);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>1 && x<15) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

//	if( 466< y && y <506 )//volume- 486
	if( 451< y && y <516 )//volume- 486
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
//	if( 380< y && y <420 )//volume+ 400
	if( 370< y && y <430 )//volume+ 400
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
//	if( 294< y && y <334 ) ///back 314
	if( 284< y && y <344 ) ///back 314
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
//	if( 206< y && y <246 ) ///home 226
	if( 196< y && y <256 ) ///home 226
	{       
		short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;
                }
	}
//	if( 122< y && y <162 )	//power 142
	if( 112< y && y <172 )	//power 142
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_787(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down787 x=%u y=%u...\n",x,y);

    if(x>1 && x<40) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x< 60)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-60);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>1 && x<40) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

	if( 570< y && y <600 )//volume- 590
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
	if( 521< y && y <561 )//volume+ 541
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
	if( 475< y && y <515 ) ///back 495
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
	if( 430< y && y <470 ) ///home 450
	{       
		short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;
                }
	}
	if( 387< y && y <427 )	//power 407
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}


static void gtp_touch_down_789(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down789 x=%u y=%u...\n",x,y);

    if(x>1 && x<60) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x< 80)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-80);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>1 && x<60) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

//	if( 344< y && y <404 )//volume- 374
	if( 354< y && y <394 )//volume- 374
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
//	if( 268< y && y <328 )//volume+ 298
	if( 278< y && y <318 )//volume+ 298
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
//	if( 188< y && y <248 ) ///back 218
	if( 198< y && y <238 ) ///back 218
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
//	if( 113< y && y <173 ) ///home 143
	if( 123< y && y <163 ) ///home 143
	{       
		short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;
                }
	}
//	if( 35< y && y <95 )	//power 65
	if( 45< y && y <85 )	//power 65
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_791(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down791 x=%u y=%u...\n",x,y);

    if(y>640 && y<670) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(y>600)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(y>650 && y<670) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

//	if( 489< x && x <529 )//volume+ 509
	if( 479< x && x <534 )//volume+ 509
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
//	if( 398< x && x <438 )//volume- 418
	if( 388< x && x <448 )//volume- 418
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
//	if( 304< x && x <344 ) ///back 324
	if( 294< x && x <354 ) ///back 324
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
//	if( 221< x && x <251 ) ///home 231
	if( 201< x && x <261 ) ///home 231
	{       
		short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;
                }
	}
//	if( 120< x && x <160 )	//power 140
	if( 110< x && x <165 )	//power 140
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_792(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down792 x=%u y=%u...\n",x,y);

    if(y>640 && y<690) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(y>600)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(y>670 && y<690) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

//	if( 395< x && x <435 )//volume+ 415
	if( 390< x && x <440 )//volume+ 415
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
//	if( 313< x && x <353 )//volume- 333
	if( 308< x && x <358 )//volume- 333
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
//	if( 229< x && x <269 ) ///back 249
	if( 224< x && x <274 ) ///back 249
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
//	if( 144< x && x <184 ) ///home 164
	if( 139< x && x <189 ) ///home 164
	{       
		short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;
                }
	}
//	if( 66< x && x <106 )	//power 86
	if( 61< x && x <111 )	//power 86
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_793(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down793 x=%u y=%u...\n",x,y);

    if(y>640 && y<706) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(y>600)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(y>665 && y<706) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

//	if( 403< x && x <443 ) ///back 423
	if( 398< x && x <448 ) ///back 423
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
//	if( 324< x && x <364 ) ///home 344
	if( 319< x && x <369 ) ///home 344
	{       
		short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;
                }
	}
//	if( 240< x && x <280 )//volume- 260
	if( 235< x && x <285 )//volume- 260
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
//	if( 140< x && x <200 )//volume+ 170
	if( 145< x && x <195 )//volume+ 170
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
//	if( 67< x && x <107 )	//power 87
	if( 62< x && x <112 )	//power 87
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_794(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down794 x=%u y=%u...\n",x,y);

    if(x>1 && x<60) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>60 && x< 70)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-79);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>1 && x<36) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

//	if( 300< y && y <330 )//volume- 315
	if( 290< y && y <340 )//volume- 315
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
//	if( 235< y && y <265 )//volume+ 250
	if( 225< y && y <275 )//volume+ 250
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
//	if( 164< y && y <194 ) ///back 179
	if( 154< y && y <204 ) ///back 179
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
//	if( 98< y && y <128 ) ///home 113
	if( 88< y && y <138 ) ///home 113
	{       
		short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;
                }
	}
//	if( 34< y && y <64 )	//power 49
	if( 24< y && y <74 )	//power 49
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_798(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down798 x=%u y=%u...\n",x,y);

    if(x>1 && x<45) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>45 && x< 55)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-55);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>1 && x<40) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

//	if( 265< y && y <305 )//volume- 285
	if( 266< y && y <306 )//volume- 286
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
//	if( 208< y && y <248 )//volume+ 228
	if( 205< y && y <245 )//volume+ 225
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
//	if( 142< y && y <182 ) ///back 162
	if( 141< y && y <181 ) ///back 161
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
//	if( 83< y && y <123 ) ///home 103
	if( 79< y && y <119 ) ///home 99
	{       
		short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;
                }
	}
//	if( 15< y && y <55 )	//power 35
	if( 20< y && y <60 )	//power 40
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_799(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down799 x=%u y=%u...\n",x,y);

    if(x>1 && x<40) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(x>40 && x< 50)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x-50);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(x>1 && x<36) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

//	if( 465< y && y <525 )//volume- 495
	if( 475< y && y <515 )//volume- 495
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
//	if( 360< y && y <420 )//volume+ 390
	if( 370< y && y <410 )//volume+ 390
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
//	if( 255< y && y <315 ) ///back 285
	if( 265< y && y <305 ) ///back 285
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
//	if( 143< y && y <203 ) ///home 173
	if( 153< y && y <193 ) ///home 173
	{       
		short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;
                }
	}
//	if( 45< y && y <105 )	//power 75
	if( 50< y && y <90 )	//power 70
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}

static void gtp_touch_down_800(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
//	x = 1073 -x;
//    printk("gtp_touch_down800 x=%u y=%u...\n",x,y);

    if(y>610 && y<640) ///GTP_HAVE_TOUCH_KEY
        goto key_event;
    else if(y>640)
        return;
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id);
///input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false); ///?need to add ?,rk drv have  
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
  ///  input_report_abs(ts->input_dev, ABS_MT_POSITION_X,800- x*800/864);
  	input_report_abs(ts->input_dev, ABS_MT_POSITION_X,x);

    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,y);//modify by lusterzhang 
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w); ///.press
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

key_event:
	if(y>610 && y<640) ///GTP_HAVE_TOUCH_KEY
	{  ///    #define GTP_KEY_TAB  {KEY_MENU,  KEY_BACK,KEY_HOME,KEY_VOLUMEDOWN_NWD,KEY_VOLUMEUP_NWD,KEY_DISPLAYTOGGLE_NWD}
	int key_value = 1; static timecnt=1;
	static timecnthome=1;

	if( 460< x && x <520 ) ///back 490
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 200 ) 
			return ;
		key_pressed= 1;
	}
//	if( 370< x && x <430 ) ///home 400
	if( 365< x && x <425 ) ///home 395
	{       
		short_pressedhome = 0; long_pressedhome = 0;
                if( press_douhome == 0)
                        press_oncehome++;
                if (timecnthome != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
                {
                        press_douhome = 1;
                        return ;
                }
                printk("press_once:%d.............\n",press_oncehome);
                press_douhome = 0; // time_prev5 = jiffies;
//              if(press_oncehome>2&&press_oncehome < 40 &&key_releasedhome!= 1)
                if(press_oncehome>2 && key_releasedhome!= 1)
                {
                        printk("press_once home:%d.............\n",press_oncehome);
                        mod_timer(&tpsl_timerhome, jiffies + msecs_to_jiffies(300));
                        timecnthome++;
                        key_pressed=5;
                        time_prev5home = jiffies;
                        key_releasedhome = 0;
                        return ;
                }
	}
//	if( 278< x && x <338 )//volume- 308
	if( 273< x && x <333 )//volume- 303
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) 
			return ;
		input_report_key(ts->input_dev, KEY_VOLUMEDOWN_NWD, 1); 
		key_pressed= 3;
	}
//	if( 185< x && x <245 )//volume+ 215
	if( 180< x && x <240 )//volume+ 210
	{       
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev) < 150 ) return ;
		input_report_key(ts->input_dev, KEY_VOLUMEUP_NWD, 1); nwd_dbg_tp();key_pressed= 4;
	}
//	if( 90< x && x <150 )	//power 120
	if( 95< x && x <155 )	//power 125
	{       
		short_pressed = 0; 
		long_pressed = 0;
		if( press_dou == 0) 
			press_once++;
		if (timecnt != 1  && jiffies_to_msecs (jiffies - time_prev5) < 50 )
		{
			press_dou = 1; 
			return ;
		}
		press_dou = 0; // time_prev5 = jiffies;
		if(press_once>2&&press_once < 39 &&key_released!= 1)
		{
			mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
			timecnt++; 
			key_pressed=5;
			time_prev5 = jiffies;
			key_released = 0; 
			return ;
		}
		if(press_once==40)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,1);
			input_sync(input_dev_g);
			input_sync(ts->input_dev);key_pressed = 5;
			key_released = 0;
			press_once = 0;
			input_report_key(input_dev_g, POWEROFFKEYFUN,1);input_sync(input_dev_g);
			long_pressed = 1; long_press_time =  jiffies;

			timecnt++;
			time_prev5 = jiffies;
			key_released = 0; return ;
		}
		nwd_dbg_tp();
		key_pressed= 5;
		timecnt++;
		time_prev = jiffies;
		time_prev5 = jiffies;
		key_released = 0;
		tp_int_trigeredhome = 1;
		mod_timer(&tpsl_timer, jiffies + msecs_to_jiffies(300));
		return ;
	}
	timecnt++;time_prev = jiffies;
	time_prev5 = jiffies;
	key_released = 0;
	tp_int_trigeredhome = 1;
    }
}

static void tpsl_timer_callback(unsigned long data )
{

	
printk("===========%d %s==long_pressed%d==\n",__LINE__,__FUNCTION__,long_pressed);
 ///if(long_pressed == 1) 
{  ///long_press_time =  jiffies;
printk("===========%d %s==long_pressed%d==\n",__LINE__,__FUNCTION__,long_pressed);
if (jiffies_to_msecs (jiffies - long_press_time) <1500)
{
printk("===========%d %s==long_pressed%d==\n",__LINE__,__FUNCTION__,long_pressed);
return ;
}		
}

printk("===========%d %s==long_pressed%d==\n",__LINE__,__FUNCTION__,long_pressed);
if (jiffies_to_msecs (jiffies - time_prev5) >260)
{

	input_report_key( input_dev_g, MUTE_SOUND_FUN,1);
	press_once = 0;
	///short_key_to1();
		
 	input_sync(input_dev_g); key_pressed=5;short_pressed = 1;
	input_report_key(input_dev_g, MUTE_SOUND_FUN,0);input_sync(input_dev_g);
//printk("=====ssssssssssssssssssssssssssssssssss0000000000000000000000======%d %s====\n",__LINE__,__FUNCTION__);
}

key_pressed=5;

if(tp_int_trigered ==0)
	press_once=0;

tp_int_trigered = 0;

}



static void tpsl_timer_callbackhome(unsigned long data )
{
	
	///if(long_pressed == 1) 
	{  ///long_press_time =  jiffies;
		printk("===========%d %s==long_pressed%d==\n",__LINE__,__FUNCTION__,long_pressedhome);
		if (jiffies_to_msecs (jiffies - long_press_timehome) <300)//1500)
		{
		printk("===========%d %s==long_pressed%d==\n",__LINE__,__FUNCTION__,long_pressedhome);
		return ;
		}		
	}

	printk("===========%d %s==long_pressed%d==\n",__LINE__,__FUNCTION__,long_pressed);
//	if(press_oncehome<40)
	{
		if (jiffies_to_msecs (jiffies - time_prev5home) >260)
		{

			home_key_to1();
			press_oncehome = 0;
		
	 		input_sync(input_dev_g); key_pressed=2;short_pressedhome = 1;
			home_key_to0();
		 	printk("tpsl_timer_callbackhome ==%d %s====\n",__LINE__,__FUNCTION__);
		}
	}
/*
	else
	{
		press_oncehome=0;
	}
*/
	key_pressed=2;

	if(tp_int_trigeredhome ==0)
	{
		
		printk("tpsl_timer_callbackhome %d %s==long_pressed%d==\n",__LINE__,__FUNCTION__,long_pressedhome);
		press_oncehome=0;
	}
	tp_int_trigeredhome = 0;

}
unsigned long timerfunc_in = 0;
unsigned long up_func_in = 0;
unsigned char mKeyDisplayFlag=0;
static void tpsl_timer_callbackmenu_back(unsigned long data )
{
	timerfunc_in++;

	if(key_pressed == 0) 
	{  
	// 	printk("===========%d %s==long_pressed%d==\n",__LINE__,__FUNCTION__,long_pressedhome);
		input_report_key(input_dev_g,KEY_SELF_MENU ,1);  input_sync(input_dev_g);
		input_report_key(input_dev_g,KEY_SELF_MENU ,0);  input_sync(input_dev_g);
		timerfunc_in = up_func_in=0;

	}
	if(key_pressed == 1) 
	{ 
	//  	printk("===========%d %s==long_pressed%d==\n",__LINE__,__FUNCTION__,long_pressedhome);
		input_report_key(input_dev_g,KEY_BACK ,1);  	input_sync(input_dev_g);
		input_report_key(input_dev_g,KEY_BACK ,0);      input_sync(input_dev_g);
		timerfunc_in = up_func_in=0;
	}
        if(key_pressed == 10)
        {
          //      printk("===========%d %s==long_pressed%d==\n",__LINE__,__FUNCTION__,long_pressedhome);
                input_report_key(input_dev_g,MUTE_SOUND_FUN ,1);
                input_sync(input_dev_g);
                input_report_key(input_dev_g,MUTE_SOUND_FUN ,0);
                input_sync(input_dev_g);
                timerfunc_in = up_func_in=0;
        }
	if(key_pressed == 11)
        {
		
		if(mKeyDisplayFlag==0x55)
		{
			// printk("===========%d %s==display ..........\n",__LINE__,__FUNCTION__);
                	input_report_key(input_dev_g,KEY_SL_DISPLAY ,0);
                	input_sync(input_dev_g);
                	timerfunc_in = up_func_in=0;
		}
		mKeyDisplayFlag= 0;
        }
        if(mKeyDisplayFlag==0x55)
        {
	    //printk("11111======%d %s==display ..........\n",__LINE__,__FUNCTION__);
            input_report_key(input_dev_g,KEY_SL_DISPLAY ,0);
            input_sync(input_dev_g);
            timerfunc_in = up_func_in=0;
	     mKeyDisplayFlag= 0;
        }
// 	printk("===========%d %s====\n",__LINE__,__FUNCTION__);



	key_pressed=9;



}

static void timer_callbackmenu_6718(unsigned long data )
{
	unsigned char mKeyFlag=0;
	if(mKeyboardValue &0x01) 
	{  
		input_report_key(input_dev_g, KEY_NAVI_NWD, 1); 
		input_sync(input_dev_g);
		input_report_key(input_dev_g, KEY_NAVI_NWD, 0); 
		input_sync(input_dev_g);
	}
	if(mKeyboardValue &0x02) 
	{ 
		input_report_key(input_dev_g, KEY_MODE_NWD, 1); 
		input_sync(input_dev_g);
		input_report_key(input_dev_g, KEY_MODE_NWD, 0); 
		input_sync(input_dev_g);
	}
	if(mKeyboardValue &0x04) 
	{
		input_report_key(input_dev_g, KEY_VOLUMEUP_NWD, 1); 
		input_sync(input_dev_g);
		input_report_key(input_dev_g, KEY_VOLUMEUP_NWD, 0);
		input_sync(input_dev_g);
		
		mKeyFlag = 1;
	}
	if(mKeyboardValue &0x08) 
	{
		input_report_key(input_dev_g, KEY_VOLUMEDOWN_NWD, 1); 
		input_sync(input_dev_g);
		input_report_key(input_dev_g, KEY_VOLUMEDOWN_NWD, 0); 
		input_sync(input_dev_g);
		
		mKeyFlag = 1;
	}
	if(mKeyFlag)
	{
		mod_timer(&Timer_keyboard_6718, jiffies + msecs_to_jiffies(150));
	}
}
/*******************************************************
Function:
    Report touch release event
Input:
    ts: goodix i2c_client private data
Output:
    None.
*********************************************************/
static void gtp_touch_up(struct goodix_ts_data* ts, s32 id) //add a timer, and the time this func is called 
{	///printk("===========%d %s====\n",__LINE__,__FUNCTION__);
	int i=6;/****/
	if(key_pressed==2)
	{  /***
		home_key_to0();key_pressed= 9;
		return;*/
		
		
				if( long_pressedhome ==1)
		{
			input_report_key(ts->input_dev, 252,0); 
			long_pressedhome = 0;
		}
		if( short_pressedhome == 1)
		{
	
			home_key_to0();
			short_pressedhome = 0;
		 }
//	printk("===========%d %s====\n",__LINE__,__FUNCTION__);
	///if (press_once >5)
		key_pressed= 9;	/////if(press_once==7) key_released= 1;key_pressed= 9;
		return;
	}
	else if(key_pressed==5)
	{
///tp_int_trigered = 0;
///if(tp_int_trigered ==0)
///press_once==0;
		if( long_pressed ==1)
		{
			input_report_key(ts->input_dev, POWEROFFKEYFUN,0); 
			long_pressed = 0;
		}
		if( short_pressed == 1)
		{
	
			input_report_key(ts->input_dev, MUTE_SOUND_FUN,0);
			short_pressed = 0;
		 }
//	printk("===========%d %s====\n",__LINE__,__FUNCTION__);
	///if (press_once >5)
		key_pressed= 9;	/////if(press_once==7) key_released= 1;key_pressed= 9;
		return;
	}

	else if(key_pressed==0)
	{	up_func_in++;
 		/***
		input_report_key(ts->input_dev,KEY_SELF_MENU ,0);  
		key_pressed= 9;*/
		mod_timer(&tpsl_timer_backmenu_afterup, jiffies + msecs_to_jiffies(300));
	//	printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);
		return;
	}
	else if(key_pressed==1)
	{	up_func_in++;
 		/***
		input_report_key(ts->input_dev,KEY_BACK ,0);  
		key_pressed= 9;*/
		mod_timer(&tpsl_timer_backmenu_afterup, jiffies + msecs_to_jiffies(300));
		//	printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);
		return;
	}

	else if(key_pressed==10)
        {       up_func_in++;
                /***
                input_report_key(ts->input_dev,KEY_BACK ,0);  
                key_pressed= 9;*/
                mod_timer(&tpsl_timer_backmenu_afterup, jiffies + msecs_to_jiffies(300));
            //    printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);
                return;
        }

        else if(key_pressed==11)
        {       
		up_func_in++;
                mod_timer(&tpsl_timer_backmenu_afterup, jiffies + msecs_to_jiffies(300));
		if(mKeyDisplayFlag!=0x55)
		{
			input_report_key(input_dev_g,KEY_SL_DISPLAY ,1);
                	input_sync(input_dev_g);
			mKeyDisplayFlag = 0x55;
		//	printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);
		}
		
                return;
        }

	else if(key_pressed!=9)
	{
    		input_report_key(ts->input_dev, touch_key_array[key_pressed], 0); key_pressed= 9;///nwd_dbg_tp(); 
		printk("======zyj=====%d %s====\n",__LINE__,__FUNCTION__);	        
		input_sync(ts->input_dev);
		return;
	}

#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->input_dev, id); ///	input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);  ?
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, -1);
    GTP_DEBUG("Touch id[%2d] release!", id);
#else
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0);
    input_mt_sync(ts->input_dev);
#endif
 	
  
}


/*******************************************************
Function:
    Goodix touchscreen work function
Input:
    work: work struct of goodix_workqueue
Output:
    None.
*********************************************************/
static void goodix_ts_work_func(struct work_struct *work)
{
    u8  end_cmd[3] = {GTP_READ_COOR_ADDR >> 8, GTP_READ_COOR_ADDR & 0xFF, 0};
    u8  point_data[2 + 1 + 8 * GTP_MAX_TOUCH + 1]={GTP_READ_COOR_ADDR >> 8, GTP_READ_COOR_ADDR & 0xFF};
    u8  touch_num = 0;
    u8  finger = 0;
    static u16 pre_touch = 0;
    static u8 pre_key = 0;
#if GTP_WITH_PEN
    static u8 pre_pen = 0;
#endif
    u8  key_value = 0;
    u8* coor_data = NULL;
    s32 input_x = 0;
    s32 input_y = 0;
    s32 input_w = 0;
    s32 id = 0;
    s32 i  = 0;
    s32 ret = -1;
    struct goodix_ts_data *ts = NULL;

#if GTP_SLIDE_WAKEUP
    u8 doze_buf[3] = {0x81, 0x4B};
#endif
#if 1//add keyboard by jiawq
    u8 key_temp = 0;
    u8 key_buf[4] = {1, 2, 4, 8};
	u8 key_buff[4][7] = {
		{1, 0x33, 0x04, 0x3C, 0x00, 0xFF, 0x00},
		{2, 0x33, 0x04, 0x96, 0x00, 0xFF, 0x00},	
		{4, 0x33, 0x04, 0xC2, 0x01, 0xFF, 0x00},
		{8, 0x33, 0x04, 0x1c, 0x02, 0xFF, 0x00},
	};
#endif

    GTP_DEBUG_FUNC();
    ts = container_of(work, struct goodix_ts_data, work);
    if (ts->enter_update)
    {
        return;
    }
#if GTP_SLIDE_WAKEUP
    if (DOZE_ENABLED == doze_status)
    {               
        ret = gtp_i2c_read(i2c_connect_client, doze_buf, 3);
        GTP_DEBUG("0x814B = 0x%02X", doze_buf[2]);
        if (ret > 0)
        {               
            if (doze_buf[2] == 0xAA)
            {		
		printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                GTP_INFO("Slide(0xAA) To Light up the screen!");
                doze_status = DOZE_WAKEUP;
                input_report_key(ts->input_dev, KEY_POWER, 1);
                input_sync(ts->input_dev);
                input_report_key(ts->input_dev, KEY_POWER, 0);
                input_sync(ts->input_dev);
                // clear 0x814B
                doze_buf[2] = 0x00;
                gtp_i2c_write(i2c_connect_client, doze_buf, 3);
            }
            else if (doze_buf[2] == 0xBB)
            {			printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                GTP_INFO("Slide(0xBB) To Light up the screen!");
                doze_status = DOZE_WAKEUP;
                input_report_key(ts->input_dev, KEY_POWER, 1);
                input_sync(ts->input_dev);
                input_report_key(ts->input_dev, KEY_POWER, 0);
                input_sync(ts->input_dev);
                // clear 0x814B
                doze_buf[2] = 0x00;
                gtp_i2c_write(i2c_connect_client, doze_buf, 3);
            }
            else if (0xC0 == (doze_buf[2] & 0xC0))
            {   	printk("===========%d %s====\n",__LINE__,__FUNCTION__);
                GTP_INFO("double click to light up the screen!");
                doze_status = DOZE_WAKEUP;
                input_report_key(ts->input_dev, KEY_POWER, 1);
                input_sync(ts->input_dev);
                input_report_key(ts->input_dev, KEY_POWER, 0);
                input_sync(ts->input_dev);
                // clear 0x814B
                doze_buf[2] = 0x00;
                gtp_i2c_write(i2c_connect_client, doze_buf, 3);
            }
            else
            {
                gtp_enter_doze(ts);
            }
        }
        if (ts->use_irq)
        {
            gtp_irq_enable(ts);
        }
        return;
    }
#endif

    ret = gtp_i2c_read(ts->client, point_data, 12);
    if (ret < 0)
    {
        GTP_ERROR("I2C transfer error. errno:%d\n ", ret);
        goto exit_work_func;
    }

    finger = point_data[GTP_ADDR_LENGTH];    
    if((finger & 0x80) == 0)
    {
        goto exit_work_func;
    }

    touch_num = finger & 0x0f;
    if (touch_num > GTP_MAX_TOUCH)
    {
        goto exit_work_func;
    }

    if (touch_num > 1)
    {
        u8 buf[8 * GTP_MAX_TOUCH] = {(GTP_READ_COOR_ADDR + 10) >> 8, (GTP_READ_COOR_ADDR + 10) & 0xff};

        ret = gtp_i2c_read(ts->client, buf, 2 + 8 * (touch_num - 1)); 
        memcpy(&point_data[12], &buf[2], 8 * (touch_num - 1));
    }

#if (GTP_HAVE_TOUCH_KEY || GTP_PEN_HAVE_BUTTON)
    key_value = point_data[3 + 8 * touch_num];
    
    if(key_value || pre_key)
    {
    
    #if 0//GTP_HAVE_TOUCH_KEY///wj add an other way
        if (!pre_touch)
        {
            for (i = 0; i < GTP_MAX_KEY_NUM; i++)
            {
            #if GTP_DEBUG_ON
                for (ret = 0; ret < 4; ++ret)
                {
                    if (key_codes[ret] == touch_key_array[i])
                    {
                        GTP_DEBUG("Key: %s %s", key_names[ret], (key_value & (0x01 << i)) ? "Down" : "Up");
                        break;
                    }
                }
            #endif
                input_report_key(ts->input_dev, touch_key_array[i], key_value & (0x01<<i));   
            }
            touch_num = 0;  // shield fingers
        }
	#endif
	#if 0
	if (!pre_touch)
	{   
		if(key_value)	
			key_temp = key_value;
		else
			key_temp = pre_key;
    
		#if GTP_DEBUG_ON      
		for (i = 0; i < 4; ++i)       
		{         
			if(key_temp == key_buf[i])       
			{       
				GTP_DEBUG("Key: %s %s", key_names[i], (key_value & (0x01 << i)) ? "Down" : "Up");

				break;                    
			}   
		}       
		#endif  			
		
		input_report_key(ts->input_dev, key_codes[i], !!key_value);          
	}
     
	touch_num = 0;  // shield fingers
	#else
	if(key_value)
	{
		key_temp = key_value;
		for(i=0; i<4; i++){
			//add keyboard
			if(key_temp == key_buff[i][0])
			{
				point_data[3] = !!key_value;
				memcpy(&point_data[4], &key_buff[i][1], 6);
				break;
			}
		}

		touch_num = 1;
	}
	else
	{
		key_temp = pre_key;

		if(pre_key & 0x01)
			mKeyboardValue &= 0xe;
		if(pre_key & 0x02)
			mKeyboardValue &= 0xd;
		if(pre_key & 0x04)
			mKeyboardValue &= 0xb;
		if(pre_key & 0x08)
			mKeyboardValue &= 0x7;
	}

	#endif
   }
#endif
    pre_key = key_value;

    GTP_DEBUG("pre_touch:%02x, finger:%02x.", pre_touch, finger);

#if GTP_ICS_SLOT_REPORT

#if GTP_WITH_PEN
    if (pre_pen && (touch_num == 0))
    {
        GTP_DEBUG("Pen touch UP(Slot)!");
        input_report_key(ts->input_dev, BTN_TOOL_PEN, 0);
        input_mt_slot(ts->input_dev, 5);
        input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, -1);
        pre_pen = 0;
    }
#endif
    if (pre_touch || touch_num)
    {
        s32 pos = 0;
        u16 touch_index = 0;

        coor_data = &point_data[3];
        
        if(touch_num)
        {
            id = coor_data[pos] & 0x0F;///coor_data[0]
        
        #if GTP_WITH_PEN
            id = coor_data[pos];
            if ((id == 128))  
            {
                GTP_DEBUG("Pen touch DOWN(Slot)!");
                input_x  = coor_data[pos + 1] | (coor_data[pos + 2] << 8);
                input_y  = coor_data[pos + 3] | (coor_data[pos + 4] << 8);
                input_w  = coor_data[pos + 5] | (coor_data[pos + 6] << 8);
                
                input_report_key(ts->input_dev, BTN_TOOL_PEN, 1);
                input_mt_slot(ts->input_dev, 5);
                input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, 5);
                input_report_abs(ts->input_dev, ABS_MT_POSITION_X, input_x);
                input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, input_y);
                input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, input_w);
                GTP_DEBUG("Pen/Stylus: (%d, %d)[%d]", input_x, input_y, input_w);
                pre_pen = 1;
                pre_touch = 0;
            }    
        #endif
        
            touch_index |= (0x01<<id);
        }
        
        GTP_DEBUG("id = %d,touch_index = 0x%x, pre_touch = 0x%x\n",id, touch_index,pre_touch);
        for (i = 0; i < GTP_MAX_TOUCH; i++)  ///the slot off the point/dot 
        {
        #if GTP_WITH_PEN
            if (pre_pen == 1)
            {
                break;
            }
        #endif
            
            if (touch_index & (0x01<<i))
            {
                input_x  = coor_data[pos + 1] | (coor_data[pos + 2] << 8);
                input_y  = coor_data[pos + 3] | (coor_data[pos + 4] << 8);
                input_w  = coor_data[pos + 5] | (coor_data[pos + 6] << 8);
		
           if(memcmp(RunSystemTouchBuf,"528",3)==0)
            {
                gtp_touch_down_528(ts, id, input_x, input_y, input_w);
            }
            else if(memcmp(RunSystemTouchBuf,"520W",4)==0)
            {
                gtp_touch_down_520W(ts, id, input_x, input_y, input_w);
            }

            else if(memcmp(RunSystemTouchBuf,"520",3)==0)
            {
                gtp_touch_down_520(ts, id, input_x, input_y, input_w);
            }
            else if(memcmp(RunSystemTouchBuf,"568Y",4)==0)
            {
                gtp_touch_down_568Y(ts, id, input_x, input_y, input_w);
	    }
            else if(memcmp(RunSystemTouchBuf,"568",3)==0)
            {
                gtp_touch_down_568(ts, id, input_x, input_y, input_w);
            }
            else if(memcmp(RunSystemTouchBuf,"584",3)==0)
            {
                gtp_touch_down_584(ts, id, input_x, input_y, input_w);
            }
            else if(memcmp(RunSystemTouchBuf,"585",3)==0)
            {
                gtp_touch_down_585(ts, id, input_x, input_y, input_w);
            }
            else if(memcmp(RunSystemTouchBuf,"586",3)==0)
            {
                gtp_touch_down_586(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"589W",4)==0)
	    {
		 gtp_touch_down_589W(ts, id, input_x, input_y, input_w);
	    }
            else if(memcmp(RunSystemTouchBuf,"589",3)==0)
            {
                gtp_touch_down_589(ts, id, input_x, input_y, input_w);
            }
            else if(memcmp(RunSystemTouchBuf,"591",3)==0)
            {
                gtp_touch_down_591(ts, id, input_x, input_y, input_w);
            }
            else if(memcmp(RunSystemTouchBuf,"592",3)==0)
            {
                gtp_touch_down_592(ts, id, input_x, input_y, input_w);
            } 
	    else if(memcmp(RunSystemTouchBuf,"594",3)==0)
            {
                gtp_touch_down_594(ts, id, input_x, input_y, input_w);
            }
            else if(memcmp(RunSystemTouchBuf,"596Z",4)==0)
            {
                gtp_touch_down_596Z(ts, id, input_x, input_y, input_w);
            }
            else if(memcmp(RunSystemTouchBuf,"596",3)==0)
            {
                gtp_touch_down_596(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"8198D",5)==0)
            {
                gtp_touch_down_8198D(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"709",3)==0)
            {
                gtp_touch_down_709(ts, id, input_x, input_y, input_w);
            }			
	    else if(memcmp(RunSystemTouchBuf,"721",3)==0)
            {
                gtp_touch_down_721(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"720",3)==0)
            {
                gtp_touch_down_720(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"701",3)==0)
            {
                gtp_touch_down_701(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"708",3)==0)
            {
                gtp_touch_down_708(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"702",3)==0)
            {
                gtp_touch_down_702(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"722",3)==0)
            {
                gtp_touch_down_722(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"521",3)==0)
            {
                gtp_touch_down_521(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"600",3)==0)//add by Jiawq
            {
                gtp_touch_down_700(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"700",3)==0)
            {
                gtp_touch_down_700(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"8198Z",5)==0)
            {
                gtp_touch_down_8198Z(ts, id, input_x, input_y, input_w);
            }
		else if(memcmp(RunSystemTouchBuf,"723W",4)==0)
		    {
		   		gtp_touch_down_723W(ts, id, input_x, input_y, input_w);
		    }
	    else if(memcmp(RunSystemTouchBuf,"723",3)==0)
            {
                gtp_touch_down_723(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"703",3)==0)
            {
                gtp_touch_down_703(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"704",3)==0)
            {
                gtp_touch_down_704(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"529",3)==0)
            {
                gtp_touch_down_529(ts, id, input_x, input_y, input_w);
            }
            else if(memcmp(RunSystemTouchBuf,"735W",4)==0)
            {
                gtp_touch_down_735W(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"735",3)==0)
	    {
		gtp_touch_down_735(ts, id, input_x, input_y, input_w);
	    }
	    else if(memcmp(RunSystemTouchBuf,"724",3)==0)
            {
                gtp_touch_down_724(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"738",3)==0)
            {
                gtp_touch_down_738(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"765",3)==0)
            {
                gtp_touch_down_765(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"764",3)==0)
            {
                gtp_touch_down_764(ts, id, input_x, input_y, input_w);
            }
		else if(memcmp(RunSystemTouchBuf,"737L",4)==0)
		   {
		   	gtp_touch_down_737L(ts, id, input_x, input_y, input_w);
		   }
            else if(memcmp(RunSystemTouchBuf,"737",3)==0)
            {
            	gtp_touch_down_737(ts, id, input_x, input_y, input_w);
	    }
		else if(memcmp(RunSystemTouchBuf,"534",3)==0)
		{
			gtp_touch_down_534(ts, id, input_x, input_y, input_w);
		}
	    else if(memcmp(RunSystemTouchBuf,"535",3)==0)
	    {
		gtp_touch_down_535(ts, id, input_x, input_y, input_w);
	    }
	    else if(memcmp(RunSystemTouchBuf,"900",3)==0)
            {
                gtp_touch_down_900(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"6718",4)==0)
	    {
		gtp_touch_down_6718(ts, id, input_x, input_y, input_w);
	    }
	    else if(memcmp(RunSystemTouchBuf,"781",3)==0)
            {
                gtp_touch_down_781(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"787",3)==0)
            {
                gtp_touch_down_787(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"789",3)==0)
            {
                gtp_touch_down_789(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"791",3)==0)
            {
                gtp_touch_down_791(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"792",3)==0)
            {
                gtp_touch_down_792(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"793",3)==0)
            {
                gtp_touch_down_793(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"794",3)==0)
            {
                gtp_touch_down_794(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"798",3)==0)
            {
                gtp_touch_down_798(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"799",3)==0)
            {
                gtp_touch_down_799(ts, id, input_x, input_y, input_w);
            }
	    else if(memcmp(RunSystemTouchBuf,"800",3)==0)
            {
                gtp_touch_down_800(ts, id, input_x, input_y, input_w);
            }
	    else
	    {
		printk("ccc  touchsreen_event_handle %s \n",RunSystemTouchBuf);
                gtp_touch_down_528(ts, id, input_x, input_y, input_w);
	    }

                pre_touch |= 0x01 << i;   ///every bit record a touch point of mt
                
                pos += 8;
                id = coor_data[pos] & 0x0F;
                touch_index |= (0x01<<id);  ////the next touch point  
            }
            else
            {
                gtp_touch_up(ts, i);
                pre_touch &= ~(0x01 << i);
            }
        }
    }
#else   ///GTP_ICS_SLOT_REPORT
    input_report_key(ts->input_dev, BTN_TOUCH, (touch_num || key_value));
    if (touch_num)
    {
        for (i = 0; i < touch_num; i++)
        {
            coor_data = &point_data[i * 8 + 3];

            id = coor_data[0];      //  & 0x0F;
            input_x  = coor_data[1] | (coor_data[2] << 8);
            input_y  = coor_data[3] | (coor_data[4] << 8);
            input_w  = coor_data[5] | (coor_data[6] << 8);
        
        #if GTP_WITH_PEN
            if (id == 128)
            {
                GTP_DEBUG("Pen touch DOWN!");
                input_report_key(ts->input_dev, BTN_TOOL_PEN, 1);
                pre_pen = 1;
                id = 0;   
            }
        #endif
      
            gtp_touch_down(ts, id, input_x, input_y, input_w);
        }
    }
    else if (pre_touch)
    {
    
    #if GTP_WITH_PEN
        if (pre_pen == 1)
        {
            GTP_DEBUG("Pen touch UP!");
            input_report_key(ts->input_dev, BTN_TOOL_PEN, 0);
            pre_pen = 0;
        }
    #endif
    
        GTP_DEBUG("Touch Release!");
        gtp_touch_up(ts, 0);
    }

    pre_touch = touch_num;
#endif  ///GTP_ICS_SLOT_REPORT
//	printk("===========%d %s====\n",__LINE__,__FUNCTION__);
    input_sync(ts->input_dev);

exit_work_func:
    if(!ts->gtp_rawdiff_mode)
    {
        ret = gtp_i2c_write(ts->client, end_cmd, 3);
        if (ret < 0)
        {
            GTP_INFO("I2C write end_cmd error!");
        }
    }
    if (ts->use_irq)
    {
        gtp_irq_enable(ts);
    }
}

/*******************************************************
Function:
    Timer interrupt service routine for polling mode.
Input:
    timer: timer struct pointer
Output:
    Timer work mode. 
        HRTIMER_NORESTART: no restart mode
*********************************************************/
static enum hrtimer_restart goodix_ts_timer_handler(struct hrtimer *timer)
{
    struct goodix_ts_data *ts = container_of(timer, struct goodix_ts_data, timer);

    GTP_DEBUG_FUNC();

    queue_work(goodix_wq, &ts->work);
    hrtimer_start(&ts->timer, ktime_set(0, (GTP_POLL_TIME+6)*1000000), HRTIMER_MODE_REL);
    return HRTIMER_NORESTART;
}

/*******************************************************
Function:
    External interrupt service routine for interrupt mode.
Input:
    irq:  interrupt number.
    dev_id: private data pointer
Output:
    Handle Result.
        IRQ_HANDLED: interrupt handled successfully
*********************************************************/
static irqreturn_t goodix_ts_irq_handler(int irq, void *dev_id)
{
    struct goodix_ts_data *ts = dev_id;

    GTP_DEBUG_FUNC();
 
    gtp_irq_disable(ts);

    queue_work(goodix_wq, &ts->work);
    
    return IRQ_HANDLED;
}
/*******************************************************
Function:
    Synchronization.
Input:
    ms: synchronization time in millisecond.
Output:
    None.
*******************************************************/
void gtp_int_sync(s32 ms)
{
    GTP_GPIO_OUTPUT(GTP_INT_PORT, 0);
    msleep(ms);
    GTP_GPIO_AS_INT(GTP_INT_PORT);
}

/*******************************************************
Function:
    Reset chip.
Input:
    ms: reset time in millisecond
Output:
    None.
*******************************************************/
void gtp_reset_guitar(struct i2c_client *client, s32 ms)
{
    GTP_DEBUG_FUNC();

    GTP_GPIO_OUTPUT(GTP_RST_PORT, 0);   // begin select I2C slave addr

 gpio_set_value(GTP_RST_PORT,0);
    msleep(ms);                         // T2: > 10ms
    // HIGH: 0x28/0x29, LOW: 0xBA/0xBB
    GTP_GPIO_OUTPUT(GTP_INT_PORT, client->addr == 0x14);  

    msleep(2);                          // T3: > 100us
    ///GTP_GPIO_OUTPUT(GTP_RST_PORT, 1);
    gpio_set_value(GTP_RST_PORT,1);
    msleep(6); ///printk("==========%d=====\n",__LINE__);  mdelay(4000);printk("==========%d=====\n",__LINE__);
                    // T4: > 5ms
///gpio_set_value(GTP_RST_PORT,0);
  ///  msleep(6); 
 ///   gpio_set_value(GTP_RST_PORT,0);
 ///  GTP_GPIO_AS_INPUT(GTP_RST_PORT);    // end select I2C slave addr

    gtp_int_sync(50);                  
    
#if GTP_ESD_PROTECT
    gtp_init_ext_watchdog(client);
#endif
}

#if GTP_SLIDE_WAKEUP
/*******************************************************
Function:
    Enter doze mode for sliding wakeup.
Input:
    ts: goodix tp private data
Output:
    1: succeed, otherwise failed
*******************************************************/
static s8 gtp_enter_doze(struct goodix_ts_data *ts)
{
    s8 ret = -1;
    s8 retry = 0;
    u8 i2c_control_buf[3] = {(u8)(GTP_REG_SLEEP >> 8), (u8)GTP_REG_SLEEP, 8};

    GTP_DEBUG_FUNC();

#if GTP_DBL_CLK_WAKEUP
    i2c_control_buf[2] = 0x09;
#endif

    gtp_irq_disable(ts);
    
    GTP_DEBUG("entering doze mode...");
    while(retry++ < 5)
    {
        i2c_control_buf[0] = 0x80;
        i2c_control_buf[1] = 0x46;
        ret = gtp_i2c_write(ts->client, i2c_control_buf, 3);
        if (ret < 0)
        {
            GTP_DEBUG("failed to set doze flag into 0x8046, %d", retry);
            continue;
        }
        i2c_control_buf[0] = 0x80;
        i2c_control_buf[1] = 0x40;
        ret = gtp_i2c_write(ts->client, i2c_control_buf, 3);
        if (ret > 0)
        {
            doze_status = DOZE_ENABLED;
            GTP_INFO("GTP has been working in doze mode!");
            gtp_irq_enable(ts);
            return ret;
        }
        msleep(10);
    }
    GTP_ERROR("GTP send doze cmd failed.");
    gtp_irq_enable(ts);
    return ret;
}
#else 
/*******************************************************
Function:
    Enter sleep mode.
Input:
    ts: private data.
Output:
    Executive outcomes.
       1: succeed, otherwise failed.
*******************************************************/
static s8 gtp_enter_sleep(struct goodix_ts_data * ts)
{
    s8 ret = -1;
    s8 retry = 0;
    u8 i2c_control_buf[3] = {(u8)(GTP_REG_SLEEP >> 8), (u8)GTP_REG_SLEEP, 5};

    GTP_DEBUG_FUNC();
    
    GTP_GPIO_OUTPUT(GTP_INT_PORT, 0);
    msleep(5);
    
    while(retry++ < 5)
    {
        ret = gtp_i2c_write(ts->client, i2c_control_buf, 3);
        if (ret > 0)
        {
            GTP_INFO("GTP enter sleep!");
            
            return ret;
        }
        msleep(10);
    }
    GTP_ERROR("GTP send sleep cmd failed.");
    return ret;
}
#endif 
/*******************************************************
Function:
    Wakeup from sleep.
Input:
    ts: private data.
Output:
    Executive outcomes.
        >0: succeed, otherwise: failed.
*******************************************************/
static s8 gtp_wakeup_sleep(struct goodix_ts_data * ts)
{
    u8 retry = 0;
    s8 ret = -1;
    
    GTP_DEBUG_FUNC();
    
#if GTP_POWER_CTRL_SLEEP
    while(retry++ < 5)
    {
        gtp_reset_guitar(ts->client, 20);
        
        ret = gtp_send_cfg(ts->client);
        if (ret < 0)
        {
            GTP_INFO("Wakeup sleep send config failed!");
            continue;
        }
        GTP_INFO("GTP wakeup sleep");
        return 1;
    }
#else
    while(retry++ < 10)
    {
    #if GTP_SLIDE_WAKEUP
        if (DOZE_WAKEUP != doze_status)       // wakeup not by slide 
        {
            gtp_reset_guitar(ts->client, 10);
        }
        else              // wakeup by slide 
        {
            doze_status = DOZE_DISABLED;
        }
    #else
        if (chip_gt9xxs == 1)
        {
           gtp_reset_guitar(ts->client, 10);
        }
        else
        {
            GTP_GPIO_OUTPUT(GTP_INT_PORT, 1);
            msleep(5);
        }
    #endif
        ret = gtp_i2c_test(ts->client);
        if (ret > 0)
        {
            GTP_INFO("GTP wakeup sleep.");
            
        #if (!GTP_SLIDE_WAKEUP)
           if (chip_gt9xxs == 0)
            {
                gtp_int_sync(25);
                msleep(20);
            #if GTP_ESD_PROTECT
                gtp_init_ext_watchdog(ts->client);
            #endif
            }
        #endif
            return ret;
        }
        gtp_reset_guitar(ts->client, 20);
    }
#endif

    GTP_ERROR("GTP wakeup sleep failed.");
    return ret;
}

/*******************************************************
Function:
    Initialize gtp.
Input:
    ts: goodix private data
Output:
    Executive outcomes.
        0: succeed, otherwise: failed
*******************************************************/
//lusterzhang
#if 0
static s32 gtp_init_panel(struct goodix_ts_data *ts,u8 *CTP_CFG_GROUP)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;
    u8 cfg_info_group1[] = CTP_CFG_GROUP1_528;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};
    /*  
    printk("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);
*/
    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
 //   printk("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
   //     printk("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
    //                send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
    
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
 //   memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);
    memcpy(&config[GTP_ADDR_LENGTH], CTP_CFG_GROUP, ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }

    msleep(10);
    return 0;
}
#else
//lusterzhang  
static s32 gtp_init_panel_528(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_528;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else	
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif	
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_520(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_520;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
			grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}
static s32 gtp_init_panel_520W(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_520W;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
			grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}
static s32 gtp_init_panel_568(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_568;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0   
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 99)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else       
	grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
    send_cfg_buf[sensor_id][0] = 0x00;
    ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}
static s32 gtp_init_panel_568Y(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_568Y;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0   
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 99)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else       
	grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
    send_cfg_buf[sensor_id][0] = 0x00;
    ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}



static s32 gtp_init_panel_584(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_584;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else	
        grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
        send_cfg_buf[sensor_id][0] = 0x00;
       	ts->fixed_cfg = 0;		
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_585(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_585;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_586(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_586;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_589(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_589;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}
static s32 gtp_init_panel_589W(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_589W;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_591(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_591;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;	
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_591W(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_591W;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;	
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_592W(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_592W;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif

    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_592(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_592;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif

    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_594(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_594;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_596(struct goodix_ts_data *ts)   ///hangtai cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_596;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}
static s32 gtp_init_panel_596W(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_596W;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_596Z(struct goodix_ts_data *ts)   ///zc cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_596Z;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_8198D(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_8198D;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_709(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_709;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_721(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_721;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}
static s32 gtp_init_panel_721W(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_721W;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_720(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_720;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}


static s32 gtp_init_panel_701(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_701;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_708(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_708;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_702(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_702;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_722H(struct goodix_ts_data *ts)   ///ht cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_722H;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_722(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_722;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_521(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_521;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_600(struct goodix_ts_data *ts)   ///add by Jiawq
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_600;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_700(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_700;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_8198Z(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_8198Z;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_723(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
	s32 i;
	u8 check_sum = 0;
	u8 opr_buf[16];
	u8 sensor_id = 0;

	u8 cfg_info_group1[] = CTP_CFG_GROUP1_723;
	u8 cfg_info_group2[] = CTP_CFG_GROUP2;
	u8 cfg_info_group3[] = CTP_CFG_GROUP3;
	u8 cfg_info_group4[] = CTP_CFG_GROUP4;
	u8 cfg_info_group5[] = CTP_CFG_GROUP5;
	u8 cfg_info_group6[] = CTP_CFG_GROUP6;
	u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3, cfg_info_group4, cfg_info_group5, cfg_info_group6};
	u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
		CFG_GROUP_LEN(cfg_info_group2),
		CFG_GROUP_LEN(cfg_info_group3),
		CFG_GROUP_LEN(cfg_info_group4),
		CFG_GROUP_LEN(cfg_info_group5),
		CFG_GROUP_LEN(cfg_info_group6)};

	GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
			cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
			cfg_info_len[4], cfg_info_len[5]);

	ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
	if (SUCCESS == ret) 
	{
		if (opr_buf[0] != 0xBE)
		{
			ts->fw_error = 1;
			GTP_ERROR("Firmware error, no config sent!");
			return -1;
		}
	}

	if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
			(!cfg_info_len[3]) && (!cfg_info_len[4]) && 
			(!cfg_info_len[5]))
	{
		sensor_id = 0;
	}
	else
	{
		ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
		if (SUCCESS == ret)
		{
			if (sensor_id >= 0x06)
			{
				GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
				return -1;
			}
		}
		else
		{
			GTP_ERROR("Failed to get sensor_id, No config sent!");
			return -1;
		}
	}
	GTP_DEBUG("Sensor_ID: %d", sensor_id);
	ts->gtp_cfg_len = cfg_info_len[sensor_id];
	if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
	{
		GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
		return -1;
	}

	grp_cfg_version = send_cfg_buf[sensor_id][0];
	send_cfg_buf[sensor_id][0] = 0x00;
	ts->fixed_cfg = 0;
	memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
	memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
	config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
	config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
	config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);

	if (GTP_INT_TRIGGER == 0)  //RISING
	{
		config[TRIGGER_LOC] &= 0xfe; 
	}
	else if (GTP_INT_TRIGGER == 1)  //FALLING
	{
		config[TRIGGER_LOC] |= 0x01;
	}
#endif  // GTP_CUSTOM_CFG

	check_sum = 0;
	for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
	{
		check_sum += config[i];
	}
	config[ts->gtp_cfg_len] = (~check_sum) + 1;
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
	ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
	ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
	if (ret < 0)
	{
		GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
		ts->abs_x_max = GTP_MAX_WIDTH;
		ts->abs_y_max = GTP_MAX_HEIGHT;
		ts->int_trigger_type = GTP_INT_TRIGGER;
	}
#endif // GTP_DRIVER_SEND_CFG

	GTP_DEBUG_FUNC();
	if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
	{
		ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
		ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
		ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
	}
	ret = gtp_send_cfg(ts->client);
	if (ret < 0)
	{
		GTP_ERROR("Send config error.");
	}
	GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
			ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

	msleep(10);
	return 0;
}

static s32 gtp_init_panel_723W(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_723W;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_703(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_703;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_704(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_704;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}


static s32 gtp_init_panel_529(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_529;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}
static s32 gtp_init_panel_529W(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_529W;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}


static s32 gtp_init_panel_735(struct goodix_ts_data *ts)   ///wr cfg
{
	s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
	s32 i;
	u8 check_sum = 0;
	u8 opr_buf[16];
	u8 sensor_id = 0;

	u8 cfg_info_group1[] = CTP_CFG_GROUP1_735;
	u8 cfg_info_group2[] = CTP_CFG_GROUP2;
	u8 cfg_info_group3[] = CTP_CFG_GROUP3;
	u8 cfg_info_group4[] = CTP_CFG_GROUP4;
	u8 cfg_info_group5[] = CTP_CFG_GROUP5;
	u8 cfg_info_group6[] = CTP_CFG_GROUP6;
	u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3, cfg_info_group4, cfg_info_group5, cfg_info_group6};
	u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1), CFG_GROUP_LEN(cfg_info_group2), CFG_GROUP_LEN(cfg_info_group3), CFG_GROUP_LEN(cfg_info_group4), CFG_GROUP_LEN(cfg_info_group5), CFG_GROUP_LEN(cfg_info_group6)};
	GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d",
			cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
			cfg_info_len[4], cfg_info_len[5]);

	ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
	if (SUCCESS == ret) 
	{
		if (opr_buf[0] != 0xBE)
		{
			ts->fw_error = 1;
			GTP_ERROR("Firmware error, no config sent!");
			return -1;
		}
	}

	if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && (!cfg_info_len[3]) && (!cfg_info_len[4]) && (!cfg_info_len[5]))
	{
		sensor_id = 0;
	}
	else
	{
		ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
		if (SUCCESS == ret)
		{
			if (sensor_id >= 0x06)
			{
				GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
				return -1;
			}
		}
		else
		{
			GTP_ERROR("Failed to get sensor_id, No config sent!");
			return -1;
		}
	}
	GTP_DEBUG("Sensor_ID: %d", sensor_id);
	ts->gtp_cfg_len = cfg_info_len[sensor_id];
	if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
	{
		GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
		return -1;
	}

	grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
	send_cfg_buf[sensor_id][0] = 0x00;
	ts->fixed_cfg = 0;

	memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
	memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
	config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
	config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
	config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
	config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);

	if (GTP_INT_TRIGGER == 0)  //RISING
	{
		config[TRIGGER_LOC] &= 0xfe; 
	}
	else if (GTP_INT_TRIGGER == 1)  //FALLING
	{
		config[TRIGGER_LOC] |= 0x01;
	}
#endif  // GTP_CUSTOM_CFG

	check_sum = 0;
	for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
	{
		check_sum += config[i];
	}
	config[ts->gtp_cfg_len] = (~check_sum) + 1;

#else
	ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
	ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
	if (ret < 0)
	{
		GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
		ts->abs_x_max = GTP_MAX_WIDTH;
		ts->abs_y_max = GTP_MAX_HEIGHT;
		ts->int_trigger_type = GTP_INT_TRIGGER;
	}
#endif // GTP_DRIVER_SEND_CFG

	GTP_DEBUG_FUNC();
	if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
	{
		ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
		ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
		ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03;
	}
	ret = gtp_send_cfg(ts->client);
	if (ret < 0)
	{
		GTP_ERROR("Send config error.");
	}
	GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
			ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

	msleep(10);
	return 0;
}

static s32 gtp_init_panel_735W(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_735W;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_724(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_724;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}


static s32 gtp_init_panel_738(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_738;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_765(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_765;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_764(struct goodix_ts_data *ts)   ///wr cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_764;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_737(struct goodix_ts_data *ts)
{
	s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
	s32 i;
	u8 check_sum = 0;
	u8 opr_buf[16];
	u8 sensor_id = 0;

	u8 cfg_info_group1[] = CTP_CFG_GROUP1_737;
	u8 cfg_info_group2[] = CTP_CFG_GROUP2;
	u8 cfg_info_group3[] = CTP_CFG_GROUP3;
	u8 cfg_info_group4[] = CTP_CFG_GROUP4;
	u8 cfg_info_group5[] = CTP_CFG_GROUP5;
	u8 cfg_info_group6[] = CTP_CFG_GROUP6;
	u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
		cfg_info_group4, cfg_info_group5, cfg_info_group6};
	u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
		                          CFG_GROUP_LEN(cfg_info_group2),
								  CFG_GROUP_LEN(cfg_info_group3),
								  CFG_GROUP_LEN(cfg_info_group4),
								  CFG_GROUP_LEN(cfg_info_group5),
								  CFG_GROUP_LEN(cfg_info_group6)};

	GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d",
			cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
			cfg_info_len[4], cfg_info_len[5]);
	
	ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
	if (SUCCESS == ret) 
	{
		if (opr_buf[0] != 0xBE)
		{
			ts->fw_error = 1;
			GTP_ERROR("Firmware error, no config sent!");
			return -1;
		}
	}

	if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
			(!cfg_info_len[3]) && (!cfg_info_len[4]) && 
			(!cfg_info_len[5]))
	{
		sensor_id = 0; 
	}
	else
	{
		ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
		if (SUCCESS == ret)
		{
			if (sensor_id >= 0x06)
			{
				GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
				return -1;
			}
		}
		else
		{
			GTP_ERROR("Failed to get sensor_id, No config sent!");
			return -1;
		}
	}
	GTP_DEBUG("Sensor_ID: %d", sensor_id);

	ts->gtp_cfg_len = cfg_info_len[sensor_id];

	if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
	{
		GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
		return -1;
	}
	grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
	send_cfg_buf[sensor_id][0] = 0x00;
	ts->fixed_cfg = 0;
	memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
	memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
	config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
	config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
	config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
	config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);

	if (GTP_INT_TRIGGER == 0)  //RISING
	{
		config[TRIGGER_LOC] &= 0xfe; 
	}
	else if (GTP_INT_TRIGGER == 1)  //FALLING
	{
		config[TRIGGER_LOC] |= 0x01;
	}
#endif

	check_sum = 0;
	for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
	{
		check_sum += config[i];
	}
	config[ts->gtp_cfg_len] = (~check_sum) + 1;
#else
	ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH;
	ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
	if (ret < 0)
	{
		GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
		ts->abs_x_max = GTP_MAX_WIDTH;
		ts->abs_y_max = GTP_MAX_HEIGHT;
		ts->int_trigger_type = GTP_INT_TRIGGER;
	}
#endif

	GTP_DEBUG_FUNC();
	if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
	{
		ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
		ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
		ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
	}
	ret = gtp_send_cfg(ts->client);
	if (ret < 0)
	{
		GTP_ERROR("Send config error.");
	}
	GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
			ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

	msleep(10);
	return 0;
}

static s32 gtp_init_panel_737L(struct goodix_ts_data *ts)
{
	s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
	s32 i;
	u8 check_sum = 0;
	u8 opr_buf[16];
	u8 sensor_id = 0;

	u8 cfg_info_group1[] = CTP_CFG_GROUP1_737L;
	u8 cfg_info_group2[] = CTP_CFG_GROUP2;
	u8 cfg_info_group3[] = CTP_CFG_GROUP3;
	u8 cfg_info_group4[] = CTP_CFG_GROUP4;
	u8 cfg_info_group5[] = CTP_CFG_GROUP5;
	u8 cfg_info_group6[] = CTP_CFG_GROUP6;
	u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
		cfg_info_group4, cfg_info_group5, cfg_info_group6};
	u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
		                          CFG_GROUP_LEN(cfg_info_group2),
								  CFG_GROUP_LEN(cfg_info_group3),
								  CFG_GROUP_LEN(cfg_info_group4),
								  CFG_GROUP_LEN(cfg_info_group5),
								  CFG_GROUP_LEN(cfg_info_group6)};

	GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d",
			cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
			cfg_info_len[4], cfg_info_len[5]);
	
	ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
	if (SUCCESS == ret) 
	{
		if (opr_buf[0] != 0xBE)
		{
			ts->fw_error = 1;
			GTP_ERROR("Firmware error, no config sent!");
			return -1;
		}
	}

	if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
			(!cfg_info_len[3]) && (!cfg_info_len[4]) && 
			(!cfg_info_len[5]))
	{
		sensor_id = 0; 
	}
	else
	{
		ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
		if (SUCCESS == ret)
		{
			if (sensor_id >= 0x06)
			{
				GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
				return -1;
			}
		}
		else
		{
			GTP_ERROR("Failed to get sensor_id, No config sent!");
			return -1;
		}
	}
	GTP_DEBUG("Sensor_ID: %d", sensor_id);

	ts->gtp_cfg_len = cfg_info_len[sensor_id];

	if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
	{
		GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
		return -1;
	}
	grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
	send_cfg_buf[sensor_id][0] = 0x00;
	ts->fixed_cfg = 0;
	memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
	memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
	config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
	config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
	config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
	config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);

	if (GTP_INT_TRIGGER == 0)  //RISING
	{
		config[TRIGGER_LOC] &= 0xfe; 
	}
	else if (GTP_INT_TRIGGER == 1)  //FALLING
	{
		config[TRIGGER_LOC] |= 0x01;
	}
#endif

	check_sum = 0;
	for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
	{
		check_sum += config[i];
	}
	config[ts->gtp_cfg_len] = (~check_sum) + 1;
#else
	ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH;
	ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
	if (ret < 0)
	{
		GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
		ts->abs_x_max = GTP_MAX_WIDTH;
		ts->abs_y_max = GTP_MAX_HEIGHT;
		ts->int_trigger_type = GTP_INT_TRIGGER;
	}
#endif

	GTP_DEBUG_FUNC();
	if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
	{
		ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
		ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
		ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
	}
	ret = gtp_send_cfg(ts->client);
	if (ret < 0)
	{
		GTP_ERROR("Send config error.");
	}
	GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
			ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

	msleep(10);
	return 0;
}

static s32 gtp_init_panel_900(struct goodix_ts_data *ts)   ///ht cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_900;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_6718(struct goodix_ts_data *ts)   ///zc cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_6718;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_781(struct goodix_ts_data *ts)   ///zc cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_781;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_787(struct goodix_ts_data *ts)   ///zhongpei中沛 cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_787;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 7;
}

static s32 gtp_init_panel_789(struct goodix_ts_data *ts)   ///zc cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_789;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_791(struct goodix_ts_data *ts)   ///zc cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_791;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_792(struct goodix_ts_data *ts)   ///zc cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_792;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_793(struct goodix_ts_data *ts)   ///zc cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_793;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_794(struct goodix_ts_data *ts)   ///zc cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_794;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_798(struct goodix_ts_data *ts)   ///zhongchu cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_798;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_799(struct goodix_ts_data *ts)   ///hangtai cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_799;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_800(struct goodix_ts_data *ts)   ///zb cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_800;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_534(struct goodix_ts_data *ts)   ///zc cfg
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i;
    u8 check_sum = 0;
    u8 opr_buf[16];
    u8 sensor_id = 0;

    u8 cfg_info_group1[] = CTP_CFG_GROUP1_534;
    u8 cfg_info_group2[] = CTP_CFG_GROUP2;
    u8 cfg_info_group3[] = CTP_CFG_GROUP3;
    u8 cfg_info_group4[] = CTP_CFG_GROUP4;
    u8 cfg_info_group5[] = CTP_CFG_GROUP5;
    u8 cfg_info_group6[] = CTP_CFG_GROUP6;
    u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
                        cfg_info_group4, cfg_info_group5, cfg_info_group6};
    u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
                          CFG_GROUP_LEN(cfg_info_group2),
                          CFG_GROUP_LEN(cfg_info_group3),
                          CFG_GROUP_LEN(cfg_info_group4),
                          CFG_GROUP_LEN(cfg_info_group5),
                          CFG_GROUP_LEN(cfg_info_group6)};

    GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
        cfg_info_len[4], cfg_info_len[5]);

    ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
    if (SUCCESS == ret) 
    {
        if (opr_buf[0] != 0xBE)
        {
            ts->fw_error = 1;
            GTP_ERROR("Firmware error, no config sent!");
            return -1;
        }
    }

    if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
        (!cfg_info_len[3]) && (!cfg_info_len[4]) && 
        (!cfg_info_len[5]))
    {
        sensor_id = 0; 
    }
    else
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0x06)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
                return -1;
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            return -1;
        }
    }
    GTP_DEBUG("Sensor_ID: %d", sensor_id);
    
    ts->gtp_cfg_len = cfg_info_len[sensor_id];
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
        return -1;
    }
#if 0    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
    
    if (ret == SUCCESS)
    {
        GTP_DEBUG("CFG_GROUP%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id+1, 
                    send_cfg_buf[sensor_id][0], send_cfg_buf[sensor_id][0], opr_buf[0], opr_buf[0]);
        
        if (opr_buf[0] < 90)    
        {
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
        }
        else        // treated as fixed config, not send config
        {
            GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
            ts->fixed_cfg = 1;
        }
    }
    else
    {
        GTP_ERROR("Failed to get ic config version!No config sent!");
        return -1;
    }
#else    
            grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
            send_cfg_buf[sensor_id][0] = 0x00;
            ts->fixed_cfg = 0;
#endif
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;
    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {                          
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
#endif // GTP_DRIVER_SEND_CFG

    GTP_DEBUG_FUNC();
    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }
    ret = gtp_send_cfg(ts->client);
    if (ret < 0)
    {
        GTP_ERROR("Send config error.");
    }
    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

    msleep(10);
    return 0;
}

static s32 gtp_init_panel_535(struct goodix_ts_data *ts)
{
	s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
	s32 i;
	u8 check_sum = 0;
	u8 opr_buf[16];
	u8 sensor_id = 0;

	u8 cfg_info_group1[] = CTP_CFG_GROUP1_535;
	u8 cfg_info_group2[] = CTP_CFG_GROUP2;
	u8 cfg_info_group3[] = CTP_CFG_GROUP3;
	u8 cfg_info_group4[] = CTP_CFG_GROUP4;
	u8 cfg_info_group5[] = CTP_CFG_GROUP5;
	u8 cfg_info_group6[] = CTP_CFG_GROUP6;
	u8 *send_cfg_buf[] = {cfg_info_group1, cfg_info_group2, cfg_info_group3,
		cfg_info_group4, cfg_info_group5, cfg_info_group6};
	u8 cfg_info_len[] = { CFG_GROUP_LEN(cfg_info_group1),
		CFG_GROUP_LEN(cfg_info_group2),
		CFG_GROUP_LEN(cfg_info_group3),
		CFG_GROUP_LEN(cfg_info_group4),
		CFG_GROUP_LEN(cfg_info_group5),
		CFG_GROUP_LEN(cfg_info_group6)};

	GTP_DEBUG("Config Groups\' Lengths: %d, %d, %d, %d, %d, %d", 
			        cfg_info_len[0], cfg_info_len[1], cfg_info_len[2], cfg_info_len[3],
					cfg_info_len[4], cfg_info_len[5]);

	ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
	if (SUCCESS == ret) 
	{
		if (opr_buf[0] != 0xBE)
		{
			ts->fw_error = 1;
			GTP_ERROR("Firmware error, no config sent!");
			return -1;
		}
	}

	if ((!cfg_info_len[1]) && (!cfg_info_len[2]) && 
		(!cfg_info_len[3]) && (!cfg_info_len[4]) && 
		(!cfg_info_len[5]))
	{
		sensor_id = 0; 
	}
	else
	{
		ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
		if (SUCCESS == ret)
		{
			if (sensor_id >= 0x06)
			{
				GTP_ERROR("Invalid sensor_id(0x%02X), No Config Sent!", sensor_id);
				return -1;
			}
		}
		else
		{
			GTP_ERROR("Failed to get sensor_id, No config sent!");
			return -1;
		}
	}
	GTP_DEBUG("Sensor_ID: %d", sensor_id);

	ts->gtp_cfg_len = cfg_info_len[sensor_id];
	    
	if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
	{
		GTP_ERROR("Sensor_ID(%d) matches with NULL or INVALID CONFIG GROUP! NO Config Sent! You need to check you header file CFG_GROUP section!", sensor_id);
		return -1;
	}

	grp_cfg_version = send_cfg_buf[sensor_id][0];       // backup group config version
	send_cfg_buf[sensor_id][0] = 0x00;
	ts->fixed_cfg = 0;
	
	memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
	    memcpy(&config[GTP_ADDR_LENGTH], send_cfg_buf[sensor_id], ts->gtp_cfg_len);

#if GTP_CUSTOM_CFG
		    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
			    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
				    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
					    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
						    
						    if (GTP_INT_TRIGGER == 0)  //RISING
								    {
										        config[TRIGGER_LOC] &= 0xfe; 
												    }
							    else if (GTP_INT_TRIGGER == 1)  //FALLING
									    {
											        config[TRIGGER_LOC] |= 0x01;
													    }
#endif  // GTP_CUSTOM_CFG
								    
								    check_sum = 0;
									    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
											    {
													        check_sum += config[i];
															    }
										    config[ts->gtp_cfg_len] = (~check_sum) + 1;
											    
#else // DRIVER NOT SEND CONFIG  ///GTP_DRIVER_SEND_CFG
											    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH; /// the arg3 is the len of agr2,not the read data len
												    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
													    if (ret < 0)
															    {                          
																	        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
																			        ts->abs_x_max = GTP_MAX_WIDTH;
																					        ts->abs_y_max = GTP_MAX_HEIGHT;
																							        ts->int_trigger_type = GTP_INT_TRIGGER;
																									    }
#endif // GTP_DRIVER_SEND_CFG

														    GTP_DEBUG_FUNC();
															    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
																	    {
																			        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
																					        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
																							        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
																									    }
																    ret = gtp_send_cfg(ts->client);
																	    if (ret < 0)
																			    {
																					        GTP_ERROR("Send config error.");
																							    }
																		    GTP_DEBUG("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
																					        ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);

																			    msleep(10);
																				    return 0;
}

#endif
/*******************************************************
Function:
    Read chip version.
Input:
    client:  i2c device
    version: buffer to keep ic firmware version
Output:
    read operation return.
        2: succeed, otherwise: failed
*******************************************************/
s32 gtp_read_version(struct i2c_client *client, u16* version)
{
    s32 ret = -1;
    u8 buf[8] = {GTP_REG_VERSION >> 8, GTP_REG_VERSION & 0xff};

    GTP_DEBUG_FUNC();

    ret = gtp_i2c_read(client, buf, sizeof(buf));
    if (ret < 0)
    {
        GTP_ERROR("GTP read version failed");
        return ret;
    }

    if (version)
    {
        *version = (buf[7] << 8) | buf[6];
    }
    
    if (buf[5] == 0x00)
    {
        printk("IC Version1: %c%c%c_%02x%02x", buf[2], buf[3], buf[4], buf[7], buf[6]);
    }
    else
    {
        if (buf[5] == 'S' || buf[5] == 's')
        {
            chip_gt9xxs = 1;
        }
        printk("IC Version2: %c%c%c%c_%02x%02x", buf[2], buf[3], buf[4], buf[5], buf[7], buf[6]);
    }
    return ret;
}

/*******************************************************
Function:
    I2c test Function.
Input:
    client:i2c client.
Output:
    Executive outcomes.
        2: succeed, otherwise failed.
*******************************************************/
static s8 gtp_i2c_test(struct i2c_client *client)
{
    u8 test[3] = {GTP_REG_CONFIG_DATA >> 8, GTP_REG_CONFIG_DATA & 0xff};
    u8 retry = 0;
    s8 ret = -1;
  
    GTP_DEBUG_FUNC();
  
    while(retry++ < 5)
    {
        ret = gtp_i2c_read(client, test, 3);
        if (ret > 0)
        {
            return ret;
        }
        GTP_ERROR("GTP i2c test failed time %d.",retry);
        msleep(10);
    }
    return ret;
}

/*******************************************************
Function:
    Request gpio(INT & RST) ports.
Input:
    ts: private data.
Output:
    Executive outcomes.
        >= 0: succeed, < 0: failed
GTP_INT_PORT
GTP_INT_IRQ gpio_to_irq(GTP_INT_PORT)
*******************************************************/
static s8 gtp_request_io_port(struct goodix_ts_data *ts)
{
    s32 ret = 0;

    ret = GTP_GPIO_REQUEST(GTP_INT_PORT, "GTP_INT_IRQ");
	/// gpio_request
    if (ret < 0) 
    {	nwd_dbg_tp();
        GTP_ERROR("Failed to request GPIO:%d, ERRNO:%d", (s32)GTP_INT_PORT, ret);
        ret = -ENODEV;
    }
    else
    {   nwd_dbg_tp();
        GTP_GPIO_AS_INT(GTP_INT_PORT);  
        ts->client->irq = GTP_INT_IRQ;///in .h have gpio_to_irq
    }



    ret = GTP_GPIO_REQUEST(GTP_RST_PORT, "GTP_RST_PORT");
    if (ret < 0) 
    {	nwd_dbg_tp();gpio_free(GTP_RST_PORT);
    		ret = GTP_GPIO_REQUEST(GTP_RST_PORT, "GTP_RST_PORT");
    		if (ret < 0) {
        	GTP_ERROR("Failed to request GPIO:%d, ERRNO:%d",(s32)GTP_RST_PORT,ret);
       		 ret = -ENODEV;
	}
    }

  ///  GTP_GPIO_AS_INPUT(GTP_RST_PORT); ///OUTPUT
    gtp_reset_guitar(ts->client, 20);
   nwd_dbg_tp();
    
    if(ret < 0)
    { nwd_dbg_tp();
        GTP_GPIO_FREE(GTP_RST_PORT);
        GTP_GPIO_FREE(GTP_INT_PORT);
    }

    return ret;
}

/*******************************************************
Function:
    Request interrupt.
Input:
    ts: private data.
Output:
    Executive outcomes.
        0: succeed, -1: failed.
*******************************************************/
static s8 gtp_request_irq(struct goodix_ts_data *ts)
{
    s32 ret = -1;
    const u8 irq_table[] = GTP_IRQ_TAB;

    GTP_DEBUG("INT trigger type:%x", ts->int_trigger_type);

    ret  = request_irq(ts->client->irq, 
                       goodix_ts_irq_handler,
                      irq_table[0],     ///  irq_table[ts->int_trigger_type],
                       ts->client->name,
                       ts);
    if (ret)
    {
        GTP_ERROR("Request IRQ failed!ERRNO:%d.", ret);
        GTP_GPIO_AS_INPUT(GTP_INT_PORT);
        GTP_GPIO_FREE(GTP_INT_PORT);

        hrtimer_init(&ts->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
        ts->timer.function = goodix_ts_timer_handler;
        hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
        return -1;
    }
    else 
    {
        gtp_irq_disable(ts);
        ts->use_irq = 1;
        return 0;
    }
}

/*******************************************************
Function:
    Request input device Function.
Input:
    ts:private data.
Output:
    Executive outcomes.
        0: succeed, otherwise: failed.
*******************************************************/
static s8 gtp_request_input_dev(struct goodix_ts_data *ts)
{
    s8 ret = -1;
    s8 phys[32];
#if GTP_HAVE_TOUCH_KEY
    u8 index = 0;
#endif
  
    GTP_DEBUG_FUNC();
  
    ts->input_dev = input_allocate_device();
    if (ts->input_dev == NULL)
    {
        GTP_ERROR("Failed to allocate input device.");
        return -ENOMEM;
    }
    input_dev_g = ts->input_dev;
    ts->input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS) ;
#if GTP_ICS_SLOT_REPORT
    __set_bit(INPUT_PROP_DIRECT, ts->input_dev->propbit);
    input_mt_init_slots(ts->input_dev, 10);     // in case of "out of memory"
    ///input_mt_init_slots(ts->input_dev, ts->max_touch_num); ///rk drv
#else
    ts->input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
#endif

#if GTP_HAVE_TOUCH_KEY
    for (index = 0; index < GTP_MAX_KEY_NUM; index++)
    {
        input_set_capability(ts->input_dev, EV_KEY, touch_key_array[index]);  
    }
#endif

#if GTP_SLIDE_WAKEUP
    input_set_capability(ts->input_dev, EV_KEY, KEY_POWER);
#endif 
 	input_set_capability(ts->input_dev, EV_KEY, 252);
 	 	input_set_capability(input_dev_g, EV_KEY, 252);
#if GTP_WITH_PEN
    // pen support
    __set_bit(BTN_TOOL_PEN, ts->input_dev->keybit);
    __set_bit(INPUT_PROP_DIRECT, ts->input_dev->propbit);
    __set_bit(INPUT_PROP_POINTER, ts->input_dev->propbit);
#endif

#if GTP_CHANGE_X2Y
    GTP_SWAP(ts->abs_x_max, ts->abs_y_max);
#endif

    input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, ts->abs_x_max, 0, 0);
    input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, ts->abs_y_max, 0, 0);
    input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);
    input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
    input_set_abs_params(ts->input_dev, ABS_MT_TRACKING_ID, 0, 255, 0, 0);

    sprintf(phys, "input/ts");
    ts->input_dev->name = goodix_ts_name;
    ts->input_dev->phys = phys;
    ts->input_dev->id.bustype = BUS_I2C;
    ts->input_dev->id.vendor = 0xDEAD;
    ts->input_dev->id.product = 0xBEEF;
    ts->input_dev->id.version = 10427;

    	 ts->input_dev->id.bustype = BUS_HOST;
	 ts->input_dev->id.vendor = 0x0001;
	  ts->input_dev->id.product = 0x0001;
	 ts->input_dev->id.version = 0x0100;

    ret = input_register_device(ts->input_dev);
    if (ret)
    {
        GTP_ERROR("Register %s input device failed", ts->input_dev->name);
        return -ENODEV;
    }
    
#ifdef CONFIG_HAS_EARLYSUSPEND
    ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
    ts->early_suspend.suspend = goodix_ts_early_suspend;
    ts->early_suspend.resume = goodix_ts_late_resume;
    register_early_suspend(&ts->early_suspend);
#endif

    return 0;
}
//extern unsigned char RunSystemTouchFlag;
//extern unsigned char RunSystemTouchBuf[4];
static unsigned int touchsreen_event_handler(void *data)
{
    struct goodix_ts_data *ts = (struct goodix_ts_data *)data;
/*
    u8 cfg_info_group1_528[]= CTP_CFG_GROUP1_528;
    u8 cfg_info_group1_520[]= CTP_CFG_GROUP1_520;
    u8 cfg_info_group1_568[]= CTP_CFG_GROUP1_568;
    u8 cfg_info_group1_584[]= CTP_CFG_GROUP1_584;
    u8 cfg_info_group1_585[]= CTP_CFG_GROUP1_585;
    u8 cfg_info_group1_586[]= CTP_CFG_GROUP1_586;
    u8 cfg_info_group1_589[]= CTP_CFG_GROUP1_589;
    u8 cfg_info_group1_591[]= CTP_CFG_GROUP1_591;
    u8 cfg_info_group1_592[]= CTP_CFG_GROUP1_592;
    u8 cfg_info_group1_594[]= CTP_CFG_GROUP1_594;
    u8 cfg_info_group1_596[]= CTP_CFG_GROUP1_596;
    u8 cfg_info_group1_8198[]= CTP_CFG_GROUP1_8198;
*/     
    while(1)
    {
	if(RunSystemTouchFlag==0x55)
	{
/*
	    printk("touchsreen_event_handle %s............\n",RunSystemTouchBuf);
	    if(memcmp(RunSystemTouchBuf,"528",3)==0)
	    {
		gtp_init_panel(ts,cfg_info_group1_528);
	    }
	    else if(memcmp(RunSystemTouchBuf,"520",3)==0)
	    {
		gtp_init_panel(ts,cfg_info_group1_520);
	    }
	    else if(memcmp(RunSystemTouchBuf,"568",3)==0)
	    {
		gtp_init_panel(ts,cfg_info_group1_568);
	    }
	    else if(memcmp(RunSystemTouchBuf,"584",3)==0)
            {
                gtp_init_panel(ts,cfg_info_group1_584);
            }
	    else if(memcmp(RunSystemTouchBuf,"585",3)==0)
            {
                gtp_init_panel(ts,cfg_info_group1_585);
            }
	    else if(memcmp(RunSystemTouchBuf,"586",3)==0)
            {
                gtp_init_panel(ts,cfg_info_group1_586);
            }
	    else if(memcmp(RunSystemTouchBuf,"589",3)==0)
            {
                gtp_init_panel(ts,cfg_info_group1_589);
            }
	    else if(memcmp(RunSystemTouchBuf,"591",3)==0)
            {
                gtp_init_panel(ts,cfg_info_group1_591);
            }
	    else if(memcmp(RunSystemTouchBuf,"592",3)==0)
            {
                gtp_init_panel(ts,cfg_info_group1_592);
            }
	    else if(memcmp(RunSystemTouchBuf,"594",3)==0)
            {
                gtp_init_panel(ts,cfg_info_group1_594);
            }
	    else if(memcmp(RunSystemTouchBuf,"596",3)==0)
            {
                gtp_init_panel(ts,cfg_info_group1_596);
            }
	    else if(memcmp(RunSystemTouchBuf,"8198",4)==0)
	    {
		gtp_init_panel(ts,cfg_info_group1_8198);
	    }
	    else 
	    {
		gtp_init_panel(ts,cfg_info_group1_528);
	    }
*/	
            printk("touchsreen_event_handle %s............\n",RunSystemTouchBuf);
	    if(memcmp(RunSystemTouchBuf,"528",3)==0)
	    {
		gtp_init_panel_528(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"520W",4)==0)
	    {
		gtp_init_panel_520W(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"520",3)==0)
	    {
		gtp_init_panel_520(ts);
	    }
            else if(memcmp(RunSystemTouchBuf,"568Y",4)==0)
            {
                gtp_init_panel_568Y(ts);
            }
	    else if(memcmp(RunSystemTouchBuf,"568",3)==0)
	    {
		gtp_init_panel_568(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"584",3)==0)
            {
                gtp_init_panel_584(ts);
            }
	    else if(memcmp(RunSystemTouchBuf,"585",3)==0)
            {
                gtp_init_panel_585(ts);
            }
	    else if(memcmp(RunSystemTouchBuf,"586",3)==0)
            {
                gtp_init_panel_586(ts);
            }
	    else if(memcmp(RunSystemTouchBuf,"589W",4)==0)
            {
                gtp_init_panel_589W(ts);
            }
	    else if(memcmp(RunSystemTouchBuf,"589",3)==0)
            {
                gtp_init_panel_589(ts);
            }
	    else if(memcmp(RunSystemTouchBuf,"591W",4)==0)
            {
                gtp_init_panel_591W(ts);
            }
	    else if(memcmp(RunSystemTouchBuf,"591",3)==0)
            {
                gtp_init_panel_591(ts);
            }	    
	    else if(memcmp(RunSystemTouchBuf,"592W",4)==0)
            {
                gtp_init_panel_592W(ts);
            }

	    else if(memcmp(RunSystemTouchBuf,"592",3)==0)
            {
                gtp_init_panel_592(ts);
            }
	    else if(memcmp(RunSystemTouchBuf,"594",3)==0)
            {
                gtp_init_panel_594(ts);
            }
	    else if(memcmp(RunSystemTouchBuf,"596W",4)==0)
            {
                gtp_init_panel_596W(ts);
            }
	    else if(memcmp(RunSystemTouchBuf,"596Z",4)==0)
	    {
			gtp_init_panel_596Z(ts);
	    }

	    else if(memcmp(RunSystemTouchBuf,"596",3)==0)
            {
                gtp_init_panel_596(ts);
            }
	    else if(memcmp(RunSystemTouchBuf,"8198D",5)==0)
	    {
		gtp_init_panel_8198D(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"709",3)==0)
	    {
			gtp_init_panel_709(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"721W",4)==0)
	    {
			gtp_init_panel_721W(ts);
	    }
            else if(memcmp(RunSystemTouchBuf,"721",3)==0)
	    {
			gtp_init_panel_721(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"720",3)==0)
	    {
			gtp_init_panel_720(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"701",3)==0)
	    {
			gtp_init_panel_701(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"708",3)==0)
	    {
			gtp_init_panel_708(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"702",3)==0)
	    {
			gtp_init_panel_702(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"722H",4)==0)
	    {
			gtp_init_panel_722H(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"722",3)==0)
	    {
			gtp_init_panel_722(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"521",3)==0)
	    {
			gtp_init_panel_521(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"600",3)==0)
	    {
			gtp_init_panel_600(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"700",3)==0)
	    {
			gtp_init_panel_700(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"8198Z",5)==0)
	    {
			gtp_init_panel_8198Z(ts);
	    }
                else if(memcmp(RunSystemTouchBuf,"723W",4)==0)
                {
                        gtp_init_panel_723W(ts);
                }
	    else if(memcmp(RunSystemTouchBuf,"723",3)==0)
	    {
			gtp_init_panel_723(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"703",3)==0)
	    {
			gtp_init_panel_703(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"704",3)==0)
	    {
			gtp_init_panel_704(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"529W",4)==0)
	    {
			gtp_init_panel_529W(ts);
	    }	    
            else if(memcmp(RunSystemTouchBuf,"529",3)==0)
	    {
			gtp_init_panel_529(ts);
	    }	    
	    else if(memcmp(RunSystemTouchBuf,"735W",4)==0)
	    {
			gtp_init_panel_735W(ts);
	    }

		else if(memcmp(RunSystemTouchBuf,"735",3)==0)
		{
			gtp_init_panel_735(ts);
		}
	    else if(memcmp(RunSystemTouchBuf,"724",3)==0)
	    {
			gtp_init_panel_724(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"738",3)==0)
	    {
			gtp_init_panel_738(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"765",3)==0)
	    {
			gtp_init_panel_765(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"764",3)==0)
	    {
			gtp_init_panel_764(ts);
	    }
		else if(memcmp(RunSystemTouchBuf,"737L",4)==0)
		{
			gtp_init_panel_737L(ts);
		}
	    else if(memcmp(RunSystemTouchBuf,"737",3)==0)
	    {
		gtp_init_panel_737(ts);
	    }
		else if(memcmp(RunSystemTouchBuf,"534",3)==0)
		{
			gtp_init_panel_534(ts);
		}
	    else if(memcmp(RunSystemTouchBuf,"535",3)==0)
	    {
		gtp_init_panel_535(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"900",3)==0)
	    {
			gtp_init_panel_900(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"6718",4)==0)
	    {
			gtp_init_panel_6718(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"781",3)==0)
	    {
			gtp_init_panel_781(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"787",3)==0)
	    {
			gtp_init_panel_787(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"789",3)==0)
	    {
			gtp_init_panel_789(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"791",3)==0)
	    {
			gtp_init_panel_791(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"792",3)==0)
	    {
			gtp_init_panel_792(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"793",3)==0)
	    {
			gtp_init_panel_793(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"794",3)==0)
	    {
			gtp_init_panel_794(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"798",3)==0)
	    {
			gtp_init_panel_798(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"799",3)==0)
	    {
			gtp_init_panel_799(ts);
	    }
	    else if(memcmp(RunSystemTouchBuf,"800",3)==0)
	    {
			gtp_init_panel_800(ts);
	    }
	    else 
	    {
		gtp_init_panel_528(ts);
	    }

    	    msleep(500);
            if (ts->use_irq)
    	    {
                gtp_irq_enable(ts);
    	    }
	    break;
    	}
	msleep(500);
    }
    return 0;	
}

/*******************************************************
Function:
    I2c probe.
Input:
    client: i2c device struct.
    id: device id.
Output:
    Executive outcomes. 
        0: succeed.
*******************************************************/
static int goodix_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    s32 ret = -1;
    struct goodix_ts_data *ts;
    u16 version_info;
    struct goodix_platform_data *pdata ; 
    setup_timer( &tpsl_timer, tpsl_timer_callback, 0 );
    setup_timer( &tpsl_timerhome, tpsl_timer_callbackhome, 0 );
    setup_timer( &tpsl_timer_backmenu_afterup, tpsl_timer_callbackmenu_back, 0 );
    setup_timer( &Timer_keyboard_6718, timer_callbackmenu_6718, 0 );
    i2c_connect_client = client;
    
    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) 
    {
        GTP_ERROR("I2C check functionality failed.");
        return -ENODEV;
    }
    ts = kzalloc(sizeof(*ts), GFP_KERNEL);
    if (ts == NULL)
    {
        GTP_ERROR("Alloc GFP_KERNEL memory failed.");
        return -ENOMEM;
    }
   pdata = client->dev.platform_data; ///
    memset(ts, 0, sizeof(*ts));
    INIT_WORK(&ts->work, goodix_ts_work_func);
    ts->client = client;
    spin_lock_init(&ts->irq_lock);          // 2.6.39 later
    // ts->irq_lock = SPIN_LOCK_UNLOCKED;   // 2.6.39 & before
    i2c_set_clientdata(client, ts);
    
    ts->gtp_rawdiff_mode = 0;

	if (pdata->init_platform_hw)   
	{
	   pdata->init_platform_hw();
	}



	input_dev_g = ts->input_dev;



    ret = gtp_request_io_port(ts);
    if (ret < 0)
    {
        GTP_ERROR("GTP request IO port failed.");
        kfree(ts);
        return ret;
    }
/*
    nwd_dbg_tp();
//    ret = gtp_init_panel(ts);
    if (ret < 0)
    {
        GTP_ERROR("GTP init panel failed.");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
*/
        ts->abs_x_max = 1024;
        ts->abs_y_max = 600;
        ts->int_trigger_type = GTP_INT_TRIGGER;

	nwd_dbg_tp();
    ret = gtp_request_input_dev(ts);
    if (ret < 0)
    {
        GTP_ERROR("GTP request input dev failed");
    }
    	nwd_dbg_tp();
    ret = gtp_request_irq(ts); 
    if (ret < 0)
    {	nwd_dbg_tp();
        GTP_INFO("GTP works in polling mode.");
    }
    else
    {	nwd_dbg_tp();
        GTP_INFO("GTP works in interrupt mode.");
    }
    
    kernel_thread(touchsreen_event_handler, ts, 0);//add by lusterzhang
/*
    if (ts->use_irq)
    {
        gtp_irq_enable(ts);
    }
    nwd_dbg_tp();
*/
#if GTP_CREATE_WR_NODE
    init_wr_node(client);
#endif
    
#if GTP_ESD_PROTECT
    gtp_esd_switch(client, SWITCH_ON);
#endif
    gtp_int_sync(50);///mdelay(4000);nwd();
    return 0;
}


/*******************************************************
Function:
    Goodix touchscreen driver release function.
Input:
    client: i2c device struct.
Output:
    Executive outcomes. 0---succeed.
*******************************************************/
static int goodix_ts_remove(struct i2c_client *client)
{
    struct goodix_ts_data *ts = i2c_get_clientdata(client);
    
    GTP_DEBUG_FUNC();
    
#ifdef CONFIG_HAS_EARLYSUSPEND
    unregister_early_suspend(&ts->early_suspend);
#endif

#if GTP_CREATE_WR_NODE
    uninit_wr_node();
#endif

#if GTP_ESD_PROTECT
    destroy_workqueue(gtp_esd_check_workqueue);
#endif

    if (ts) 
    {
        if (ts->use_irq)
        {
            GTP_GPIO_AS_INPUT(GTP_INT_PORT);
            GTP_GPIO_FREE(GTP_INT_PORT);
            free_irq(client->irq, ts);
        }
        else
        {
            hrtimer_cancel(&ts->timer);
        }
    }   
    
    GTP_INFO("GTP driver removing...");
    i2c_set_clientdata(client, NULL);
    input_unregister_device(ts->input_dev);
    kfree(ts);

    return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
/*******************************************************
Function:
    Early suspend function.
Input:
    h: early_suspend struct.
Output:
    None.
*******************************************************/
static void goodix_ts_early_suspend(struct early_suspend *h)
{
    struct goodix_ts_data *ts;
    s8 ret = -1;    
    ts = container_of(h, struct goodix_ts_data, early_suspend);
    
    GTP_DEBUG_FUNC();

#if GTP_ESD_PROTECT
    ts->gtp_is_suspend = 1;
    gtp_esd_switch(ts->client, SWITCH_OFF);
#endif

#if GTP_SLIDE_WAKEUP
    ret = gtp_enter_doze(ts);
#else
    if (ts->use_irq)
    {
        gtp_irq_disable(ts);
    }
    else
    {
        hrtimer_cancel(&ts->timer);
    }
    ret = gtp_enter_sleep(ts);
#endif 
    if (ret < 0)
    {
        GTP_ERROR("GTP early suspend failed.");
    }
    // to avoid waking up while not sleeping
    //  delay 48 + 10ms to ensure reliability    
    msleep(58);   
}

/*******************************************************
Function:
    Late resume function.
Input:
    h: early_suspend struct.
Output:
    None.
*******************************************************/
static void goodix_ts_late_resume(struct early_suspend *h)
{
    struct goodix_ts_data *ts;
    s8 ret = -1;
    ts = container_of(h, struct goodix_ts_data, early_suspend);
    
    GTP_DEBUG_FUNC();
    
    ret = gtp_wakeup_sleep(ts);

#if GTP_SLIDE_WAKEUP
    doze_status = DOZE_DISABLED;
#endif

    if (ret < 0)
    {
        GTP_ERROR("GTP later resume failed.");
    }

    if (ts->use_irq)
    {
        gtp_irq_enable(ts);
    }
    else
    {
        hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
    }

#if GTP_ESD_PROTECT
    ts->gtp_is_suspend = 0;
    gtp_esd_switch(ts->client, SWITCH_ON);
#endif
}
#endif

#if GTP_ESD_PROTECT
/*******************************************************
Function:
    switch on & off esd delayed work
Input:
    client:  i2c device
    on:      SWITCH_ON / SWITCH_OFF
Output:
    void
*********************************************************/
void gtp_esd_switch(struct i2c_client *client, s32 on)
{
    struct goodix_ts_data *ts;
    
    ts = i2c_get_clientdata(client);
    if (SWITCH_ON == on)     // switch on esd 
    {
        if (!ts->esd_running)
        {
            ts->esd_running = 1;
            GTP_INFO("Esd started");
            queue_delayed_work(gtp_esd_check_workqueue, &gtp_esd_check_work, GTP_ESD_CHECK_CIRCLE);
        }
    }
    else    // switch off esd
    {
        if (ts->esd_running)
        {
            ts->esd_running = 0;
            GTP_INFO("Esd cancelled");
            cancel_delayed_work_sync(&gtp_esd_check_work);
        }
    }
}

/*******************************************************
Function:
    Initialize external watchdog for esd protect
Input:
    client:  i2c device.
Output:
    result of i2c write operation. 
        1: succeed, otherwise: failed
*********************************************************/
static s32 gtp_init_ext_watchdog(struct i2c_client *client)
{
    u8 opr_buffer[4] = {0x80, 0x40, 0xAA, 0xAA};
    
    struct i2c_msg msg;         // in case of recursively reset by calling gtp_i2c_write
    s32 ret = -1;
    s32 retries = 0;

    GTP_DEBUG("Init external watchdog...");
    GTP_DEBUG_FUNC();

    msg.flags = !I2C_M_RD;
    msg.addr  = client->addr;
    msg.len   = 4;
    msg.buf   = opr_buffer;

    while(retries < 5)
    {
        ret = i2c_transfer(client->adapter, &msg, 1);
        if (ret == 1)
        {
            return 1;
        }
        retries++;
    }
    if (retries >= 5)
    {
        GTP_ERROR("init external watchdog failed!");
    }
    return 0;
}

/*******************************************************
Function:
    Esd protect function.
    Added external watchdog by meta, 2013/03/07
Input:
    work: delayed work
Output:
    None.
*******************************************************/
static void gtp_esd_check_func(struct work_struct *work)
{
    s32 i;
    s32 ret = -1;
    struct goodix_ts_data *ts = NULL;
    u8 test[4] = {0x80, 0x40};
    
    GTP_DEBUG_FUNC();
   
    ts = i2c_get_clientdata(i2c_connect_client);

    if (ts->gtp_is_suspend)
    {
        ts->esd_running = 0;
        GTP_INFO("Esd terminated!");
        return;
    }
    
    for (i = 0; i < 3; i++)
    {
        ret = gtp_i2c_read(ts->client, test, 4);
        
        GTP_DEBUG("0x8040 = 0x%02X, 0x8041 = 0x%02X", test[2], test[3]);
        if ((ret < 0))
        {
            // IIC communication problem
            continue;
        }
        else
        { 
            if ((test[2] == 0xAA) || (test[3] != 0xAA))
            {
                // IC works abnormally..
                i = 3;
                break;  
            }
            else 
            {
                // IC works normally, Write 0x8040 0xAA, feed the dog
                test[2] = 0xAA; 
                gtp_i2c_write(ts->client, test, 3);
                break;
            }
        }
    }
    if (i >= 3)
    {
        GTP_ERROR("IC Working ABNORMALLY, Resetting Guitar...");
        gtp_reset_guitar(ts->client, 50);
    }

    if(!ts->gtp_is_suspend)
    {
        queue_delayed_work(gtp_esd_check_workqueue, &gtp_esd_check_work, GTP_ESD_CHECK_CIRCLE);
    }
    else
    {
        GTP_INFO("Esd terminated!");
        ts->esd_running = 0;
    }
    return;
}
#endif

static const struct i2c_device_id goodix_ts_id[] = {
    { GTP_I2C_NAME, 0 },
    { }
};

static struct i2c_driver goodix_ts_driver = {
    .probe      = goodix_ts_probe,
    .remove     = goodix_ts_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
    .suspend    = goodix_ts_early_suspend,
    .resume     = goodix_ts_late_resume,
#endif
    .id_table   = goodix_ts_id,
    .driver = {
        .name     = GTP_I2C_NAME,
        .owner    = THIS_MODULE,
    },
};

/*******************************************************    
Function:
    Driver Install function.
Input:
    None.
Output:
    Executive Outcomes. 0---succeed.
********************************************************/
static int __devinit goodix_ts_init(void)
{
    s32 ret;

    GTP_DEBUG_FUNC();   
    GTP_INFO("GTP driver installing...");
    goodix_wq = create_singlethread_workqueue("goodix_wq");
    if (!goodix_wq)
    {
        GTP_ERROR("Creat workqueue failed.");
        return -ENOMEM;
    }
#if GTP_ESD_PROTECT
    INIT_DELAYED_WORK(&gtp_esd_check_work, gtp_esd_check_func);
    gtp_esd_check_workqueue = create_workqueue("gtp_esd_check");
#endif
    ret = i2c_add_driver(&goodix_ts_driver);
    return ret; 
}

/*******************************************************    
Function:
    Driver uninstall function.
Input:
    None.
Output:
    Executive Outcomes. 0---succeed.
********************************************************/
static void __exit goodix_ts_exit(void)
{
    GTP_DEBUG_FUNC();
    GTP_INFO("GTP driver exited.");
    i2c_del_driver(&goodix_ts_driver);
    if (goodix_wq)
    {
        destroy_workqueue(goodix_wq);
    }
}

late_initcall(goodix_ts_init);
module_exit(goodix_ts_exit);

MODULE_DESCRIPTION("GTP Series Driver");
MODULE_LICENSE("GPL");
