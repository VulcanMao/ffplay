//����dsp�Ż��޷�����ʹ�õĲ��ұ�,ʵ�����ʼ������
#include "avcodec.h"
#include "dsputil.h"

uint8_t cropTbl[256+2 * MAX_NEG_CROP] = {0, };

void dsputil_static_init(void)
{
    int i;

	/*
	��ʼ���޷�������ұ�,���Ľ����:ǰMAX_NEG_CROP��������Ϊ0,���ŵ�256����ֱ�Ϊ
	0��255,����MAX_ENG_CROP��������Ϊ255.�ò�����Ƚ�ʵ���޷�����
	*/
    for (i = 0; i < 256; i++)
        cropTbl[i + MAX_NEG_CROP] = i;

    for (i = 0; i < MAX_NEG_CROP; i++)
    {
        cropTbl[i] = 0;
        cropTbl[i + MAX_NEG_CROP + 256] = 255;
    }
}
