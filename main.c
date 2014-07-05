/*  Copyright (C) 2014 Gunnar Nitsche <herrnitsche@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>

#define MAX_FRAMES 5000

int main(int argc, char** argv) {

    /* initialize stuff */
    av_register_all();
    avcodec_register_all();
    avformat_network_init();

    /* options used to configure stream handler */
    AVDictionary* str_opts = NULL;
    av_dict_set(&str_opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&str_opts, "reorder_queue_size", "1000", 0);

    /* create context for format io */
    AVFormatContext* context_A = avformat_alloc_context();
    AVFormatContext* context_B = avformat_alloc_context();

    /* establish connection to video sources */
    /* connect to rtsp stream A */
    if (avformat_open_input(&context_A, "rtsp://some_video_source-A", NULL, &str_opts) != 0) {
        return EXIT_FAILURE;
    }

    /* check if stream is valid */
    if (avformat_find_stream_info(context_A, NULL) < 0){
        return EXIT_FAILURE;
    }

    /* connect to rtsp stream B */
    if (avformat_open_input(&context_B, "rtsp://some_video_source-B", NULL, &str_opts) != 0) {
        return EXIT_FAILURE;
    }

    /* check if stream is valid */
    if (avformat_find_stream_info(context_B, NULL) < 0) {
        return EXIT_FAILURE;
    }

    /* looking for video channel in rtsp stream */
    int video_stream_index_A;
    int video_stream_index_B;

    /* stream A */
    for (unsigned int i = 0; i < context_A->nb_streams; i++) {
        if(context_A->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_A = i;
        }
    }

    /* stream B */
    for (unsigned int i = 0; i < context_B->nb_streams; i++) {
        if (context_B->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_B = i;
        }
    }

    /* create and initialize packet structs to transfer packets from stream to output */
    AVPacket packet_A;
    AVPacket packet_B;
    av_init_packet(&packet_A);
    av_init_packet(&packet_B);

    /* create and open output file, we use a mkv container because to make use
     * of the ability to have multiple video streams in one file                */
    AVOutputFormat* ofguess = av_guess_format(NULL,"output.mkv",NULL);
    AVFormatContext* context_output = avformat_alloc_context();
    /* set guessed output format */
    context_output->oformat = ofguess;
    avio_open(&context_output->pb, "output.mkv", AVIO_FLAG_WRITE);

    /* put video streams into the container */
    AVStream* ostream_A = avformat_new_stream(context_output, context_A->streams[video_stream_index_A]->codec->codec);
    AVStream* ostream_B = avformat_new_stream(context_output, context_B->streams[video_stream_index_B]->codec->codec);

    /* copy basic stream settings from rtsp stream to video stream in mkv container */
    ostream_A->r_frame_rate = context_A->streams[video_stream_index_A]->r_frame_rate;
    ostream_A->avg_frame_rate = context_A->streams[video_stream_index_A]->r_frame_rate;
    ostream_A->time_base = context_A->streams[video_stream_index_A]->time_base;

    ostream_B->r_frame_rate = context_B->streams[video_stream_index_B]->r_frame_rate;
    ostream_B->avg_frame_rate = context_B->streams[video_stream_index_B]->r_frame_rate;
    ostream_B->time_base = context_B->streams[video_stream_index_B]->time_base;

    /* copy codec settings */
    avcodec_copy_context(ostream_A->codec, context_A->streams[video_stream_index_A]->codec);
    avcodec_copy_context(ostream_B->codec, context_B->streams[video_stream_index_B]->codec);

    /* writing output file header */
    avformat_write_header(context_output, NULL);

    /* frame counter for both streams to stop recording on hitting the limit MAX_FRAMES */ 
    int cnt_A = 0;
    int cnt_B = 0;

    av_dump_format(context_A, NULL, "stream A", 0);
    av_dump_format(context_B, NULL, "stream B", 0);
    av_dump_format(context_output, NULL, "stream out", 1);

    /* start reading packets from streams and write them to the file */
    /* calculate timing differenc of both streams considering NTP time */
    int64_t dt_us = context_B->start_time_realtime - context_A->start_time_realtime;
    printf("(NTP_B-NTP_A)/us: %" PRId64 "\n", dt_us);
    AVRational us2s = {1, 1000000};

    /* calculate time differences between both streams expressed in pts */
    int64_t dpts = av_rescale_q(dt_us, us2s, ostream_B->time_base);
    printf("delta pts: %" PRId64 "\n", dpts);

    while (av_read_frame(context_A, &packet_A) >= 0 &&
           av_read_frame(context_B, &packet_B) >= 0 && 
           cnt_A < MAX_FRAMES && cnt_B < MAX_FRAMES) {

        if (context_A->start_time_realtime != AV_NOPTS_VALUE && context_B->start_time_realtime != AV_NOPTS_VALUE) {

            /* calculate timing differenc of both streams considering NTP time */
            int64_t dt_us = context_B->start_time_realtime - context_A->start_time_realtime;
            printf("(NTP_B-NTP_A)/us: %" PRId64 "\n", dt_us);

            /* reset time realtime field to get noticed on new RTCP SR packet*/
            context_A->start_time_realtime = AV_NOPTS_VALUE;
            context_B->start_time_realtime = AV_NOPTS_VALUE;

            /* calculate time differences between both streams expressed in pts */
            int64_t dpts = av_rescale_q(dt_us, us2s, ostream_B->time_base);
        }

        /* writing rtsp packets into mkv container */
        if(packet_A.stream_index == video_stream_index_A){
            packet_A.stream_index = 0;
            packet_A.pts = av_rescale_q(packet_A.pts, context_A->streams[video_stream_index_A]->time_base, ostream_A->time_base);
            packet_A.dts = av_rescale_q(packet_A.dts, context_A->streams[video_stream_index_A]->time_base, ostream_A->time_base);
            av_interleaved_write_frame(context_output, &packet_A); 
            av_free_packet(&packet_A);
            av_init_packet(&packet_A);           
            cnt_A++;
        }

        /* writing rtsp packets into mkv container and applying differences in 
         * pts to make them syncronous shown to the user                        */
        if(packet_B.stream_index == video_stream_index_B){
            packet_B.stream_index = 1;
            packet_B.pts = av_rescale_q(packet_B.pts, context_B->streams[video_stream_index_B]->time_base, ostream_B->time_base)+dpts;
            packet_B.dts = av_rescale_q(packet_B.dts, context_B->streams[video_stream_index_B]->time_base, ostream_B->time_base)+dpts;
            av_interleaved_write_frame(context_output, &packet_B); 
            av_free_packet(&packet_B);
            av_init_packet(&packet_B);   
            cnt_B++;
        }
    }

    /* stop streaming from sources */
    av_read_pause(context_A);
    av_read_pause(context_B);

    /* finishing output container */
    av_write_trailer(context_output);
    avio_close(context_output->pb);
    avformat_free_context(context_output);

    /* tear down all network stuff */
    avformat_network_deinit();

    return (EXIT_SUCCESS);
}