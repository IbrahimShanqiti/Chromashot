// chromashot.c

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>


#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>


typedef struct {
    int r;
    int g;
    int b;
} Color;


// Define frame processing functions (we will implement these next!)
Color process_frame(uint8_t *rgb_data, int width, int height, int linesize);


static int write_ppm(const char *fname,
    int width, int height,
    const uint8_t *rgb)
{
    FILE *fp = fopen(fname, "wb");
    if (!fp) return -1;

    /* P6 header: "P6\n<width> <height>\n255\n" */
    fprintf(fp, "P6\n%d %d\n255\n", width, height);
    size_t n = fwrite(rgb, 1, (size_t)width*height*3, fp);
    fclose(fp);
    return (n == (size_t)width*height*3) ? 0 : -1;
}

static uint8_t *build_timeline(const Color *cols,
    int n_cols,          /* number of stripes   */
    int height,          /* image height (px)   */
    int *out_w)          /* returns width       */
{
int w = n_cols;               /* 1‑pixel‑wide stripe per colour         */
*out_w = w;
uint8_t *img = malloc((size_t)w * height * 3);
if (!img) return NULL;

for (int x = 0; x < w; ++x) {
const Color c = cols[x];
for (int y = 0; y < height; ++y) {
uint8_t *p = img + (y*w + x)*3;
p[0] = (uint8_t)c.r;
p[1] = (uint8_t)c.g;
p[2] = (uint8_t)c.b;
}
}
return img;
} 

// Main program
int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr,"Usage: %s input.mp4 output.png\n", argv[0]);
        return 1;
    }
    const char *input_filename  = argv[1];
    const char *output_filename = argv[2];

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, input_filename, NULL, NULL)            < 0 ||
        avformat_find_stream_info(fmt, NULL)                             < 0) {
        fprintf(stderr,"Cannot open %s\n", input_filename);  return 1;
    }

    int vstream = -1;
    for (unsigned i=0;i<fmt->nb_streams;i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){vstream=i;break;}
    if (vstream < 0){fprintf(stderr,"No video stream\n");return 1;}

    AVCodecParameters *par = fmt->streams[vstream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);          /* ★ */
    if (!codec){fprintf(stderr,"Unsupported codec\n");return 1;}

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->thread_count = 0;
    ctx->thread_type  = FF_THREAD_FRAME;
    ctx->skip_loop_filter = AVDISCARD_ALL;
    ctx->skip_idct        = AVDISCARD_ALL;
    
    avcodec_parameters_to_context(ctx, par);
    if (avcodec_open2(ctx, codec, NULL) < 0){fprintf(stderr,"Codec open\n");return 1;}

    int w = ctx->width, h = ctx->height;
    AVFrame *f_dec = av_frame_alloc();
    AVFrame *f_rgb = av_frame_alloc();
    int buf_sz = av_image_get_buffer_size(AV_PIX_FMT_RGB24, w, h, 1);
    uint8_t *buf = av_malloc(buf_sz);
    av_image_fill_arrays(f_rgb->data, f_rgb->linesize, buf,
                         AV_PIX_FMT_RGB24, w, h, 1);

        /* -- replace the sws context: convert & down‑scale in one shot ----------- */
    int thumb_w = 160;                       /* or user‑chosen width            */
    int thumb_h = ctx->height * thumb_w / ctx->width;
    struct SwsContext *sws = sws_getContext(
            ctx->width, ctx->height, ctx->pix_fmt,
            thumb_w,     thumb_h,     AV_PIX_FMT_RGB24,
            SWS_BILINEAR, NULL, NULL, NULL);


    AVPacket *pkt = av_packet_alloc();

    AVStream *st = fmt->streams[vstream];

    double dur_sec = (double)st->duration * st->time_base.num / st->time_base.den;
    /* If st->duration is 0, use fmt->duration (microseconds) */
    if (dur_sec == 0 && fmt->duration != AV_NOPTS_VALUE)
        dur_sec = fmt->duration / 1e6;

    /* average frame rate */
    double fps = 0.0;
    if (st->avg_frame_rate.num && st->avg_frame_rate.den)
        fps = (double)st->avg_frame_rate.num / st->avg_frame_rate.den;
    else if (st->r_frame_rate.num && st->r_frame_rate.den)
        fps = (double)st->r_frame_rate.num / st->r_frame_rate.den;

    int64_t est_frames = (fps > 0 && dur_sec > 0) ? (int64_t)(dur_sec * fps + 0.5) : -1;
    
    const int K = 1920;                     /* max stripes in the PNG          */

    int keep = (est_frames < K) ? est_frames : K;
    Color *colors = malloc(sizeof(Color) * keep);
    int kept = 0;

    int k = 0;
    int64_t frame_index = 0;     /* counts *decoded* frames           */
    int64_t *targets = malloc(sizeof(int64_t) * keep);
    double step = (double)est_frames / keep;      /* we fixed est_frames earlier */
    double t = 0.0;
    for (int i = 0; i < keep; ++i, t += step)
        targets[i] = (int64_t)(t + 0.5);          /* rounded index to keep */

    int tgt_i      = 0;     /* index in targets[]                */

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(ctx, pkt) == 0) {
            while (avcodec_receive_frame(ctx, f_dec) == 0) {

                /* Is this a frame we want? */
                if (tgt_i < keep && frame_index == targets[tgt_i]) {

                    /* convert & analyse ONLY the wanted frame */
                    sws_scale(sws, (const uint8_t* const*)f_dec->data, f_dec->linesize,
                            0, ctx->height, f_rgb->data, f_rgb->linesize);

                    Color c = process_frame(f_rgb->data[0],
                                            thumb_w, thumb_h, f_rgb->linesize[0]);

                    colors[kept++] = c;          /* store stripe colour   */
                    ++tgt_i;                     /* move to next target   */
                }

                ++frame_index;                   /* every decoded frame   */
                av_frame_unref(f_dec);
            }
        }
        av_packet_unref(pkt);
        if (tgt_i == keep) break;                /* got all 1920 stripes  */
    }

    int out_w = 0;                                   /* initialise */   
    uint8_t *rgb = build_timeline(colors, kept, 1080, &out_w);
    write_ppm(output_filename, out_w, 1080, rgb);

    /* tidy */
    av_free(buf); av_frame_free(&f_rgb); av_frame_free(&f_dec);
    av_packet_free(&pkt); sws_freeContext(sws);
    avcodec_free_context(&ctx); avformat_close_input(&fmt);


    return 0;
}


Color process_frame(uint8_t *rgb_data, int width, int height, int linesize) {
    uint64_t r = 0;
    uint64_t g = 0;
    uint64_t b = 0;

    int pixel_count = 0;

    for (int y = 0; y < height; y++) {
        uint8_t *row = rgb_data + y * linesize;
        for (int x = 0; x < width; x++) {
            uint8_t red = row[x * 3 + 0];
            uint8_t green = row[x * 3 + 1];
            uint8_t blue = row[x * 3 + 2];

            r += (uint64_t)red * red;
            g += (uint64_t)green * green;
            b += (uint64_t)blue * blue;

            pixel_count++;
        }
    }

    if (pixel_count == 0) {
        return (Color){0, 0, 0};
    }

    r = r / pixel_count;
    g = g / pixel_count;
    b = b / pixel_count;

    r = (int)(sqrt(r));
    g = (int)(sqrt(g));
    b = (int)(sqrt(b));

    Color c = { (int)r, (int)g, (int)b };
    return c;
}

