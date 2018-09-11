/*
�����ļ�,��ʼ�����л���,�Ѹ������ݽṹ�͹��ܺ����л���֯����,Э���������͹��ܺ���,
��Ӧ�û�����,���������Ƴ�������
*/
#include "./libavformat/avformat.h"

#if defined(CONFIG_WIN32)
#include <sys/types.h>
#include <sys/timeb.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/time.h>
#endif

#include <time.h>

#include <math.h>
#include <SDL.h>
#include <SDL_thread.h>

//SDL���涨����main����,��������ȡ��SDL�е�main����,�����ظ�����
#ifdef CONFIG_WIN32
#undef main // We don't want SDL to override our main()
#endif

//����SDL��
#if _DEBUG
#pragma comment(lib, "./Debug/SDL.lib")
#else
#pragma comment(lib, "./Release/SDL.lib")
#endif

//�򵥵ļ�����������
#define FF_QUIT_EVENT   (SDL_USEREVENT + 2)

#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)

#define VIDEO_PICTURE_QUEUE_SIZE 1

//����Ƶ���ݰ�/����֡�������ݽṹ����
typedef struct PacketQueue
{
    AVPacketList *first_pkt,  *last_pkt;
    int size;
    int abort_request;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

//��Ƶͼ�����ݽṹ����,
typedef struct VideoPicture
{
    SDL_Overlay *bmp;
    int width, height; // source height & width
} VideoPicture;

//�ܿ����ݽṹ,�������������ݽṹ������һ��,��һ����ת������,�����ڸ����ӽṹ֮����ת
typedef struct VideoState
{
    SDL_Thread *parse_tid;			//Demux�⸴���߳�ָ��
    SDL_Thread *video_tid;			//video�����߳�ָ��

    int abort_request;				//�쳣�˳�������

    AVFormatContext *ic;			//�����ļ���ʽ������ָ��,��iformat����ʹ��

    int audio_stream;				//��Ƶ������,��ʾAVFormatContext��AVStream*streams[]��������
    int video_stream;				//��Ƶ������,��ʾAVFormatContext��AVStream*streams[]��������

    AVStream *audio_st;			//��Ƶ��ָ��
    AVStream *video_st;			//��Ƶ��ָ��

    PacketQueue audioq;				//��Ƶ����֡/���ݰ�����
    PacketQueue videoq;				//��Ƶ����֡/���ݰ�����

    VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];		//�������Ƶͼ���������
    double frame_last_delay;		//��Ƶ֡�ӳ�,�ɼ���Ϊ����ʾ���ʱ��

    uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE *3) / 2];	//�����Ƶ����
    unsigned int audio_buf_size;	//�������Ƶ���ݴ�С
    int audio_buf_index;			//�������Ƶ���ݴ�С
    AVPacket audio_pkt;			//���һ����Ƶ�����ж��֡,���ڱ����м�״̬
    uint8_t *audio_pkt_data;		//��Ƶ�������׵�ַ,���audio_pkt�����м�״̬
    int audio_pkt_size;			//��Ƶ�����ݴ�С,���audio_pkt�����м�״̬

    SDL_mutex *video_decoder_mutex;	//��Ƶ���ݰ�����ͬ������������Ļ�����ָ��
    SDL_mutex *audio_decoder_mutex;	//��Ƶ���ݰ�����ͬ������������Ļ�����ָ��

    char filename[240];				//ý���ļ���

} VideoState;

static AVInputFormat *file_iformat;
static const char *input_filename;
static VideoState *cur_stream;

//SDL����Ҫ����ʾ����
static SDL_Surface *screen;

//ȡ�õ�ǰʱ��,��1/1000000��Ϊ��λ,Ϊ�����ڸ���ƽ̨����ֲ,�ɺ꿪�ؿ��Ʊ���Ĵ���
int64_t av_gettime(void)
{
#if defined(CONFIG_WINCE)
    return timeGetTime() *int64_t_C(1000);
#elif defined(CONFIG_WIN32)
    struct _timeb tb;
    _ftime(&tb);
    return ((int64_t)tb.time *int64_t_C(1000) + (int64_t)tb.millitm) *int64_t_C(1000);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec *1000000+tv.tv_usec;
#endif
}

/*
����֡/���ݰ���������:
1.��av_get_packet()�����е���av_malloc()���������ڴ�,������url_fread()���ý������
2.�������Ƶ������packet_queue_put()��is->videoq����,�������Ƶ����is->audioq����,
	�����������,�͵���av_free_packet()����ֱ���ͷ��ڴ�
3.������еİ�,��packet_queue_get()ȡ������,��av_free_packet()�ͷ��ڴ�
*/

//��ʼ������,��ʼ��Ϊ0���ٴ����߳�ͬ��ʹ�õĻ��������
static void packet_queue_init(PacketQueue *q) // packet queue handling
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

//ˢ�¶���,�ͷŵ����������ж�̬������ڴ�,��������Ƶ������ռ�õ��ڴ��AVPacketList�ṹռ
//�õ��ڴ�,
static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt,  *pkt1;

	//�����Ƕ��̳߳���,��Ҫͬ��,�����ڱ��������ͷ����ж�̬�����ڴ�ǰ�����
    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1)
    {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);	//�ͷ�����Ƶ�����ڴ�
        av_freep(&pkt);				//�ͷ�AVPacketList�ṹ
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}

//�ͷŶ���ռ�õ�������Դ,�����ͷŵ����ж�̬������ڴ�,�����ͷ�����Ļ���������������
static void packet_queue_end(PacketQueue *q)
{
    packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

//������Ƶ�����йҽ�����Ƶ����֡/���ݰ�
static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;

	//�ȷ���һ��AVPacketList�ṹ,���Ŵ�AVPacketǳ��������,
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return  - 1;
    pkt1->pkt =  *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);

	//�������йҽ�AVPacketList,��ͳ�ƻ����ý�����ݴ�С
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->size += pkt1->pkt.size;

	//������������Ϊsignal״̬,��������߳���ȴ���˯�߾ͻ���
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

//�����쳣�����˳�״̬
static void packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

	//�����쳣�˳�
    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

//�Ӷ�����ȡ��һ֡/������
/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;)
    {
		//����쳣�����˳������λ,�ʹ������뷵��
        if (q->abort_request)
        {
            ret =  - 1; // �쳣
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1)
        {
			//���������������,��ȡ��һ�����ݰ�,���������ý���С,ǳ����֡/������
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
			//�ͷŵ�AVPacketList�ṹ,�˽ṹ��packet_queue_put()�����е���̬����
            av_free(pkt1);
            ret = 1;
            break;
        }
        else if (!block)// ������ǣ�1(����ģʽ)��0(������ģʽ)
        {
			//����Ƿ�����ģʽ,û���ݾ�ֱ�ӷ���0
            ret = 0; // ������ģʽ��û����ֱ�ӷ���0
            break;
        }
        else
        {
			//���������ģʽ,û���ݾͽ���˯��״̬�ȴ�,packet_queue_put()�л���
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

//����SDL����Ҫ��Overlay��ʾ����,�����ó�������
static void alloc_picture(void *opaque)
{
    VideoState *is = opaque;
    VideoPicture *vp;

    vp = &is->pictq[0];

    if (vp->bmp)
        SDL_FreeYUVOverlay(vp->bmp);

    vp->bmp = SDL_CreateYUVOverlay(is->video_st->actx->width, 
                                   is->video_st->actx->height,
                                   SDL_YV12_OVERLAY,
                                   screen);

    vp->width = is->video_st->actx->width;
    vp->height = is->video_st->actx->height;
}

//��������Ƶͼ���ڵȴ���ʾ���ʱ���,����ɫ�ռ�ת��,����SDL����ʾ.����Ϊcpu����
//ǰ����ļ�,�⸴��,����ʱ��Ϊ0,���򵥵�ͬ�������߼�
static int video_display(VideoState *is, AVFrame *src_frame, double pts)
{
    VideoPicture *vp;
    int dst_pix_fmt;
    AVPicture pict;

    if (is->videoq.abort_request)
        return  - 1;

    vp = &is->pictq[0];

    /* if the frame is not skipped, then display it */
    if (vp->bmp)
    {
        SDL_Rect rect;

		//�ȴ���ʾ���ʱ��,����Sleep()������ʵ��
        if (pts)
            Sleep((int)(is->frame_last_delay *1000));
#if 1
        /* get a pointer on the bitmap */
        SDL_LockYUVOverlay(vp->bmp);

		//������ʾͼ�������
        dst_pix_fmt = PIX_FMT_YUV420P;
        pict.data[0] = vp->bmp->pixels[0];
        pict.data[1] = vp->bmp->pixels[2];
        pict.data[2] = vp->bmp->pixels[1];

        pict.linesize[0] = vp->bmp->pitches[0];
        pict.linesize[1] = vp->bmp->pitches[2];
        pict.linesize[2] = vp->bmp->pitches[1];

		//�ѽ�������ɫ�ռ�ת��Ϊ��ʾ��ɫ�ռ�
        img_convert(&pict, 
                    dst_pix_fmt,
                    (AVPicture*)src_frame, 
                    is->video_st->actx->pix_fmt, 
                    is->video_st->actx->width,
                    is->video_st->actx->height);

        SDL_UnlockYUVOverlay(vp->bmp); /* update the bitmap content */

        rect.x = 0;
        rect.y = 0;
        rect.w = is->video_st->actx->width;
        rect.h = is->video_st->actx->height;
		//ʵ������ʾ,ˢ������
        SDL_DisplayYUVOverlay(vp->bmp, &rect);
#endif
    }
    return 0;
}

//��Ƶ�����߳�,��Ҫ�����Ƿ������֡�����SDL��ʾ�����������ѭ��(�Ӷ�����ȡ����֡,
//����,����ʱ��,��ʾ),�ͷ���Ƶ����֡/���ݰ�����
static int video_thread(void *arg)
{
    VideoState *is = arg;
    AVPacket pkt1,  *pkt = &pkt1;
    int len1, got_picture;
    double pts = 0;

	//�������֡����
    AVFrame *frame = av_malloc(sizeof(AVFrame));
    memset(frame, 0, sizeof(AVFrame));

	//����SDL��ʾ����
    alloc_picture(is);

    for (;;)
    {
		//�Ӷ�����ȡ����֡/���ݰ�
        if (packet_queue_get(&is->videoq, pkt, 1) < 0)
            break;

		//ʵ���Խ���
        SDL_LockMutex(is->video_decoder_mutex);
        len1 = avcodec_decode_video(is->video_st->actx, frame, &got_picture, pkt->data, pkt->size);
        SDL_UnlockMutex(is->video_decoder_mutex);

		//����ͬ��ʱ��
        if (pkt->dts != AV_NOPTS_VALUE)
            pts = av_q2d(is->video_st->time_base) *pkt->dts;

        if (got_picture)
        {
			//�жϵõ�ͼ��,������ʾ����ͬ����ʾ��Ƶͼ��
            if (video_display(is, frame, pts) < 0)
                goto the_end;
        }
		//�ͷ���Ƶ����֡/���ݰ��ڴ�,�����ݰ��ڴ�����av_get_packet()�����е���av_malloc()�����
        av_free_packet(pkt);
    }

the_end: 
    av_free(frame);
    return 0;
}

/*
����һ����Ƶ֡,���ؽ�ѹ�����ݴ�С,�ر�ע��һ����Ƶ�����ܰ��������Ƶ֡,��һ��ֻ
����һ����Ƶ֡,����һ������Ҫ��β��ܽ�����.����������while����жϰ������Ƿ�ȫ
������,���û�оͽ��뵱ǰ���е�֡,�޸�״̬����,����,�ͷ����ݰ�,�ٴӶ�����ȡ,��¼
��ʼֵ,�ٽ�ѭ��
*/

/* decode one audio frame and returns its uncompressed size */
static int audio_decode_frame(VideoState *is, uint8_t *audio_buf, double *pts_ptr)
{
    AVPacket *pkt = &is->audio_pkt;
    int len1, data_size;
    
    for (;;)
    {
		//�ر�ע��,һ����Ƶ�����ܰ��������Ƶ֡,������Ҫ��ν���,VideoState��һ��AVPacket�ͱ�������
		//��ν�����м�״̬,�����ν��뵫�������ν���,audio_decode_frameֱ�ӽ���whileѭ��
        /* NOTE: the audio packet can contain several frames */
        while (is->audio_pkt_size > 0)
        {
			//���ý��뺯������,avcodec_decode_audio()�������ؽ����õ����ֽ���
            SDL_LockMutex(is->audio_decoder_mutex);
            len1 = avcodec_decode_audio(is->audio_st->actx, (int16_t*)audio_buf,
                            &data_size, is->audio_pkt_data, is->audio_pkt_size);

            SDL_UnlockMutex(is->audio_decoder_mutex);
            if (len1 < 0)
            {
				//�����������,������ǰ֡,�����ײ�ѭ��
                /* if error, we skip the frame */
                is->audio_pkt_size = 0;
                break;
            }

			//������������Ƶ֡�����׵�ַ�ʹ�С
            is->audio_pkt_data += len1;
            is->audio_pkt_size -= len1;
			//���û�еõ�����������,��������,������Щ֡��һ�ν���ʱֻ��һ��֡ͷ�ͷ���,
			//��ʱ��Ҫ������������֡
            if (data_size <= 0)
                continue;

			//���ؽ��������ݴ�С
            return data_size;
        }

		//��������,�����ǳ�ʼʱaudio_pktû�и�ֵ,����һ���Ѿ�������,��ʱ��Ҫ�ͷŰ������ڴ�
        /* free the current packet */
        if (pkt->data)
            av_free_packet(pkt);

		//��ȡ��һ�����ݰ�
        /* read next packet */
        if (packet_queue_get(&is->audioq, pkt, 1) < 0)
            return  - 1;

		//��ʼ�����ݰ��׵�ַ�ʹ�С,����һ���а��������Ƶ֡���ν�������
        is->audio_pkt_data = pkt->data;
        is->audio_pkt_size = pkt->size;
    }
}

//��Ƶ����ص�����,ÿ����Ƶ�������Ϊ��ʱ,ϵͳ�͵��ô˺��������Ƶ�������,Ŀǰ���ñȽ�
//�򵥵�ͬ����ʽ,��Ƶ�����Լ��Ľ�����ǰ�߼���,����Ҫsynchronize_audio()����ͬ������
/* prepare a new audio buffer */
void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    VideoState *is = opaque;
    int audio_size, len1;
    double pts = 0;

    while (len > 0)
    {
        if (is->audio_buf_index >= is->audio_buf_size)
        {
			//�����������������ȫ�����,�ͽ�����Ƶ����,�����ֽ������ݴ�С,����������0
            audio_size = audio_decode_frame(is, is->audio_buf, &pts);
            if (audio_size < 0)
            {
                /* if error, just output silence */
                is->audio_buf_size = 1024;
                memset(is->audio_buf, 0, is->audio_buf_size);
            }
            else
            {
//              audio_size = synchronize_audio(is, (int16_t*)is->audio_buf, audio_size, pts);
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
		//�����ʵ������ݵ��������,���޸Ľ��뻺��Ĳ���,����һ��ѭ��
		//�ر�ע��:�ɽ���һ��ѭ����֪,����Ӧ����SDL��������������
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

//����ģ��,���Ĺ����Ǵ���Ӧ��codec,���������߳�(���ǰ���Ƶ�ص�����������һ��������߳�).
/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *enc;
    AVCodec *codec;
    SDL_AudioSpec wanted_spec, spec;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return  - 1;

    enc = ic->streams[stream_index]->actx;
	
	//��ʼ����Ƶ�������,������SDL_OpenAudio()���õ�SDL��
    /* prepare audio output */
    if (enc->codec_type == CODEC_TYPE_AUDIO)
    {
        wanted_spec.freq = enc->sample_rate;
        wanted_spec.format = AUDIO_S16SYS;
        /* hack for AC3. XXX: suppress that */
        if (enc->channels > 2)
            enc->channels = 2;
        wanted_spec.channels = enc->channels;
        wanted_spec.silence = 0;
        wanted_spec.samples = 1024; //SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback = sdl_audio_callback;
        wanted_spec.userdata = is;
		//wanted_spec��Ӧ�ó����趨��SDL�����Ƶ����,spec��SDL�ⷵ�ظ�Ӧ�ó�����֧�ֵ���Ƶ
		//����,ͨ����һ�µ�,�������SDL֧�ֵĲ�����Χ,�᷵��������Ĳ���
        if (SDL_OpenAudio(&wanted_spec, &spec) < 0)
        {
            fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
            return  - 1;
        }
    }

	//���ձ���������ĵ�codec_id,���������������,�ҵ���Ӧ�Ĺ��ܺ���
    codec = avcodec_find_decoder(enc->codec_id);

	//���Ĺ���֮һ,�򿪱������,��ʼ�����������������л���
    if (!codec || avcodec_open(enc, codec) < 0)
        return  - 1;

    switch (enc->codec_type)
    {
		//��VideoState�м�¼��Ƶ������
    case CODEC_TYPE_AUDIO:
        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];
        is->audio_buf_size = 0;
        is->audio_buf_index = 0;

		//��ʼ����Ƶ����,�������������Ƶ�����߳�
        memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
        packet_queue_init(&is->audioq);
        SDL_PauseAudio(0);
        break;
		
		//��VideoState�м�¼��Ƶ������
    case CODEC_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        is->frame_last_delay = is->video_st->frame_last_delay;

		//��ʼ����Ƶ����,��ֱ��������Ƶ�����߳�
        packet_queue_init(&is->videoq);
        is->video_tid = SDL_CreateThread(video_thread, is);
        break;
    default:
        break;
    }
    return 0;
}

//�ر���ģ��,ֹͣ�����߳�,�ͷŶ�����Դ
//ͨ��packet_queue_abort()������abort_request��־λ,�����߳��б�˱�׼λ����ȫ�˳��߳�
static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *enc;

	//�򵥵�����������У��
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return ;
	//�ҵ����ļ���ʽ�����еõ��Ľ�����������ָ��,�����������еĲ���
    enc = ic->streams[stream_index]->actx;

    switch (enc->codec_type)
    {
		//ֹͣ�����߳�,�ͷŶ�����Դ
    case CODEC_TYPE_AUDIO:
        packet_queue_abort(&is->audioq);
        SDL_CloseAudio();
        packet_queue_end(&is->audioq);
        break;
    case CODEC_TYPE_VIDEO:
        packet_queue_abort(&is->videoq);
        SDL_WaitThread(is->video_tid, NULL);
        packet_queue_end(&is->videoq);
        break;
    default:
        break;
    }

	//�ͷű��������������Դ
    avcodec_close(enc);
}

//�ļ������߳�,�������е㲻������ʵ.���������,ֱ��ʶ���ļ���ʽ�ͼ��ʶ��ý���ʽ,
//�򿪾���ı�����������������߳�,��������Ƶý������ҽӵ���Ӧ�Ķ���
static int decode_thread(void *arg)
{
    VideoState *is = arg;
    AVFormatContext *ic;
    int err, i, ret, video_index, audio_index;
    AVPacket pkt1,  *pkt = &pkt1;
    AVFormatParameters params,  *ap = &params;
    
    int flags = SDL_HWSURFACE | SDL_ASYNCBLIT | SDL_HWACCEL | SDL_RESIZABLE;

	//��ʼ����������ָʾû����Ӧ����
    video_index =  - 1;
    audio_index =  - 1;

    is->video_stream =  - 1;
    is->audio_stream =  - 1;

    memset(ap, 0, sizeof(*ap));

	//���ú���ֱ��ʶ���ļ���ʽ,�ڴ˺������ٵ��������������ʶ��ý���ʽ
    err = av_open_input_file(&ic, is->filename, NULL, 0, ap);
    if (err < 0)
    {
        ret =  - 1;
        goto fail;
    }
	//�����ļ���ʽ������,���ڸ����ݽṹ����ת
    is->ic = ic;

    for (i = 0; i < ic->nb_streams; i++)
    {
        AVCodecContext *enc = ic->streams[i]->actx;
		//��������Ƶ������,������ʾ��Ƶ�������õ�SDL��
        switch (enc->codec_type)
        {
        case CODEC_TYPE_AUDIO:
            if (audio_index < 0)
               audio_index = i;
            break;
        case CODEC_TYPE_VIDEO:
            if (video_index < 0)
               video_index = i;

            screen = SDL_SetVideoMode(enc->width, enc->height, 0, flags);

            SDL_WM_SetCaption("FFplay", "FFplay"); // �޸���Ϊ��������Ƶ��С

//          schedule_refresh(is, 40);
            break;
        default:
            break;
        }
    }

	//�������Ƶ��,�͵��ú�������Ƶ��������������Ƶ��������߳�
    if (audio_index >= 0)
        stream_component_open(is, audio_index);

	//�������Ƶ��,�͵��ú�������Ƶ��������������Ƶ�����߳�
    if (video_index >= 0)
        stream_component_open(is, video_index);

	//�����û����Ƶ��,��û����Ƶ��,�����ô����뷵��
    if (is->video_stream < 0 && is->audio_stream < 0)
    {
        fprintf(stderr, "%s: could not open codecs\n", is->filename);
        ret =  - 1;
        goto fail;
    }

    for (;;)
    {
		//����쳣�˳�������λ,���˳��ļ������߳�
        if (is->abort_request)
            break;

		//���������,����΢��ʱһ��
        if (is->audioq.size > MAX_AUDIOQ_SIZE || is->videoq.size > MAX_VIDEOQ_SIZE || url_feof(&ic->pb))
        {
            SDL_Delay(10); // if the queue are full, no need to read more,wait 10 ms
            continue;
        }

		//��ý���ļ��������Ķ�ȡһ������Ƶ����
        ret = av_read_packet(ic, pkt); //av_read_frame(ic, pkt);		
        if (ret < 0)
        {
            if (url_ferror(&ic->pb) == 0)
            {
                SDL_Delay(100); // wait for user event
                continue;
            }
            else
                break;
        }

		{
			unsigned int *p1=(unsigned int*)(pkt->data);
			unsigned int *p2=p1+1;

			if((*p1==0x3c8638) && (*p2==0x1185148))
			{
				int dbg=0;
			}
		}

		//�жϰ����ݵ�����,�ֱ�ҽӵ���Ӧ����,����ǲ�ʶ�������,��ֱ���ͷŶ�����
        if (pkt->stream_index == is->audio_stream)
        {
            packet_queue_put(&is->audioq, pkt);
        }
        else if (pkt->stream_index == is->video_stream)
        {
            packet_queue_put(&is->videoq, pkt);
        }
        else
        {
            av_free_packet(pkt);
        }
    }

	//�򵥵���ʱ,�ú�����߳��л�������ݽ�����ʾ��,��Ȼ����������һ�������Ҳ����
    while (!is->abort_request)   // wait until the end
    {
        SDL_Delay(100);
    }

    ret = 0;

	//�ͷŵ��ڱ��߳��з���ĸ�����Դ,������˭����˭�ͷŵĳ����Է����
fail: 
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);

    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);

    if (is->ic)
    {
        av_close_input_file(is->ic);
        is->ic = NULL;
    }

    if (ret != 0)
    {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    return 0;
}

//����,�������Ҳ�е�������ʵ,��Ҫ�����Ƿ���ȫ���ܿ����ݽṹ,��ʼ����ز���,�����ļ������߳�
static VideoState *stream_open(const char *filename, AVInputFormat *iformat)
{
    VideoState *is;

    is = av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;
    pstrcpy(is->filename, sizeof(is->filename), filename);

    is->audio_decoder_mutex = SDL_CreateMutex();
    is->video_decoder_mutex = SDL_CreateMutex();

    is->parse_tid = SDL_CreateThread(decode_thread, is);
    if (!is->parse_tid)
    {
        av_free(is);
        return NULL;
    }
    return is;
}

//�ر���,�������Ҳ�е�������ʵ,��Ҫ�������ͷ���Դ
static void stream_close(VideoState *is)
{
    VideoPicture *vp;
    int i;

    is->abort_request = 1;
    SDL_WaitThread(is->parse_tid, NULL);

    for (i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++)
    {
        vp = &is->pictq[i];
        if (vp->bmp)
        {
            SDL_FreeYUVOverlay(vp->bmp);
            vp->bmp = NULL;
        }
    }

    SDL_DestroyMutex(is->audio_decoder_mutex);
    SDL_DestroyMutex(is->video_decoder_mutex);

	free(is);
}

//�����˳�ʱ���õĺ���,�ر��ͷ�һЩ��Դ
void do_exit(void)
{
    if (cur_stream)
    {
        stream_close(cur_stream);
        cur_stream = NULL;
    }

    SDL_Quit();
    exit(0);
}

//SDL�����Ϣ�¼�ѭ��
void event_loop(void) // handle an event sent by the GUI
{
    SDL_Event event;

    for (;;)
    {
        SDL_WaitEvent(&event);
        switch (event.type)
        {
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym)
            {
            case SDLK_ESCAPE:
            case SDLK_q:
                do_exit();
                break;
            default:
                break;
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:
            do_exit();
            break;
        default:
            break;
        }
    }
}

//��ں���,��ʼ��SDL��,ע��SDL��Ϣ�¼�,�����ļ������߳�,������Ϣѭ��
int main(int argc, char **argv)
{
    int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;

    av_register_all();

    //input_filename = "d:/yuv/clocktxt_320.avi";
	input_filename = "./clocktxt_320.avi";
//    input_filename = "d:/yuv/clocktxt.avi";

    if (SDL_Init(flags))
        exit(1);

    SDL_EventState(SDL_ACTIVEEVENT, SDL_IGNORE);
    SDL_EventState(SDL_MOUSEMOTION, SDL_IGNORE);
    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    cur_stream = stream_open(input_filename, file_iformat);

    event_loop();

    return 0;
}
