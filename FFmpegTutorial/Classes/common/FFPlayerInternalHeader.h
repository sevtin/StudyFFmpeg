//
//  FFPlayerInternalHeader.h
//  FFmpegTutorial
//
//  Created by Matt Reach on 2020/4/28.
//

#ifndef FFPlayerInternalHeader_h
#define FFPlayerInternalHeader_h

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25

static __inline__ NSError * _make_nserror(int code)
{
    return [NSError errorWithDomain:@"com.debugly.fftutorial" code:(NSInteger)code userInfo:nil];
}

static __inline__ NSError * _make_nserror_desc(int code,NSString *desc)
{
    if (!desc || desc.length == 0) {
        desc = @"";
    }
    
    return [NSError errorWithDomain:@"com.debugly.fftutorial" code:(NSInteger)code userInfo:@{
        NSLocalizedDescriptionKey:desc
    }];
}

///packet 链表
typedef struct MyAVPacketList {
    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;
} MyAVPacketList;

///packet 队列
typedef struct PacketQueue {
    ///头尾指针
    MyAVPacketList *first_pkt, *last_pkt;
    //队列里包含了多少个包
    int nb_packets;
    //所有包暂用的内存大小
    int size;
    //所有包总的时长，注意单位不是s
    int64_t duration;
    //最后一个包的序号
    int serial;
    //锁
    dispatch_semaphore_t mutex;
//    SDL_mutex *mutex;
//    SDL_cond *cond;
} PacketQueue;

///packet 队列初始化
static __inline__ int packet_queue_init(PacketQueue *q)
{
    memset((void*)q, 0, sizeof(PacketQueue));
    q->mutex = dispatch_semaphore_create(1);
//    q->cond = SDL_CreateCond();
//    if (!q->cond) {
//        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
//        return AVERROR(ENOMEM);
//    }
    return 0;
}

///向队列追加入一个packet(非线程安全操作)
static __inline__ int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList *pkt1;
    //创建链表节点
    pkt1 = av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    pkt1->serial = q->serial;

    ///队尾是空的，则说明队列为空，作为队首即可
    if (!q->last_pkt){
        q->first_pkt = pkt1;
    }
    ///队尾不空，则把这个节点和当前队列的最后一个节点连接
    else {
        q->last_pkt->next = pkt1;
    }
    ///更新尾结点为当前
    q->last_pkt = pkt1;
    //更新队列相关记录信息
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->duration += pkt1->pkt.duration;
    /* XXX: should duplicate packet data in DV case */
//    SDL_CondSignal(q->cond);
    return 0;
}

///向队列加入一个packet(线程安全的操作)
static __inline__ int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;
    ///获取信号量
    dispatch_semaphore_wait(q->mutex, DISPATCH_TIME_FOREVER);
    ret = packet_queue_put_private(q, pkt);
    dispatch_semaphore_signal(q->mutex);

    if (ret < 0)
        av_packet_unref(pkt);

    return ret;
}

///向队列加入一个空packet(线程安全的操作)
static __inline__ int packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

///缓存队列是否满
/*
 AV_DISPOSITION_ATTACHED_PIC ：有些流存在 video stream，但是却只是一张图片而已，常见于 mp3 的封面。
 */
static __inline__ int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
    (queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0));
}

///从队列里获取一个 packet；正常获取时返回值大于0
static __inline__ int packet_queue_get(PacketQueue *q, AVPacket *pkt, int *serial)
{
    assert(q);
    assert(pkt);
    //不阻塞
    int block = 0;
    int ret;

    dispatch_semaphore_wait(q->mutex, DISPATCH_TIME_FOREVER);
    for (;;) {
        //队列的头结点存在？
        MyAVPacketList *pkt1 = q->first_pkt;
        if (pkt1) {
            //修改队列头结点，将第二结点改为头结点
            q->first_pkt = pkt1->next;
            //头结点为空，则尾结点也置空，此时队列空了
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            //更新队列相关记录信息
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            q->duration -= pkt1->pkt.duration;
            //给结果指针赋值
            if (pkt) {
                *pkt = pkt1->pkt;
            }
            if (serial) {
                *serial = pkt1->serial;
            }
            //释放掉链表节点内存
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        }
//        else {
//            SDL_CondWait(q->cond, q->mutex);
//        }
    }
    dispatch_semaphore_signal(q->mutex);
    return ret;
}

///清理队列里的全部缓存，重置队列；
static __inline__ void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList *pkt, *pkt1;

    dispatch_semaphore_wait(q->mutex, DISPATCH_TIME_FOREVER);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    dispatch_semaphore_signal(q->mutex);
}

#endif /* FFPlayerInternalHeader_h */
