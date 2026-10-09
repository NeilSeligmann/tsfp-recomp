/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Optional host decode evidence only; no guest XMV adapter or playback timing. */
#include <stdio.h>
#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>

typedef struct stats { unsigned frames,errors; uint64_t samples,hash; int64_t first,last; } stats;
static void bytes(stats *s,const uint8_t *p,size_t n)
{for(size_t j=0;j<n;j++)s->hash=(s->hash^p[j])*UINT64_C(1099511628211);}
static void drain(AVCodecContext *c,AVFrame *f,stats *s)
{
    int r;
    while((r=avcodec_receive_frame(c,f))>=0) {
        if(!s->frames)s->first=f->pts;s->last=f->pts;s->frames++;
        if(c->codec_type==AVMEDIA_TYPE_VIDEO) {
            int n=av_image_get_buffer_size(f->format,f->width,f->height,1);
            uint8_t *b=n>0?av_malloc((size_t)n):NULL;
            if(!b || av_image_copy_to_buffer(b,n,(const uint8_t *const *)f->data,f->linesize,
                f->format,f->width,f->height,1)<0)s->errors++;
            else bytes(s,b,(size_t)n);
            av_free(b);
        } else {
            int channels=f->ch_layout.nb_channels;
            int planar=av_sample_fmt_is_planar(f->format);
            int planes=planar?channels:1;
            int unit=av_get_bytes_per_sample(f->format);
            if(unit<=0 || channels<=0 || f->nb_samples<0)s->errors++;
            else for(int j=0;j<planes;j++)bytes(s,f->extended_data[j],
                (size_t)f->nb_samples*(size_t)unit*(size_t)(planar?1:channels));
            s->samples+=(unsigned)f->nb_samples;
        }
        av_frame_unref(f);
    }
    if(r!=AVERROR(EAGAIN) && r!=AVERROR_EOF)s->errors++;
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    if((avformat_version()>>16)!=LIBAVFORMAT_VERSION_MAJOR ||
       (avcodec_version()>>16)!=LIBAVCODEC_VERSION_MAJOR ||
       (avutil_version()>>16)!=LIBAVUTIL_VERSION_MAJOR) {
        fprintf(stderr,"FFmpeg runtime/header major ABI mismatch\n");return 3;
    }
    AVFormatContext *fmt=NULL;
    if(avformat_open_input(&fmt,argv[1],NULL,NULL)<0 || avformat_find_stream_info(fmt,NULL)<0)return 4;
    AVCodecContext **contexts=av_calloc(fmt->nb_streams,sizeof(*contexts));
    stats *counts=av_calloc(fmt->nb_streams,sizeof(*counts));
    AVFrame *frame=av_frame_alloc();AVPacket *packet=av_packet_alloc();
    if(!contexts || !counts || !frame || !packet)return 5;
    for(unsigned i=0;i<fmt->nb_streams;i++) {
        const AVCodec *codec=avcodec_find_decoder(fmt->streams[i]->codecpar->codec_id);
        if(!codec)return 6;
        contexts[i]=avcodec_alloc_context3(codec);
        if(!contexts[i] || avcodec_parameters_to_context(contexts[i],fmt->streams[i]->codecpar)<0 ||
           avcodec_open2(contexts[i],codec,NULL)<0)return 7;
        counts[i].hash=UINT64_C(14695981039346656037);
    }
    int result;unsigned packets=0,send_errors=0;
    while((result=av_read_frame(fmt,packet))>=0) {
        packets++;int i=packet->stream_index;
        if(i<0 || (unsigned)i>=fmt->nb_streams)return 8;
        if(avcodec_send_packet(contexts[i],packet)<0)send_errors++;
        drain(contexts[i],frame,&counts[i]);av_packet_unref(packet);
    }
    printf("{\"runtime_versions\":[%u,%u,%u],\"raw_format_duration_us\":%lld,"
        "\"demux_end_code\":%d,\"demux_offset\":%lld,\"file_size\":%lld,"
        "\"packet_count\":%u,\"send_errors\":%u,\"streams\":[",
        avformat_version(),avcodec_version(),avutil_version(),(long long)fmt->duration,
        result,(long long)avio_tell(fmt->pb),(long long)avio_size(fmt->pb),packets,send_errors);
    unsigned errors=send_errors;
    for(unsigned i=0;i<fmt->nb_streams;i++) {
        if(avcodec_send_packet(contexts[i],NULL)<0)counts[i].errors++;
        drain(contexts[i],frame,&counts[i]);errors+=counts[i].errors;
        AVCodecContext *c=contexts[i];AVStream *st=fmt->streams[i];
        const char *pixel=av_get_pix_fmt_name(c->pix_fmt),*sample=av_get_sample_fmt_name(c->sample_fmt);
        printf("%s{\"codec\":\"%s\",\"media_type\":%d,\"width\":%d,\"height\":%d,"
            "\"sample_rate\":%d,\"channels\":%d,\"pixel_format\":\"%s\","
            "\"sample_format\":\"%s\",\"frames\":%u,\"samples\":%llu,"
            "\"first_pts\":%lld,\"last_pts\":%lld,\"time_base\":[%d,%d],"
            "\"raw_stream_duration\":%lld,\"decoded_bytes_fnv64\":\"%016llx\",\"errors\":%u}",
            i?",":"",c->codec->name,c->codec_type,c->width,c->height,c->sample_rate,
            c->ch_layout.nb_channels,pixel?pixel:"",sample?sample:"",counts[i].frames,
            (unsigned long long)counts[i].samples,(long long)counts[i].first,(long long)counts[i].last,
            st->time_base.num,st->time_base.den,(long long)st->duration,
            (unsigned long long)counts[i].hash,counts[i].errors);
        avcodec_free_context(&contexts[i]);
    }
    printf("]}\n");av_free(contexts);av_free(counts);av_frame_free(&frame);
    av_packet_free(&packet);avformat_close_input(&fmt);return errors?9:0;
}
