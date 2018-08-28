//有缓存功能的广义文件ByteIOContext相关的文件操作,比如open,read,close,seek等
#include "../berrno.h"
#include "avformat.h"
#include "avio.h"
#include <stdarg.h>

#define IO_BUFFER_SIZE 32768

//初始化广义文件ByteIOContext结构,一些简单的赋值操作
int init_put_byte(ByteIOContext *s, 
				  unsigned char *buffer, 
				  int buffer_size, 
				  int write_flag, 
				  void *opaque, 
				  int(*read_buf)(void *opaque, uint8_t *buf, int buf_size), 
				  int(*write_buf)(void *opaque, uint8_t *buf, int buf_size), 
				  offset_t(*seek)(void *opaque, offset_t offset, int whence))
{
    s->buffer = buffer;
    s->buffer_size = buffer_size;
    s->buf_ptr = buffer;
    s->write_flag = write_flag;
    if (!s->write_flag)
        s->buf_end = buffer;	//初始化情况下,缓存中没有有效数据,所以buf_end指向缓存首地址
    else
        s->buf_end = buffer + buffer_size;
    s->opaque = opaque;
    s->write_buf = write_buf;
    s->read_buf = read_buf;
    s->seek = seek;
    s->pos = 0;
    s->must_flush = 0;
    s->eof_reached = 0;
    s->error = 0;
    s->max_packet_size = 0;

    return 0;
}

//广义文件ByteIOContext的seek操作,
//输入变量: s为广义文件句柄,offset为偏移量,whence为定位方式
//输出变量: 相对广义文件开始的偏移量
offset_t url_fseek(ByteIOContext *s, offset_t offset, int whence)
{
    offset_t offset1;

	//只支持SEEK_CUR和SEEK_SET定位方式,不支持SEEK_END方式.
	//SEEK_CUR:从文件开始当前读写位置为基准偏移offset字节
	//SEEK_SET: 从文件开始位置偏移offset字节
    if (whence != SEEK_CUR && whence != SEEK_SET)
        return  - EINVAL;

	//ffplay把SEEK_CUR和SEEK_SET统一成SEEK_SET方式处理,所以如果是SEEK_CUR方式就要
	//转换成SEEK_SET的偏移量
    if (whence == SEEK_CUR)
    {
        offset1 = s->pos - (s->buf_end - s->buffer) + (s->buf_ptr - s->buffer);
        if (offset == 0)
            return offset1;	//如果偏移量为0,返回实际偏移位置
		//计算绝对偏移量,赋值给offset.
        offset += offset1;		//加上实际偏移量0,返回实际偏移位置
    }
	//计算绝对偏移量相对当前缓存的偏移量,赋值给offset1.
    offset1 = offset - (s->pos - (s->buf_end - s->buffer));
	//判断绝对偏移量是否在当前缓存中,如果在当前缓存中,就简单的修改buf_ptr指针
    if (offset1 >= 0 && offset1 <= (s->buf_end - s->buffer))
    {
        s->buf_ptr = s->buffer + offset1; // can do the seek inside the buffer
    }
    else
    {
		//判断当前广义文件是否可以seek,如果不能seek就返回错误
        if (!s->seek)
            return  - EPIPE;
		//调用底层具体的文件系统的seek函数完成实际的seek操作,此时缓存需要重新初始化,buf_end重新
		//指向缓存首地址,并修改pos变量为广义文件当前实际偏移量
        s->buf_ptr = s->buffer;
        s->buf_end = s->buffer;
        if (s->seek(s->opaque, offset, SEEK_SET) == (offset_t) - EPIPE)
            return  - EPIPE;
        s->pos = offset;
    }
    s->eof_reached = 0;
	//返回广义文件当前的实际偏移量
    return offset;
}

//广义文件ByteIOContext的当前实际偏移量再偏移offset字节,调用url_fseek实现
void url_fskip(ByteIOContext *s, offset_t offset)
{
    url_fseek(s, offset, SEEK_CUR);
}

//返回广义文件ByteIOContext的当前实际偏移量
offset_t url_ftell(ByteIOContext *s)
{
    return url_fseek(s, 0, SEEK_CUR);
}

//返回广义文件ByteIOContext的大小
offset_t url_fsize(ByteIOContext *s)
{
    offset_t size;

    if (!s->seek)
        return  - EPIPE;
	//调用底层seek函数取得文件大小
    size = s->seek(s->opaque,  - 1, SEEK_END) + 1;
	//注意seek操作改变了读指针,所以要重新seek到当前读指针位置
    s->seek(s->opaque, s->pos, SEEK_SET);
    return size;
}

//判断当前广义文件ByteIOContext是否到末尾
int url_feof(ByteIOContext *s)
{
    return s->eof_reached;
}

//返回当前广义文件ByteIOContext操作错误码
int url_ferror(ByteIOContext *s)
{
    return s->error;
}

// Input stream

//填充广义文件ByteIOContext内部的数据缓冲区
static void fill_buffer(ByteIOContext *s)
{
    int len;

	//如果到了广义文件ByteIOContext末尾就直接返回
    if (s->eof_reached)
        return ;

	//调用底层文件系统对的读函数实际读数据填到缓存,注意这里经过了好几次跳转才到底层函数.首先跳转到
	//url_read_buf()函数,再跳转到url_read(),再跳转到实际文件协议的读函数完成读操作
    len = s->read_buf(s->opaque, s->buffer, s->buffer_size);
    if (len <= 0)
    {   // do not modify buffer if EOF reached so that a seek back can be done without rereading data
		//如果是到达文件末尾就不要改buffer参数,这样不用重新读数据就可以做seek back操作
        s->eof_reached = 1;

		//设置错误码,便于分析定位
        if (len < 0)
            s->error = len;
    }
    else
    {
		//如果正确读取,修改一下基本参数,
        s->pos += len;
        s->buf_ptr = s->buffer;
        s->buf_end = s->buffer + len;
    }
}

//从广义文件ByteIOContext中读取一个字节
int get_byte(ByteIOContext *s) // NOTE: return 0 if EOF, so you cannot use it if EOF handling is necessary
{
    if (s->buf_ptr < s->buf_end)
    {
		//如果广义文件ByteIOContext内部缓存有数据,就修改读指针,返回读取的数据
        return  *s->buf_ptr++;
    }
    else
    {
		//如果广义文件ByteIOContext内部缓存没有数据,就先填充内部缓存
        fill_buffer(s);
		//如果广义文件ByteIOContext内部缓存有数据,就修改读指针,返回读取的数据,
		//如果没有数据就是到了文件末尾,返回0.
        if (s->buf_ptr < s->buf_end)
            return  *s->buf_ptr++;
        else
            return 0;
    }
}

//从广义文件ByteIOContext中以小端方式读取两个字节,实现代码充分复用get_byte()函数
unsigned int get_le16(ByteIOContext *s)
{
    unsigned int val;
    val = get_byte(s);
    val |= get_byte(s) << 8;
    return val;
}

//从广义文件ByteIOContext中以小端方式读取四个字节,
unsigned int get_le32(ByteIOContext *s)
{
    unsigned int val;
    val = get_le16(s);
    val |= get_le16(s) << 16;
    return val;
}

#define url_write_buf NULL

//简单中转读操作函数
static int url_read_buf(void *opaque, uint8_t *buf, int buf_size)
{
    URLContext *h = opaque;
    return url_read(h, buf, buf_size);
}

//简单中转seek操作函数
static offset_t url_seek_buf(void *opaque, offset_t offset, int whence)
{
    URLContext *h = opaque;
    return url_seek(h, offset, whence);
}

//设置并分配广义文件ByteIOContext内部缓存的大小,更多的应用在修改内部缓存大小的场合
int url_setbufsize(ByteIOContext *s, int buf_size) // must be called before any I/O
{
    uint8_t *buffer;
	//分配广义文件ByteIOContext内部缓存
    buffer = av_malloc(buf_size);
    if (!buffer)
        return  - ENOMEM;

	//释放掉原来广义文件ByteIOContext内部缓存,这是一个保险的操作
    av_free(s->buffer);
	//设置广义文件ByteIOContext的内部缓存相关参数
    s->buffer = buffer;
    s->buffer_size = buf_size;
    s->buf_ptr = buffer;
    if (!s->write_flag)
		//此时只是分配了内存,并没有读入数据,所以buf_end指向首地址
        s->buf_end = buffer;
    else
        s->buf_end = buffer + buf_size;
    return 0;
}

//打开广义文件ByteIOContext
int url_fopen(ByteIOContext *s, const char *filename, int flags)
{
    URLContext *h;
	uint8_t *buffer;
    int buffer_size, max_packet_size;
    int err;

	//调用底层文件系统的open函数实质性打开文件
    err = url_open(&h, filename, flags);
    if (err < 0)
        return err;
   
	//读取底层文件系统支持的最大包大小,如果非0,则设置为内部缓存大小,否则内部缓存
	//设置为默认大小IO_BUFFER_SIZE(32767字节)
    max_packet_size = url_get_max_packet_size(h);
    if (max_packet_size)
    {
        buffer_size = max_packet_size; // no need to bufferize more than one packet
    }
    else
    {
        buffer_size = IO_BUFFER_SIZE;
    }

	//分配广义文件ByteIOContext内部缓存,如果错误就关闭文件返回错误码
    buffer = av_malloc(buffer_size);
    if (!buffer)
	{
        url_close(h);
        return  - ENOMEM;
	}

	//初始化广义文件ByteIOContext数据结构,如果错误就关闭文件,释放内部缓存,返回错误码
    if (init_put_byte(s,
					  buffer, 
					  buffer_size, 
					  (h->flags & URL_WRONLY || h->flags & URL_RDWR), 
					  h, 
					  url_read_buf, 
					  url_write_buf, 
					  url_seek_buf) < 0)
    {
        url_close(h);
        av_free(buffer);
        return AVERROR_IO;
    }

	//保存最大包大小
    s->max_packet_size = max_packet_size;

    return 0;
}

//关闭广义文件ByteIOContext,首先释放掉内部使用的缓存,再把自己的字段置0,
//最后转入底层文件系统的关闭函数实质性关闭文件
int url_fclose(ByteIOContext *s)
{
    URLContext *h = s->opaque;

    av_free(s->buffer);
    memset(s, 0, sizeof(ByteIOContext));
    return url_close(h);
}

//广义文件ByteIOContext读操作,注意此函数从get_buffer改名而来,更贴切函数功能,也
//为了完备广义文件操作函数集
int url_fread(ByteIOContext *s, unsigned char *buf, int size) // get_buffer
{
    int len, size1;

	//考虑到size可能比缓存中的数据大得多,此时就多次读缓存,所以用size1保存要读取的总字节数,
	//size意义变更为还需要读取的字节数
    size1 = size;
    while (size > 0)
    {
		//计算当次循环应该读取的字节数len,首先设置len为内部缓存数据长度,再和需要读的字节数size比较,修正len的值
        len = s->buf_end - s->buf_ptr;
        if (len > size)
            len = size;
        if (len == 0)
        {
			//如果内部缓存没有数据
            if (size > s->buffer_size)		//读操作是否绕过内部缓存的判别条件
            {
				//如果要读取的数据量比内部缓存数据量大,就调用底层函数读取数据绕过内部缓存直接到目标缓存
                len = s->read_buf(s->opaque, buf, size);
                if (len <= 0)
                {
					//如果底层文件系统读错误,设置文件末尾标价和错误码,跳出循环,返回实际读到的字节数
                    s->eof_reached = 1;
                    if (len < 0)
                        s->error = len;
                    break;
                }
                else
                {
					//如果底层文件系统正确读,则修改相关参数,进入下一轮循环.特别注意此处读文件绕过了内部缓存
                    s->pos += len;
                    size -= len;
                    buf += len;		//因为绕过了内部缓存,特别注意此处的修改
                    s->buf_ptr = s->buffer;
                    s->buf_end = s->buffer /* + len*/;	//因为绕过了内部缓存,特别注意此处 
                }
            }
            else
            {
				//如果要读取的数据量比内部缓存数据量少,就调用底层函数读取数据到内部缓存,判断读成功否
                fill_buffer(s);
                len = s->buf_end - s->buf_ptr;
                if (len == 0)
                    break;
            }
        }
        else
        {
			//如果内部缓存有数据,就拷贝len长度的数据到缓冲区,并修改相关参数,进入下一个循环的条件判断
            memcpy(buf, s->buf_ptr, len);
            buf += len;
            s->buf_ptr += len;
            size -= len;
        }
    }
	//返回实际读取的字节数
    return size1 - size;
}
