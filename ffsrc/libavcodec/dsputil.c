//定义dsp优化限幅运算使用的查找表,实现其初始化函数
#include "avcodec.h"
#include "dsputil.h"

uint8_t cropTbl[256+2 * MAX_NEG_CROP] = {0, };

void dsputil_static_init(void)
{
    int i;

	/*
	初始化限幅运算查找表,最后的结果是:前MAX_NEG_CROP个数组项为0,接着的256个项分别为
	0到255,后面MAX_ENG_CROP个数组项为255.用查表代替比较实现限幅运算
	*/
    for (i = 0; i < 256; i++)
        cropTbl[i + MAX_NEG_CROP] = i;

    for (i = 0; i < MAX_NEG_CROP; i++)
    {
        cropTbl[i] = 0;
        cropTbl[i + MAX_NEG_CROP + 256] = 255;
    }
}
