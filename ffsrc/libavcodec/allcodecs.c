/*
�򵥵�ע��/��ʼ������,�ѱ���������Ӧ������������,���ڲ���ʶ��
*/
#include "avcodec.h"

extern AVCodec truespeech_decoder;
extern AVCodec msrle_decoder;

void avcodec_register_all(void)
{
	//inited������static,��һ�±Ƚ���Ϊ�˱���˺�����ε���.
	//��̻���ԭ��֮һ,��ʼ������ֻ����һ��,���������ε���
    static int inited = 0;

    if (inited != 0)
        return ;

    inited = 1;

	//��msrle_decoder���������ӵ�����������,����ͷָ����first_avcodec.
    register_avcodec(&msrle_decoder);
	//��truespeedh_decoder���������ӵ�����������,����ͷָ����first_avcodec
    register_avcodec(&truespeech_decoder);
}
