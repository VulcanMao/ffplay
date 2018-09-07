/*
简单的注册/初始化函数,把编码器用相应的链表串联起来,便于查找识别
*/
#include "avcodec.h"

extern AVCodec truespeech_decoder;
extern AVCodec msrle_decoder;

void avcodec_register_all(void)
{
	//inited声明成static,做一下比较是为了避免此函数多次调用.
	//编程基本原则之一,初始化函数只调用一次,不能随意多次调用
    static int inited = 0;

    if (inited != 0)
        return ;

    inited = 1;

	//把msrle_decoder解码器串接到解码器链表,链表头指针是first_avcodec.
    register_avcodec(&msrle_decoder);
	//把truespeedh_decoder解码器串接到解码器链表,链表头指针是first_avcodec
    register_avcodec(&truespeech_decoder);
}
