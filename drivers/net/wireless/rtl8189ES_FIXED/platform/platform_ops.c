/******************************************************************************
 *
 * Copyright(c) 2013 Realtek Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 *
 ******************************************************************************/
#ifndef CONFIG_PLATFORM_OPS
//#include<linux/print.h>
/*
 * Return:
 *	0:	power on successfully
 *	others: power on failed
 */
extern void wifi_power_on(unsigned char onoff);
int platform_wifi_power_on(void)
{
	int ret = 0;
//	printk("===%d %s===\n",__LINE__,__FUNCTION__);
#if defined(CONFIG_KEWEI_ZHENGJI)||defined (CONFIG_HANGSHENG_ZHENGJI)
	wifi_power_on(0);
#else
	wifi_power_on(1);
#endif
	return ret;
}

void platform_wifi_power_off(void)
{
//	printk("===%d %s===\n",__LINE__,__FUNCTION__);
#if defined(CONFIG_KEWEI_ZHENGJI)||defined (CONFIG_HANGSHENG_ZHENGJI)
	wifi_power_on(1);
#else
	wifi_power_on(0);
#endif
}
#endif // !CONFIG_PLATFORM_OPS