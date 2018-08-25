//简单的注册/初始化函数
#include "avformat.h"

extern URLProtocol file_protocol;

void av_register_all(void)
{
    static int inited = 0;

	//保证初始化函数只调用一次
    if (inited != 0)
        return ;
    inited = 1;

	//ffplay把cpu当做一个广义的DSP.有些计算可以用CPU自带的加速指令来优化,
	//ffplay把这类函数独立出来放到dsputil.h和dsputil.c中,用函数指针的方法映射
	//到各个CPU具体的加速优化实现函数,此处初始化这些函数指针
    avcodec_init();

	//把所有的解码器用链表的方式串联起来,链表头指针是first_avcodec
    avcodec_register_all();

	//把所有的输入文件格式用链表的方式串联起来,链表头指针是first_iformat
    avidec_init();

	//把所有的输入协议用链表的方式串联起来,比如tcp/udp/file等,链表头指针是first_protocol
    register_protocol(&file_protocol);
}
